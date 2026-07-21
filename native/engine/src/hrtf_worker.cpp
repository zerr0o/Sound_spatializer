#include "sound_spatializer/hrtf_worker.hpp"

#include "sound_spatializer/spsc_ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <optional>
#include <thread>

namespace sound_spatializer {
namespace {

class AtomicHrtfRequestMailbox {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    void store(const HrtfPreparationRequest& request) noexcept {
        guard_.fetch_add(1, std::memory_order_acq_rel);
        generation_.store(request.generation, std::memory_order_relaxed);
        scene_revision_.store(request.scene_revision, std::memory_order_relaxed);
        database_.store(reinterpret_cast<std::uintptr_t>(request.database), std::memory_order_relaxed);
        source_count_.store(static_cast<std::uint32_t>(request.source_count), std::memory_order_relaxed);
        for (std::size_t source = 0; source < request.head_relative_directions.size(); ++source) {
            const Vec3f direction = request.head_relative_directions[source];
            directions_[source * 3].store(bits(direction.x), std::memory_order_relaxed);
            directions_[source * 3 + 1].store(bits(direction.y), std::memory_order_relaxed);
            directions_[source * 3 + 2].store(bits(direction.z), std::memory_order_relaxed);
            gains_[source].store(bits(request.speaker_gains[source]), std::memory_order_relaxed);
        }
        world_to_head_[0].store(bits(request.world_to_head.w), std::memory_order_relaxed);
        world_to_head_[1].store(bits(request.world_to_head.x), std::memory_order_relaxed);
        world_to_head_[2].store(bits(request.world_to_head.y), std::memory_order_relaxed);
        world_to_head_[3].store(bits(request.world_to_head.z), std::memory_order_relaxed);
        room_enabled_.store(request.room_enabled ? 1U : 0U, std::memory_order_relaxed);
        guard_.fetch_add(1, std::memory_order_release);
        published_generation_.store(request.generation, std::memory_order_release);
        guard_.notify_one();
    }

    [[nodiscard]] bool load_if_new(std::uint64_t last_generation, HrtfPreparationRequest& request) const noexcept {
        for (;;) {
            const std::uint64_t begin = guard_.load(std::memory_order_acquire);
            if ((begin & 1U) != 0U) {
                std::this_thread::yield();
                continue;
            }
            request.generation = generation_.load(std::memory_order_relaxed);
            if (request.generation == 0 || request.generation == last_generation)
                return false;
            request.scene_revision = scene_revision_.load(std::memory_order_relaxed);
            request.database = reinterpret_cast<const IHrtfDatabase*>(database_.load(std::memory_order_relaxed));
            request.source_count = source_count_.load(std::memory_order_relaxed);
            for (std::size_t source = 0; source < request.head_relative_directions.size(); ++source) {
                request.head_relative_directions[source] = {
                    value(directions_[source * 3].load(std::memory_order_relaxed)),
                    value(directions_[source * 3 + 1].load(std::memory_order_relaxed)),
                    value(directions_[source * 3 + 2].load(std::memory_order_relaxed)),
                };
                request.speaker_gains[source] = value(gains_[source].load(std::memory_order_relaxed));
            }
            request.world_to_head = {
                value(world_to_head_[0].load(std::memory_order_relaxed)),
                value(world_to_head_[1].load(std::memory_order_relaxed)),
                value(world_to_head_[2].load(std::memory_order_relaxed)),
                value(world_to_head_[3].load(std::memory_order_relaxed)),
            };
            request.room_enabled = room_enabled_.load(std::memory_order_relaxed) != 0;
            const std::uint64_t end = guard_.load(std::memory_order_acquire);
            if (begin == end)
                return true;
        }
    }

    [[nodiscard]] std::uint64_t guard_value() const noexcept { return guard_.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint64_t latest_generation() const noexcept {
        return published_generation_.load(std::memory_order_acquire);
    }

    void wait(std::uint64_t unchanged_guard) const noexcept { guard_.wait(unchanged_guard, std::memory_order_acquire); }

    void wake() noexcept {
        guard_.fetch_add(2, std::memory_order_release);
        guard_.notify_one();
    }

private:
    [[nodiscard]] static std::uint32_t bits(float input) noexcept { return std::bit_cast<std::uint32_t>(input); }
    [[nodiscard]] static float value(std::uint32_t input) noexcept { return std::bit_cast<float>(input); }

    mutable std::atomic<std::uint64_t> guard_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<std::uint64_t> published_generation_{};
    std::atomic<std::uint64_t> scene_revision_{};
    std::atomic<std::uintptr_t> database_{};
    std::array<std::atomic<std::uint32_t>, kDirectionalSourceCount * 3> directions_{};
    std::array<std::atomic<std::uint32_t>, kDirectionalSourceCount> gains_{};
    std::atomic<std::uint32_t> source_count_{2};
    std::array<std::atomic<std::uint32_t>, 4> world_to_head_{};
    std::atomic<std::uint32_t> room_enabled_{};
};

} // namespace

struct HrtfPreparationWorker::Impl {
    static constexpr std::size_t kSlotCount = 3;
    static constexpr auto kRoomUpdateInterval = std::chrono::microseconds(66'667);
    static constexpr auto kPendingRoomPollInterval = std::chrono::milliseconds(1);

    Impl()
        : direct_slots(std::make_unique<std::array<PreparedDirectHrtfUpdate, kSlotCount>>()),
          room_slots(std::make_unique<std::array<PreparedRoomHrtfUpdate, kSlotCount>>()) {
        for (std::uint8_t slot = 0; slot < static_cast<std::uint8_t>(kSlotCount); ++slot) {
            (void)direct_free_slots.try_push(slot);
            (void)room_free_slots.try_push(slot);
        }
        thread = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
    }

    ~Impl() {
        thread.request_stop();
        mailbox.wake();
        direct_free_epoch.fetch_add(1, std::memory_order_release);
        direct_free_epoch.notify_one();
        room_free_epoch.fetch_add(1, std::memory_order_release);
        room_free_epoch.notify_one();
        if (thread.joinable())
            thread.join();
    }

    [[nodiscard]] bool acquire_direct_slot(std::uint8_t& slot, std::stop_token stop_token) noexcept {
        while (!direct_free_slots.try_pop(slot)) {
            const std::uint64_t unchanged = direct_free_epoch.load(std::memory_order_acquire);
            if (stop_token.stop_requested())
                return false;
            direct_free_epoch.wait(unchanged, std::memory_order_acquire);
        }
        return true;
    }

    [[nodiscard]] bool acquire_room_slot(std::uint8_t& slot, std::stop_token stop_token) noexcept {
        while (!room_free_slots.try_pop(slot)) {
            const std::uint64_t unchanged = room_free_epoch.load(std::memory_order_acquire);
            if (stop_token.stop_requested())
                return false;
            room_free_epoch.wait(unchanged, std::memory_order_acquire);
        }
        return true;
    }

    void release_direct_slot(std::uint8_t slot) noexcept {
        if (slot >= kSlotCount)
            return;
        if (direct_free_slots.try_push(slot)) {
            direct_free_epoch.fetch_add(1, std::memory_order_release);
            direct_free_epoch.notify_one();
        }
    }

    void release_room_slot(std::uint8_t slot) noexcept {
        if (slot >= kSlotCount)
            return;
        if (room_free_slots.try_push(slot)) {
            room_free_epoch.fetch_add(1, std::memory_order_release);
            room_free_epoch.notify_one();
        }
    }

    [[nodiscard]] bool prepare_direct(const HrtfPreparationRequest& request, std::stop_token stop_token) noexcept {
        std::uint8_t slot = 0;
        if (!acquire_direct_slot(slot, stop_token))
            return false;

        PreparedDirectHrtfUpdate& update = (*direct_slots)[slot];
        update.generation = request.generation;
        update.scene_revision = request.scene_revision;
        update.valid = false;
        if (request.database != nullptr) {
            update.valid = build_binaural_filter_bank(*request.database, request.head_relative_directions,
                                                      request.speaker_gains, request.source_count, update.filters);
        }
        if (!direct_ready_slots.try_push(slot)) {
            release_direct_slot(slot);
            return false;
        }
        latest_completed.store(request.generation, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool prepare_room(const HrtfPreparationRequest& request, std::stop_token stop_token) noexcept {
        if (mailbox.latest_generation() > request.generation)
            return true;

        std::uint8_t slot = 0;
        if (!acquire_room_slot(slot, stop_token))
            return false;
        PreparedRoomHrtfUpdate& update = (*room_slots)[slot];
        update = {};
        update.generation = request.generation;
        update.scene_revision = request.scene_revision;
        if (request.database != nullptr) {
            update.valid = projector.prepare_filter_bank(request.world_to_head, *request.database, update.filters);
        }

        // A room projection is deliberately monolithic so only this thread ever
        // queries libmysofa. If a fresher pose arrived meanwhile, discard the stale
        // room bank and service its direct filter first. The already-published
        // direct bank for this generation remains unaffected.
        if (mailbox.latest_generation() > request.generation) {
            release_room_slot(slot);
            return true;
        }
        if (!room_ready_slots.try_push(slot)) {
            release_room_slot(slot);
            return false;
        }
        latest_room_completed.store(request.generation, std::memory_order_release);
        return true;
    }

    void run(std::stop_token stop_token) noexcept {
        using Clock = std::chrono::steady_clock;
        std::uint64_t consumed_generation = 0;
        std::uint64_t last_room_scene_revision = 0;
        std::optional<HrtfPreparationRequest> pending_room{};
        Clock::time_point next_room_update = Clock::time_point::min();
        while (!stop_token.stop_requested()) {
            const std::uint64_t unchanged_mailbox = mailbox.guard_value();
            HrtfPreparationRequest request{};
            if (mailbox.load_if_new(consumed_generation, request)) {
                if (!prepare_direct(request, stop_token))
                    return;
                consumed_generation = request.generation;
                if (request.room_enabled)
                    pending_room = request;
                else
                    pending_room.reset();
                // Re-read the latest mailbox before starting any room work. This makes
                // a burst of head poses converge on the newest direct bank before
                // spending time on Ambisonics.
                continue;
            }

            if (pending_room) {
                if (mailbox.latest_generation() > pending_room->generation)
                    continue;
                const auto now = Clock::now();
                const bool scene_changed = pending_room->scene_revision != last_room_scene_revision;
                if (scene_changed || now >= next_room_update) {
                    const HrtfPreparationRequest room_request = *pending_room;
                    pending_room.reset();
                    if (!prepare_room(room_request, stop_token))
                        return;
                    // Count a completed projection attempt against the cadence even if a
                    // newer pose made its result stale. Otherwise a room job slower than
                    // the camera cadence could immediately restart forever and starve
                    // direct updates.
                    last_room_scene_revision = room_request.scene_revision;
                    next_room_update = Clock::now() + kRoomUpdateInterval;
                    continue;
                }

                const auto remaining = next_room_update - now;
                std::this_thread::sleep_for(std::min(remaining, Clock::duration(kPendingRoomPollInterval)));
                continue;
            }

            if (!stop_token.stop_requested())
                mailbox.wait(unchanged_mailbox);
        }
    }

    AtomicHrtfRequestMailbox mailbox{};
    std::unique_ptr<std::array<PreparedDirectHrtfUpdate, kSlotCount>> direct_slots{};
    std::unique_ptr<std::array<PreparedRoomHrtfUpdate, kSlotCount>> room_slots{};
    SpscRingBuffer<std::uint8_t> direct_free_slots{4};
    SpscRingBuffer<std::uint8_t> direct_ready_slots{4};
    SpscRingBuffer<std::uint8_t> room_free_slots{4};
    SpscRingBuffer<std::uint8_t> room_ready_slots{4};
    std::atomic<std::uint64_t> direct_free_epoch{};
    std::atomic<std::uint64_t> room_free_epoch{};
    std::atomic<std::uint64_t> latest_completed{};
    std::atomic<std::uint64_t> latest_room_completed{};
    AmbisonicBinauralDecoderOrder3 projector{};
    std::jthread thread{};
};

HrtfPreparationWorker::HrtfPreparationWorker() : impl_(std::make_unique<Impl>()) {}
HrtfPreparationWorker::~HrtfPreparationWorker() = default;

void HrtfPreparationWorker::submit_latest(const HrtfPreparationRequest& request) noexcept {
    impl_->mailbox.store(request);
}

bool HrtfPreparationWorker::try_acquire_latest_direct(const PreparedDirectHrtfUpdate*& update,
                                                      std::uint8_t& slot_token) noexcept {
    bool found = false;
    std::uint8_t candidate = 0;
    std::uint8_t ready = 0;
    while (impl_->direct_ready_slots.try_pop(ready)) {
        if (!found || (*impl_->direct_slots)[ready].generation > (*impl_->direct_slots)[candidate].generation) {
            if (found)
                release_direct(candidate);
            candidate = ready;
            found = true;
        } else {
            release_direct(ready);
        }
    }
    if (!found)
        return false;
    update = &(*impl_->direct_slots)[candidate];
    slot_token = candidate;
    return true;
}

bool HrtfPreparationWorker::try_acquire_latest_room(const PreparedRoomHrtfUpdate*& update,
                                                    std::uint8_t& slot_token) noexcept {
    bool found = false;
    std::uint8_t candidate = 0;
    std::uint8_t ready = 0;
    while (impl_->room_ready_slots.try_pop(ready)) {
        if (!found || (*impl_->room_slots)[ready].generation > (*impl_->room_slots)[candidate].generation) {
            if (found)
                release_room(candidate);
            candidate = ready;
            found = true;
        } else {
            release_room(ready);
        }
    }
    if (!found)
        return false;
    update = &(*impl_->room_slots)[candidate];
    slot_token = candidate;
    return true;
}

void HrtfPreparationWorker::release_direct(std::uint8_t slot_token) noexcept { impl_->release_direct_slot(slot_token); }

void HrtfPreparationWorker::release_room(std::uint8_t slot_token) noexcept { impl_->release_room_slot(slot_token); }

std::uint64_t HrtfPreparationWorker::latest_completed_generation() const noexcept {
    return impl_->latest_completed.load(std::memory_order_acquire);
}

std::uint64_t HrtfPreparationWorker::latest_room_completed_generation() const noexcept {
    return impl_->latest_room_completed.load(std::memory_order_acquire);
}

} // namespace sound_spatializer

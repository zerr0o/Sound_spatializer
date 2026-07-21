#pragma once

#include "sound_spatializer/acoustics.hpp"
#include "sound_spatializer/hrtf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace sound_spatializer {

struct HrtfPreparationRequest {
    std::uint64_t generation{};
    std::uint64_t scene_revision{};
    const IHrtfDatabase* database{};
    std::array<Vec3f, 2> head_relative_directions{};
    std::array<float, 2> speaker_gains{};
    Quaternionf world_to_head{};
    bool room_enabled{};
};

static_assert(std::is_trivially_copyable_v<HrtfPreparationRequest>);

struct PreparedDirectHrtfUpdate {
    std::uint64_t generation{};
    std::uint64_t scene_revision{};
    HrirFilterBank filters{};
    bool valid{};
};

struct PreparedRoomHrtfUpdate {
    std::uint64_t generation{};
    std::uint64_t scene_revision{};
    AmbisonicBinauralFilterBankOrder3 filters{};
    bool valid{};
};

// A single producer publishes only the newest pose/scene request through an
// atomic mailbox. One worker owns every IHrtfDatabase call (libmysofa handles
// are therefore never queried concurrently). Direct and room banks use
// independent preallocated slot pools: the direct bank is published after two
// speaker queries, while the more expensive order-3 room projection is
// latest-wins and rate limited. The audio callback only acquires immutable
// ready slots and releases their indices.
class HrtfPreparationWorker {
public:
    HrtfPreparationWorker();
    ~HrtfPreparationWorker();

    HrtfPreparationWorker(const HrtfPreparationWorker&) = delete;
    HrtfPreparationWorker& operator=(const HrtfPreparationWorker&) = delete;

    void submit_latest(const HrtfPreparationRequest& request) noexcept;
    [[nodiscard]] bool try_acquire_latest_direct(const PreparedDirectHrtfUpdate*& update,
                                                 std::uint8_t& slot_token) noexcept;
    [[nodiscard]] bool try_acquire_latest_room(const PreparedRoomHrtfUpdate*& update,
                                               std::uint8_t& slot_token) noexcept;
    void release_direct(std::uint8_t slot_token) noexcept;
    void release_room(std::uint8_t slot_token) noexcept;
    [[nodiscard]] std::uint64_t latest_completed_generation() const noexcept;
    [[nodiscard]] std::uint64_t latest_room_completed_generation() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sound_spatializer

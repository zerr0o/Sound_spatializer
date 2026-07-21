#pragma once

// Preallocated single-producer/single-consumer byte history used between the
// WaveRT render and loopback streams. Both streams are fixed to the same
// 48 kHz/stereo/float32 format, so byte-for-byte transfer is intentional.

#if defined(SOUND_SPATIALIZER_KERNEL)
#include <ntddk.h>
#else
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#endif

#if defined(SOUND_SPATIALIZER_KERNEL)
using SoundSpatializerRenderHistoryMutex = KSPIN_LOCK;

class SoundSpatializerRenderHistoryLockGuard final
{
public:
    _IRQL_requires_max_(DISPATCH_LEVEL)
    _IRQL_raises_(DISPATCH_LEVEL)
    _IRQL_saves_global_(SoundSpatializerRenderHistoryLock, this)
    _Acquires_lock_(m_lock)
    explicit SoundSpatializerRenderHistoryLockGuard(SoundSpatializerRenderHistoryMutex& lock) noexcept
        : m_lock(lock)
    {
        KeAcquireSpinLock(&m_lock, &m_previousIrql);
    }

    _IRQL_requires_(DISPATCH_LEVEL)
    _IRQL_restores_global_(SoundSpatializerRenderHistoryLock, this)
    _Releases_lock_(m_lock)
    ~SoundSpatializerRenderHistoryLockGuard() noexcept
    {
        KeReleaseSpinLock(&m_lock, m_previousIrql);
    }

    SoundSpatializerRenderHistoryLockGuard(const SoundSpatializerRenderHistoryLockGuard&) = delete;
    SoundSpatializerRenderHistoryLockGuard& operator=(const SoundSpatializerRenderHistoryLockGuard&) = delete;

private:
    SoundSpatializerRenderHistoryMutex& m_lock;
    KIRQL m_previousIrql{};
};
#else
using SoundSpatializerRenderHistoryMutex = std::mutex;

class SoundSpatializerRenderHistoryLockGuard final
{
public:
    explicit SoundSpatializerRenderHistoryLockGuard(SoundSpatializerRenderHistoryMutex& lock) noexcept
        : m_guard(lock)
    {
    }

private:
    std::lock_guard<SoundSpatializerRenderHistoryMutex> m_guard;
};
#endif

class SoundSpatializerRenderHistory final
{
public:
    static constexpr unsigned long CapacityBytes = 65536;
    static constexpr unsigned long CapacityMask = CapacityBytes - 1;
    static constexpr unsigned long BytesPerFrame = 8;

    struct Diagnostics
    {
        unsigned long long bytesPublished;
        unsigned long long bytesConsumed;
        unsigned long long bytesDropped;
        unsigned long long bytesSilenced;
    };

    SoundSpatializerRenderHistory() noexcept
    {
#if defined(SOUND_SPATIALIZER_KERNEL)
        KeInitializeSpinLock(&m_lock);
#endif
        Reset();
    }

    void Reset() noexcept
    {
        SoundSpatializerRenderHistoryLockGuard guard(m_lock);
        Store(m_writeCounter, 0);
        Store(m_readCounter, UnprimedCounter);
        Store(m_bytesDropped, 0);
        Store(m_bytesSilenced, 0);
        Zero(m_storage, CapacityBytes);
    }

    void SetMuted(bool muted) noexcept
    {
        SoundSpatializerRenderHistoryLockGuard guard(m_lock);
        if (m_muted == muted)
        {
            return;
        }

        m_muted = muted;
        // Both edges jump to live. Entering mute prevents a pre-protection
        // backlog from being replayed; leaving mute prevents protected bytes
        // accumulated while the consumer was paused from becoming audible.
        Store(m_readCounter, Load(m_writeCounter));
    }

    void Write(const unsigned char* source, unsigned long byteCount) noexcept
    {
        if (source == nullptr || byteCount == 0)
        {
            return;
        }

        SoundSpatializerRenderHistoryLockGuard guard(m_lock);

        // Keep the newest capacity-sized suffix if a delayed callback supplies
        // more history than the bounded transport can represent.
        const unsigned long originalByteCount = byteCount;
        unsigned long skippedPrefix = 0;
        if (byteCount > CapacityBytes)
        {
            skippedPrefix = byteCount - CapacityBytes;
            source += skippedPrefix;
            byteCount = CapacityBytes;
        }

        const unsigned long long write = Load(m_writeCounter);
        // Preserve the logical position of a discarded prefix. Otherwise the
        // following write would use the wrong circular offset whenever the
        // delayed block is not an exact multiple of the capacity.
        unsigned long offset = static_cast<unsigned long>((write + skippedPrefix) & CapacityMask);
        unsigned long remaining = byteCount;
        while (remaining != 0)
        {
            const unsigned long run = Minimum(remaining, CapacityBytes - offset);
            Copy(m_storage + offset, source, run);
            source += run;
            remaining -= run;
            offset = 0;
        }

        // Publish only after all bytes are visible to the consumer.
        Store(m_writeCounter, write + originalByteCount);
    }

    // Returns the number of real render bytes copied. Any remainder is always
    // initialized to silence; callers never observe stale or synthetic data.
    unsigned long Read(unsigned char* destination, unsigned long byteCount) noexcept
    {
        if (destination == nullptr || byteCount == 0)
        {
            return 0;
        }

        SoundSpatializerRenderHistoryLockGuard guard(m_lock);

        unsigned long long read = Load(m_readCounter);
        const unsigned long long write = Load(m_writeCounter);

        // First loopback callback establishes a low-latency live edge. It does
        // not replay stale history accumulated before capture started.
        if (read == UnprimedCounter)
        {
            Store(m_readCounter, write);
            Zero(destination, byteCount);
            Add(m_bytesSilenced, byteCount);
            return 0;
        }

        if (write < read)
        {
            // Defensive recovery after a reset racing a consumer callback.
            read = write;
        }

        unsigned long long available = write - read;
        if (available > CapacityBytes)
        {
            // An overrun is a real-time discontinuity: jump to the newest
            // request-sized suffix instead of replaying hundreds of ms of
            // stale audio from the back of the ring.
            const unsigned long retained = Minimum(byteCount, CapacityBytes);
            const unsigned long long dropped = available - retained;
            read = write - retained;
            available = retained;
            Add(m_bytesDropped, dropped);
        }

        const unsigned long realByteCount = Minimum64(byteCount, available);
        if (m_muted)
        {
            Zero(destination, byteCount);
            Add(m_bytesSilenced, byteCount);
            Store(m_readCounter, read + realByteCount);
            return 0;
        }

        unsigned long offset = static_cast<unsigned long>(read & CapacityMask);
        unsigned long remaining = realByteCount;
        unsigned char* output = destination;
        while (remaining != 0)
        {
            const unsigned long run = Minimum(remaining, CapacityBytes - offset);
            Copy(output, m_storage + offset, run);
            output += run;
            remaining -= run;
            offset = 0;
        }

        if (realByteCount < byteCount)
        {
            const unsigned long silentByteCount = byteCount - realByteCount;
            Zero(destination + realByteCount, silentByteCount);
            Add(m_bytesSilenced, silentByteCount);
        }

        Store(m_readCounter, read + realByteCount);
        return realByteCount;
    }

    Diagnostics GetDiagnostics() const noexcept
    {
        SoundSpatializerRenderHistoryLockGuard guard(m_lock);
        const unsigned long long write = Load(m_writeCounter);
        const unsigned long long read = Load(m_readCounter);
        return
        {
            write,
            read == UnprimedCounter ? 0 : read,
            Load(m_bytesDropped),
            Load(m_bytesSilenced),
        };
    }

private:
    static constexpr unsigned long long UnprimedCounter = ~0ull;
    static_assert((CapacityBytes & CapacityMask) == 0, "Capacity must be a power of two");
    static_assert((CapacityBytes % BytesPerFrame) == 0, "Capacity must contain complete frames");

#if defined(SOUND_SPATIALIZER_KERNEL)
    using AtomicCounter = volatile LONG64;

    static unsigned long long Load(const AtomicCounter& value) noexcept
    {
        return static_cast<unsigned long long>(InterlockedCompareExchange64(
            const_cast<AtomicCounter*>(&value), 0, 0));
    }

    static void Store(AtomicCounter& value, unsigned long long next) noexcept
    {
        InterlockedExchange64(&value, static_cast<LONG64>(next));
    }

    static void Add(AtomicCounter& value, unsigned long long amount) noexcept
    {
        InterlockedAdd64(&value, static_cast<LONG64>(amount));
    }

    static void Copy(void* destination, const void* source, unsigned long bytes) noexcept
    {
        RtlCopyMemory(destination, source, bytes);
    }

    static void Zero(void* destination, unsigned long bytes) noexcept
    {
        RtlZeroMemory(destination, bytes);
    }
#else
    using AtomicCounter = std::atomic<unsigned long long>;

    static unsigned long long Load(const AtomicCounter& value) noexcept
    {
        return value.load(std::memory_order_acquire);
    }

    static void Store(AtomicCounter& value, unsigned long long next) noexcept
    {
        value.store(next, std::memory_order_release);
    }

    static void Add(AtomicCounter& value, unsigned long long amount) noexcept
    {
        value.fetch_add(amount, std::memory_order_relaxed);
    }

    static void Copy(void* destination, const void* source, unsigned long bytes) noexcept
    {
        std::memcpy(destination, source, bytes);
    }

    static void Zero(void* destination, unsigned long bytes) noexcept
    {
        std::memset(destination, 0, bytes);
    }
#endif

    static unsigned long Minimum(unsigned long left, unsigned long right) noexcept
    {
        return left < right ? left : right;
    }

    static unsigned long Minimum64(unsigned long requested, unsigned long long available) noexcept
    {
        return available < requested ? static_cast<unsigned long>(available) : requested;
    }

    mutable SoundSpatializerRenderHistoryMutex m_lock;
    bool m_muted{false};
    unsigned char m_storage[CapacityBytes];
    alignas(8) AtomicCounter m_writeCounter;
    alignas(8) AtomicCounter m_readCounter;
    alignas(8) AtomicCounter m_bytesDropped;
    alignas(8) AtomicCounter m_bytesSilenced;
};

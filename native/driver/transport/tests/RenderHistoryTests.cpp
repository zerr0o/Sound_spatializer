#include "SoundSpatializerRenderHistory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
int Fail(const char* message)
{
    std::cerr << "RenderHistory self-test: " << message << '\n';
    return 1;
}

int StressProducerConsumer()
{
    constexpr std::size_t blockBytes = 256;
    constexpr unsigned long iterations = 50000;

    SoundSpatializerRenderHistory history;
    std::array<unsigned char, blockBytes> block{};
    std::array<unsigned char, blockBytes> output{};

    // Prime the live edge before both threads start.
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != 0)
    {
        return Fail("stress transport did not prime with silence");
    }

    std::atomic<bool> start{false};
    std::atomic<bool> producerDone{false};
    std::atomic<bool> integrityFailure{false};

    std::thread producer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        for (unsigned long sequence = 0; sequence < iterations; ++sequence)
        {
            const auto marker = static_cast<unsigned char>((sequence % 251u) + 1u);
            block.fill(marker);
            history.Write(block.data(), static_cast<unsigned long>(block.size()));
            if ((sequence & 63u) == 0)
            {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        unsigned long passes = 0;
        while (!producerDone.load(std::memory_order_acquire) || passes < iterations)
        {
            output.fill(0xFF);
            const auto copied = history.Read(output.data(), static_cast<unsigned long>(output.size()));
            if (copied == 0)
            {
                if (!std::all_of(output.begin(), output.end(), [](unsigned char value) { return value == 0; }))
                {
                    integrityFailure.store(true, std::memory_order_release);
                    break;
                }
            }
            else if (copied != output.size() ||
                     !std::all_of(output.begin(), output.end(), [&](unsigned char value) {
                         return value == output.front() && value != 0;
                     }))
            {
                integrityFailure.store(true, std::memory_order_release);
                break;
            }

            ++passes;
            if ((passes & 63u) == 0)
            {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    return integrityFailure.load(std::memory_order_acquire)
        ? Fail("concurrent producer/consumer observed a torn block")
        : 0;
}
}

int main()
{
    SoundSpatializerRenderHistory history;
    std::array<unsigned char, 32> output{};

    std::array<unsigned char, 32> stale{};
    stale.fill(0x55);
    history.Write(stale.data(), static_cast<unsigned long>(stale.size()));
    output.fill(0xFF);
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != 0 ||
        !std::all_of(output.begin(), output.end(), [](unsigned char value) { return value == 0; }))
    {
        return Fail("the first capture callback must prime at the live edge with silence");
    }

    std::array<unsigned char, 32> pattern{};
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        pattern[index] = static_cast<unsigned char>(index + 1);
    }
    history.Write(pattern.data(), static_cast<unsigned long>(pattern.size()));
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != output.size() || output != pattern)
    {
        return Fail("published render bytes were not reproduced bit-for-bit");
    }

    output.fill(0xFF);
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != 0 ||
        !std::all_of(output.begin(), output.end(), [](unsigned char value) { return value == 0; }))
    {
        return Fail("underflow must produce initialized silence");
    }

    std::vector<unsigned char> first(SoundSpatializerRenderHistory::CapacityBytes, 0x11);
    std::vector<unsigned char> newest(SoundSpatializerRenderHistory::CapacityBytes, 0x22);
    history.Write(first.data(), static_cast<unsigned long>(first.size()));
    history.Write(newest.data(), static_cast<unsigned long>(newest.size()));

    std::vector<unsigned char> wrapped(newest.size());
    if (history.Read(wrapped.data(), static_cast<unsigned long>(wrapped.size())) != wrapped.size() || wrapped != newest)
    {
        return Fail("overrun recovery did not retain the newest bounded history");
    }

    history.Reset();
    output.fill(0xFF);
    (void)history.Read(output.data(), static_cast<unsigned long>(output.size()));
    std::vector<unsigned char> oversized(SoundSpatializerRenderHistory::CapacityBytes + 64);
    for (std::size_t index = 0; index < oversized.size(); ++index)
    {
        oversized[index] = static_cast<unsigned char>((index % 251u) + 1u);
    }
    history.Write(oversized.data(), static_cast<unsigned long>(oversized.size()));
    std::vector<unsigned char> oversizedOutput(SoundSpatializerRenderHistory::CapacityBytes);
    if (history.Read(oversizedOutput.data(), static_cast<unsigned long>(oversizedOutput.size())) !=
            oversizedOutput.size() ||
        !std::equal(oversizedOutput.begin(), oversizedOutput.end(), oversized.end() - oversizedOutput.size()))
    {
        return Fail("an oversized publish did not retain the correctly aligned newest suffix");
    }

    history.Reset();
    output.fill(0xFF);
    (void)history.Read(output.data(), static_cast<unsigned long>(output.size()));
    history.SetMuted(true);
    history.Write(pattern.data(), static_cast<unsigned long>(pattern.size()));
    output.fill(0xFF);
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != 0 ||
        !std::all_of(output.begin(), output.end(), [](unsigned char value) { return value == 0; }))
    {
        return Fail("loopback protection did not replace protected history with silence");
    }
    history.Write(pattern.data(), static_cast<unsigned long>(pattern.size()));
    history.SetMuted(false);
    std::array<unsigned char, 32> afterProtection{};
    afterProtection.fill(0xA5);
    history.Write(afterProtection.data(), static_cast<unsigned long>(afterProtection.size()));
    if (history.Read(output.data(), static_cast<unsigned long>(output.size())) != output.size() ||
        output != afterProtection)
    {
        return Fail("protected backlog was replayed after loopback protection was disabled");
    }

    const auto diagnostics = history.GetDiagnostics();
    if (diagnostics.bytesPublished != pattern.size() * 2 + afterProtection.size() ||
        diagnostics.bytesSilenced < output.size())
    {
        return Fail("deterministic diagnostics did not record publish and protection silence counters");
    }

    if (StressProducerConsumer() != 0)
    {
        return 1;
    }

    std::cout << "RenderHistory self-test: PASS\n";
    return 0;
}

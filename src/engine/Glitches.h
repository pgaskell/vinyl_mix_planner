#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vmp
{

// Something that made the output drop out or go silent when it shouldn't have.
enum class GlitchKind : uint8_t
{
    Overload,      // the engine took longer than the block it was rendering lasts
    DriverDropout, // the audio driver reported a dropout (reported by the host, not the engine)
    NotLoaded,     // a clip came up whose audio isn't loaded, so it played silence
    Underrun       // audio wasn't read from disk in time
};

struct Glitch
{
    GlitchKind kind = GlitchKind::Overload;
    int deck = -1;          // -1 when it isn't about one deck
    double seconds = 0.0;   // timeline position where it happened
    float usedMs = 0.0f;    // Overload: how long the block took to render...
    float budgetMs = 0.0f;  // ...and how long it lasts
};

// Single producer (audio thread), single consumer (message thread), lock-free and
// allocation-free. When full, new items are dropped and counted.
template <typename T, size_t Capacity>
class SpscQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer.
    bool push(const T& item)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        items_[head & (Capacity - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer.
    bool pop(T& item)
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        item = items_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer: items dropped because the queue was full, since the last call.
    uint32_t takeDropped() { return dropped_.exchange(0); }

private:
    std::array<T, Capacity> items_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<uint32_t> dropped_{0};
};

} // namespace vmp

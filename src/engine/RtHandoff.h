#pragma once

#include <atomic>
#include <memory>

namespace vmp
{

// Passes heap objects from the message thread to the audio thread without locking, allocating
// or freeing on the audio thread. The message thread publishes with set(); the audio thread
// picks the newest one up with acquire(); the one it replaced is deleted later by
// collectGarbage() on the message thread.
template <typename T>
class RtHandoff
{
public:
    RtHandoff() = default;
    RtHandoff(const RtHandoff&) = delete;
    RtHandoff& operator=(const RtHandoff&) = delete;

    ~RtHandoff()
    {
        delete current_;
        delete pending_.load();
        delete retired_.load();
    }

    // Message thread. An object published but never picked up is simply replaced.
    void set(std::unique_ptr<T> object) { delete pending_.exchange(object.release()); }

    // Message thread.
    void collectGarbage() { delete retired_.exchange(nullptr); }

    // Audio thread. Returns the current object, which stays valid until the next acquire().
    T* acquire()
    {
        // One retired slot: wait for the message thread to free the previous object first.
        if (retired_.load() == nullptr)
            if (T* next = pending_.exchange(nullptr))
            {
                retired_.store(current_);
                current_ = next;
            }
        return current_;
    }

private:
    std::atomic<T*> pending_{nullptr};
    std::atomic<T*> retired_{nullptr};
    T* current_ = nullptr; // audio thread only
};

} // namespace vmp

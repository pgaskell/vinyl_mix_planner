#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

namespace vmp
{

// Counts the dropouts Core Audio reports for the device in use (its "processor overload"
// notifications), including ones that aren't the engine's fault. JUCE's getXRunCount() would do
// this, but in JUCE 9.0.4 it always returns -1 on macOS.
class DriverDropouts
{
public:
    DriverDropouts() = default;
    ~DriverDropouts() { watch(nullptr); }
    DriverDropouts(const DriverDropouts&) = delete;
    DriverDropouts& operator=(const DriverDropouts&) = delete;

    // Message thread. Starts watching the device (found by name), or stops if null or not found.
    void watch(juce::AudioIODevice* device);
    bool watching() const { return deviceId_ != 0; }

    // Any thread. Dropouts since watching began.
    int count() const { return count_.load(); }

private:
    uint32_t deviceId_ = 0; // AudioObjectID
    std::atomic<int> count_{0};
};

} // namespace vmp

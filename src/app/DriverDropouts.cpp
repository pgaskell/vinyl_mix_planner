#include "app/DriverDropouts.h"

#include <CoreAudio/CoreAudio.h>

#include <vector>

namespace vmp
{

namespace
{
constexpr AudioObjectPropertyAddress kOverload{kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                               0 /* main element */};

OSStatus onOverload(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* counter)
{
    static_cast<std::atomic<int>*>(counter)->fetch_add(1);
    return noErr;
}

// The Core Audio device JUCE calls `name`; 0 if there isn't one.
AudioObjectID findDevice(const juce::String& name)
{
    const AudioObjectPropertyAddress devices{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, nullptr, &size) != noErr)
        return 0;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, nullptr, &size, ids.data()) != noErr)
        return 0;

    const AudioObjectPropertyAddress nameAddress{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0};
    for (const AudioObjectID id : ids)
    {
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof cfName;
        if (AudioObjectGetPropertyData(id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || cfName == nullptr)
            continue;
        const bool match = juce::String::fromCFString(cfName) == name;
        CFRelease(cfName);
        if (match)
            return id;
    }
    return 0;
}
} // namespace

void DriverDropouts::watch(juce::AudioIODevice* device)
{
    const AudioObjectID id = device != nullptr ? findDevice(device->getName()) : 0;
    if (id == deviceId_)
        return;
    if (deviceId_ != 0)
        AudioObjectRemovePropertyListener(deviceId_, &kOverload, onOverload, &count_);
    deviceId_ = 0;
    if (id != 0 && AudioObjectAddPropertyListener(id, &kOverload, onOverload, &count_) == noErr)
        deviceId_ = id;
}

} // namespace vmp

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmp
{

// What a hardware mixer channel's input selector is set to.
enum class MixerInput
{
    Unknown, // not reported yet: the mixer sends it when the selector moves, or in a snapshot
    Phono,
    Line,
    Digital,
    Usb
};

enum class CrossfaderAssign
{
    A,
    Thru,
    B
};

// How a hardware mixer reports itself over MIDI, and how its USB audio reaches its channels.
// Control numbers are MIDI CCs; any MIDI channel is accepted (the port belongs to the mixer).
struct MixerProfile
{
    struct Channel
    {
        int trim, high, mid, low, color, fader, crossfaderAssign;
        int usbPair; // stereo output pair (0 = channels 1/2) that the USB input position plays
    };
    struct InputNote // a note that switches on while the selector is at `input`
    {
        int note;
        int channel;
        MixerInput input;
    };

    std::string name; // matched against the start of Core Audio and MIDI device names
    std::vector<Channel> channels;
    std::vector<InputNote> inputs;
    int crossfader;
};

// The profile for a device name ("DJM-900SRT"), or nullptr if the mixer isn't known.
const MixerProfile* findMixerProfile(std::string_view deviceName);

// MIDI 0..127 to our 0..1 positions. Knobs have a centre detent at 64, which must land
// exactly on 0.5; faders are linear.
double knobFromMidi(int value);
double faderFromMidi(int value);

// The positions of a hardware mixer's controls, followed from its MIDI. Values are unknown
// (empty) until the control moves or the mixer sends a snapshot (DJM: hold START/STOP).
class HardwareMixer
{
public:
    struct ChannelState
    {
        std::optional<double> trim, high, mid, low, color, fader;
        std::optional<CrossfaderAssign> crossfaderAssign;
        MixerInput input = MixerInput::Unknown;
    };

    explicit HardwareMixer(const MixerProfile& profile);

    const MixerProfile& profile() const { return *profile_; }

    // Takes one raw MIDI message. Returns true if it changed a position.
    bool handle(const uint8_t* data, size_t size);

    int numChannels() const { return static_cast<int>(channels_.size()); }
    const ChannelState& channel(int index) const { return channels_[static_cast<size_t>(index)]; }
    std::optional<double> crossfader() const { return crossfader_; }

    // The mixer channel whose USB input plays output pair `pair`, or -1.
    int channelForUsbPair(int pair) const;

private:
    const MixerProfile* profile_;
    std::vector<ChannelState> channels_;
    std::optional<double> crossfader_;
};

const char* mixerInputName(MixerInput input);

} // namespace vmp

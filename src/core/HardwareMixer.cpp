#include "core/HardwareMixer.h"

#include <algorithm>

namespace vmp
{

namespace
{
// The DJM-900 family's MIDI map, from the DJM-900SRT operating instructions (MIDI assignment
// map), checked against the SRT's snapshot. Serato's and rekordbox's deck names don't matter
// here: USB pair n plays on channel n.
MixerProfile makeDjm900(std::string name)
{
    MixerProfile p;
    p.name = std::move(name);
    //            trim hi  mid low color fader assign pair
    p.channels = {{1, 2, 3, 4, 5, 17, 65, 0},
                  {6, 7, 8, 9, 10, 18, 66, 1},
                  {12, 14, 15, 21, 22, 19, 67, 2},
                  {80, 81, 92, 82, 83, 20, 68, 3}};
    for (int c = 0; c < 4; ++c)
    {
        const int first = 50 + 4 * c;
        p.inputs.push_back({first, c, MixerInput::Digital});
        p.inputs.push_back({first + 1, c, MixerInput::Line});
        p.inputs.push_back({first + 2, c, MixerInput::Phono});
        p.inputs.push_back({first + 3, c, MixerInput::Usb});
    }
    p.crossfader = 11;
    return p;
}

// The DJM-900NXS2's "List of MIDI messages" (version 1.00) gives the same controls and notes as
// the SRT, and two more selector positions per channel: USB B (its second USB port, each port
// its own 12-out sound card, pair n on channel n) and RETURN AUX. Tried on a real one (2026-11-05).
MixerProfile makeDjm900Nxs2()
{
    MixerProfile p = makeDjm900("DJM-900NXS2");
    p.twoUsbPorts = true;
    p.snapshotKnown = false; // the NXS2's manual doesn't say START/STOP sends one
    for (int c = 0; c < 4; ++c)
    {
        p.inputs.push_back({66 + 2 * c, c, MixerInput::UsbB});
        p.inputs.push_back({67 + 2 * c, c, MixerInput::ReturnAux});
    }
    return p;
}

const std::vector<MixerProfile>& profiles()
{
    static const std::vector<MixerProfile> all{makeDjm900("DJM-900SRT"), makeDjm900Nxs2()};
    return all;
}
} // namespace

const MixerProfile* findMixerProfile(std::string_view deviceName)
{
    for (const auto& p : profiles())
        if (deviceName.substr(0, p.name.size()) == p.name)
            return &p;
    return nullptr;
}

double knobFromMidi(int value)
{
    value = std::clamp(value, 0, 127);
    return value <= 64 ? value / 128.0 : 0.5 + (value - 64) / 126.0;
}

double faderFromMidi(int value)
{
    return std::clamp(value, 0, 127) / 127.0;
}

HardwareMixer::HardwareMixer(const MixerProfile& profile) : profile_(&profile), channels_(profile.channels.size())
{
}

bool HardwareMixer::handle(const uint8_t* data, size_t size)
{
    if (size < 3)
        return false;
    const int kind = data[0] & 0xf0;
    const int number = data[1] & 0x7f;
    const int value = data[2] & 0x7f;

    auto set = [](std::optional<double>& target, double v) {
        if (target == v)
            return false;
        target = v;
        return true;
    };

    if (kind == 0xb0)
    {
        if (number == profile_->crossfader)
            return set(crossfader_, faderFromMidi(value));
        for (size_t c = 0; c < channels_.size(); ++c)
        {
            const auto& map = profile_->channels[c];
            auto& state = channels_[c];
            if (number == map.trim)
                return set(state.trim, knobFromMidi(value));
            if (number == map.high)
                return set(state.high, knobFromMidi(value));
            if (number == map.mid)
                return set(state.mid, knobFromMidi(value));
            if (number == map.low)
                return set(state.low, knobFromMidi(value));
            if (number == map.color)
                return set(state.color, knobFromMidi(value));
            if (number == map.fader)
                return set(state.fader, faderFromMidi(value));
            if (number == map.crossfaderAssign)
            {
                const auto assign = value < 32 ? CrossfaderAssign::A : value > 96 ? CrossfaderAssign::B : CrossfaderAssign::Thru;
                const bool changed = state.crossfaderAssign != assign;
                state.crossfaderAssign = assign;
                return changed;
            }
        }
        return false;
    }

    // Selectors send note on (127) for the new position and off for the old one, but an off
    // can go missing when the switch is flicked quickly: only the last "on" counts.
    if (kind == 0x90 && value > 0)
        for (const auto& in : profile_->inputs)
            if (in.note == number)
            {
                auto& state = channels_[static_cast<size_t>(in.channel)];
                const bool changed = state.input != in.input;
                state.input = in.input;
                return changed;
            }
    return false;
}

bool isUsbInput(MixerInput input)
{
    return input == MixerInput::Usb || input == MixerInput::UsbB;
}

int HardwareMixer::channelForUsbPair(int pair) const
{
    for (size_t c = 0; c < profile_->channels.size(); ++c)
        if (profile_->channels[c].usbPair == pair)
            return static_cast<int>(c);
    return -1;
}

const char* mixerInputName(MixerInput input, bool twoUsbPorts)
{
    switch (input)
    {
    case MixerInput::Phono:
        return "PHONO";
    case MixerInput::Line:
        return "CD/LINE";
    case MixerInput::Digital:
        return "DIGITAL";
    case MixerInput::Usb:
        return twoUsbPorts ? "USB A" : "USB";
    case MixerInput::UsbB:
        return "USB B";
    case MixerInput::ReturnAux:
        return "RETURN AUX";
    case MixerInput::Unknown:
        break;
    }
    return "?";
}

} // namespace vmp

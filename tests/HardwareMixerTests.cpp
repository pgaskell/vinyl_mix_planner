#include "core/HardwareMixer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace vmp;
using Catch::Approx;

namespace
{
using Message = std::array<uint8_t, 3>;

void feed(HardwareMixer& mixer, std::initializer_list<Message> messages)
{
    for (const auto& m : messages)
        mixer.handle(m.data(), m.size());
}
} // namespace

TEST_CASE("Mixer profiles are found by device name")
{
    REQUIRE(findMixerProfile("DJM-900SRT") != nullptr);
    CHECK(findMixerProfile("DJM-900SRT")->channels.size() == 4);
    CHECK(findMixerProfile("DJM-900SRT MIDI") == findMixerProfile("DJM-900SRT"));
    CHECK(findMixerProfile("MacBook Pro Speakers") == nullptr);
    CHECK(findMixerProfile("DJM") == nullptr);
}

TEST_CASE("Knob centre detents land exactly on 0.5; ends reach 0 and 1")
{
    CHECK(knobFromMidi(64) == 0.5);
    CHECK(knobFromMidi(0) == 0.0);
    CHECK(knobFromMidi(127) == 1.0);
    CHECK(knobFromMidi(63) < 0.5);
    CHECK(knobFromMidi(65) > 0.5);
    CHECK(faderFromMidi(0) == 0.0);
    CHECK(faderFromMidi(127) == 1.0);
    CHECK(faderFromMidi(200) == 1.0);
}

TEST_CASE("A DJM-900SRT snapshot sets the positions and input selectors")
{
    HardwareMixer mixer(*findMixerProfile("DJM-900SRT"));
    CHECK_FALSE(mixer.channel(0).fader.has_value());
    CHECK(mixer.channel(0).input == MixerInput::Unknown);

    // Recorded from the real mixer (START/STOP held), minus the beat FX and cue buttons.
    feed(mixer, {{0xB0, 1, 55},  {0xB0, 2, 60},  {0xB0, 3, 64},  {0xB0, 4, 63},   {0xB0, 5, 62},  {0xB0, 6, 28},
                 {0xB0, 7, 65},  {0xB0, 8, 77},  {0xB0, 9, 0},   {0xB0, 10, 22},  {0xB0, 11, 0},  {0xB0, 12, 31},
                 {0xB0, 14, 89}, {0xB0, 15, 88}, {0xB0, 17, 77}, {0xB0, 19, 127}, {0xB0, 20, 0},  {0xB0, 21, 75},
                 {0xB0, 22, 88}, {0xB0, 65, 0},  {0xB0, 66, 64}, {0xB0, 67, 64},  {0xB0, 68, 127}, {0xB0, 81, 64},
                 {0xB0, 82, 64}, {0xB0, 83, 63}, {0xB0, 92, 65}, {0x90, 50, 0},   {0x90, 51, 0},  {0x90, 52, 0},
                 {0x90, 53, 127}, {0x90, 55, 0}, {0x90, 56, 0},  {0x90, 57, 127}, {0x90, 58, 0},  {0x90, 59, 0},
                 {0x90, 60, 127}, {0x90, 61, 0}, {0x90, 62, 0},  {0x90, 63, 0},   {0x90, 64, 0},  {0x90, 65, 127}});

    const auto& ch1 = mixer.channel(0);
    CHECK(ch1.trim == knobFromMidi(55));
    CHECK(ch1.high == knobFromMidi(60));
    CHECK(ch1.mid == 0.5);
    CHECK(ch1.fader == faderFromMidi(77));
    CHECK(ch1.crossfaderAssign == CrossfaderAssign::A);
    CHECK(ch1.input == MixerInput::Usb);

    CHECK(mixer.channel(1).low == 0.0); // CC 9
    CHECK(mixer.channel(1).crossfaderAssign == CrossfaderAssign::Thru);
    CHECK(mixer.channel(1).input == MixerInput::Usb);
    CHECK(mixer.channel(2).fader == 1.0); // CC 19
    CHECK(mixer.channel(2).input == MixerInput::Phono);
    CHECK(mixer.channel(3).high == 0.5); // CC 81
    CHECK(mixer.channel(3).mid == Approx(knobFromMidi(65)));
    CHECK(mixer.channel(3).crossfaderAssign == CrossfaderAssign::B);
    CHECK(mixer.channel(3).input == MixerInput::Usb);
    CHECK(mixer.crossfader() == 0.0);
}

TEST_CASE("Selectors follow the last position switched on, even if an off goes missing")
{
    HardwareMixer mixer(*findMixerProfile("DJM-900SRT"));
    // Flicked quickly: the "off" for CD/LINE never arrived.
    feed(mixer, {{0x90, 50, 127}, {0x90, 50, 0}, {0x90, 51, 127}, {0x90, 52, 127}, {0x90, 52, 0}});
    CHECK(mixer.channel(0).input == MixerInput::Phono);
    feed(mixer, {{0x80, 52, 0}, {0x90, 53, 127}});
    CHECK(mixer.channel(0).input == MixerInput::Usb);
}

TEST_CASE("Only real changes are reported; other messages are ignored")
{
    HardwareMixer mixer(*findMixerProfile("DJM-900SRT"));
    const Message fader{0xB0, 17, 100};
    CHECK(mixer.handle(fader.data(), 3));
    CHECK_FALSE(mixer.handle(fader.data(), 3));
    const Message onAnotherMidiChannel{0xB3, 17, 101};
    CHECK(mixer.handle(onAnotherMidiChannel.data(), 3));
    CHECK(mixer.channel(0).fader == faderFromMidi(101));

    const Message beatFx{0xB0, 91, 30}, clock{0xF8, 0, 0}, unknownNote{0x90, 118, 127};
    CHECK_FALSE(mixer.handle(beatFx.data(), 3));
    CHECK_FALSE(mixer.handle(clock.data(), 1));
    CHECK_FALSE(mixer.handle(unknownNote.data(), 3));
}

TEST_CASE("USB pair n plays on mixer channel n")
{
    HardwareMixer mixer(*findMixerProfile("DJM-900SRT"));
    CHECK(mixer.channelForUsbPair(0) == 0);
    CHECK(mixer.channelForUsbPair(3) == 3);
    CHECK(mixer.channelForUsbPair(4) == -1);
}

#include "engine/Deck.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vmp
{

namespace
{
constexpr double kMaxSrcRatio = 256.0; // libsamplerate's limit in both directions
// How much faster or slower than requested the deck may play while catching up. 10% closes a
// 10 ms gap in about 100 ms and sounds like a nudge on the platter.
constexpr double kMaxSlew = 0.1;
} // namespace

Deck::Deck()
{
    int error = 0;
    src_ = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &error);
    if (src_ == nullptr)
        throw std::runtime_error(src_strerror(error));
}

Deck::~Deck()
{
    src_delete(src_);
}

void Deck::prepare(double, int maxBlockFrames)
{
    scratch_.assign(static_cast<size_t>(std::max(maxBlockFrames, 1)) * 2, 0.0f);
    track_ = nullptr;
    position_ = -1.0;
}

void Deck::restart(const TrackAudio* track, double startFrame)
{
    src_reset(src_);
    track_ = track;
    readFrame_ = std::clamp<int64_t>(static_cast<int64_t>(std::floor(startFrame)), 0, track->frames);
    position_ = startFrame;
}

void Deck::renderSilence(float* left, float* right, int numFrames)
{
    std::fill_n(left, numFrames, 0.0f);
    std::fill_n(right, numFrames, 0.0f);
    track_ = nullptr;
    position_ = -1.0;
}

void Deck::render(const TrackAudio* track, double startFrame, double endFrame, float* left, float* right,
                  int numFrames)
{
    const int chunkMax = static_cast<int>(scratch_.size() / 2);
    if (track == nullptr || chunkMax == 0 || endFrame <= startFrame || startFrame >= track->frames)
    {
        renderSilence(left, right, numFrames);
        return;
    }

    if (track != track_ || std::abs(startFrame - position_) > kSlewWindowSeconds * track->sampleRate)
        restart(track, startFrame);

    // Head for endFrame from where the deck really is, within the slew limit.
    const double requested = endFrame - startFrame;
    const double consumed = std::clamp(endFrame - position_, requested * (1.0 - kMaxSlew), requested * (1.0 + kMaxSlew));
    position_ += consumed;

    // Output frames per input frame. The resampler ramps linearly from the previous block's
    // ratio, which follows pitch ramps smoothly.
    const double ratio = std::clamp(numFrames / consumed, 1.0 / kMaxSrcRatio, kMaxSrcRatio);

    int produced = 0;
    while (produced < numFrames && readFrame_ < track->frames)
    {
        const int want = std::min(chunkMax, numFrames - produced);
        // Feed only about what is needed. libsamplerate buffers whatever it is offered, so
        // offering the whole file would make readFrame_ run far ahead of what is heard.
        const auto needed = static_cast<int64_t>(std::ceil(want / ratio)) + 4;

        // Streamed audio is loaded in chunks, so feed at most to the end of the current one.
        int64_t available = 0;
        const float* input = track->framesAt(readFrame_, available);
        if (input == nullptr)
        {
            track_ = nullptr; // start afresh at the requested position next time
            break;
        }

        SRC_DATA data{};
        data.data_in = input;
        data.input_frames = static_cast<long>(std::min(needed, available));
        data.data_out = scratch_.data();
        data.output_frames = want;
        data.src_ratio = ratio;

        if (src_process(src_, &data) != 0)
            break;
        readFrame_ += data.input_frames_used;

        const auto got = static_cast<int>(data.output_frames_gen);
        for (int i = 0; i < got; ++i)
        {
            left[produced + i] = scratch_[static_cast<size_t>(i) * 2];
            right[produced + i] = scratch_[static_cast<size_t>(i) * 2 + 1];
        }
        produced += got;
        if (data.input_frames_used == 0 && got == 0)
            break;
    }

    std::fill(left + produced, left + numFrames, 0.0f);
    std::fill(right + produced, right + numFrames, 0.0f);
}

} // namespace vmp

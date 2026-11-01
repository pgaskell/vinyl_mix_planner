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
// A call starting further than this from where the last one ended is a jump, not a continuation.
constexpr double kJumpToleranceFrames = 2.0;
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
    expectedStart_ = -1.0;
}

void Deck::restart(const TrackAudio* track, double startFrame)
{
    src_reset(src_);
    track_ = track;
    readFrame_ = std::clamp<int64_t>(static_cast<int64_t>(std::floor(startFrame)), 0, track->frames);
}

void Deck::renderSilence(float* left, float* right, int numFrames)
{
    std::fill_n(left, numFrames, 0.0f);
    std::fill_n(right, numFrames, 0.0f);
    track_ = nullptr;
    expectedStart_ = -1.0;
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

    if (track != track_ || std::abs(startFrame - expectedStart_) > kJumpToleranceFrames)
        restart(track, startFrame);
    expectedStart_ = endFrame;

    // Output frames per input frame. The resampler ramps linearly from the previous block's
    // ratio, which follows pitch ramps smoothly.
    const double ratio = std::clamp(numFrames / (endFrame - startFrame), 1.0 / kMaxSrcRatio, kMaxSrcRatio);

    int produced = 0;
    while (produced < numFrames && readFrame_ < track->frames)
    {
        const int want = std::min(chunkMax, numFrames - produced);
        // Feed only about what is needed. libsamplerate buffers whatever it is offered, so
        // offering the whole file would make readFrame_ run far ahead of what is heard.
        const auto needed = static_cast<int64_t>(std::ceil(want / ratio)) + 4;

        SRC_DATA data{};
        data.data_in = track->interleaved.data() + readFrame_ * 2;
        data.input_frames = static_cast<long>(std::min(needed, track->frames - readFrame_));
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

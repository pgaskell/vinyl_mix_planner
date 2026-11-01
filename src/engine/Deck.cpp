#include "engine/Deck.h"

#include "core/Pitch.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vmp
{

namespace
{
// Below this the platter is effectively stopped; also keeps the resampler ratio in range.
constexpr double kMinSpeed = 1.0 / 200.0;
constexpr double kMaxSrcRatio = 256.0; // libsamplerate's limit in both directions
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
    delete current_;
    delete pending_.load();
    delete retired_.load();
}

void Deck::prepare(double deviceSampleRate, int maxBlockFrames)
{
    deviceRate_ = deviceSampleRate;
    scratch_.assign(static_cast<size_t>(std::max(maxBlockFrames, 1)) * 2, 0.0f);
    src_reset(src_);
    wasMoving_ = false;
}

void Deck::setTrack(TrackAudio* track)
{
    // A track that was set but never picked up by the audio thread is simply replaced.
    delete pending_.exchange(track);
}

void Deck::collectGarbage()
{
    delete retired_.exchange(nullptr);
}

void Deck::setMotorTimes(double startSeconds, double stopSeconds)
{
    startSeconds_.store(startSeconds);
    stopSeconds_.store(stopSeconds);
}

void Deck::adoptPendingTrack()
{
    // Only one retired slot: wait for the message thread to free the previous track first.
    if (retired_.load() != nullptr)
        return;

    TrackAudio* next = pending_.exchange(nullptr);
    if (next == nullptr)
        return;

    retired_.store(current_);
    current_ = next;
    readFrame_ = 0;
    src_reset(src_);
    wasMoving_ = false;
}

void Deck::render(float* left, float* right, int numFrames)
{
    adoptPendingTrack();

    const int64_t seekTo = seekRequest_.exchange(-1);
    if (seekTo >= 0 && current_ != nullptr)
    {
        readFrame_ = std::clamp<int64_t>(seekTo, 0, current_->frames);
        src_reset(src_);
    }

    motor_.setTimes(startSeconds_.load(), stopSeconds_.load());
    motor_.setRunning(running_.load());

    const int chunkMax = static_cast<int>(scratch_.size() / 2);
    if (chunkMax == 0)
    {
        std::fill_n(left, numFrames, 0.0f);
        std::fill_n(right, numFrames, 0.0f);
        return;
    }

    // Devices occasionally deliver blocks larger than they announced.
    for (int done = 0; done < numFrames;)
    {
        const int chunk = std::min(chunkMax, numFrames - done);
        renderChunk(left + done, right + done, chunk);
        done += chunk;
    }

    position_.store(readFrame_);
}

void Deck::renderChunk(float* left, float* right, int numFrames)
{
    const double fraction = motor_.advance(numFrames, deviceRate_);
    const double speed = fraction * speedFromPitchSteps(pitchSteps_.load());

    int produced = 0;
    if (current_ != nullptr && speed >= kMinSpeed && readFrame_ < current_->frames)
    {
        // Flush filter history from before a stop or seek so it doesn't smear into the restart.
        if (!wasMoving_)
        {
            src_reset(src_);
            wasMoving_ = true;
        }

        // The resampler ramps linearly from the previous ratio, which smooths pitch moves and
        // motor start/stop across the block.
        const double ratio =
            std::clamp(deviceRate_ / (current_->sampleRate * speed), 1.0 / kMaxSrcRatio, kMaxSrcRatio);

        while (produced < numFrames && readFrame_ < current_->frames)
        {
            // Feed only about what this block needs. libsamplerate buffers whatever it is offered,
            // so offering the whole file would make readFrame_ run far ahead of what is heard.
            const auto needed = static_cast<int64_t>(std::ceil((numFrames - produced) / ratio)) + 4;

            SRC_DATA data{};
            data.data_in = current_->interleaved.data() + readFrame_ * 2;
            data.input_frames = static_cast<long>(std::min(needed, current_->frames - readFrame_));
            data.data_out = scratch_.data() + produced * 2;
            data.output_frames = numFrames - produced;
            data.src_ratio = ratio;

            if (src_process(src_, &data) != 0)
                break;

            readFrame_ += data.input_frames_used;
            produced += static_cast<int>(data.output_frames_gen);
            if (data.input_frames_used == 0 && data.output_frames_gen == 0)
                break;
        }
    }
    else
    {
        wasMoving_ = false;
    }

    for (int i = 0; i < produced; ++i)
    {
        left[i] = scratch_[static_cast<size_t>(i) * 2];
        right[i] = scratch_[static_cast<size_t>(i) * 2 + 1];
    }
    std::fill(left + produced, left + numFrames, 0.0f);
    std::fill(right + produced, right + numFrames, 0.0f);
}

} // namespace vmp

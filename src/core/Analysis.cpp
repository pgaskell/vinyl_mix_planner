#include "core/Analysis.h"

#include "core/Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>
#include <tuple>

namespace vmp
{

namespace
{
constexpr double kAnalysisRate = 11025.0;

// Radix-2 FFT of real frames; returns magnitudes of bins 0..n/2.
class Fft
{
public:
    explicit Fft(size_t size) : size_(size), buffer_(size), window_(size), twiddles_(size / 2)
    {
        for (size_t i = 0; i < size; ++i)
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
        for (size_t k = 0; k < size / 2; ++k)
            twiddles_[k] = std::polar(1.0f, static_cast<float>(-2.0 * std::numbers::pi * k / size));
    }

    // Applies a Hann window to `frame` (size() samples) and writes size()/2 + 1 magnitudes.
    void magnitudes(const float* frame, std::vector<float>& out)
    {
        for (size_t i = 0; i < size_; ++i)
            buffer_[i] = {frame[i] * window_[i], 0.0f};

        for (size_t i = 1, j = 0; i < size_; ++i)
        {
            size_t bit = size_ >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(buffer_[i], buffer_[j]);
        }
        for (size_t len = 2; len <= size_; len <<= 1)
        {
            const size_t stride = size_ / len;
            for (size_t i = 0; i < size_; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = buffer_[i + k];
                    const auto v = buffer_[i + k + len / 2] * twiddles_[k * stride];
                    buffer_[i + k] = u + v;
                    buffer_[i + k + len / 2] = u - v;
                }
        }

        out.resize(size_ / 2 + 1);
        for (size_t k = 0; k <= size_ / 2; ++k)
            out[k] = std::abs(buffer_[k]);
    }

private:
    size_t size_;
    std::vector<std::complex<float>> buffer_;
    std::vector<float> window_;
    std::vector<std::complex<float>> twiddles_;
};

// Removes the local average and keeps the rises, so steady loudness doesn't count as onsets.
std::vector<float> peaksOverLocalMean(const std::vector<float>& flux)
{
    constexpr size_t kHalfWidth = 16;
    std::vector<float> envelope(flux.size());
    for (size_t i = 0; i < flux.size(); ++i)
    {
        const size_t from = i > kHalfWidth ? i - kHalfWidth : 0;
        const size_t to = std::min(flux.size(), i + kHalfWidth + 1);
        const float mean = std::accumulate(flux.begin() + static_cast<long>(from), flux.begin() + static_cast<long>(to),
                                           0.0f) /
                           static_cast<float>(to - from);
        envelope[i] = std::max(0.0f, flux[i] - mean);
    }
    return envelope;
}

struct OnsetEnvelopes
{
    std::vector<float> full; // whole spectrum: finds the tempo
    std::vector<float> low;  // kick range: places the beats
};

// Onset strength: spectral flux on a log-magnitude spectrum, over the whole spectrum and over
// the low end only. Hats and claps dominate full-band flux, but in DJ music the kick is the beat.
OnsetEnvelopes onsetEnvelopes(const std::vector<float>& mono, double sampleRate, size_t window, size_t hop)
{
    constexpr double kLowBandHz = 200.0;
    if (mono.size() < window)
        return {};
    const size_t frames = (mono.size() - window) / hop + 1;
    const auto lowBins = static_cast<size_t>(kLowBandHz * static_cast<double>(window) / sampleRate);
    Fft fft(window);
    std::vector<float> mags, previous(window / 2 + 1, 0.0f), full(frames, 0.0f), low(frames, 0.0f);
    for (size_t i = 0; i < frames; ++i)
    {
        fft.magnitudes(mono.data() + i * hop, mags);
        float sumAll = 0.0f, sumLow = 0.0f;
        for (size_t k = 1; k < mags.size(); ++k)
        {
            const float level = std::log1p(100.0f * mags[k]);
            const float rise = std::max(0.0f, level - previous[k]);
            sumAll += rise;
            if (k <= lowBins)
                sumLow += rise;
            previous[k] = level;
        }
        if (i > 0)
        {
            full[i] = sumAll;
            low[i] = sumLow;
        }
    }
    return {peaksOverLocalMean(full), peaksOverLocalMean(low)};
}

// Folds the envelope at a beat period and returns the histogram of onset strength by phase.
std::vector<float> foldAtPeriod(const std::vector<float>& envelope, double periodFrames, size_t bins)
{
    std::vector<float> histogram(bins, 0.0f);
    const double binsPerFrame = static_cast<double>(bins) / periodFrames;
    double phase = 0.0; // in bins; stepped rather than computed with fmod, which is slow here
    for (float value : envelope)
    {
        histogram[std::min(bins - 1, static_cast<size_t>(phase))] += value;
        phase += binsPerFrame;
        if (phase >= static_cast<double>(bins))
            phase -= static_cast<double>(bins);
    }
    // Smooth over three bins (circular) so the peak isn't decided by bin boundaries.
    std::vector<float> smoothed(bins);
    for (size_t b = 0; b < bins; ++b)
        smoothed[b] = histogram[(b + bins - 1) % bins] + histogram[b] + histogram[(b + 1) % bins];
    return smoothed;
}
} // namespace

std::vector<float> analysisSignal(const TrackAudio& audio, double& sampleRate)
{
    const auto factor = static_cast<int64_t>(std::max(1L, std::lround(audio.sampleRate / kAnalysisRate)));
    sampleRate = audio.sampleRate / static_cast<double>(factor);

    LR4 antiAlias;
    antiAlias.design(FilterType::LowPass, 0.45 * sampleRate, audio.sampleRate);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(audio.frames / factor + 1));
    const float* samples = audio.data();
    for (int64_t f = 0; f < audio.frames; ++f)
    {
        const float x = antiAlias.process(0.5f * (samples[f * 2] + samples[f * 2 + 1]));
        if (f % factor == 0)
            out.push_back(x);
    }
    return out;
}

std::optional<BeatGrid> detectBeatGrid(const std::vector<float>& mono, double sampleRate)
{
    constexpr size_t kWindow = 512, kHop = 64;
    constexpr double kMinBpm = 60.0, kMaxBpm = 190.0, kPreferredBpm = 120.0;
    constexpr size_t kPhaseBins = 128;

    const auto envelopes = onsetEnvelopes(mono, sampleRate, kWindow, kHop);
    const auto& envelope = envelopes.full;
    const double frameRate = sampleRate / kHop;
    const auto maxLag = static_cast<size_t>(std::ceil(frameRate * 60.0 / kMinBpm));
    const auto minLag = static_cast<size_t>(std::floor(frameRate * 60.0 / kMaxBpm));
    if (envelope.size() < maxLag * 8)
        return std::nullopt; // too short to find a tempo

    auto autocorrelation = [&](size_t lag) {
        if (lag >= envelope.size())
            return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i + lag < envelope.size(); ++i)
            sum += static_cast<double>(envelope[i]) * envelope[i + lag];
        return sum / static_cast<double>(envelope.size() - lag);
    };

    // Coarse tempo: autocorrelation, reinforced by the next metrical level and weighted towards
    // 120 BPM to settle octave ambiguity (85 vs 170).
    size_t bestLag = 0;
    double bestScore = 0.0;
    for (size_t lag = minLag; lag <= maxLag; ++lag)
    {
        const double bpm = 60.0 * frameRate / static_cast<double>(lag);
        const double octaves = std::log2(bpm / kPreferredBpm);
        const double prior = std::exp(-0.5 * (octaves / 0.5) * (octaves / 0.5));
        const double score = (autocorrelation(lag) + 0.5 * autocorrelation(lag * 2)) * prior;
        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }
    if (bestLag == 0)
        return std::nullopt;

    // Fine tempo: the period at which the whole track's onsets line up most sharply. Searched
    // coarse then fine, which is ~6x cheaper than one fine sweep.
    auto sharpestBpm = [&](double from, double to, double step) {
        double best = from, bestSharpness = 0.0;
        for (double bpm = from; bpm <= to; bpm += step)
        {
            const auto folded = foldAtPeriod(envelope, 60.0 * frameRate / bpm, kPhaseBins);
            const float sharpness = *std::max_element(folded.begin(), folded.end());
            if (sharpness > bestSharpness)
            {
                bestSharpness = sharpness;
                best = bpm;
            }
        }
        return best;
    };
    const double coarseBpm = 60.0 * frameRate / static_cast<double>(bestLag);
    const double nearBpm = sharpestBpm(coarseBpm * 0.98, coarseBpm * 1.02, 0.05);
    const double bestBpm = sharpestBpm(nearBpm - 0.05, nearBpm + 0.05, 0.002);

    // Phase: the peak of the folded histogram, interpolated between bins. From the kick range
    // when it has a clear pulse, otherwise from the whole spectrum.
    const double period = 60.0 * frameRate / bestBpm;
    auto pulse = [&](const std::vector<float>& env) {
        auto folded = foldAtPeriod(env, period, kPhaseBins);
        const double mean = std::accumulate(folded.begin(), folded.end(), 0.0) / kPhaseBins;
        const double peakValue = *std::max_element(folded.begin(), folded.end());
        return std::pair{std::move(folded), mean > 0.0 ? peakValue / mean : 0.0};
    };
    auto [folded, clarity] = pulse(envelopes.low);
    if (clarity < 1.5)
        std::tie(folded, clarity) = pulse(envelope);
    if (clarity < 1.5)
        return std::nullopt; // no steady beat
    const auto peak = static_cast<size_t>(std::max_element(folded.begin(), folded.end()) - folded.begin());

    const double before = folded[(peak + kPhaseBins - 1) % kPhaseBins], after = folded[(peak + 1) % kPhaseBins];
    const double curvature = before - 2.0 * folded[peak] + after;
    const double offset = curvature < 0.0 ? 0.5 * (before - after) / curvature : 0.0;
    const double phaseFrames = (static_cast<double>(peak) + 0.5 + offset) / kPhaseBins * period;

    // Flux peaks when an onset is about 5/6 of the way into the window (measured on synthetic
    // kicks; worth re-checking against real records).
    constexpr double kOnsetPositionInWindow = 0.83;
    const double beatSeconds = (phaseFrames * kHop + kOnsetPositionInWindow * kWindow) / sampleRate;
    const double periodSeconds = 60.0 / bestBpm;
    return BeatGrid{bestBpm, std::fmod(beatSeconds, periodSeconds)};
}

std::optional<MusicalKey> detectKey(const std::vector<float>& mono, double sampleRate)
{
    constexpr size_t kWindow = 4096, kHop = 2048;
    constexpr double kMinHz = 100.0, kMaxHz = 2000.0;
    // Krumhansl-Kessler probe-tone profiles, tonic first.
    constexpr std::array<double, 12> kMajor{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    constexpr std::array<double, 12> kMinor{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

    if (mono.size() < kWindow)
        return std::nullopt;

    std::vector<int> pitchClass(kWindow / 2 + 1, -1);
    for (size_t k = 1; k < pitchClass.size(); ++k)
    {
        const double hz = static_cast<double>(k) * sampleRate / kWindow;
        if (hz >= kMinHz && hz <= kMaxHz)
            pitchClass[k] = static_cast<int>(((std::lround(69.0 + 12.0 * std::log2(hz / 440.0)) % 12) + 12) % 12);
    }

    std::array<double, 12> chroma{};
    Fft fft(kWindow);
    std::vector<float> mags;
    for (size_t start = 0; start + kWindow <= mono.size(); start += kHop)
    {
        fft.magnitudes(mono.data() + start, mags);
        for (size_t k = 0; k < mags.size(); ++k)
            if (pitchClass[k] >= 0)
                chroma[static_cast<size_t>(pitchClass[k])] += mags[k];
    }
    if (std::accumulate(chroma.begin(), chroma.end(), 0.0) < 1e-3)
        return std::nullopt; // silence

    auto correlation = [&](const std::array<double, 12>& profile, int tonic) {
        const double meanC = std::accumulate(chroma.begin(), chroma.end(), 0.0) / 12.0;
        const double meanP = std::accumulate(profile.begin(), profile.end(), 0.0) / 12.0;
        double num = 0.0, denC = 0.0, denP = 0.0;
        for (int pc = 0; pc < 12; ++pc)
        {
            const double c = chroma[static_cast<size_t>(pc)] - meanC;
            const double p = profile[static_cast<size_t>(((pc - tonic) % 12 + 12) % 12)] - meanP;
            num += c * p;
            denC += c * c;
            denP += p * p;
        }
        return num / std::sqrt(denC * denP);
    };

    MusicalKey best;
    double bestScore = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (bool minor : {false, true})
        {
            const double score = correlation(minor ? kMinor : kMajor, tonic);
            if (score > bestScore)
            {
                bestScore = score;
                best = {tonic, minor};
            }
        }
    return best;
}

TrackAnalysis analyseTrack(const TrackAudio& audio)
{
    double rate = 0.0;
    const auto signal = analysisSignal(audio, rate);
    return {detectBeatGrid(signal, rate), detectKey(signal, rate)};
}

} // namespace vmp

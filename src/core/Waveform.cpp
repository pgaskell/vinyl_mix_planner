#include "core/Waveform.h"

#include "core/Filters.h"
#include "core/StreamedAudio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace vmp
{

namespace
{
constexpr int64_t kFinestBin = 16;
constexpr int64_t kLevelFactor = 8;
constexpr int kNumLevels = 5; // 16, 128, 1024, 8192, 65536 frames per bin

constexpr char kMagic[8] = {'V', 'M', 'P', 'P', 'E', 'A', 'K', 'S'};
constexpr uint32_t kFileVersion = 1;

template <typename T>
void write(std::ostream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof value);
}

template <typename T>
bool read(std::istream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof value));
}

uint8_t toByte(float level)
{
    return static_cast<uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 255.0f));
}
} // namespace

WaveformPeaks::WaveformPeaks(const TrackAudio& audio) : frames_(audio.frames)
{
    ThreeBandSplitter splitter;
    splitter.design(kLowMidHz, kMidHighHz, audio.sampleRate);

    Level finest{kFinestBin, std::vector<Bin>(static_cast<size_t>((frames_ + kFinestBin - 1) / kFinestBin))};
    const float* samples = audio.data();
    std::array<float, 3> peak{};
    for (int64_t f = 0; f < frames_; ++f)
    {
        float low, mid, high;
        splitter.process(0.5f * (samples[f * 2] + samples[f * 2 + 1]), low, mid, high);
        peak[0] = std::max(peak[0], std::abs(low));
        peak[1] = std::max(peak[1], std::abs(mid));
        peak[2] = std::max(peak[2], std::abs(high));
        if ((f + 1) % kFinestBin == 0 || f + 1 == frames_)
        {
            finest.bins[static_cast<size_t>(f / kFinestBin)] = {toByte(peak[0]), toByte(peak[1]), toByte(peak[2])};
            peak = {};
        }
    }
    levels_.push_back(std::move(finest));

    for (int level = 1; level < kNumLevels; ++level)
    {
        const auto& finer = levels_.back().bins;
        Level coarser{levels_.back().binFrames * kLevelFactor,
                      std::vector<Bin>((finer.size() + kLevelFactor - 1) / kLevelFactor)};
        for (size_t i = 0; i < finer.size(); ++i)
        {
            auto& bin = coarser.bins[i / kLevelFactor];
            for (size_t b = 0; b < 3; ++b)
                bin[b] = std::max(bin[b], finer[i][b]);
        }
        levels_.push_back(std::move(coarser));
    }
}

bool WaveformPeaks::save(const std::filesystem::path& file) const
{
    // Written beside the target and renamed into place, so a crash never leaves half a file.
    const auto temp = temporaryPathFor(file);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(kMagic, sizeof kMagic);
        write(out, kFileVersion);
        write(out, frames_);
        write(out, static_cast<uint32_t>(levels_.size()));
        for (const auto& level : levels_)
        {
            write(out, level.binFrames);
            write(out, static_cast<uint64_t>(level.bins.size()));
            out.write(reinterpret_cast<const char*>(level.bins.data()),
                      static_cast<std::streamsize>(level.bins.size() * sizeof(Bin)));
        }
        if (!out.flush())
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temp, file, error);
    if (error)
        std::filesystem::remove(temp, error);
    return !error;
}

std::optional<WaveformPeaks> WaveformPeaks::load(const std::filesystem::path& file, int64_t expectedFrames)
{
    std::ifstream in(file, std::ios::binary);
    char magic[sizeof kMagic];
    uint32_t version = 0, numLevels = 0;
    WaveformPeaks peaks;
    if (!in.read(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || !read(in, version) ||
        version != kFileVersion || !read(in, peaks.frames_) || peaks.frames_ != expectedFrames ||
        !read(in, numLevels) || numLevels != kNumLevels)
        return std::nullopt;

    int64_t binFrames = kFinestBin;
    for (uint32_t i = 0; i < numLevels; ++i, binFrames *= kLevelFactor)
    {
        Level level{0, {}};
        uint64_t count = 0;
        if (!read(in, level.binFrames) || level.binFrames != binFrames || !read(in, count) ||
            count != static_cast<uint64_t>((peaks.frames_ + binFrames - 1) / binFrames))
            return std::nullopt;
        level.bins.resize(static_cast<size_t>(count));
        if (!in.read(reinterpret_cast<char*>(level.bins.data()), static_cast<std::streamsize>(count * sizeof(Bin))))
            return std::nullopt;
        peaks.levels_.push_back(std::move(level));
    }
    return peaks;
}

WaveformPeaks::Bands WaveformPeaks::range(int64_t first, int64_t last) const
{
    first = std::clamp<int64_t>(first, 0, frames_);
    last = std::clamp<int64_t>(last, 0, frames_);
    if (last <= first)
        return {};

    // Coarsest level with at least four bins across the range. Edges round out to whole bins, so
    // coarser bins would smear peaks into neighbouring ranges (and make waveforms look blurred).
    const int64_t length = last - first;
    const Level* level = &levels_.front();
    for (const auto& l : levels_)
        if (l.binFrames * 4 <= length)
            level = &l;

    const auto firstBin = static_cast<size_t>(first / level->binFrames);
    const auto lastBin =
        std::min(static_cast<size_t>((last + level->binFrames - 1) / level->binFrames), level->bins.size());
    Bin peak{};
    for (size_t i = firstBin; i < lastBin; ++i)
        for (size_t b = 0; b < 3; ++b)
            peak[b] = std::max(peak[b], level->bins[i][b]);
    return {peak[0] / 255.0f, peak[1] / 255.0f, peak[2] / 255.0f};
}

} // namespace vmp

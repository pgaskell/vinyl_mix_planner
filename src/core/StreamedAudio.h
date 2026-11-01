#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace vmp
{

struct TrackAudio;

// A unique name beside `file` to write to before renaming into place, so two threads writing the
// same file never write into one temporary file. Ends in ".tmp".
std::filesystem::path temporaryPathFor(const std::filesystem::path& file);

// A track's decoded audio in the decode cache: a file with a small header followed by interleaved
// stereo float. It is read in chunks by the prefetcher (engine/Prefetcher) into memory it owns;
// the audio thread only ever reads chunks that have been loaded, through chunk(), and never the
// file itself. A file that is damaged, truncated or deleted therefore shows up as a read error on
// the prefetcher thread (and silence), never as a crash on the audio thread.
class StreamedAudio
{
public:
    static constexpr int64_t kChunkFrames = int64_t{1} << 15; // ~0.7 s at 44.1 kHz, 256 KiB
    static constexpr size_t kChunkFloats = static_cast<size_t>(kChunkFrames) * 2;

    // Writes a cache file. Data is flushed to the disk (F_FULLFSYNC) before the file is renamed
    // into place, so after a crash it is either complete or absent.
    static bool write(const std::filesystem::path& file, const TrackAudio& audio);

    // Opens a cache file; nothing if it's missing, damaged or the wrong size.
    static std::unique_ptr<StreamedAudio> open(const std::filesystem::path& file);

    ~StreamedAudio();
    StreamedAudio(const StreamedAudio&) = delete;
    StreamedAudio& operator=(const StreamedAudio&) = delete;

    int64_t frames() const { return frames_; }
    double sampleRate() const { return sampleRate_; }
    int64_t numChunks() const { return numChunks_; }

    // --- Any thread, lock-free -----------------------------------------------
    // The loaded samples of a chunk (kChunkFrames frames, fewer for the last one), or null.
    const float* chunk(int64_t index) const
    {
        return index >= 0 && index < numChunks_ ? chunks_[static_cast<size_t>(index)].load(std::memory_order_acquire)
                                                : nullptr;
    }
    int64_t chunkFrames(int64_t index) const;

    // --- Prefetcher thread ----------------------------------------------------
    // Reads a chunk from the file into `destination` (kChunkFloats floats). False on a read error.
    bool read(int64_t index, float* destination) const;
    // Makes a loaded chunk visible to readers, or hides it (null). The caller owns the memory and
    // must not reuse it until no reader can still be using it.
    void publish(int64_t index, const float* samples) { chunks_[static_cast<size_t>(index)].store(samples, std::memory_order_release); }

    // Reads the whole file into memory, for analysis. Background threads only.
    std::shared_ptr<TrackAudio> readAll() const;

private:
    StreamedAudio() = default;

    int fd_ = -1;
    int64_t frames_ = 0;
    double sampleRate_ = 44100.0;
    int64_t numChunks_ = 0;
    std::unique_ptr<std::atomic<const float*>[]> chunks_;
};

} // namespace vmp

#pragma once

#include "app/AnalysisCache.h"
#include "core/Analysis.h"
#include "core/Set.h"
#include "core/TrackAudio.h"
#include "core/Waveform.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace vmp
{

// Decoded audio, waveform peaks and analysis for the set's tracks, keyed by file path. Decoding
// and analysis run on background threads; listeners hear about it when a track finishes
// loading or fails.
//
// Each file is decoded once into the decode cache (~21 MB per minute) and played from there by
// streaming (engine/Prefetcher), so memory use doesn't grow with the length of the set. Waveform
// peaks are cached beside it, so reopening a set needs no decoding at all. If the cache can't be
// written (e.g. the disk is full), the track is kept in memory instead.
class AudioPool : public juce::ChangeBroadcaster
{
public:
    enum class Status
    {
        Loading,
        Ready,
        Failed
    };

    struct Entry
    {
        Status status = Status::Loading;
        std::shared_ptr<const TrackAudio> audio;
        std::shared_ptr<const WaveformPeaks> peaks;
        TrackAnalysis analysis;
        juce::String error;
        bool streamed = false; // from the decode cache; otherwise held in memory
    };

    static constexpr juce::int64 kDefaultCacheLimitBytes = juce::int64{10} << 30;

    AudioPool(AnalysisCache& cache, juce::File cacheFolder, juce::int64 cacheLimitBytes = kDefaultCacheLimitBytes);
    ~AudioPool() override;

    juce::AudioFormatManager& formats() { return formats_; }
    bool canLoad(const juce::File& file);

    // Reads length and sample rate from the file header, without decoding. Fast.
    std::optional<Track> probe(const juce::File& file);

    // Starts loading the file unless it is already loaded or loading.
    void request(const std::string& path);
    const Entry* find(const std::string& path) const;

    // Drops audio no longer used by the set.
    void retainOnly(const std::set<std::string>& paths);

    // The decode cache. Least recently used files beyond the limit are deleted. Clearing it is
    // safe while playing (open files stay readable); tracks are decoded again when next opened.
    void setCacheLimit(juce::int64 bytes);
    juce::int64 cacheLimit() const { return cacheLimit_.load(); }
    juce::int64 cacheBytes() const;
    void clearCache();

    // The DJ's correction if there is one, otherwise what analysis found (once loaded).
    std::optional<BeatGrid> gridFor(const Track& track) const;
    std::optional<MusicalKey> keyFor(const Track& track) const;

private:
    void finished(const std::string& path, Entry entry, bool analysed);

    AnalysisCache& cache_;
    juce::File cacheFolder_;
    std::atomic<juce::int64> cacheLimit_;
    juce::AudioFormatManager formats_;
    std::map<std::string, Entry> entries_;
    juce::ThreadPool pool_{2}; // declared after formats_ so jobs finish before it goes

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioPool)
};

} // namespace vmp

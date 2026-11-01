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
    };

    explicit AudioPool(AnalysisCache& cache);
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

    // The DJ's correction if there is one, otherwise what analysis found (once loaded).
    std::optional<BeatGrid> gridFor(const Track& track) const;
    std::optional<MusicalKey> keyFor(const Track& track) const;

private:
    void finished(const std::string& path, Entry entry, bool analysed);

    AnalysisCache& cache_;
    juce::AudioFormatManager formats_;
    std::map<std::string, Entry> entries_;
    juce::ThreadPool pool_{2}; // declared after formats_ so jobs finish before it goes

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioPool)
};

} // namespace vmp

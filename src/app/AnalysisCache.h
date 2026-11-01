#pragma once

#include "core/Analysis.h"

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <string>

namespace vmp
{

// Analysis results per audio file, kept between sessions so each file is analysed once.
// An entry is ignored if the file's size or modification time has changed, or if it was made
// by an older version of the analyser. Message thread only.
class AnalysisCache
{
public:
    explicit AnalysisCache(juce::File storage);

    std::optional<TrackAnalysis> find(const juce::File& audioFile) const;
    void store(const juce::File& audioFile, const TrackAnalysis& analysis);

private:
    struct Entry
    {
        juce::int64 size;
        juce::int64 modified;
        TrackAnalysis analysis;
    };

    void save() const;

    juce::File storage_;
    std::map<juce::String, Entry> entries_;
};

} // namespace vmp

#include "app/AnalysisCache.h"

namespace vmp
{

namespace
{
// Bump when the detectors change enough that old results should be redone.
constexpr int kAnalyserVersion = 1;
} // namespace

AnalysisCache::AnalysisCache(juce::File storage) : storage_(std::move(storage))
{
    const auto root = juce::JSON::parse(storage_);
    if (static_cast<int>(root.getProperty("analyser", 0)) != kAnalyserVersion)
        return;

    if (auto* files = root.getProperty("files", {}).getArray())
        for (const auto& v : *files)
        {
            Entry entry{static_cast<juce::int64>(v.getProperty("size", -1)),
                        static_cast<juce::int64>(v.getProperty("modified", -1)),
                        {}};
            if (v.hasProperty("bpm"))
                entry.analysis.grid = BeatGrid{v["bpm"], v["firstBeat"]};
            if (v.hasProperty("key"))
                entry.analysis.key = MusicalKey::parse(v["key"].toString().toStdString());
            entries_[v["path"].toString()] = entry;
        }
}

std::optional<TrackAnalysis> AnalysisCache::find(const juce::File& audioFile) const
{
    auto it = entries_.find(audioFile.getFullPathName());
    if (it == entries_.end() || it->second.size != audioFile.getSize() ||
        it->second.modified != audioFile.getLastModificationTime().toMilliseconds())
        return std::nullopt;
    return it->second.analysis;
}

void AnalysisCache::store(const juce::File& audioFile, const TrackAnalysis& analysis)
{
    entries_[audioFile.getFullPathName()] = {audioFile.getSize(),
                                             audioFile.getLastModificationTime().toMilliseconds(), analysis};
    save();
}

void AnalysisCache::save() const
{
    juce::Array<juce::var> files;
    for (const auto& [path, entry] : entries_)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty("path", path);
        e->setProperty("size", entry.size);
        e->setProperty("modified", entry.modified);
        if (entry.analysis.grid)
        {
            e->setProperty("bpm", entry.analysis.grid->bpm);
            e->setProperty("firstBeat", entry.analysis.grid->firstBeatSeconds);
        }
        if (entry.analysis.key)
            e->setProperty("key", juce::String(entry.analysis.key->name()));
        files.add(juce::var(e));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("analyser", kAnalyserVersion);
    root->setProperty("files", files);

    storage_.getParentDirectory().createDirectory();
    storage_.replaceWithText(juce::JSON::toString(juce::var(root)));
}

} // namespace vmp

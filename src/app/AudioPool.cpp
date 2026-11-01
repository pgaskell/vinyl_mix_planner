#include "app/AudioPool.h"

namespace vmp
{

namespace
{
std::shared_ptr<TrackAudio> decode(juce::AudioFormatManager& formats, const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = file.existsAsFile() ? "Unsupported or unreadable file" : "File not found";
        return nullptr;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "File contains no audio";
        return nullptr;
    }

    auto track = std::make_shared<TrackAudio>();
    track->sampleRate = reader->sampleRate;
    track->frames = reader->lengthInSamples;
    track->interleaved.resize(static_cast<size_t>(track->frames) * 2);

    // Read in chunks so we never hold planar and interleaved copies of the whole file at once.
    constexpr int kChunk = 1 << 16;
    juce::AudioBuffer<float> buffer(2, kChunk);
    const bool mono = reader->numChannels == 1;
    for (int64_t pos = 0; pos < track->frames; pos += kChunk)
    {
        const int n = static_cast<int>(std::min<int64_t>(kChunk, track->frames - pos));
        if (!reader->read(&buffer, 0, n, pos, true, !mono))
        {
            error = "Read error";
            return nullptr;
        }
        // Mono plays on both sides; anything beyond stereo is dropped.
        const float* left = buffer.getReadPointer(0);
        const float* right = buffer.getReadPointer(mono ? 0 : 1);
        float* dest = track->interleaved.data() + pos * 2;
        for (int i = 0; i < n; ++i)
        {
            dest[i * 2] = left[i];
            dest[i * 2 + 1] = right[i];
        }
    }
    return track;
}
} // namespace

AudioPool::AudioPool()
{
    formats_.registerBasicFormats();
}

AudioPool::~AudioPool()
{
    pool_.removeAllJobs(true, 10000);
}

bool AudioPool::canLoad(const juce::File& file)
{
    return file.existsAsFile() && formats_.findFormatForFileExtension(file.getFileExtension()) != nullptr;
}

std::optional<Track> AudioPool::probe(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return std::nullopt;

    Track track;
    track.path = file.getFullPathName().toStdString();
    track.title = file.getFileNameWithoutExtension().toStdString();
    track.frames = reader->lengthInSamples;
    track.sampleRate = reader->sampleRate;
    return track;
}

void AudioPool::request(const std::string& path)
{
    if (entries_.count(path) != 0)
        return;
    entries_[path] = Entry{};

    juce::WeakReference<AudioPool> self(this);
    pool_.addJob([this, self, path] {
        Entry entry;
        const auto audio = decode(formats_, juce::File(juce::String(path)), entry.error);
        if (audio != nullptr)
        {
            entry.status = Status::Ready;
            entry.audio = audio;
            entry.peaks = std::make_shared<WaveformPeaks>(audio);
        }
        else
        {
            entry.status = Status::Failed;
        }
        juce::MessageManager::callAsync([self, path, entry] {
            if (auto* pool = self.get())
                pool->finished(path, entry);
        });
    });
}

void AudioPool::finished(const std::string& path, Entry entry)
{
    // Ignore results for files dropped from the set while they were loading.
    auto it = entries_.find(path);
    if (it == entries_.end())
        return;
    it->second = std::move(entry);
    sendChangeMessage();
}

const AudioPool::Entry* AudioPool::find(const std::string& path) const
{
    auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

void AudioPool::retainOnly(const std::set<std::string>& paths)
{
    std::erase_if(entries_, [&](const auto& item) { return paths.count(item.first) == 0; });
}

} // namespace vmp

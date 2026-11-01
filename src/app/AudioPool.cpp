#include "app/AudioPool.h"

#include <sys/stat.h>

#include <algorithm>

namespace vmp
{

namespace
{
// Bump when decoding changes in a way that changes the decoded samples or their count.
constexpr int kDecodeCacheVersion = 1;

// Names a file's decoded audio in the cache. Anything that could change the decoded samples
// changes the name: the path, size, modification time (ns) and inode of the file, and the
// decoder. Replacing a file, even with one of the same size and time, gives it a new inode.
juce::String cacheKey(const juce::File& file)
{
    struct stat info{};
    if (::stat(file.getFullPathName().toRawUTF8(), &info) != 0)
        return {};
    const juce::String identity = file.getFullPathName() + "|" + juce::String(static_cast<juce::int64>(info.st_size)) +
                                  "|" + juce::String(static_cast<juce::int64>(info.st_mtimespec.tv_sec)) + "." +
                                  juce::String(static_cast<juce::int64>(info.st_mtimespec.tv_nsec)) + "|" +
                                  juce::String(static_cast<juce::int64>(info.st_ino)) + "|" +
                                  juce::String(kDecodeCacheVersion) + "|" + juce::SystemStats::getJUCEVersion();
    uint64_t hash = 14695981039346656037ull; // FNV-1a
    for (const char* p = identity.toRawUTF8(); *p != 0; ++p)
        hash = (hash ^ static_cast<unsigned char>(*p)) * 1099511628211ull;
    return juce::String::toHexString(static_cast<juce::int64>(hash)).paddedLeft('0', 16);
}

// Deletes the least recently used decoded files until the cache fits in `limit`, and leftovers
// from interrupted writes. Files in use can safely go: open files stay readable until closed.
void trimCache(const juce::File& folder, juce::int64 limit)
{
    const auto now = juce::Time::getCurrentTime();
    for (const auto& temp : folder.findChildFiles(juce::File::findFiles, false, "*.tmp"))
        if (now - temp.getLastModificationTime() > juce::RelativeTime::hours(1))
            temp.deleteFile();

    auto files = folder.findChildFiles(juce::File::findFiles, false, "*.f32");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() < b.getLastModificationTime();
    });
    juce::int64 total = 0;
    for (const auto& file : files)
        total += file.getSize();
    for (const auto& file : files)
    {
        if (total <= limit)
            break;
        total -= file.getSize();
        file.deleteFile();
        file.withFileExtension("peaks").deleteFile();
    }
}

std::shared_ptr<TrackAudio> streamedTrack(std::unique_ptr<StreamedAudio> stream)
{
    if (stream == nullptr)
        return nullptr;
    auto track = std::make_shared<TrackAudio>();
    track->frames = stream->frames();
    track->sampleRate = stream->sampleRate();
    track->stream = std::move(stream);
    return track;
}

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

AudioPool::AudioPool(AnalysisCache& cache, juce::File cacheFolder) : cache_(cache), cacheFolder_(std::move(cacheFolder))
{
    formats_.registerBasicFormats();
    cacheFolder_.createDirectory();
    pool_.addJob([folder = cacheFolder_] { trimCache(folder, kCacheLimitBytes); });
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
    const auto cached = cache_.find(juce::File(juce::String(path)));

    juce::WeakReference<AudioPool> self(this);
    pool_.addJob([this, self, path, cached] {
        Entry entry;
        const juce::File source{juce::String(path)};
        const juce::String key = cacheKey(source);
        const auto audioFile = cacheFolder_.getChildFile(key + ".f32");
        const auto peaksFile = audioFile.withFileExtension("peaks");
        const std::filesystem::path audioPath(audioFile.getFullPathName().toStdString());
        const std::filesystem::path peaksPath(peaksFile.getFullPathName().toStdString());

        // Already decoded: stream it as it is. Otherwise decode, keep a copy in the cache, and
        // stream that; if the cache can't be written, play from memory.
        std::shared_ptr<TrackAudio> decoded; // in memory, if we had to decode
        bool analysed = false;
        auto audio = key.isNotEmpty() ? streamedTrack(StreamedAudio::open(audioPath)) : nullptr;
        if (audio != nullptr)
        {
            audioFile.setLastModificationTime(juce::Time::getCurrentTime()); // recently used
        }
        else if ((decoded = decode(formats_, source, entry.error)) != nullptr)
        {
            if (key.isNotEmpty() && StreamedAudio::write(audioPath, *decoded))
                audio = streamedTrack(StreamedAudio::open(audioPath));
            if (audio == nullptr)
                audio = decoded;
            trimCache(cacheFolder_, kCacheLimitBytes);
        }

        if (audio != nullptr)
        {
            // Peaks and analysis need the whole track; read it back only if they aren't cached.
            auto whole = [&] {
                if (decoded == nullptr)
                    decoded = audio->stream != nullptr ? audio->stream->readAll() : audio;
                return decoded;
            };
            if (auto saved = WaveformPeaks::load(peaksPath, audio->frames))
                entry.peaks = std::make_shared<WaveformPeaks>(std::move(*saved));
            else if (whole() != nullptr)
            {
                entry.peaks = std::make_shared<WaveformPeaks>(*decoded);
                entry.peaks->save(peaksPath);
            }
            if (cached)
                entry.analysis = *cached;
            else if (whole() != nullptr)
            {
                entry.analysis = analyseTrack(*decoded);
                analysed = true;
            }

            if (entry.peaks != nullptr)
            {
                entry.status = Status::Ready;
                entry.audio = audio;
                entry.streamed = audio->stream != nullptr;
            }
            else
            {
                entry.error = "Couldn't read the decoded audio back";
            }
        }
        if (entry.status != Status::Ready)
            entry.status = Status::Failed;

        analysed = analysed && entry.status == Status::Ready;
        juce::MessageManager::callAsync([self, path, entry, analysed] {
            if (auto* pool = self.get())
                pool->finished(path, entry, analysed);
        });
    });
}

void AudioPool::finished(const std::string& path, Entry entry, bool analysed)
{
    if (analysed)
        cache_.store(juce::File(juce::String(path)), entry.analysis);

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

std::optional<BeatGrid> AudioPool::gridFor(const Track& track) const
{
    if (track.grid)
        return track.grid;
    const auto* entry = find(track.path);
    return entry != nullptr ? entry->analysis.grid : std::nullopt;
}

std::optional<MusicalKey> AudioPool::keyFor(const Track& track) const
{
    if (track.key)
        return track.key;
    const auto* entry = find(track.path);
    return entry != nullptr ? entry->analysis.key : std::nullopt;
}

void AudioPool::retainOnly(const std::set<std::string>& paths)
{
    std::erase_if(entries_, [&](const auto& item) { return paths.count(item.first) == 0; });
}

} // namespace vmp

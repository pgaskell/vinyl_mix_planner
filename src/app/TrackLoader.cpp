#include "app/TrackLoader.h"

namespace vmp
{

namespace
{
std::unique_ptr<TrackAudio> decode(juce::AudioFormatManager& formats, const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "Unsupported or unreadable file";
        return nullptr;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "File contains no audio";
        return nullptr;
    }

    auto track = std::make_unique<TrackAudio>();
    track->sampleRate = reader->sampleRate;
    track->frames = reader->lengthInSamples;
    track->interleaved.resize(static_cast<size_t>(track->frames) * 2);

    // Read in chunks so we never hold the planar and interleaved copies of the whole file at once.
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

TrackLoader::TrackLoader()
{
    formats_.registerBasicFormats();
}

bool TrackLoader::canLoad(const juce::File& file)
{
    return file.existsAsFile() && formats_.findFormatForFileExtension(file.getFileExtension()) != nullptr;
}

void TrackLoader::load(const juce::File& file, Callback onDone)
{
    pool_.addJob([this, file, callback = std::move(onDone)] {
        juce::String error;
        auto track = decode(formats_, file, error);
        // std::function must be copyable, so the unique_ptr travels as a shared_ptr.
        auto shared = std::make_shared<std::unique_ptr<TrackAudio>>(std::move(track));
        juce::MessageManager::callAsync(
            [callback, shared, error] { callback(std::move(*shared), error); });
    });
}

} // namespace vmp

#include "app/MixExporter.h"

#include "app/TimeFormat.h"
#include "engine/Bounce.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>

namespace vmp
{

namespace
{
enum class Codec
{
    Wav,
    Aiff,
    Flac,
    Ogg
};

struct FileFormat
{
    const char* name;
    const char* extension;
    Codec codec;
    int bits; // 16 is dithered
};

const std::array<FileFormat, 6> kFormats{{
    {"WAV, 24-bit", ".wav", Codec::Wav, 24},
    {"WAV, 16-bit (CD)", ".wav", Codec::Wav, 16},
    {"AIFF, 24-bit", ".aiff", Codec::Aiff, 24},
    {"FLAC, 24-bit (lossless, smaller)", ".flac", Codec::Flac, 24},
    {"FLAC, 16-bit", ".flac", Codec::Flac, 16},
    {"Ogg Vorbis, 320 kbps (lossy)", ".ogg", Codec::Ogg, 32},
}};
const std::array<double, 2> kSampleRates{44100.0, 48000.0};

std::unique_ptr<juce::AudioFormat> makeFormat(Codec codec)
{
    switch (codec)
    {
    case Codec::Wav:
        return std::make_unique<juce::WavAudioFormat>();
    case Codec::Aiff:
        return std::make_unique<juce::AiffAudioFormat>();
    case Codec::Flac:
        return std::make_unique<juce::FlacAudioFormat>();
    case Codec::Ogg:
        return std::make_unique<juce::OggVorbisAudioFormat>();
    }
    return nullptr;
}

int qualityIndex(juce::AudioFormat& format, Codec codec)
{
    const auto options = format.getQualityOptions();
    if (codec == Codec::Ogg)
        return std::max(0, options.indexOf("320 kbps"));
    if (codec == Codec::Flac)
        return std::max(0, options.indexOf("5 (Default)"));
    return 0;
}

juce::String dBText(float gain)
{
    return juce::String::formatted("%+.1f dBFS", 20.0 * std::log10(std::max(gain, 1e-9f)));
}
} // namespace

// Renders on its own thread; the window shows progress and has the Cancel button.
class MixExporter::Job : public juce::ThreadWithProgressWindow
{
public:
    Job(MixExporter& owner, const Schedule& schedule, BounceRange range, const FileFormat& format, double sampleRate,
        juce::File target)
        : juce::ThreadWithProgressWindow("Exporting the mix", true, true), owner_(owner), schedule_(schedule),
          range_(range), format_(format), sampleRate_(sampleRate), target_(std::move(target)), temp_(target_)
    {
        setStatusMessage("Writing " + target_.getFileName() + " (" + formatTime(range.end - range.start, false) + ")");
    }

    void run() override
    {
        auto format = makeFormat(format_.codec);
        std::unique_ptr<juce::OutputStream> stream = temp_.getFile().createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (stream != nullptr)
            writer = format->createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                         .withSampleRate(sampleRate_)
                                                         .withNumChannels(2)
                                                         .withBitsPerSample(format_.bits)
                                                         .withQualityOptionIndex(qualityIndex(*format, format_.codec)));
        if (writer == nullptr)
        {
            opened_ = false;
            return;
        }

        std::array<std::vector<float>, 2> dithered;
        juce::Random random;
        result_ = bounce(
            schedule_, sampleRate_, range_,
            [&](const float* const* channels, int n) {
                if (format_.bits != 16)
                    return writer->writeFromFloatArrays(channels, 2, n);
                // TPDF dither, +/-1 LSB, so quiet passages fade out smoothly instead of in steps.
                const float* out[2];
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    dithered[ch].resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i)
                        dithered[ch][static_cast<size_t>(i)] =
                            channels[ch][i] + (random.nextFloat() - random.nextFloat()) / 32768.0f;
                    out[ch] = dithered[ch].data();
                }
                return writer->writeFromFloatArrays(out, 2, n);
            },
            [this](double fraction) {
                setProgress(fraction);
                return !threadShouldExit();
            });
        writer.reset(); // finishes the file
    }

    void threadComplete(bool userPressedCancel) override
    {
        using Status = BounceResult::Status;
        const auto file = target_.getFileName();
        if (!opened_)
            return owner_.finished("Couldn't export", "Couldn't create " + file + ".", true);
        if (userPressedCancel || result_.status == Status::Cancelled)
            return owner_.finished({}, {}, false); // the temporary file goes with temp_
        if (result_.status == Status::MissingAudio)
            return owner_.finished("Couldn't export",
                                   "Some tracks aren't loaded yet (or failed to load). Wait for them to finish "
                                   "loading, then export again.",
                                   true);
        if (result_.status == Status::WriteFailed || !temp_.overwriteTargetFileWithTemporary())
            return owner_.finished("Couldn't export", "Writing " + file + " failed. Is the disk full?", true);

        juce::String message = "Exported " + formatTime(static_cast<double>(result_.frames) / sampleRate_, false) +
                               " to " + file + ".\n\nPeak level " + dBText(result_.peak) + ".";
        bool warning = false;
        if (result_.clippedSamples > 0)
        {
            warning = true;
            message << "\n\n" << juce::String(result_.clippedSamples)
                    << " samples went over full scale and are clipped in the file. Turn down the trims or faders "
                       "where the mix is loudest.";
        }
        if (result_.status == Status::Glitched)
        {
            warning = true;
            message << "\n\nAudio wasn't read from disk in time";
            if (!result_.glitches.empty())
                message << " at " << formatTime(result_.glitches.front().seconds, false);
            message << ", so the file has gaps. Export again.";
        }
        owner_.finished(warning ? "Exported, with problems" : "Exported", message, warning);
    }

private:
    MixExporter& owner_;
    const Schedule& schedule_;
    const BounceRange range_;
    const FileFormat format_;
    const double sampleRate_;
    const juce::File target_;
    juce::TemporaryFile temp_; // beside the target; replaces it only when complete
    bool opened_ = true;
    BounceResult result_;
};

MixExporter::MixExporter(juce::PropertiesFile& settings) : settings_(settings) {}

MixExporter::~MixExporter()
{
    if (job_ != nullptr)
        job_->stopThread(10000);
}

void MixExporter::start(std::unique_ptr<Schedule> schedule, const juce::String& name)
{
    if (isRunning())
        return;
    if (!bounceRange(*schedule))
    {
        finished("Nothing to export", "The set is empty.", false);
        return;
    }
    schedule_ = std::move(schedule);
    name_ = name;

    options_ = std::make_unique<juce::AlertWindow>("Export the Mix",
                                                   "Renders the internal mix, as you hear it on the laptop, from the "
                                                   "first record to the end of the last.",
                                                   juce::MessageBoxIconType::NoIcon);
    juce::StringArray formats, rates;
    for (const auto& f : kFormats)
        formats.add(f.name);
    for (double r : kSampleRates)
        rates.add(juce::String(r / 1000.0, 1) + " kHz");
    options_->addComboBox("format", formats, "Format");
    options_->addComboBox("rate", rates, "Sample rate");
    options_->getComboBoxComponent("format")->setSelectedItemIndex(
        std::clamp(settings_.getIntValue("exportFormat", 0), 0, static_cast<int>(kFormats.size()) - 1));
    options_->getComboBoxComponent("rate")->setSelectedItemIndex(
        std::clamp(settings_.getIntValue("exportRate", 0), 0, static_cast<int>(kSampleRates.size()) - 1));
    options_->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    options_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    options_->enterModalState(true, juce::ModalCallbackFunction::create([this](int button) {
        const int format = options_->getComboBoxComponent("format")->getSelectedItemIndex();
        const int rate = options_->getComboBoxComponent("rate")->getSelectedItemIndex();
        juce::MessageManager::callAsync([this] { options_.reset(); }); // not from inside its own callback
        if (button != 1 || format < 0 || rate < 0)
        {
            schedule_.reset();
            return;
        }
        settings_.setValue("exportFormat", format);
        settings_.setValue("exportRate", rate);
        chooseFile(format, kSampleRates[static_cast<size_t>(rate)]);
    }));
}

void MixExporter::chooseFile(int formatIndex, double sampleRate)
{
    const auto& format = kFormats[static_cast<size_t>(formatIndex)];
    const auto folder = juce::File(settings_.getValue(
        "exportFolder", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName()));
    chooser_ = std::make_unique<juce::FileChooser>("Export the mix", folder.getChildFile(name_ + format.extension),
                                                   juce::String("*") + format.extension);
    chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this, format, sampleRate](const juce::FileChooser& fc) {
            const auto chosen = fc.getResult();
            juce::MessageManager::callAsync([this] { chooser_.reset(); });
            if (chosen == juce::File())
            {
                schedule_.reset();
                return;
            }
            const auto file = chosen.withFileExtension(format.extension);
            settings_.setValue("exportFolder", file.getParentDirectory().getFullPathName());
            job_ = std::make_unique<Job>(*this, *schedule_, *bounceRange(*schedule_), format, sampleRate, file);
            job_->launchThread();
        });
}

void MixExporter::finished(const juce::String& title, const juce::String& message, bool warning)
{
    // Called from the job's threadComplete: let it return before the job goes.
    juce::MessageManager::callAsync([this] {
        job_.reset();
        schedule_.reset();
    });
    if (title.isEmpty())
        return;
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(warning ? juce::MessageBoxIconType::WarningIcon
                                                           : juce::MessageBoxIconType::InfoIcon)
                                     .withTitle(title)
                                     .withMessage(message)
                                     .withButton("OK"),
                                 nullptr);
}

} // namespace vmp

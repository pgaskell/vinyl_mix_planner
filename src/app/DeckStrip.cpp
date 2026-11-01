#include "app/DeckStrip.h"

#include "core/Pitch.h"

namespace vmp
{

void fillOutputBox(juce::ComboBox& box, const std::vector<OutputChoice>& choices, int selectedFirstChannel)
{
    box.clear(juce::dontSendNotification);
    box.addItem("None", kNoOutputId);
    for (const auto& choice : choices)
    {
        box.addItem(choice.name, choice.firstChannel + 1);
        box.setItemEnabled(choice.firstChannel + 1, choice.enabled);
    }
    box.setSelectedId(selectedFirstChannel == Engine::kNoOutput ? kNoOutputId : selectedFirstChannel + 1,
                      juce::dontSendNotification);
}

// --- WaveformView -----------------------------------------------------------

WaveformView::WaveformView(juce::AudioFormatManager& formats, juce::AudioThumbnailCache& cache)
    : thumbnail_(512, formats, cache)
{
    thumbnail_.addChangeListener(this);
}

WaveformView::~WaveformView()
{
    thumbnail_.removeChangeListener(this);
}

void WaveformView::setFile(const juce::File& file)
{
    thumbnail_.setSource(new juce::FileInputSource(file));
    playheadSeconds_ = 0.0;
}

void WaveformView::setPlayhead(double seconds)
{
    if (std::abs(seconds - playheadSeconds_) > 1.0e-3)
    {
        playheadSeconds_ = seconds;
        repaint();
    }
}

void WaveformView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff15171a));
    const auto bounds = getLocalBounds();
    const double length = thumbnail_.getTotalLength();
    if (length <= 0.0)
    {
        g.setColour(juce::Colours::grey);
        g.drawText("Drop an audio file here, or click Load", bounds, juce::Justification::centred);
        return;
    }

    g.setColour(juce::Colour(0xff4fa3d9));
    thumbnail_.drawChannels(g, bounds, 0.0, length, 1.0f);

    const auto x = static_cast<float>(playheadSeconds_ / length * bounds.getWidth());
    g.setColour(juce::Colours::orange);
    g.drawLine(x, 0.0f, x, static_cast<float>(bounds.getHeight()), 2.0f);
}

void WaveformView::mouseDown(const juce::MouseEvent& e)
{
    seekTo(e.position.x);
}

void WaveformView::mouseDrag(const juce::MouseEvent& e)
{
    seekTo(e.position.x);
}

void WaveformView::seekTo(float x)
{
    const double length = thumbnail_.getTotalLength();
    if (length > 0.0 && onSeek)
        onSeek(juce::jlimit(0.0, length, static_cast<double>(x) / getWidth() * length));
}

// --- DeckStrip --------------------------------------------------------------

DeckStrip::DeckStrip(int index, Engine& engine, TrackLoader& loader, juce::AudioThumbnailCache& cache)
    : index_(index), engine_(engine), loader_(loader), waveform_(loader.formats(), cache)
{
    title_.setText("Deck " + juce::String(index + 1) + ": empty", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(title_);

    addAndMakeVisible(loadButton_);
    loadButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load a track", juce::File(),
                                                       loader_.formats().getWildcardForAllFormats());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      loadFile(fc.getResult());
                              });
    };

    addAndMakeVisible(startStopButton_);
    startStopButton_.setClickingTogglesState(true);
    startStopButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2e7d32));
    startStopButton_.onClick = [this] { deck().setRunning(startStopButton_.getToggleState()); };

    // Real fader: + is toward the DJ. Horizontal here for now; orientation is a UI decision for later.
    addAndMakeVisible(pitchLabel_);
    addAndMakeVisible(pitchSlider_);
    pitchSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchSlider_.setRange(-kMaxPitchSteps * kPitchStepPercent, kMaxPitchSteps * kPitchStepPercent,
                          kPitchStepPercent);
    pitchSlider_.setValue(0.0, juce::dontSendNotification);
    pitchSlider_.setDoubleClickReturnValue(true, 0.0);
    pitchSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    // "+ 0.0" turns -0.0 into 0.0 so the centre reads +0.00%.
    pitchSlider_.textFromValueFunction = [](double v) { return juce::String::formatted("%+.2f%%", v + 0.0); };
    pitchSlider_.valueFromTextFunction = [](const juce::String& s) { return s.retainCharacters("+-.0123456789").getDoubleValue(); };
    pitchSlider_.setTooltip("Pitch, 0.05% steps. Double-click for 0 (quartz lock).");
    pitchSlider_.onValueChange = [this] {
        deck().setPitchSteps(pitchStepsFromPercent(pitchSlider_.getValue()));
        updatePitchLabel();
    };

    addAndMakeVisible(keyShiftLabel_);
    keyShiftLabel_.setJustificationType(juce::Justification::centredLeft);
    updatePitchLabel();

    addAndMakeVisible(volumeLabel_);
    addAndMakeVisible(volumeSlider_);
    volumeSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider_.setRange(0.0, 1.0);
    volumeSlider_.setValue(1.0, juce::dontSendNotification);
    volumeSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider_.onValueChange = [this] {
        engine_.setChannelGain(index_, static_cast<float>(volumeSlider_.getValue()));
    };

    addAndMakeVisible(outputBox_);
    outputBox_.setTooltip("Output pair in direct-out mode");
    outputBox_.onChange = [this] {
        const int id = outputBox_.getSelectedId();
        if (id != 0)
            engine_.setDeckOutput(index_, id == kNoOutputId ? Engine::kNoOutput : id - 1);
    };

    addAndMakeVisible(waveform_);
    waveform_.onSeek = [this](double seconds) {
        if (trackSampleRate_ > 0.0)
            deck().seek(static_cast<int64_t>(seconds * trackSampleRate_));
    };
}

void DeckStrip::setOutputChoices(const std::vector<OutputChoice>& choices)
{
    fillOutputBox(outputBox_, choices, engine_.deckOutput(index_));
}

void DeckStrip::setDirectOut(bool directOut)
{
    outputBox_.setEnabled(directOut);
    volumeSlider_.setEnabled(!directOut);
}

void DeckStrip::loadFile(const juce::File& file)
{
    if (!loader_.canLoad(file))
    {
        title_.setText("Deck " + juce::String(index_ + 1) + ": can't load " + file.getFileName(),
                       juce::dontSendNotification);
        return;
    }

    title_.setText("Deck " + juce::String(index_ + 1) + ": loading " + file.getFileName() + "...",
                   juce::dontSendNotification);
    juce::Component::SafePointer<DeckStrip> self(this);
    loader_.load(file, [self, file](std::unique_ptr<TrackAudio> track, juce::String error) {
        if (self != nullptr)
            self->trackLoaded(file, std::move(track), error);
    });
}

void DeckStrip::trackLoaded(const juce::File& file, std::unique_ptr<TrackAudio> track, const juce::String& error)
{
    const juce::String prefix = "Deck " + juce::String(index_ + 1) + ": ";
    if (track == nullptr)
    {
        title_.setText(prefix + file.getFileName() + " - " + error, juce::dontSendNotification);
        return;
    }

    // Putting a new record on stops the platter.
    deck().setRunning(false);
    startStopButton_.setToggleState(false, juce::dontSendNotification);

    trackSampleRate_ = track->sampleRate;
    const auto seconds = static_cast<int>(static_cast<double>(track->frames) / track->sampleRate);
    title_.setText(prefix + file.getFileNameWithoutExtension() + "  (" + juce::String(seconds / 60) + ":" +
                       juce::String(seconds % 60).paddedLeft('0', 2) + ", " +
                       juce::String(track->sampleRate / 1000.0, 1) + " kHz)",
                   juce::dontSendNotification);

    deck().setTrack(track.release());
    waveform_.setFile(file);
}

void DeckStrip::updatePitchLabel()
{
    const double semitones = semitoneShiftForSpeed(speedFromPitchSteps(deck().pitchSteps()));
    keyShiftLabel_.setText(juce::String(semitones, 2) + " st", juce::dontSendNotification);
}

void DeckStrip::update()
{
    if (trackSampleRate_ > 0.0)
        waveform_.setPlayhead(static_cast<double>(deck().positionFrames()) / trackSampleRate_);
}

bool DeckStrip::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
        if (loader_.canLoad(juce::File(path)))
            return true;
    return false;
}

void DeckStrip::filesDropped(const juce::StringArray& files, int, int)
{
    for (const auto& path : files)
    {
        const juce::File file(path);
        if (loader_.canLoad(file))
        {
            loadFile(file);
            return;
        }
    }
}

void DeckStrip::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
    g.setColour(juce::Colour(0xff3a3e45));
    g.drawRect(getLocalBounds());
}

void DeckStrip::resized()
{
    auto area = getLocalBounds().reduced(6);

    auto top = area.removeFromTop(24);
    outputBox_.setBounds(top.removeFromRight(110));
    top.removeFromRight(6);
    startStopButton_.setBounds(top.removeFromRight(90));
    top.removeFromRight(6);
    loadButton_.setBounds(top.removeFromRight(70));
    title_.setBounds(top);

    area.removeFromTop(4);
    auto controls = area.removeFromTop(24);
    pitchLabel_.setBounds(controls.removeFromLeft(40));
    volumeSlider_.setBounds(controls.removeFromRight(160));
    volumeLabel_.setBounds(controls.removeFromRight(34));
    keyShiftLabel_.setBounds(controls.removeFromRight(70));
    pitchSlider_.setBounds(controls);

    area.removeFromTop(4);
    waveform_.setBounds(area);
}

} // namespace vmp

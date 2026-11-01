#include "app/MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace vmp
{

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(const juce::String& name, juce::PropertiesFile& settings)
        : DocumentWindow(name, juce::Colour(0xff1b1d21), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content_ = new MainComponent(settings);
        setContentOwned(content_, true);
        setResizable(true, true);
        setResizeLimits(1000, 760, 6000, 4000); // room for the inspector and the mixer strips
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    MainComponent& content() { return *content_; }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    MainComponent* content_; // owned by the window
};

class Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "VinylMixPlanner";
        options.folderName = "VinylMixPlanner";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties_.setStorageParameters(options);

        window_ = std::make_unique<MainWindow>(getApplicationName(), *properties_.getUserSettings());
    }

    void shutdown() override
    {
        window_.reset(); // saves routing settings
        properties_.saveIfNeeded();
    }

    // Window close and Cmd+Q both come here: offer to save first.
    void systemRequestedQuit() override
    {
        if (window_ == nullptr)
            quit();
        else
            window_->content().confirmDiscard([] { juce::JUCEApplication::quit(); });
    }

private:
    juce::ApplicationProperties properties_;
    std::unique_ptr<MainWindow> window_;
};

} // namespace vmp

START_JUCE_APPLICATION(vmp::Application)

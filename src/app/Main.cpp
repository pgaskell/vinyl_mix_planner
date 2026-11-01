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
        setContentOwned(new MainComponent(settings), true);
        setResizable(true, true);
        setResizeLimits(800, 500, 4000, 3000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
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

    void systemRequestedQuit() override { quit(); }

private:
    juce::ApplicationProperties properties_;
    std::unique_ptr<MainWindow> window_;
};

} // namespace vmp

START_JUCE_APPLICATION(vmp::Application)

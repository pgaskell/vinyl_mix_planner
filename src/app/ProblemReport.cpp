#include "app/ProblemReport.h"

#include <algorithm>

namespace vmp
{

namespace
{
constexpr int kMaxCrashReports = 5;
constexpr int kCrashReportDays = 30;

// macOS's crash and hang reports for this app, newest first.
juce::Array<juce::File> crashReports()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                            .getChildFile("Library/Logs/DiagnosticReports");
    const auto app = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFileNameWithoutExtension();
    const auto since = juce::Time::getCurrentTime() - juce::RelativeTime::days(kCrashReportDays);
    juce::Array<juce::File> found;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.ips;*.crash;*.hang"))
        if (entry.getFile().getFileName().startsWith(app) && entry.getModificationTime() > since)
            found.add(entry.getFile());
    std::sort(found.begin(), found.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });
    found.removeRange(kMaxCrashReports, found.size());
    return found;
}
} // namespace

juce::File logFile()
{
    return juce::FileLogger::getSystemLogFileFolder().getChildFile("VinylMixPlanner/VinylMixPlanner.log");
}

std::unique_ptr<juce::FileLogger> openLog(const juce::String& welcome)
{
    return std::make_unique<juce::FileLogger>(logFile(), welcome, 512 * 1024);
}

bool writeProblemReport(const juce::File& zip, const ProblemReport& report)
{
    juce::ZipFile::Builder builder;
    const auto now = juce::Time::getCurrentTime();
    auto addText = [&](const juce::String& name, const juce::String& text) {
        juce::MemoryBlock data(text.toRawUTF8(), text.getNumBytesAsUTF8());
        builder.addEntry(std::make_unique<juce::MemoryInputStream>(std::move(data)), 9, name, now);
    };
    addText("description.txt", report.description.isNotEmpty() ? report.description : "(none given)");
    addText("info.txt", report.info);
    addText("glitches.txt", report.glitches);
    addText("set.vmpset", report.setJson);
    if (logFile().existsAsFile())
        builder.addFile(logFile(), 9, "log.txt");
    for (const auto& crash : crashReports())
        builder.addFile(crash, 9, "crashes/" + crash.getFileName());

    zip.deleteFile();
    juce::FileOutputStream out(zip);
    if (!out.openedOk() || !builder.writeToStream(out, nullptr))
        return false;
    out.flush();
    return out.getStatus().wasOk();
}

} // namespace vmp

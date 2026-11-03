#pragma once

#include "core/Set.h"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace vmp
{

// 2: per-control automation keypoints (1 had unused "pitchMoves"). Older versions still load.
// 3: pitch in 0.02% steps, with the step size written in the file ("pitchStepPercent").
inline constexpr int kSetFormatVersion = 3;
inline constexpr const char* kSetFileExtension = ".vmpset";

class SetFileError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// How the set was being looked at, saved with it but not part of the mix: changing it isn't an
// edit (no undo, the set isn't marked unsaved). Files without it, and new sets, get the defaults.
struct SetView
{
    // Which controls' automation each deck's lane shows; by default the volume.
    std::array<std::array<bool, kNumParams>, kMaxDecks> automation = defaultAutomation();

    static std::array<std::array<bool, kNumParams>, kMaxDecks> defaultAutomation()
    {
        std::array<std::array<bool, kNumParams>, kMaxDecks> shown{};
        for (auto& deck : shown)
            deck[static_cast<size_t>(Param::Volume)] = true;
        return shown;
    }
    bool operator==(const SetView&) const = default;
};

// JSON. Loading checks structure only; a set that loads may still have validate() issues, so
// the user can open it and fix them. All functions throw SetFileError. The view is optional both
// ways.
std::string setToJson(const Set& set, const SetView* view = nullptr);
Set setFromJson(const std::string& json, SetView* view = nullptr);

// Writes to a temporary file and renames it over the target, so a failed save never leaves a
// half-written set behind.
void saveSet(const Set& set, const std::filesystem::path& path, const SetView* view = nullptr);
Set loadSet(const std::filesystem::path& path, SetView* view = nullptr);

} // namespace vmp

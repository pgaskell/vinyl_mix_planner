#pragma once

#include "core/Set.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace vmp
{

inline constexpr int kSetFormatVersion = 1;
inline constexpr const char* kSetFileExtension = ".vmpset";

class SetFileError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// JSON. Loading checks structure only; a set that loads may still have validate() issues, so
// the user can open it and fix them. All functions throw SetFileError.
std::string setToJson(const Set& set);
Set setFromJson(const std::string& json);

// Writes to a temporary file and renames it over the target, so a failed save never leaves a
// half-written set behind.
void saveSet(const Set& set, const std::filesystem::path& path);
Set loadSet(const std::filesystem::path& path);

} // namespace vmp

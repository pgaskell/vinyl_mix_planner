#include "core/SetFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace vmp
{

using nlohmann::json;

namespace
{
constexpr const char* kFormatName = "vinyl-mix-planner-set";

const char* pressedToString(PressedSpeed speed)
{
    switch (speed)
    {
    case PressedSpeed::Rpm33:
        return "33";
    case PressedSpeed::Rpm45:
        return "45";
    case PressedSpeed::Unknown:
        break;
    }
    return "unknown";
}

PressedSpeed pressedFromString(const std::string& s)
{
    if (s == "33")
        return PressedSpeed::Rpm33;
    if (s == "45")
        return PressedSpeed::Rpm45;
    if (s == "unknown")
        return PressedSpeed::Unknown;
    throw SetFileError("unknown pressed speed '" + s + "'");
}
} // namespace

std::string setToJson(const Set& set)
{
    json tracks = json::array();
    for (const auto& t : set.tracks)
        tracks.push_back({{"id", t.id},
                          {"path", t.path},
                          {"title", t.title},
                          {"frames", t.frames},
                          {"sampleRate", t.sampleRate},
                          {"pressed", pressedToString(t.pressed)}});

    json clips = json::array();
    for (const auto& c : set.clips)
    {
        json moves = json::array();
        for (const auto& m : c.pitchMoves)
            moves.push_back({{"frame", m.sourceFrame}, {"duration", m.durationSeconds}, {"pitchSteps", m.targetSteps}});

        clips.push_back({{"id", c.id},
                         {"track", c.trackId},
                         {"deck", c.deck},
                         {"start", c.startSeconds},
                         {"in", c.inFrame},
                         {"out", c.outFrame},
                         {"pitchSteps", c.pitchSteps},
                         {"pitchMoves", moves}});
    }

    const json doc = {{"format", kFormatName},
                      {"version", kSetFormatVersion},
                      {"name", set.name},
                      {"decks", set.numDecks},
                      {"nextId", set.nextId},
                      {"tracks", tracks},
                      {"clips", clips}};
    return doc.dump(2);
}

Set setFromJson(const std::string& text)
{
    json doc;
    try
    {
        doc = json::parse(text);
    }
    catch (const json::parse_error& e)
    {
        throw SetFileError(std::string("not valid JSON: ") + e.what());
    }

    try
    {
        if (!doc.is_object() || doc.value("format", "") != kFormatName)
            throw SetFileError("not a Vinyl Mix Planner set");
        const int version = doc.at("version").get<int>();
        if (version > kSetFormatVersion)
            throw SetFileError("made by a newer version of Vinyl Mix Planner (format " + std::to_string(version) +
                               ")");

        Set set;
        set.name = doc.at("name").get<std::string>();
        set.numDecks = doc.at("decks").get<int>();
        set.nextId = doc.at("nextId").get<Id>();

        for (const auto& t : doc.at("tracks"))
        {
            Track track;
            track.id = t.at("id").get<Id>();
            track.path = t.at("path").get<std::string>();
            track.title = t.at("title").get<std::string>();
            track.frames = t.at("frames").get<int64_t>();
            track.sampleRate = t.at("sampleRate").get<double>();
            track.pressed = pressedFromString(t.at("pressed").get<std::string>());
            if (track.sampleRate <= 0.0)
                throw SetFileError("track " + std::to_string(track.id) + " has an invalid sample rate");
            set.tracks.push_back(std::move(track));
        }

        for (const auto& c : doc.at("clips"))
        {
            Clip clip;
            clip.id = c.at("id").get<Id>();
            clip.trackId = c.at("track").get<Id>();
            clip.deck = c.at("deck").get<int>();
            clip.startSeconds = c.at("start").get<double>();
            clip.inFrame = c.at("in").get<int64_t>();
            clip.outFrame = c.at("out").get<int64_t>();
            clip.pitchSteps = c.at("pitchSteps").get<int>();
            for (const auto& m : c.at("pitchMoves"))
                clip.pitchMoves.push_back({m.at("frame").get<int64_t>(), m.at("duration").get<double>(),
                                           m.at("pitchSteps").get<int>()});
            set.clips.push_back(std::move(clip));
        }

        // Ids must stay unique when new items are added.
        for (const auto& t : set.tracks)
            set.nextId = std::max(set.nextId, t.id + 1);
        for (const auto& c : set.clips)
            set.nextId = std::max(set.nextId, c.id + 1);

        return set;
    }
    catch (const json::exception& e)
    {
        throw SetFileError(std::string("malformed set: ") + e.what());
    }
}

void saveSet(const Set& set, const std::filesystem::path& path)
{
    const std::string text = setToJson(set);
    auto temp = path;
    temp += ".saving";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out)
        {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            throw SetFileError("could not write " + temp.string());
        }
    }

    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error)
    {
        std::filesystem::remove(temp, error);
        throw SetFileError("could not replace " + path.string());
    }
}

Set loadSet(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw SetFileError("could not open " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return setFromJson(text.str());
}

} // namespace vmp

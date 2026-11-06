#include "core/SetFile.h"

#include "core/Pitch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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

const char* paramKey(Param param)
{
    switch (param)
    {
    case Param::Pitch:
        return "pitch";
    case Param::Volume:
        return "volume";
    case Param::High:
        return "high";
    case Param::Mid:
        return "mid";
    case Param::Low:
        return "low";
    case Param::Color:
        return "color";
    }
    return "?";
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

std::string setToJson(const Set& set, const SetView* view)
{
    json tracks = json::array();
    for (const auto& t : set.tracks)
    {
        json track = {{"id", t.id},
                      {"path", t.path},
                      {"title", t.title},
                      {"frames", t.frames},
                      {"sampleRate", t.sampleRate},
                      {"pressed", pressedToString(t.pressed)}};
        if (t.grid)
        {
            track["grid"] = {{"bpm", t.grid->bpm}, {"firstBeat", t.grid->firstBeatSeconds}};
            if (!t.grid->pins.empty())
            {
                json pins = json::array();
                for (const auto& pin : t.grid->pins)
                    pins.push_back({pin.beat, pin.seconds});
                track["grid"]["pins"] = pins;
            }
        }
        if (t.key)
            track["key"] = t.key->name();
        tracks.push_back(track);
    }

    json clips = json::array();
    for (const auto& c : set.clips)
    {
        // Only automated controls are written: {"pitch": [[frame, value], ...], ...}
        json automation = json::object();
        for (int p = 0; p < kNumParams; ++p)
        {
            const auto param = static_cast<Param>(p);
            if (c.points(param).empty())
                continue;
            json points = json::array();
            for (const auto& point : c.points(param))
                points.push_back({point.sourceFrame, point.value});
            automation[paramKey(param)] = points;
        }

        clips.push_back({{"id", c.id},
                         {"track", c.trackId},
                         {"deck", c.deck},
                         {"start", c.startSeconds},
                         {"in", c.inFrame},
                         {"out", c.outFrame},
                         {"pitchSteps", c.pitchSteps},
                         {"automation", automation},
                         {"mixer",
                          {{"fader", c.mixer.fader},
                           {"high", c.mixer.high},
                           {"mid", c.mixer.mid},
                           {"low", c.mixer.low},
                           {"trim", c.mixer.trim},
                           {"color", c.mixer.color}}}});
        // Only clips with deck actions have them.
        if (!c.actions.empty())
        {
            json actions = json::array();
            for (const auto& action : c.actions)
            {
                json a = {{"frame", action.sourceFrame}};
                if (action.kind == DeckAction::Kind::Nudge)
                {
                    a["kind"] = "nudge";
                    a["seconds"] = action.seconds;
                    a["speed"] = action.speed;
                }
                else
                {
                    a["kind"] = "touch";
                    json speeds = json::array();
                    for (const auto& point : action.touch)
                        speeds.push_back({point.seconds, point.factor});
                    a["speeds"] = speeds;
                }
                if (action.linked)
                    a["linked"] = true;
                actions.push_back(a);
            }
            clips.back()["actions"] = actions;
        }
    }

    json doc = {{"format", kFormatName},
                {"version", kSetFormatVersion},
                {"pitchStepPercent", kPitchStepPercent},
                {"name", set.name},
                {"decks", set.numDecks},
                {"nextId", set.nextId},
                {"tracks", tracks},
                {"clips", clips}};
    if (!set.links.empty())
    {
        json links = json::array();
        for (const auto& link : set.links)
            links.push_back({{"id", link.id},
                             {"leader", link.leader},
                             {"leaderFrame", link.leaderFrame},
                             {"follower", link.follower},
                             {"followerFrame", link.followerFrame}});
        doc["links"] = links;
    }
    if (view != nullptr)
    {
        // Per deck, the names of the controls whose automation is shown.
        json decks = json::array();
        for (const auto& deck : view->automation)
        {
            json shown = json::array();
            for (int p = 0; p < kNumParams; ++p)
                if (deck[static_cast<size_t>(p)])
                    shown.push_back(paramKey(static_cast<Param>(p)));
            decks.push_back(shown);
        }
        doc["view"] = {{"automation", decks}};
    }
    return doc.dump(2);
}

Set setFromJson(const std::string& text, SetView* view)
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
            if (t.contains("grid"))
            {
                track.grid = BeatGrid{t["grid"].at("bpm").get<double>(), t["grid"].at("firstBeat").get<double>()};
                for (const auto& pin : t["grid"].value("pins", json::array()))
                    track.grid->pins.push_back({pin.at(0).get<int64_t>(), pin.at(1).get<double>()});
            }
            if (t.contains("key"))
            {
                track.key = MusicalKey::parse(t["key"].get<std::string>());
                if (!track.key)
                    throw SetFileError("unknown key '" + t["key"].get<std::string>() + "'");
            }
            if (track.grid && !(track.grid->bpm > 0.0))
                throw SetFileError("track " + std::to_string(track.id) + " has an invalid BPM");
            if (track.grid && !track.grid->valid())
                throw SetFileError("track " + std::to_string(track.id) + " has beat pins out of order");
            if (track.sampleRate <= 0.0)
                throw SetFileError("track " + std::to_string(track.id) + " has an invalid sample rate");
            set.tracks.push_back(std::move(track));
        }

        // Pitch is stored in steps; sets from before version 3 used 0.05% steps. Converted
        // exactly (an old step is 2.5 new ones), so old sets sound the same.
        const double pitchScale = doc.value("pitchStepPercent", 0.05) / kPitchStepPercent;
        if (!(pitchScale > 0.0) || !std::isfinite(pitchScale))
            throw SetFileError("invalid pitch step size");

        for (const auto& c : doc.at("clips"))
        {
            Clip clip;
            clip.id = c.at("id").get<Id>();
            clip.trackId = c.at("track").get<Id>();
            clip.deck = c.at("deck").get<int>();
            clip.startSeconds = c.at("start").get<double>();
            clip.inFrame = c.at("in").get<int64_t>();
            clip.outFrame = c.at("out").get<int64_t>();
            clip.pitchSteps = c.at("pitchSteps").get<double>() * pitchScale;
            // Version 1 had "pitchMoves" instead, which the app never let you create: ignored.
            if (c.contains("automation"))
                for (int p = 0; p < kNumParams; ++p)
                {
                    const auto param = static_cast<Param>(p);
                    if (!c["automation"].contains(paramKey(param)))
                        continue;
                    const double scale = param == Param::Pitch ? pitchScale : 1.0;
                    for (const auto& point : c["automation"][paramKey(param)])
                        clip.points(param).push_back({point.at(0).get<int64_t>(), point.at(1).get<double>() * scale});
                    std::stable_sort(clip.points(param).begin(), clip.points(param).end(),
                                     [](const auto& a, const auto& b) { return a.sourceFrame < b.sourceFrame; });
                }
            const auto& mixer = c.at("mixer");
            clip.mixer = {mixer.at("fader").get<double>(), mixer.at("high").get<double>(),
                          mixer.at("mid").get<double>(), mixer.at("low").get<double>(),
                          mixer.value("trim", 0.5), mixer.value("color", 0.5)}; // saved before trim/color: centred
            if (c.contains("actions"))
            {
                for (const auto& action : c["actions"])
                {
                    const auto kind = action.at("kind").get<std::string>();
                    DeckAction loaded;
                    loaded.sourceFrame = action.at("frame").get<int64_t>();
                    if (kind == "nudge")
                    {
                        loaded.seconds = action.at("seconds").get<double>();
                        loaded.speed = action.at("speed").get<double>();
                    }
                    else if (kind == "touch")
                    {
                        loaded.kind = DeckAction::Kind::Touch;
                        for (const auto& point : action.at("speeds"))
                            loaded.touch.push_back({point.at(0).get<double>(), point.at(1).get<double>()});
                    }
                    else
                        throw SetFileError("unknown deck action '" + kind + "'");
                    loaded.linked = action.value("linked", false);
                    clip.actions.push_back(std::move(loaded));
                }
                std::stable_sort(clip.actions.begin(), clip.actions.end(),
                                 [](const auto& a, const auto& b) { return a.sourceFrame < b.sourceFrame; });
            }
            set.clips.push_back(std::move(clip));
        }
        for (const auto& l : doc.value("links", json::array()))
            set.links.push_back({l.at("id").get<Id>(), l.at("leader").get<Id>(), l.at("leaderFrame").get<int64_t>(),
                                 l.at("follower").get<Id>(), l.at("followerFrame").get<int64_t>()});

        if (view != nullptr)
        {
            *view = SetView{};
            if (doc.contains("view") && doc["view"].contains("automation"))
            {
                // Unknown names (from a newer version) are skipped rather than refused: it's only the view.
                const auto& decks = doc["view"]["automation"];
                for (size_t d = 0; d < std::min(decks.size(), static_cast<size_t>(kMaxDecks)); ++d)
                {
                    view->automation[d] = {};
                    for (const auto& name : decks[d])
                        for (int p = 0; p < kNumParams; ++p)
                            if (name.is_string() && name.get<std::string>() == paramKey(static_cast<Param>(p)))
                                view->automation[d][static_cast<size_t>(p)] = true;
                }
            }
        }

        // Ids must stay unique when new items are added.
        for (const auto& t : set.tracks)
            set.nextId = std::max(set.nextId, t.id + 1);
        for (const auto& c : set.clips)
            set.nextId = std::max(set.nextId, c.id + 1);
        for (const auto& l : set.links)
            set.nextId = std::max(set.nextId, l.id + 1);

        return set;
    }
    catch (const json::exception& e)
    {
        throw SetFileError(std::string("malformed set: ") + e.what());
    }
}

void saveSet(const Set& set, const std::filesystem::path& path, const SetView* view)
{
    const std::string text = setToJson(set, view);
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

Set loadSet(const std::filesystem::path& path, SetView* view)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw SetFileError("could not open " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return setFromJson(text.str(), view);
}

} // namespace vmp

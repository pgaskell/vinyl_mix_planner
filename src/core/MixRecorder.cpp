#include "core/MixRecorder.h"

#include <algorithm>
#include <utility>

namespace vmp
{

void MixRecorder::move(int deck, Id clip, double clipEnd, Param param, double value, double seconds,
                       AutomationMode mode)
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return;
    auto& pass = passes_[static_cast<size_t>(deck)][index(param)];
    if (pass && (pass->clip != clip || pass->mode != mode))
        finish(pass, seconds);
    if (!pass)
        pass = Pass{.clip = clip, .param = param, .mode = mode, .clipEnd = clipEnd, .end = clipEnd};
    // The playhead can step back a little (latency changes); keep the moves in time order.
    if (!pass->moves.empty())
        seconds = std::max(seconds, pass->moves.back().seconds);
    pass->moves.push_back({seconds, value});
}

std::optional<double> MixRecorder::latched(int deck, Param param) const
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return std::nullopt;
    const auto& pass = passes_[static_cast<size_t>(deck)][index(param)];
    return pass && !isTrim(pass->mode) ? std::optional(pass->moves.back().value) : std::nullopt;
}

std::optional<double> MixRecorder::latchedTrim(int deck, Param param) const
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return std::nullopt;
    const auto& pass = passes_[static_cast<size_t>(deck)][index(param)];
    return pass && isTrim(pass->mode) ? std::optional(pass->moves.back().value) : std::nullopt;
}

const MixRecorder::Pass* MixRecorder::pass(int deck, Param param) const
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return nullptr;
    const auto& pass = passes_[static_cast<size_t>(deck)][index(param)];
    return pass ? &*pass : nullptr;
}

void MixRecorder::settle(double seconds)
{
    for (auto& deck : passes_)
        for (auto& pass : deck)
        {
            if (!pass)
                continue;
            auto& moves = pass->moves;
            const auto end = static_cast<size_t>(
                std::lower_bound(moves.begin(), moves.end(), seconds,
                                 [](const edit::RecordedMove& m, double s) { return m.seconds < s; }) -
                moves.begin());
            // Thinned from the last settled move, which stays as the join.
            const size_t from = pass->settled > 0 ? pass->settled - 1 : 0;
            if (end < from + 3)
                continue;
            const auto range = paramRange(pass->param);
            const double scale = isTrim(pass->mode) ? 1.0 : range.max - range.min; // trim amounts are in turns
            const std::vector<edit::RecordedMove> span(moves.begin() + static_cast<long>(from),
                                                       moves.begin() + static_cast<long>(end));
            const auto thinned = edit::thinMoves(span, edit::kRecordingTolerance * scale);
            moves.erase(moves.begin() + static_cast<long>(from), moves.begin() + static_cast<long>(end));
            moves.insert(moves.begin() + static_cast<long>(from), thinned.begin(), thinned.end());
            pass->settled = from + thinned.size();
        }
}

bool MixRecorder::recording() const
{
    for (const auto& deck : passes_)
        for (const auto& pass : deck)
            if (pass)
                return true;
    return false;
}

void MixRecorder::finishEnded(double seconds)
{
    for (auto& deck : passes_)
        for (auto& pass : deck)
            if (pass && pass->clipEnd <= seconds)
                finish(pass, pass->clipEnd);
}

void MixRecorder::finishAll(double seconds)
{
    for (auto& deck : passes_)
        for (auto& pass : deck)
            if (pass)
                finish(pass, seconds);
}

std::vector<MixRecorder::Pass> MixRecorder::takeFinished()
{
    return std::exchange(finished_, {});
}

void MixRecorder::finish(std::optional<Pass>& pass, double seconds)
{
    pass->end = std::min(std::max(seconds, pass->moves.back().seconds), pass->clipEnd);
    finished_.push_back(std::move(*pass));
    pass.reset();
}

} // namespace vmp

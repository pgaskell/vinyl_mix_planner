#include "core/Document.h"

#include "core/Edits.h"

namespace vmp
{

void SetDocument::reset(Set set)
{
    set_ = std::move(set);
    undo_.clear();
    redo_.clear();
    stateId_ = savedStateId_ = nextStateId_++;
    inGesture_ = gestureRecorded_ = false;
    ++revision_;
}

bool SetDocument::edit(const std::function<bool(Set&)>& change)
{
    Set candidate = set_;
    if (!change(candidate))
        return false;
    edit::solveLinks(candidate); // records follow their beat links whatever changed
    if (candidate == set_)
        return false;

    // Within a gesture only the state before the first change is kept for undo.
    if (!inGesture_ || !gestureRecorded_)
    {
        undo_.push_back({std::move(set_), stateId_});
        if (undo_.size() > kMaxUndo)
            undo_.erase(undo_.begin());
        gestureRecorded_ = inGesture_;
    }
    redo_.clear();
    set_ = std::move(candidate);
    stateId_ = nextStateId_++;
    ++revision_;
    return true;
}

void SetDocument::beginGesture()
{
    inGesture_ = true;
    gestureRecorded_ = false;
}

void SetDocument::endGesture()
{
    inGesture_ = gestureRecorded_ = false;
}

void SetDocument::undo()
{
    if (undo_.empty())
        return;
    redo_.push_back({std::move(set_), stateId_});
    set_ = std::move(undo_.back().set);
    stateId_ = undo_.back().stateId;
    undo_.pop_back();
    ++revision_;
}

void SetDocument::redo()
{
    if (redo_.empty())
        return;
    undo_.push_back({std::move(set_), stateId_});
    set_ = std::move(redo_.back().set);
    stateId_ = redo_.back().stateId;
    redo_.pop_back();
    ++revision_;
}

const ClipTiming* SetDocument::timing(Id clipId) const
{
    if (timingsRevision_ != revision_)
    {
        timings_.clear();
        timingsRevision_ = revision_;
    }
    if (auto it = timings_.find(clipId); it != timings_.end())
        return &it->second;
    const Clip* clip = set_.findClip(clipId);
    const Track* track = clip != nullptr ? set_.findTrack(clip->trackId) : nullptr;
    if (track == nullptr)
        return nullptr;
    return &timings_.emplace(clipId, ClipTiming(*clip, *track)).first->second;
}

} // namespace vmp

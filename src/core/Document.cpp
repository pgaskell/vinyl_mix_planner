#include "core/Document.h"

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
    if (!change(candidate) || candidate == set_)
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

} // namespace vmp

#pragma once

#include "core/Set.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vmp
{

// The set being edited, with undo/redo and a saved/unsaved flag. Undo stores whole-set
// snapshots: a set is a few KB, so this is simpler and safer than inverse operations.
class SetDocument
{
public:
    static constexpr size_t kMaxUndo = 500;

    const Set& set() const { return set_; }

    // Replaces the set and clears history (new / open).
    void reset(Set set);

    // Runs the edit on a copy. If it succeeds and changes something, that becomes the current
    // set. All edits between beginGesture() and endGesture() (one drag) undo as one step.
    bool edit(const std::function<bool(Set&)>& change);
    void beginGesture();
    void endGesture();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    bool isDirty() const { return stateId_ != savedStateId_; }
    void markSaved() { savedStateId_ = stateId_; }

    // Changes on every edit, undo, redo or reset; cheap for views to poll.
    uint64_t revision() const { return revision_; }

private:
    struct Snapshot
    {
        Set set;
        uint64_t stateId;
    };

    Set set_;
    std::vector<Snapshot> undo_, redo_;
    uint64_t stateId_ = 0, savedStateId_ = 0, nextStateId_ = 1, revision_ = 0;
    bool inGesture_ = false, gestureRecorded_ = false;
};

} // namespace vmp

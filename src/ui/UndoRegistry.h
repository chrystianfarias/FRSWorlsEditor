#pragma once

#include "core/Types.h"

#include <algorithm>
#include <vector>

// One Ctrl+Z for the whole editor.
//
// Every part of the editor that changes something -- roads, terrain, the
// game's ground, collision, placed objects, meshes -- keeps its own history,
// because only it knows how to take its own change back. What they share is
// the order: each step is stamped from one clock, and Ctrl+Z takes back the
// most recent step of whichever part made it.
class UndoParticipant {
public:
    virtual ~UndoParticipant() = default;
    // The stamp of this part's most recent step that can still be undone; 0
    // when it has none.
    virtual u64 LatestUndoStamp() const = 0;
    // Takes that step back. False when it could not.
    virtual bool UndoLatest() = 0;
};

namespace undo {

inline u64& Clock() {
    static u64 clock = 0;
    return clock;
}

// A stamp for a step being recorded now: later steps get larger ones.
inline u64 NextStamp() { return ++Clock(); }

inline std::vector<UndoParticipant*>& Participants() {
    static std::vector<UndoParticipant*> all;
    return all;
}

inline void Register(UndoParticipant* participant) {
    auto& all = Participants();
    if (std::find(all.begin(), all.end(), participant) == all.end()) all.push_back(participant);
}

inline void Unregister(UndoParticipant* participant) {
    auto& all = Participants();
    all.erase(std::remove(all.begin(), all.end(), participant), all.end());
}

// The part whose step is the most recent, or null when nothing can be undone.
inline UndoParticipant* Latest() {
    UndoParticipant* best = nullptr;
    u64 stamp = 0;
    for (UndoParticipant* p : Participants()) {
        const u64 s = p->LatestUndoStamp();
        if (s > stamp) { stamp = s; best = p; }
    }
    return best;
}

// Ctrl+Z: the most recent step, of whichever part made it.
inline bool UndoLatest() {
    UndoParticipant* p = Latest();
    return p && p->UndoLatest();
}

} // namespace undo

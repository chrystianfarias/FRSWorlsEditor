#pragma once

#include "io/SectionSplice.h"

#include <string>
#include <vector>

namespace nfs { class World; }

// Turns the editor's scenery placements into span overlays.
//
// Every section whose placements changed is rebuilt from the bytes the editor
// shows, with each of its scenery chunks replaced by the *shipped* chunk plus
// the placements that belong to it (nfs::AppendScenery). Starting from the
// shipped chunk rather than the current one is what makes saving repeatable:
// the instances an earlier save appended are not in it, so they are written
// again from the editor's list instead of being stacked a second time -- and a
// placement deleted in the editor really goes. Every chunk is validated
// before it goes out.
struct PlacementSavePlan {
    std::vector<SectionSpan>  spans;
    std::vector<std::wstring> superseded;
    std::vector<size_t>       sections;   // loaded indices that were rebuilt
    std::wstring report;
    bool ok = true;
};

bool PlanPlacementSave(const nfs::World& world, const std::wstring& modelsFolder,
                       PlacementSavePlan& plan);

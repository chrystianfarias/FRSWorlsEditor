#pragma once

#include "nfs/Routes.h"
#include "roads/RoadNetwork.h"

#include <string>

// The procedural roads as routes of the game's road graph, so the game's
// traffic drives them and the GPS routes over them.
//
// Roads joined end to end through a node become one route; where three or
// more meet, the two that run on most nearly straight carry one route through
// the junction and the others join it in T junctions, each at a node of its
// own (a route node holds two links, and a T takes both). The lanes and their
// directions come from each road's profile. Ends that meet the game's own
// roads -- the mouths of junctions made into them -- are joined to the game's
// route there, in both directions.
namespace roads {

struct RouteReport {
    size_t routes = 0, nodes = 0, links = 0, gameLinks = 0;
    std::wstring problems;
};

// Adds the network's routes and links to `graph`, which holds the game's own
// graph as shipped. False when the result breaks one of the game's limits.
bool AddRoutes(const Network& network, nfs::routes::Graph& graph, RouteReport& report);

} // namespace roads

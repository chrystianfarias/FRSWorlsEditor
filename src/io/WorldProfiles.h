#pragma once

#include <string>
#include <vector>

// A world is one whole set of edits: overlays, placements and replaced
// files. <game>\models always holds the open one -- it is all the game loader
// reads -- and the others wait in <game>\models\worlds\<name>. Switching moves
// files between the two, which stays inside the one folder the setup made
// writable, so it needs no administrator rights even under Program Files.
namespace io {

// The name of the world in <game>\models; "Default" until one is chosen.
std::wstring ActiveWorldName(const std::wstring& modelsFolder);

// A file in <game>\models that belongs to every world, so switching worlds
// leaves it where it is: for what extensions keep there.
void AddSharedWorldFile(const std::wstring& name);

// The worlds put away, by name, sorted.
std::vector<std::wstring> ListStoredWorlds(const std::wstring& modelsFolder);

// Letters, digits, spaces, '-', '_' and '.', not starting with a dot.
bool IsValidWorldName(const std::wstring& name);

// Puts the open world away and brings `target` in its place: a stored world,
// or a new empty one when `createNew`. Either everything moves or nothing
// does; `error` says why when it returns false.
bool SwitchWorld(const std::wstring& modelsFolder, const std::wstring& target, bool createNew,
                 std::wstring& error);

} // namespace io

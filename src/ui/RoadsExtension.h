#pragma once

#include "ui/EditorExtension.h"

#include <memory>

// Procedural roads (ui/RoadsExtension.cpp): part of every build of the editor.
std::unique_ptr<EditorExtension> CreateRoadsExtension(MainWindow& window);

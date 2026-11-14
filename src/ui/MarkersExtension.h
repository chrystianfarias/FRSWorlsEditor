#pragma once

#include "ui/EditorExtension.h"

#include <memory>

// Markers (ui/MarkersExtension.cpp): the region's shops and race starts, and
// the ones placed from the Library -- part of every build of the editor.
std::unique_ptr<EditorExtension> CreateMarkersExtension(MainWindow& window);

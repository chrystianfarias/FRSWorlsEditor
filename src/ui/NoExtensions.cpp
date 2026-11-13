#include "ui/EditorExtension.h"

// The editor as this repository builds it has no extensions. A build that
// links its own in compiles its CreateEditorExtensions instead of this file.
std::vector<std::unique_ptr<EditorExtension>> CreateEditorExtensions(MainWindow&) {
    return {};
}

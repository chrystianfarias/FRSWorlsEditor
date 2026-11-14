#pragma once

#include "ui/EditorExtension.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <functional>
#include <map>
#include <string>

namespace nfs { class World; }

struct HiddenItemsHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    Viewport* viewport = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> modelsFolder;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
};

// Objects hidden in the editor only, to get at what is behind or under them.
// The game never sees this: nothing is written to its files. H hides the
// selected objects, Alt+H shows every hidden one again, and the Explorer's
// "Hidden" branch lists them -- a click shows that one again. The list is
// kept per region in <game>\models\editor-hidden-<REGION>.txt, so it goes
// with the world (and its .frsworld file).
class HiddenItems : public EditorExtension, public ViewportTool {
public:
    explicit HiddenItems(HiddenItemsHost host) : host_(std::move(host)) {}

    static constexpr int kHideCommand = 44701, kShowAllCommand = 44702;

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override { items_.clear(); }
    size_t SceneSignature() const override { return items_.size() * 104729 + version_; }
    void PopulateTree(SceneTreeBuilder& tree) override;
    void Tick(float deltaSeconds) override;   // FRS_HIDE_SELFTEST only
    void OnTreeItem(intptr_t code) override;

    // ---- ViewportTool: H, Alt+H
    bool KeyDown(WPARAM key, bool repeat) override;

private:
    void HideSelection();
    void ShowAll();
    void Show(const std::string& key);
    void Apply();          // into the renderer, and onto disk
    std::wstring FilePath() const;
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    HiddenItemsHost host_;
    std::map<std::string, std::wstring> items_;   // renderer key -> what a person reads
    std::vector<std::string> rows_;               // tree row code -> key
    size_t version_ = 0;
};

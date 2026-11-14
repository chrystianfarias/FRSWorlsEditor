#include "ui/HiddenItems.h"

#include "core/Log.h"
#include "nfs/World.h"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

} // namespace

void HiddenItems::OnCreate() {
    host_.viewport->AddTool(this);
    if (!host_.menu) return;
    // Into the View menu, after what is there.
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"View") == std::wstring::npos) continue;
        HMENU view = ::GetSubMenu(host_.menu, i);
        if (!view) break;
        ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(view, MF_STRING, kHideCommand, L"&Hide Selected Objects (editor only)\tH");
        ::AppendMenuW(view, MF_STRING, kShowAllCommand, L"Show All &Hidden Objects\tAlt+H");
        break;
    }
}

bool HiddenItems::OnCommand(int id) {
    if (id == kHideCommand) { HideSelection(); return true; }
    if (id == kShowAllCommand) { ShowAll(); return true; }
    return false;
}

void HiddenItems::UpdateMenuChecks(HMENU menu) {
    ::EnableMenuItem(menu, kShowAllCommand, MF_BYCOMMAND | (items_.empty() ? MF_GRAYED : MF_ENABLED));
}

bool HiddenItems::KeyDown(WPARAM key, bool repeat) {
    if (key != 'H' || repeat || ::GetKeyState(VK_CONTROL) < 0 || ::GetKeyState(VK_SHIFT) < 0) return false;
    if (::GetKeyState(VK_MENU) < 0) ShowAll();
    else HideSelection();
    return true;
}

std::wstring HiddenItems::FilePath() const {
    nfs::World* world = host_.world ? host_.world() : nullptr;
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (!world || folder.empty()) return L"";
    return folder + L"\\editor-hidden-" + world->Region() + L".txt";
}

void HiddenItems::OnRegionLoaded() {
    items_.clear();
    const std::wstring path = FilePath();
    if (!path.empty()) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            char line[512];
            while (std::fgets(line, sizeof(line), file)) {
                std::string text = line;
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                const size_t tab = text.find('\t');
                if (tab == std::string::npos || text[0] == '#') continue;
                items_[text.substr(0, tab)] = FromUtf8(text.substr(tab + 1));
            }
            std::fclose(file);
        }
    }
    std::set<std::string> keys;
    for (const auto& [key, label] : items_) keys.insert(key);
    Renderer().SetHiddenInstances(std::move(keys));
    ++version_;
    if (!items_.empty()) LOG_INFO("hidden in the editor: %zu objects", items_.size());
}

void HiddenItems::Apply() {
    std::set<std::string> keys;
    for (const auto& [key, label] : items_) keys.insert(key);
    Renderer().SetHiddenInstances(std::move(keys));
    ++version_;
    const std::wstring path = FilePath();
    if (!path.empty()) {
        if (items_.empty()) {
            ::DeleteFileW(path.c_str());
        } else {
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
                std::fprintf(file, "# objects hidden in FRS World Editor only; the game ignores this file\n");
                for (const auto& [key, label] : items_)
                    std::fprintf(file, "%s\t%s\n", key.c_str(), Utf8(label).c_str());
                std::fclose(file);
            }
        }
    }
    if (host_.menuChecks) host_.menuChecks();
}

void HiddenItems::HideSelection() {
    const auto selection = Renderer().Selection();
    size_t hidden = 0, skipped = 0;
    for (const PickResult& picked : selection) {
        const std::string key = Renderer().HiddenKey(picked);
        if (key.empty()) { ++skipped; continue; }
        const std::wstring label = (picked.object ? Widen(picked.object->name) : L"?") + L"  -  " +
                                   Widen(picked.sectionName.empty() ? std::string("?") : picked.sectionName);
        if (items_.emplace(key, label).second) ++hidden;
    }
    if (selection.empty()) {
        Status(L"Select the objects to hide first, then H. Alt+H shows them all again.");
        return;
    }
    Renderer().ClearSelection();
    Apply();
    Status(std::to_wstring(hidden) + L" object(s) hidden in the editor only" +
           (skipped ? L" (" + std::to_wstring(skipped) + L" of the model library left alone)" : L"") +
           L". Alt+H or the Explorer's Hidden branch shows them again.");
}

void HiddenItems::ShowAll() {
    if (items_.empty()) { Status(L"Nothing is hidden."); return; }
    const size_t count = items_.size();
    items_.clear();
    Apply();
    Status(std::to_wstring(count) + L" hidden object(s) shown again.");
}

void HiddenItems::Show(const std::string& key) {
    if (!items_.erase(key)) return;
    Apply();
    Status(L"Shown again.");
}

void HiddenItems::PopulateTree(SceneTreeBuilder& tree) {
    rows_.clear();
    if (items_.empty()) return;
    HTREEITEM root = tree.insert(TVI_ROOT, L"Hidden in the editor (" + std::to_wstring(items_.size()) +
                                               L")  -  click one to show it, Alt+H shows all", -1);
    size_t shown = 0;
    for (const auto& [key, label] : items_) {
        if (!tree.matches(label)) continue;
        tree.insert(root, label, (intptr_t)rows_.size());
        rows_.push_back(key);
        ++shown;
    }
    tree.finish(root, shown, true);
}

// FRS_HIDE_SELFTEST=1: the object under the middle of the view is hidden,
// the same click then finds something else (or nothing), and it comes back.
void HiddenItems::Tick(float deltaSeconds) {
    static float clock = 0.0f;
    static bool done = false;
    clock += deltaSeconds;
    if (done || clock < 20.0f || !std::getenv("FRS_HIDE_SELFTEST") || !host_.world || !host_.world()) return;
    done = true;
    RECT client{};
    ::GetClientRect(host_.viewport->Handle(), &client);
    const Ray ray = host_.viewport->RayAt(client.right / 2, client.bottom / 2);
    PickResult first;
    bool ok = Renderer().Pick(ray, first) && !Renderer().HiddenKey(first).empty();
    LOG_INFO("hide selftest: under the view %s (%s)", first.object ? first.object->name.c_str() : "nothing",
             Renderer().HiddenKey(first).c_str());
    if (ok) {
        const auto before = items_;
        items_[Renderer().HiddenKey(first)] = L"selftest";
        Apply();
        PickResult second;
        const bool again = Renderer().Pick(ray, second);
        const bool gone = !again || Renderer().HiddenKey(second) != Renderer().HiddenKey(first);
        LOG_INFO("hide selftest: hidden, the same click now finds %s -- %s",
                 again && second.object ? second.object->name.c_str() : "nothing", gone ? "ok" : "FAILED");
        items_ = before;
        Apply();
        PickResult third;
        const bool back = Renderer().Pick(ray, third) && Renderer().HiddenKey(third) == Renderer().HiddenKey(first);
        LOG_INFO("hide selftest: shown again -- %s", back ? "ok" : "FAILED");
        ok = gone && back;
    }
    LOG_INFO("hide selftest: %s", ok ? "PASSED" : "FAILED");
}

void HiddenItems::OnTreeItem(intptr_t code) {
    if (code >= 0 && (size_t)code < rows_.size()) Show(rows_[(size_t)code]);
}

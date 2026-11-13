#include "ui/WorldDialogs.h"

#include "io/WorldProfiles.h"
#include "ui/Resource.h"

namespace {

struct NewState {
    const std::vector<std::wstring>* taken = nullptr;
    std::wstring name;
};

INT_PTR CALLBACK NewProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = (NewState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (message) {
        case WM_INITDIALOG:
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
            ::SetDlgItemTextW(dialog, IDC_WORLD_NOTE,
                              L"Starts from the game as shipped. The open world, with all its objects "
                              L"and edits, is put away and comes back with Open World.");
            ::SendDlgItemMessageW(dialog, IDC_WORLD_NAME, EM_LIMITTEXT, 64, 0);
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t text[80] = {};
                ::GetDlgItemTextW(dialog, IDC_WORLD_NAME, text, 80);
                std::wstring name = text;
                while (!name.empty() && name.back() == L' ') name.pop_back();
                while (!name.empty() && name.front() == L' ') name.erase(name.begin());
                const wchar_t* problem = nullptr;
                if (!io::IsValidWorldName(name))
                    problem = L"A world name uses letters, digits, spaces, '-', '_' and '.'.";
                for (const std::wstring& taken : *state->taken)
                    if (::_wcsicmp(taken.c_str(), name.c_str()) == 0) problem = L"There is a world by that name already.";
                if (problem) {
                    ::MessageBoxW(dialog, problem, L"New world", MB_ICONINFORMATION | MB_OK);
                    ::SetFocus(::GetDlgItem(dialog, IDC_WORLD_NAME));
                    return TRUE;
                }
                state->name = name;
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

struct OpenState {
    const std::wstring* active = nullptr;
    const std::vector<std::wstring>* worlds = nullptr;
    std::wstring chosen;
};

INT_PTR CALLBACK OpenProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = (OpenState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (message) {
        case WM_INITDIALOG: {
            state = (OpenState*)lParam;
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
            ::SetDlgItemTextW(dialog, IDC_WORLD_NOTE,
                              (L"Open now: " + *state->active + L". It is put away as it is.").c_str());
            HWND list = ::GetDlgItem(dialog, IDC_WORLD_LIST);
            for (const std::wstring& world : *state->worlds)
                ::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)world.c_str());
            ::SendMessageW(list, LB_SETCURSEL, 0, 0);
            ::SetFocus(list);
            return FALSE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const bool doubleClick = id == IDC_WORLD_LIST && HIWORD(wParam) == LBN_DBLCLK;
            if (id == IDOK || doubleClick) {
                const int index = (int)::SendDlgItemMessageW(dialog, IDC_WORLD_LIST, LB_GETCURSEL, 0, 0);
                wchar_t name[80] = {};
                if (index < 0 ||
                    ::SendDlgItemMessageW(dialog, IDC_WORLD_LIST, LB_GETTEXTLEN, (WPARAM)index, 0) >= 80)
                    return TRUE;
                ::SendDlgItemMessageW(dialog, IDC_WORLD_LIST, LB_GETTEXT, (WPARAM)index, (LPARAM)name);
                state->chosen = name;
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

} // namespace

bool AskNewWorldName(HINSTANCE instance, HWND parent, const std::vector<std::wstring>& taken,
                     std::wstring& name) {
    NewState state;
    state.taken = &taken;
    if (::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_NEW_WORLD), parent, NewProc, (LPARAM)&state) != IDOK)
        return false;
    name = state.name;
    return true;
}

bool ChooseWorldToOpen(HINSTANCE instance, HWND parent, const std::wstring& active,
                       const std::vector<std::wstring>& worlds, std::wstring& chosen) {
    OpenState state;
    state.active = &active;
    state.worlds = &worlds;
    if (::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPEN_WORLD), parent, OpenProc, (LPARAM)&state) != IDOK)
        return false;
    chosen = state.chosen;
    return true;
}

#include "core/Log.h"
#include "ui/MainWindow.h"
#include "ui/SetupWizard.h"

#include <windows.h>

#include <algorithm>

// Opt in to the version 6 common controls so the tree view and status bar use
// the current visual style instead of the Windows 95 look.
#pragma comment(linker,                                                     \
    "/manifestdependency:\"type='win32' "                                   \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

namespace {

double SecondsNow() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value;
        ::QueryPerformanceFrequency(&value);
        return value;
    }();

    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Started by the setup again, as administrator, only to copy the plugins.
    int installResult = 0;
    if (RunInstallCommand(installResult)) {
        ::CoUninitialize();
        return installResult;
    }

    MainWindow window;
    if (!window.Create(instance, showCommand)) {
        ::CoUninitialize();
        return 1;
    }

    // The viewport is animated, so the loop renders whenever there is nothing
    // else to do rather than waiting on messages. WaitMessage is used while the
    // window is hidden so a minimised editor does not spin the GPU.
    double previous = SecondsNow();
    MSG message{};

    while (true) {
        if (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) break;


            HWND browser = window.TextureDialog();
            if (browser && ::IsDialogMessageW(browser, &message)) continue;

            // A text field keeps its own keys: Del, Ctrl+C and Ctrl+V there edit
            // the text, not the scene.
            wchar_t focusClass[16] = {};
            ::GetClassNameW(::GetFocus(), focusClass, 16);
            const bool typing = ::lstrcmpiW(focusClass, L"Edit") == 0;

            if (typing || !::TranslateAcceleratorW(window.Handle(), window.Accelerators(), &message)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            continue;
        }

        if (::IsIconic(window.Handle())) {
            ::WaitMessage();
            previous = SecondsNow();
            continue;
        }

        const double now = SecondsNow();
        // Clamp the step so a stall (a long parse, a dragged window) does not
        // teleport the camera on the next frame.
        const float delta = (float)std::min(now - previous, 0.1);
        previous = now;

        window.Tick(delta);
    }

    window.Destroy();
    ::CoUninitialize();
    return (int)message.wParam;
}

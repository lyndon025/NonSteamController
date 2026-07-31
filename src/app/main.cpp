#include "TrayApp.h"
#include "ViiperBus.h"
#include <Windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Prevent multiple instances.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SteamControllerRemapper_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    int result = 0;
    {
        TrayApp app;
        if (app.Init(hInstance))
            result = app.Run();
    }
    // Scoped so every VirtualController has been destroyed — and so has dropped
    // its bus reference — before the shared libVIIPER server is closed.
    ViiperBus::Instance().Shutdown();

    CloseHandle(mutex);
    return result;
}

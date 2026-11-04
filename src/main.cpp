#include <windows.h>
#include <winrt/base.h>

#include "Island.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Tek kopya çalışsın.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DynamisIsland.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    static Island island;  // arka plan iş parçacıkları süreç sonuna kadar erişebilir
    if (!island.Create(instance)) {
        MessageBoxW(nullptr, L"Dynamis Island başlatılamadı.", L"Dynamis Island", MB_ICONERROR);
        return 1;
    }
    int code = island.Run();
    if (mutex) CloseHandle(mutex);
    return code;
}

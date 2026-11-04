#include <windows.h>
#include <winrt/base.h>
#include <cwchar>

#include "Island.h"
#include "Updater.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    bool afterUpdate = cmdLine && wcsstr(cmdLine, L"--updated") != nullptr;

    // Tek kopya çalışsın. Güncellemeden sonra eski sürümün kapanmasını bekle.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DynamisIsland.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!afterUpdate) return 0;
        DWORD wait = WaitForSingleObject(mutex, 10000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return 0;
    }
    Updater::CleanupOldFiles();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    static Island island;  // arka plan iş parçacıkları süreç sonuna kadar erişebilir
    if (!island.Create(instance)) {
        MessageBoxW(nullptr, L"Dynamis Island başlatılamadı.", L"Dynamis Island", MB_ICONERROR);
        return 1;
    }
    int code = island.Run();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}

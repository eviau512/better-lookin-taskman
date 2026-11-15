#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cwchar>

#include "resource.h"
#include "core/format_util.h"
#include "core/perf_monitor.h"
#include "ui/main_window.h"

namespace {

constexpr wchar_t kDaemonWndClass[] = L"BetterWin10TaskmanDaemonWnd";
constexpr wchar_t kDaemonMutexName[] = L"Local\\BetterWin10TaskmanDaemonMutex";

LRESULT CALLBACK DaemonWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* perf = reinterpret_cast<taskman::PerfMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        perf = reinterpret_cast<taskman::PerfMonitor*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(perf));
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
        return 0;
    }
    case WM_APP + 1:
        if (perf) {
            perf->Init();
        }
        SetTimer(hwnd, 1, 500, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == 1 && perf) {
            perf->Sample(0, 0, 0);
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam && perf) {
            perf->Shutdown();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (perf) {
            perf->Shutdown();
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunPerfDaemon(HINSTANCE hInstance) {
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, kDaemonMutexName);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DaemonWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kDaemonWndClass;
    RegisterClassExW(&wc);

    taskman::PerfMonitor perfMon;
    HWND hWnd = CreateWindowExW(
        0, kDaemonWndClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, &perfMon);

    int exitCode = 0;
    if (hWnd) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    }

    CloseHandle(hMutex);
    return exitCode;
}

void EnsureDaemonRunningAndRegistered() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    if (len == 0 || len >= ARRAYSIZE(exePath)) return;

    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" /daemon";
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ | KEY_WRITE, &hRun) == ERROR_SUCCESS) {
        wchar_t curVal[MAX_PATH + 32] = {};
        DWORD cb = sizeof(curVal);
        DWORD type = 0;
        if (RegQueryValueExW(hRun, L"BetterWin10TaskmanDaemon", nullptr, &type,
                             reinterpret_cast<BYTE*>(curVal), &cb) != ERROR_SUCCESS ||
            type != REG_SZ || _wcsicmp(curVal, cmd.c_str()) != 0) {
            RegSetValueExW(hRun, L"BetterWin10TaskmanDaemon", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hRun);
    }

    if (FindWindowExW(HWND_MESSAGE, nullptr, kDaemonWndClass, nullptr) == nullptr) {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        std::wstring mutableCmd = cmd;
        if (CreateProcessW(exePath, &mutableCmd[0], nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}

void StopAndUnregisterDaemon() {
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS) {
        RegDeleteValueW(hRun, L"BetterWin10TaskmanDaemon");
        RegCloseKey(hRun);
    }
    HWND hDaemon = FindWindowExW(HWND_MESSAGE, nullptr, kDaemonWndClass, nullptr);
    if (hDaemon) {
        PostMessageW(hDaemon, WM_CLOSE, 0, 0);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // Parse CLI options: /0.. /6 to open specific tab, /elevated after UAC restart, /multi for side-by-side testing, /daemon for background perf sampling
    int requestedTab = -1;
    bool isElevatedRestart = false;
    bool allowMultiple = false;
    bool isDaemon = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"/elevated") == 0 || _wcsicmp(argv[i], L"-elevated") == 0) {
                isElevatedRestart = true;
            } else if (_wcsicmp(argv[i], L"/multi") == 0 || _wcsicmp(argv[i], L"-multi") == 0) {
                allowMultiple = true;
            } else if (_wcsicmp(argv[i], L"/daemon") == 0 || _wcsicmp(argv[i], L"-daemon") == 0 ||
                       _wcsicmp(argv[i], L"--daemon") == 0) {
                isDaemon = true;
            } else if ((argv[i][0] == L'/' || argv[i][0] == L'-') &&
                       argv[i][1] >= L'0' && argv[i][1] <= L'7' && argv[i][2] == L'\0') {
                requestedTab = argv[i][1] - L'0';
            }
        }
        LocalFree(argv);
    }

    if (isDaemon) {
        return RunPerfDaemon(hInstance);
    }

    // Pin the pure-memory shared performance ring before the previous non-elevated instance
    // closes so UAC elevation keeps the full curve history with zero disk I/O.
    HANDLE hElevatedRingHold = nullptr;
    if (isElevatedRestart) {
        hElevatedRingHold = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(sizeof(taskman::SharedPerfRing)),
            L"Local\\BetterWin10TaskmanPerfRing");
        HANDLE hElevateReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\BetterWin10TaskmanElevateReady");
        if (hElevateReady) {
            SetEvent(hElevateReady);
            CloseHandle(hElevateReady);
        }
    }

    constexpr wchar_t kMutexName[] = L"Local\\BetterWin10TaskmanSingleInstance";
    HANDLE hMutex = allowMultiple ? nullptr : CreateMutexW(nullptr, FALSE, kMutexName);
    DWORD mutexErr = hMutex ? GetLastError() : 0;

    if (hMutex && mutexErr == ERROR_ALREADY_EXISTS && isElevatedRestart) {
        // Wait briefly for the previous non-elevated instance to close and release mutex
        for (int attempt = 0; attempt < 25; ++attempt) {
            CloseHandle(hMutex);
            Sleep(100);
            hMutex = CreateMutexW(nullptr, FALSE, kMutexName);
            mutexErr = GetLastError();
            if (mutexErr != ERROR_ALREADY_EXISTS) break;
        }
    }

    if (hMutex && mutexErr == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(taskman::MainWindow::kWndClass, nullptr);
        if (hExisting) {
            DWORD targetPid = 0;
            GetWindowThreadProcessId(hExisting, &targetPid);
            if (targetPid != 0) AllowSetForegroundWindow(targetPid);
            SendMessageW(hExisting, WM_APP_BRING_TO_FRONT, static_cast<WPARAM>(requestedTab), 0);
        }
        CloseHandle(hMutex);
        if (hElevatedRingHold) CloseHandle(hElevatedRingHold);
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    // Best-effort SeDebugPrivilege (succeeds when elevated)
    taskman::EnablePrivilege(SE_DEBUG_NAME);

    int exitCode = 0;
    {
        if (!allowMultiple) {
            taskman::AppSettings initSettings;
            taskman::LoadSettings(initSettings);
            if (initSettings.preLaunchSampling) {
                EnsureDaemonRunningAndRegistered();
            } else {
                StopAndUnregisterDaemon();
            }
        }
        taskman::MainWindow mainWin;
        if (mainWin.Create(nCmdShow, requestedTab)) {
            if (hElevatedRingHold) {
                CloseHandle(hElevatedRingHold);
                hElevatedRingHold = nullptr;
            }
            HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                bool skipAccel = false;
                if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.wParam == VK_DELETE) {
                    wchar_t cls[32] = {};
                    if (msg.hwnd && GetClassNameW(msg.hwnd, cls, ARRAYSIZE(cls)) > 0 && _wcsicmp(cls, L"Edit") == 0) {
                        skipAccel = true;
                    }
                }
                HWND hActive = GetActiveWindow();
                if (!skipAccel && hAccel && TranslateAcceleratorW(hActive ? hActive : mainWin.Hwnd(), hAccel, &msg)) {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
        if (hElevatedRingHold) {
            CloseHandle(hElevatedRingHold);
            hElevatedRingHold = nullptr;
        }
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    if (SUCCEEDED(hrCom)) CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return exitCode;
}

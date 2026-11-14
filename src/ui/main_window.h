#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "../core/settings.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../core/process_monitor.h"
#include "../core/perf_monitor.h"
#include "../core/app_history.h"
#include "../core/startup_manager.h"
#include "../core/user_manager.h"
#include "../core/service_manager.h"
#include "listview_theme.h"
#include "tab_processes.h"
#include "tab_performance.h"
#include "tab_app_history.h"
#include "tab_startup.h"
#include "tab_users.h"
#include "tab_details.h"
#include "tab_services.h"

namespace taskman {

class MainWindow {
public:
    static constexpr wchar_t kWndClass[] = L"BetterWin10TaskmanMainWnd";
    static constexpr int kTabCount = 7;

    bool Create(int nCmdShow, int initialTab = -1);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void InitMonitorsAndTabs();
    void RebuildMenu();
    void UpdateMenuChecks();
    void LayoutChildren();
    void SwitchTab(int tabIdx);
    void ToggleMoreDetails();
    bool IsInPerfSummary() const { return perfSummaryMode_ != PerfSummaryMode::Normal; }
    void SetPerfSummaryMode(PerfSummaryMode mode);
    void ApplyCurrentTheme(bool redrawTabs = true);
    void ToggleThemeQuick();
    void SetLanguageMode(LangMode mode);
    void SetUpdateSpeed(UpdateSpeed speed);
    void UpdateWindowTitle();
    void UpdateAlwaysOnTop();
    void UpdatePrimaryActionButton();
    void LayoutFooterButtons();
    void SaveAllTabColumnWidths();

    void RefreshAll(bool force);
    void RefreshActiveView(bool allowManagerRefresh, ULONGLONG minStaleMs = 4000);
    void RefreshSimpleList();
    void UpdateTrayIcon(double cpuPct);
    void DestroyTrayIcon();
    void ShowTrayContextMenu();

    void PaintClient(HDC hdc, const RECT& rc);
    void DrawOwnerButton(const DRAWITEMSTRUCT* dis);
    void DrawTabStrip(HDC hdc, const ThemePalette& pal, const Strings& s);
    void HandleTabStripClick(POINT pt);
    void HandleTabStripMove(POINT pt);
    void HandleSimpleListContextMenu(POINT screenPt);
    void SwitchToSimpleSelected();

    void HandleCommand(UINT id);
    void NavigateToDetails(DWORD pid);

    HWND hwnd_ = nullptr;
    HMENU hMainMenu_ = nullptr;

    // Footer & simple controls
    HWND hBtnMore_ = nullptr;
    HWND hBtnAction_ = nullptr;
    HWND hListSimple_ = nullptr;
    ThemedListView simpleLv_;

    struct SimpleAppRow {
        DWORD pid = 0;
        std::wstring title;
        std::wstring imagePath;
        HICON icon = nullptr;
    };
    std::vector<SimpleAppRow> simpleRows_;

    // Layout geometry
    RECT tabStripRect_{};
    RECT tabRects_[kTabCount]{};
    RECT themeBtnRect_{};
    RECT contentRect_{};
    RECT footerRect_{};
    int hoveredTab_ = -1;
    bool hoverThemeBtn_ = false;
    bool hoverBtnMore_ = false;
    bool hoverBtnAction_ = false;

    // Monitors & State
    AppSettings settings_;
    ProcessMonitor procMon_;
    PerfMonitor perfMon_;
    AppHistoryManager appHistory_;
    StartupManager startupMgr_;
    UserManager userMgr_;
    ServiceManager serviceMgr_;

    // Tabs
    TabProcesses tabProcesses_;
    TabPerformance tabPerf_;
    TabAppHistory tabAppHistory_;
    TabStartup tabStartup_;
    TabUsers tabUsers_;
    TabDetails tabDetails_;
    TabServices tabServices_;

    ULONGLONG lastSampleTick_ = 0;
    ULONGLONG lastStartupRefreshTick_ = 0;
    ULONGLONG lastServiceRefreshTick_ = 0;
    bool startupInited_ = false;
    bool servicesInited_ = false;
    bool timerRunning_ = false;

    // Tray Icon
    NOTIFYICONDATAW nid_{};
    HICON hTrayIcon_ = nullptr;
    bool trayAdded_ = false;
    UINT taskbarCreatedMsg_ = 0;

    PerfSummaryMode perfSummaryMode_ = PerfSummaryMode::Normal;
    RECT savedPreSummaryRect_{};
    bool savedPreSummaryMax_ = false;
};

} // namespace taskman

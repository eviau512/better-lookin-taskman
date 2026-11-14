#pragma once
#include <windows.h>
#include <cstdint>

namespace taskman {

enum class ThemeMode : int {
    System = 0,
    Light  = 1,
    Dark   = 2
};

enum class LangMode : int {
    Auto    = 0,
    English = 1,
    Chinese = 2
};

enum class UpdateSpeed : int {
    High   = 0, // 500 ms (30 seconds window)
    Normal = 1, // 1000 ms (60 seconds window)
    Low    = 2, // 4000 ms (4 minutes window)
    Paused = 3
};

enum class PerfSummaryMode : int {
    Normal      = 0,
    SidebarOnly = 1, // Double-click left nav pane: compact sidebar-only floating window
    GraphOnly   = 2  // Double-click right pane: compact header + graph floating window
};

enum DetailsColumnId : uint32_t {
    DCOL_NAME = 0,
    DCOL_PID,
    DCOL_STATUS,
    DCOL_USER,
    DCOL_CPU,
    DCOL_MEM_PRIVATE,
    DCOL_MEM_WORKING_SET,
    DCOL_MEM_COMMIT,
    DCOL_THREADS,
    DCOL_HANDLES,
    DCOL_SESSION_ID,
    DCOL_PLATFORM,
    DCOL_ELEVATED,
    DCOL_UAC_VIRT,
    DCOL_COMMAND_LINE,
    DCOL_DESCRIPTION,
    DCOL_COUNT
};

struct AppSettings {
    ThemeMode themeMode = ThemeMode::System;
    LangMode langMode = LangMode::Auto;
    UpdateSpeed updateSpeed = UpdateSpeed::Normal;

    bool alwaysOnTop = false;
    bool minimizeOnUse = false;
    bool hideWhenMinimized = false;
    bool showFullAccountName = false;
    bool showAllHistory = false;
    bool groupByType = true;
    bool showKernelTimes = false;
    bool cpuLogicalView = false;
    bool summaryView = false;
    bool moreDetails = true;

    int activeTab = 0;          // 0..6
    int perfSelectedItem = 0;   // index in Performance left rail

    bool memShowPercent = false;
    bool diskShowPercent = true;
    bool netShowPercent = false;

    RECT windowRect = { 0, 0, 0, 0 };
    bool maximized = false;

    // Column visibility and widths for Processes tab (10 cols, stored at 96 DPI)
    uint32_t procColMask = 0x3FF;
    int procColWidths[10] = { 265, 84, 74, 94, 74, 84, 74, 102, 100, 120 };

    // Column widths for AppHistory, Startup, Users, Services tabs (stored at 96 DPI)
    int appHistColWidths[5]  = { 280, 118, 118, 138, 118 };
    int startupColWidths[4]  = { 270, 225, 108, 138 };
    int usersColWidths[6]    = { 235, 118, 86, 108, 102, 102 };
    int servicesColWidths[5] = { 175, 72, 320, 102, 150 };

    // Visibility bitmask and widths for Details tab
    uint32_t detailsColMask =
        (1u << DCOL_NAME) |
        (1u << DCOL_PID) |
        (1u << DCOL_STATUS) |
        (1u << DCOL_USER) |
        (1u << DCOL_CPU) |
        (1u << DCOL_MEM_PRIVATE) |
        (1u << DCOL_DESCRIPTION);

    int detailsColWidths[DCOL_COUNT] = {
        180, 65, 85, 115, 58, 125, 110, 100, 70, 75, 75, 70, 68, 95, 240, 200
    };
};

void LoadSettings(AppSettings& s);
void SaveSettings(const AppSettings& s);
DWORD GetUpdateIntervalMs(UpdateSpeed speed);

} // namespace taskman

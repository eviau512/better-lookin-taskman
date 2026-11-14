#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include "../core/process_monitor.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabProcesses {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();

    void RefreshData(const ProcessMonitor& mon);

    // Actions & Shortcuts
    bool HasSelection() const;
    void EndSelectedTask();
    void ExpandOrCollapseSelected(int forceState = -1); // -1 toggle, 0 collapse, 1 expand
    void ExpandAll(bool expand);
    void OpenSelectedProperties();
    void HandleKeyDown(WPARAM vk);
    void HandleClick(NMITEMACTIVATE* nmia);
    void HandleDoubleClick(NMITEMACTIVATE* nmia);
    void HandleColumnClick(int col);
    void HandleContextMenu(POINT screenPt);
    void HandleHeaderContextMenu(POINT screenPt);
    void SaveColumnWidths();

    LRESULT HandleNotify(NMHDR* nm);

    HWND Hwnd() const { return hList_; }

    // Callback invoked when user clicks "Go to details"
    std::function<void(DWORD pid)> onGoToDetails;
    // Callback invoked when an action completes with minimizeOnUse
    std::function<void()> onActionMinimize;
    // Callback invoked when user resizes or toggles columns
    std::function<void()> onColumnsChanged;

private:
    struct DisplayRow {
        enum Kind { CategoryHeader, GroupParent, SingleOrChild };
        Kind kind = SingleOrChild;
        ProcessCategory category = ProcessCategory::App;
        std::wstring groupKey;
        std::wstring title;
        std::wstring statusText;
        HICON icon = nullptr;
        DWORD primaryPid = 0;
        std::vector<DWORD> pids;
        std::wstring imagePath;
        std::wstring procName;
        int childCount = 0;
        bool expanded = false;

        double cpuPct = 0.0;
        uint64_t memBytes = 0;
        double diskBps = 0.0;
        double netBps = 0.0;
        double gpuPct = 0.0;
        std::wstring gpuEngine;
        int powerUsage = 0;
        int powerTrend = 0;
    };

    void RebuildRows(const ProcessMonitor& mon);
    void ApplyCurrentRows();
    int GetSelectedPidList(std::vector<DWORD>& outPids, std::wstring& outName, std::wstring& outPath) const;
    void EndPidList(const std::vector<DWORD>& pids);

    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;
    const ProcessMonitor* lastMon_ = nullptr;

    std::vector<DisplayRow> rows_;
    std::unordered_set<std::wstring> expandedGroups_;
    bool collapsedCategories_[3] = { false, false, false };

    int sortCol_ = 2; // Default sort by CPU descending or Name? Win10 defaults unsorted/Name or CPU; Name ascending or user clicked
    bool sortAsc_ = true;
    uint64_t totalMemBytes_ = 1;
    ProcessTotals lastTotals_{};

    // Visible columns mapping (0..9 fixed logical IDs mapped to ListView columns)
    std::vector<int> visibleCols_;
    uint32_t colVisMask_ = 0x3FF; // all 10 columns visible by default
};

} // namespace taskman

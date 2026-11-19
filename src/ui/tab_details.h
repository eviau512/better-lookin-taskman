#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "../core/process_monitor.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabDetails {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void RefreshData(const ProcessMonitor& mon);
    void SetFilter(const std::wstring& query);
    const std::wstring& Filter() const { return filterText_; }
    LRESULT HandleNotify(NMHDR* nm);

    bool HasSelection() const;
    bool IsSelectedExplorer() const;
    bool NeedsCommandLines() const;
    void EndSelectedTask();
    void RestartSelectedExplorer();
    void EndSelectedProcessTree();
    void OpenSelectedProperties();
    void SelectProcessByPid(DWORD pid);
    void HandleKeyDown(WPARAM vk);
    void HandleContextMenu(POINT screenPt);

    HWND Hwnd() const { return hList_; }

    std::function<void(DWORD pid)> onGoToService;
    std::function<void()> onSelectionChanged;
    std::function<void()> onRefreshNeeded;
    std::function<void()> onActionMinimize;
    std::function<void()> onColumnsChanged;

private:
    void ApplyFilterAndSort();
    void SortRows();
    void HandleHeaderContextMenu(POINT screenPt);

    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;

    std::vector<ProcessInfo> allProcs_;
    std::vector<ProcessInfo> procs_;
    std::wstring filterText_;
    std::wstring filterLower_;
    std::vector<int> visibleCols_; // DetailsColumnId
    int sortColId_ = DCOL_NAME;
    bool sortAsc_ = true;
};

} // namespace taskman

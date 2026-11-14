#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include "../core/user_manager.h"
#include "../core/process_monitor.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabUsers {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void RefreshData(const UserManager& mgr, uint64_t totalMemBytes);
    LRESULT HandleNotify(NMHDR* nm);

    bool HasSelection() const;
    bool IsUserSelected() const;
    void DisconnectSelected();
    void ExpandAll(bool expand);
    void OpenSelectedProperties();
    void HandleContextMenu(POINT screenPt);

    HWND Hwnd() const { return hList_; }

    std::function<void(DWORD pid)> onGoToDetails;
    std::function<void()> onSelectionChanged;
    std::function<void()> onRefreshNeeded;
    std::function<void()> onColumnsChanged;

private:
    struct DisplayRow {
        bool isSession = true;
        DWORD sessionId = 0;
        bool active = true;
        bool expanded = false;
        DWORD pid = 0;
        HICON icon = nullptr;
        std::wstring title;
        std::wstring status;
        std::wstring imagePath;
        double cpuPct = 0.0;
        uint64_t memBytes = 0;
        double diskBps = 0.0;
        double netBps = 0.0;
    };

    void RebuildRows(const UserManager& mgr);

    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;

    std::vector<DisplayRow> rows_;
    std::unordered_set<DWORD> expandedSessions_;
    int sortCol_ = 0;
    bool sortAsc_ = true;
    uint64_t totalMemBytes_ = 1;
    const UserManager* lastMgr_ = nullptr;
};

} // namespace taskman

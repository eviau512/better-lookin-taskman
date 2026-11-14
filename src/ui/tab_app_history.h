#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "../core/app_history.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabAppHistory {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void RefreshData(const AppHistoryManager& mgr);
    LRESULT HandleNotify(NMHDR* nm);
    bool HandleCommand(WORD id);
    bool HandleParentDraw(HWND hChild, HDC hdc, const RECT& rc);
    bool HitTestLink(POINT clientPt) const { return PtInRect(&linkRect_, clientPt) != FALSE; }
    void HandleContextMenu(POINT screenPt);

    HWND Hwnd() const { return hList_; }

    std::function<void()> onDeleteHistory;
    std::function<void()> onColumnsChanged;

private:
    void SortEntries();

    HWND hParent_ = nullptr;
    HWND hBanner_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;

    std::vector<AppHistoryEntry> entries_;
    std::wstring sinceDate_;
    int sortCol_ = 1; // CPU time descending by default
    bool sortAsc_ = false;
    bool hoverLink_ = false;
    RECT linkRect_{};
};

} // namespace taskman

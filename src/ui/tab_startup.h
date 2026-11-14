#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "../core/startup_manager.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabStartup {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void RefreshData(StartupManager& mgr);
    LRESULT HandleNotify(NMHDR* nm);

    bool HasSelection() const;
    bool SelectedIsEnabled() const;
    void SetSelectedEnabled(bool enable);
    void ToggleSelected();
    void OpenSelectedProperties();
    void HandleContextMenu(POINT screenPt);

    HWND Hwnd() const { return hList_; }

    std::function<void()> onSelectionChanged;
    std::function<void()> onRefreshNeeded;
    std::function<void()> onColumnsChanged;

private:
    void SortRows();

    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;
    StartupManager* mgr_ = nullptr;

    std::vector<size_t> order_; // indices into mgr_->Items()
    int sortCol_ = 0;
    bool sortAsc_ = true;
};

} // namespace taskman

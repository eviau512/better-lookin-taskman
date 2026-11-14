#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "../core/service_manager.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabServices {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void RefreshData(const ServiceManager& mgr);
    LRESULT HandleNotify(NMHDR* nm);

    bool HasSelection() const;
    void StartSelected();
    void StopSelected();
    void RestartSelected();
    void OpenServicesMsc();
    void SelectServiceByPid(DWORD pid);
    void HandleContextMenu(POINT screenPt);

    HWND Hwnd() const { return hList_; }

    std::function<void(DWORD pid)> onGoToDetails;
    std::function<void()> onRefreshNeeded;
    std::function<void()> onColumnsChanged;

private:
    void SortRows();

    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;
    AppSettings* settings_ = nullptr;

    std::vector<ServiceItem> items_;
    int sortCol_ = 0;
    bool sortAsc_ = true;
};

} // namespace taskman

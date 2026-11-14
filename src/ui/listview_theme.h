#pragma once
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <vector>

namespace taskman {

struct HeaderColumnMeta {
    std::wstring title;
    std::wstring totalText; // optional top line for Processes tab (e.g. "12%")
    bool rightAlign = false;
};

struct ListViewThemeCallbacks {
    // Return number of rows for virtual ListView (or unused if non-virtual)
    std::function<std::wstring(int row, int col)> getCellText;
    // Notify owner when header column tracking or divider double-click completes
    std::function<void()> onHeaderTrack;
    // Return optional icon for col 0
    std::function<HICON(int row)> getRowIcon;
    // Return indent level (0=category header, 1=top item, 2=child item)
    std::function<int(int row)> getRowIndent;
    // Return group expand state: -1 = no chevron, 0 = collapsed, 1 = expanded
    std::function<int(int row)> getRowExpandState;
    // Return true if row is a category section header ("Apps (3)")
    std::function<bool(int row)> isCategoryHeader;
    // Return true if row is a member or expanded parent of an expanded group (subtle tint)
    std::function<bool(int row)> isExpandedGroupRow;
    // Return heatmap level 0..4 for cell
    std::function<int(int row, int col)> getCellHeat;
    // Return optional custom text color (0 = use default)
    std::function<COLORREF(int row, int col, bool& overrideColor)> getCellTextColor;
};

class ThemedListView {
public:
    void Attach(HWND hList, bool twoLineHeader = false);
    void Detach();

    void ApplyTheme();
    void SetColumnsMeta(std::vector<HeaderColumnMeta> cols);
    void UpdateHeaderTotals(const std::vector<std::wstring>& totals);
    void SetSortIndicator(int col, bool ascending);
    void SetCallbacks(ListViewThemeCallbacks cb) { cb_ = std::move(cb); }

    // Handles NM_CUSTOMDRAW from parent WM_NOTIFY
    LRESULT HandleCustomDraw(NMLVCUSTOMDRAW* cd);

    // Hit-test if click at client point (pt) hit the expand/collapse chevron on row
    bool HitTestChevron(int row, POINT clientPt) const;

    int HoveredRow() const { return hoveredRow_; }
    HWND ListHwnd() const { return hList_; }
    HWND HeaderHwnd() const { return hHeader_; }

    static int WindowDpi(HWND hwnd);
    static void SaveColumnsToArray(HWND hList, const int* colToSlot, int count, int* outWidths, int maxSlots);
    static bool IsHeaderTrackNotify(const NMHDR* nm, HWND hHeader);

private:
    static LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    void DrawHeader(HDC hdc);
    void DrawRow(NMLVCUSTOMDRAW* cd);
    int AutoFitColumnWidth(int col, bool includeHeader = true);

    HWND hList_ = nullptr;
    HWND hHeader_ = nullptr;
    bool twoLineHeader_ = false;
    int hoveredRow_ = -1;
    int hoveredHeaderCol_ = -1;
    int sortCol_ = -1;
    bool sortAsc_ = true;
    std::vector<HeaderColumnMeta> colsMeta_;
    ListViewThemeCallbacks cb_;
    HFONT fontMain_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontTotal_ = nullptr;
    HFONT fontGroup_ = nullptr;
    HIMAGELIST hRowHeightImg_ = nullptr;
};

} // namespace taskman

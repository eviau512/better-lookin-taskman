#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "../core/lock_checker.h"
#include "../core/process_monitor.h"
#include "../core/settings.h"
#include "listview_theme.h"

namespace taskman {

class TabLocks {
public:
    ~TabLocks();

    void Create(HWND parent, AppSettings* settings, ProcessMonitor* procMon);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();
    void RebuildColumns();
    void SaveColumnWidths();

    void StartScan(const std::wstring& targetPath);
    LRESULT HandleNotify(NMHDR* nm);
    void HandleContextMenu(POINT screenPt);

    bool HasKillableSelection() const;
    void EndSelectedLockingProcess();

    HWND Hwnd() const { return hwnd_; }

    std::function<void(DWORD pid)> onGoToDetails;
    std::function<void()> onSelectionChanged;
    std::function<void()> onColumnsChanged;
    std::function<void()> onElevateRequested;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    static LRESULT CALLBACK BtnHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void LayoutControls();
    void RefreshDrivePills();
    void PaintPanel(HDC hdc, const RECT& rc);
    void DrawOwnerButton(const DRAWITEMSTRUCT* dis);
    void BrowseFile();
    void BrowseFolder();
    void SortEntries();
    std::wstring GetCellText(int row, int col) const;
    std::wstring FormatSourceMask(uint32_t mask, int handleCount) const;
    int GetSelectedRow() const;

    HWND hParent_ = nullptr;
    HWND hwnd_ = nullptr;
    AppSettings* settings_ = nullptr;
    ProcessMonitor* procMon_ = nullptr;

    HWND hEditPath_ = nullptr;
    HWND hBtnFile_ = nullptr;
    HWND hBtnFolder_ = nullptr;
    HWND hBtnScan_ = nullptr;
    HWND hBtnElevate_ = nullptr;
    HWND hList_ = nullptr;
    ThemedListView themedLv_;

    bool hoverFile_ = false;
    bool hoverFolder_ = false;
    bool hoverScan_ = false;
    bool hoverElevate_ = false;

    struct DrivePill {
        std::wstring driveRoot;
        RECT rc{};
    };
    std::vector<DrivePill> drivePills_;
    int hoveredPill_ = -1;

    RECT leftPaneRc_{};
    RECT editFrameRc_{};
    RECT dropZoneRc_{};
    RECT rightPaneRc_{};
    RECT diagCardRc_{};

    std::wstring currentTarget_;
    LockCheckResult result_;
    int sortCol_ = 2; // default sort by critical level
    bool sortAsc_ = false;

    std::thread scanThread_;
    std::atomic<uint64_t> scanGen_{ 0 };
    std::atomic<bool> scanning_{ false };
};

} // namespace taskman

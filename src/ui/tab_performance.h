#pragma once
#include <windows.h>
#include <functional>
#include <vector>
#include <string>
#include "../core/perf_monitor.h"
#include "../core/settings.h"

namespace taskman {

class TabPerformance {
public:
    void Create(HWND parent, AppSettings* settings);
    void Destroy();
    void Resize(const RECT& rc);
    void Show(bool visible);
    void ApplyTheme();

    void UpdateData(const PerfMonitor& perf);
    void CopySelectedSummary();
    void HandleContextMenu(POINT screenPt);

    PerfSummaryMode SummaryMode() const { return summaryMode_; }
    void SetSummaryMode(PerfSummaryMode mode, bool notifyParent = true);
    int GetSidebarPreferredHeight() const;
    RECT GetRightGraphScreenRect() const;

    HWND Hwnd() const { return hwnd_; }

    std::function<void(PerfSummaryMode)> onSummaryModeChanged;
    std::function<void(UpdateSpeed)> onSpeedChanged;

private:
    enum class NavKind { Cpu, Memory, Disk, Network, Gpu };
    struct NavItem {
        NavKind kind = NavKind::Cpu;
        int index = 0; // index into disks/nets/gpus
        RECT cardRect{};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RebuildNavItems();
    void Paint(HDC hdc, const RECT& clientRc);
    void HandleLeftClick(POINT pt);
    void HandleDoubleClick(POINT pt);
    void ClampScroll();

    HWND hParent_ = nullptr;
    HWND hwnd_ = nullptr;
    AppSettings* settings_ = nullptr;
    const PerfMonitor* perf_ = nullptr;

    std::vector<NavItem> navItems_;
    int hoveredNav_ = -1;
    int navScrollY_ = 0;
    int navTotalHeight_ = 0;
    int scrollTick_ = 0;
    PerfSummaryMode summaryMode_ = PerfSummaryMode::Normal;
    bool lbtnDown_ = false;
    POINT dragStartPt_{};

    RECT leftPaneRect_{};
    RECT rightPaneRect_{};
    RECT graphAreaRect_{};
};

} // namespace taskman

#include "tab_performance.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../core/format_util.h"
#include "../resource.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace taskman {

namespace {

constexpr wchar_t kPerfWndClass[] = L"BetterWin10TaskmanPerfView";
bool g_perfClassRegistered = false;

inline Gdiplus::Color ToGpColor(COLORREF cr, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(cr), GetGValue(cr), GetBValue(cr));
}

void DrawChartSeries(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& rect,
    const std::vector<double>& series,
    double maxVal,
    const ChartColorSet& colors,
    int scrollTick,
    bool drawGrid = true,
    bool dashed = false,
    const std::vector<double>* overlaySeries = nullptr)
{
    if (rect.Width < 4.0f || rect.Height < 4.0f) return;

    Gdiplus::Region prevClip;
    g.GetClip(&prevClip);
    g.SetClip(rect, Gdiplus::CombineModeIntersect);

    // Background of graph box
    // Keep surfaceBg inside plot; fill is drawn under curve
    if (drawGrid) {
        Gdiplus::Pen gridPen(ToGpColor(colors.grid, 210), 1.0f);
        // Horizontal 4 rows (25%, 50%, 75%)
        for (int i = 1; i < 4; ++i) {
            float y = rect.Y + rect.Height * (static_cast<float>(i) / 4.0f);
            g.DrawLine(&gridPen, rect.X, y, rect.GetRight(), y);
        }
        // Vertical 10 divisions with smooth scrolling offset (Win10 style)
        constexpr int kDivs = 10;
        float stepX = rect.Width / static_cast<float>(kDivs);
        float fracOffset = static_cast<float>(scrollTick % 6) / 6.0f; // 60 samples -> 6 samples per grid cell
        for (int i = 0; i <= kDivs; ++i) {
            float x = rect.X + (static_cast<float>(i) - fracOffset) * stepX;
            if (x > rect.X + 1.0f && x < rect.GetRight() - 1.0f) {
                g.DrawLine(&gridPen, x, rect.Y, x, rect.GetBottom());
            }
        }
    }

    auto plotCurve = [&](const std::vector<double>& data, COLORREF lineCr, COLORREF fillCr, BYTE alpha, bool isDashed) {
        if (data.empty() || maxVal <= 0.0) return;
        size_t n = data.size();
        if (n < 2) return;

        std::vector<Gdiplus::PointF> pts;
        pts.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(n - 1);
            double v = std::clamp(data[i], 0.0, maxVal);
            float x = rect.X + t * rect.Width;
            float y = (rect.GetBottom() - 0.5f) - static_cast<float>(v / maxVal) * (std::max)(1.0f, rect.Height - 1.5f);
            pts.emplace_back(x, y);
        }

        if (alpha > 0 && !isDashed) {
            std::vector<Gdiplus::PointF> poly = pts;
            poly.emplace_back(rect.GetRight(), rect.GetBottom());
            poly.emplace_back(rect.X, rect.GetBottom());
            Gdiplus::SolidBrush fillBrush(ToGpColor(fillCr, alpha));
            g.FillPolygon(&fillBrush, poly.data(), static_cast<INT>(poly.size()));
        }

        Gdiplus::Pen linePen(ToGpColor(lineCr, 255), 1.4f);
        linePen.SetLineJoin(Gdiplus::LineJoinRound);
        linePen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        if (isDashed) {
            linePen.SetDashStyle(Gdiplus::DashStyleDash);
        }
        auto oldMode = g.GetSmoothingMode();
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.DrawLines(&linePen, pts.data(), static_cast<INT>(pts.size()));
        g.SetSmoothingMode(oldMode);
    };

    plotCurve(series, colors.line, colors.fill, colors.fillAlpha, dashed);
    if (overlaySeries && !overlaySeries->empty()) {
        plotCurve(*overlaySeries, colors.kernelLine, colors.kernelFill, colors.kernelAlpha, false);
    }

    g.SetClip(&prevClip, Gdiplus::CombineModeReplace);

    Gdiplus::Pen borderPen(ToGpColor(colors.border, 255), 1.0f);
    g.DrawRectangle(&borderPen, rect);
}

double ComputeNiceMaxScale(double peakVal, double minFloor) {
    double v = std::max(peakVal, minFloor);
    // Win10 steps: 100 KB, 500 KB, 1 MB, 5 MB, 10 MB, 50 MB, 100 MB, 500 MB, 1 GB, 10 GB...
    static const double kSteps[] = {
        0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0,
        250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0, 50000.0, 100000.0
    };
    for (double s : kSteps) {
        if (s >= v * 1.15) return s;
    }
    return v * 1.2;
}

double SeriesMax(const std::vector<double>& a, const std::vector<double>* b = nullptr) {
    double m = 0.0;
    for (double x : a) m = std::max(m, x);
    if (b) {
        for (double x : *b) m = std::max(m, x);
    }
    return m;
}

void DrawStatBlock(HDC hdc, HWND hwnd, int labelPt, int valPt, COLORREF crLabel, COLORREF crVal,
                   int x, int y, int w, int labelH, int valH,
                   const wchar_t* label, const std::wstring& val) {
    auto& tm = ThemeManager::Instance();
    RECT rcL{ x, y, x + w, y + labelH };
    tm.DrawModernText(hdc, hwnd, labelPt, false, label, rcL,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, crLabel);

    RECT rcV{ x, y + labelH, x + w, y + labelH + valH };
    tm.DrawModernText(hdc, hwnd, valPt, false, val.c_str(), rcV,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, crVal);
}

void DrawKeyValueRow(HDC hdc, HWND hwnd, int fontPt, COLORREF crKey, COLORREF crVal,
                     int x, int y, int keyW, int valW, int rowH,
                     const wchar_t* key, const std::wstring& val) {
    auto& tm = ThemeManager::Instance();
    RECT rcK{ x, y, x + keyW, y + rowH };
    tm.DrawModernText(hdc, hwnd, fontPt, false, key, rcK,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, crKey);

    RECT rcV{ x + keyW, y, x + keyW + valW, y + rowH };
    tm.DrawModernText(hdc, hwnd, fontPt, false, val.c_str(), rcV,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, crVal);
}

int MeasureMaxTextW(HDC hdc, HWND hwnd, int fontPt, std::initializer_list<const wchar_t*> texts) {
    auto& tm = ThemeManager::Instance();
    int best = 0;
    for (const wchar_t* t : texts) {
        if (!t || !t[0]) continue;
        best = (std::max)(best, tm.MeasureTextWidth(hdc, hwnd, fontPt, false, t, -1));
    }
    return best;
}

void ComputeCompactKvRegion(HDC hdc, HWND hwnd, int rx, int rw, int leftUsedW,
                            std::initializer_list<const wchar_t*> keys,
                            int& outKx, int& outKw, int& outVw) {
    int maxKeyW = MeasureMaxTextW(hdc, hwnd, 10, keys);
    int leftCapW = rw * 52 / 100;
    int clampedLeftW = (std::min)(leftUsedW, leftCapW);
    outKx = rx + clampedLeftW + DpiScale(hwnd, 28);
    int availW = (std::max)(DpiScale(hwnd, 120), (rx + rw) - outKx);
    int desiredKw = maxKeyW + DpiScale(hwnd, 20);
    outKw = std::clamp(desiredKw, DpiScale(hwnd, 56), availW * 48 / 100);
    outVw = (std::max)(DpiScale(hwnd, 60), availW - outKw);
}

std::wstring FormatMemGB(uint64_t bytes) {
    wchar_t buf[64];
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    swprintf_s(buf, L"%.1f GB", gb);
    return buf;
}

std::wstring FormatMemPairGB(uint64_t usedBytes, uint64_t totalBytes) {
    wchar_t buf[64];
    double uGb = static_cast<double>(usedBytes) / (1024.0 * 1024.0 * 1024.0);
    double tGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
    swprintf_s(buf, L"%.1f/%.1f GB", uGb, tGb);
    return buf;
}

std::wstring FormatCacheSize(uint64_t bytes) {
    if (bytes == 0) return L"-";
    wchar_t buf[64];
    if (bytes >= 1024ULL * 1024ULL) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%llu KB", bytes / 1024ULL);
    }
    return buf;
}

std::wstring FormatScaleNetMbps(double mbps) {
    wchar_t buf[64];
    if (mbps >= 1000.0) swprintf_s(buf, L"%.1f Gbps", mbps / 1000.0);
    else if (mbps >= 1.0) swprintf_s(buf, L"%.0f Mbps", mbps);
    else swprintf_s(buf, L"%.0f Kbps", mbps * 1000.0);
    return buf;
}

std::wstring FormatScaleDiskMBs(double mbs) {
    wchar_t buf[64];
    if (mbs >= 1024.0) swprintf_s(buf, L"%.1f GB/s", mbs / 1024.0);
    else if (mbs >= 1.0) swprintf_s(buf, L"%.0f MB/s", mbs);
    else swprintf_s(buf, L"%.0f KB/s", mbs * 1024.0);
    return buf;
}

} // namespace

void TabPerformance::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!g_perfClassRegistered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPerfWndClass;
        RegisterClassExW(&wc);
        g_perfClassRegistered = true;
    }

    hwnd_ = CreateWindowExW(
        0, kPerfWndClass, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        parent, nullptr, hInst, this);
}

void TabPerformance::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TabPerformance::Resize(const RECT& rc) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ClampScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabPerformance::Show(bool visible) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void TabPerformance::ApplyTheme() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabPerformance::SetSummaryMode(PerfSummaryMode mode, bool notifyParent) {
    if (summaryMode_ == mode) return;
    summaryMode_ = mode;
    if (settings_) {
        settings_->summaryView = (mode != PerfSummaryMode::Normal);
    }
    hoveredNav_ = -1;
    navScrollY_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (notifyParent && onSummaryModeChanged) {
        onSummaryModeChanged(mode);
    }
}

int TabPerformance::GetSidebarPreferredWidth() const {
    if (!hwnd_) return 256;
    HDC hdc = GetDC(hwnd_);
    if (!hdc) return DpiScale(hwnd_, 270);

    ThemeManager& tm = ThemeManager::Instance();
    int maxTextW = 0;
    const int titleCap = DpiScale(hwnd_, 180);
    const int subCap = DpiScale(hwnd_, 175);

    const wchar_t* baselines[] = {
        L"S: 999 Kbps  R: 999 Kbps",
        L"S: 99.9 Mbps  R: 99.9 Mbps",
        L"99.9/99.9 GB (100%)",
        L"100% 5.99 GHz",
        L"100% (99 \x00B0C)"
    };
    for (const wchar_t* b : baselines) {
        maxTextW = (std::max)(maxTextW, (std::min)(subCap, tm.MeasureTextWidth(hdc, hwnd_, 9, false, b, -1)));
    }

    if (perf_ && settings_) {
        const Strings& s = GetStrings(settings_->langMode);
        auto considerTitle = [&](const wchar_t* t) {
            if (t && t[0]) {
                maxTextW = (std::max)(maxTextW, (std::min)(titleCap, tm.MeasureTextWidth(hdc, hwnd_, 12, false, t, -1)));
            }
        };
        auto considerSub = [&](const wchar_t* t) {
            if (t && t[0]) {
                maxTextW = (std::max)(maxTextW, (std::min)(subCap, tm.MeasureTextWidth(hdc, hwnd_, 9, false, t, -1)));
            }
        };

        considerTitle(s.perfCpu);
        considerTitle(s.perfMemory);

        wchar_t buf[128];
        const auto& c = perf_->Cpu();
        swprintf_s(buf, L"%.0f%% %.2f GHz", c.utilization, c.currentSpeedGhz);
        considerSub(buf);

        const auto& m = perf_->Memory();
        double uGb = static_cast<double>(m.inUseBytes) / (1024.0 * 1024.0 * 1024.0);
        double tGb = static_cast<double>(m.totalBytes) / (1024.0 * 1024.0 * 1024.0);
        swprintf_s(buf, L"%.1f/%.1f GB (%.0f%%)", uGb, tGb, m.utilization);
        considerSub(buf);

        for (const auto& d : perf_->Disks()) {
            considerTitle(d.title.c_str());
            considerSub(d.type.c_str());
        }
        for (const auto& n : perf_->Networks()) {
            considerTitle(n.title.c_str());
            considerSub(n.connType.c_str());
            swprintf_s(buf, L"S: %s  R: %s",
                       FormatNetSpeedAuto(n.sendBytesPerSec).c_str(),
                       FormatNetSpeedAuto(n.recvBytesPerSec).c_str());
            considerSub(buf);
        }
        for (const auto& g : perf_->Gpus()) {
            considerTitle(g.title.c_str());
            considerSub(g.name.c_str());
        }
    }

    ReleaseDC(hwnd_, hdc);

    int overhead = DpiScale(hwnd_, 6) * 2 + DpiScale(hwnd_, 8) + DpiScale(hwnd_, 72) +
                   DpiScale(hwnd_, 10) + DpiScale(hwnd_, 12);
    return std::clamp(overhead + maxTextW, DpiScale(hwnd_, 248), DpiScale(hwnd_, 300));
}

int TabPerformance::GetSidebarPreferredHeight() const {
    if (!hwnd_) return 420;
    int cardPad = DpiScale(hwnd_, 6);
    int cardH = DpiScale(hwnd_, 70);
    int gap = DpiScale(hwnd_, 2);
    int count = (std::max)(1, static_cast<int>(navItems_.size()));
    return cardPad + count * (cardH + gap) + cardPad + DpiScale(hwnd_, 2);
}

RECT TabPerformance::GetRightGraphScreenRect() const {
    RECT out{};
    if (!hwnd_) return out;
    RECT wrc{};
    GetWindowRect(hwnd_, &wrc);
    int rLeft = rightPaneRect_.left;
    int rTop = rightPaneRect_.top;
    int rw = rightPaneRect_.right - rightPaneRect_.left;
    int gh = (graphAreaRect_.bottom > rTop) ? (graphAreaRect_.bottom - rTop + DpiScale(hwnd_, 16)) : 0;
    if (rw < DpiScale(hwnd_, 420)) rw = DpiScale(hwnd_, 620);
    if (gh < DpiScale(hwnd_, 280)) gh = DpiScale(hwnd_, 340);
    POINT pt{ rLeft, rTop };
    ClientToScreen(hwnd_, &pt);
    out = { pt.x, pt.y, pt.x + rw, pt.y + gh };
    return out;
}

void TabPerformance::UpdateData(const PerfMonitor& perf) {
    perf_ = &perf;
    ++scrollTick_;
    RebuildNavItems();
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabPerformance::RebuildNavItems() {
    navItems_.clear();
    navItems_.push_back({ NavKind::Cpu, 0, {} });
    navItems_.push_back({ NavKind::Memory, 0, {} });
    if (perf_) {
        for (size_t i = 0; i < perf_->Disks().size(); ++i) {
            navItems_.push_back({ NavKind::Disk, static_cast<int>(i), {} });
        }
        for (size_t i = 0; i < perf_->Networks().size(); ++i) {
            navItems_.push_back({ NavKind::Network, static_cast<int>(i), {} });
        }
        for (size_t i = 0; i < perf_->Gpus().size(); ++i) {
            navItems_.push_back({ NavKind::Gpu, static_cast<int>(i), {} });
        }
    }
    if (settings_->perfSelectedItem < 0 || settings_->perfSelectedItem >= static_cast<int>(navItems_.size())) {
        settings_->perfSelectedItem = 0;
    }
}

void TabPerformance::ClampScroll() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int h = rc.bottom - rc.top;
    int maxScroll = std::max(0, navTotalHeight_ - h);
    navScrollY_ = std::clamp(navScrollY_, 0, maxScroll);
}

LRESULT CALLBACK TabPerformance::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabPerformance* self = reinterpret_cast<TabPerformance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TabPerformance*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabPerformance::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC memDc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDc, bmp));
            Paint(memDc, rc);
            BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
            SelectObject(memDc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(memDc);
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        if (lbtnDown_ && (wParam & MK_LBUTTON) && summaryMode_ != PerfSummaryMode::Normal) {
            int dx = std::abs(pt.x - dragStartPt_.x);
            int dy = std::abs(pt.y - dragStartPt_.y);
            if (dx > DpiScale(hwnd_, 5) || dy > DpiScale(hwnd_, 5)) {
                lbtnDown_ = false;
                ReleaseCapture();
                HWND hostWnd = GetParent(hwnd_);
                if (!hostWnd) hostWnd = hParent_;
                SendMessageW(hostWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
        }
        int newHover = -1;
        if (summaryMode_ != PerfSummaryMode::GraphOnly && PtInRect(&leftPaneRect_, pt)) {
            for (int i = 0; i < static_cast<int>(navItems_.size()); ++i) {
                if (PtInRect(&navItems_[i].cardRect, pt)) {
                    newHover = i;
                    break;
                }
            }
        }
        if (newHover != hoveredNav_) {
            hoveredNav_ = newHover;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd_, &leftPaneRect_, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoveredNav_ != -1) {
            hoveredNav_ = -1;
            InvalidateRect(hwnd_, &leftPaneRect_, FALSE);
        }
        return 0;
    case WM_MOUSEHWHEEL:
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int absDelta = std::abs(delta);
        ULONGLONG now = GetTickCount64();
        if (now - lastWheelTick_ > 180) {
            wheelLockedAxis_ = 0;
            wheelAccumV_ = 0;
            wheelAccumH_ = 0;
            navWheelRemainder_ = 0;
        }
        lastWheelTick_ = now;
        bool isHorz = (msg == WM_MOUSEHWHEEL) || ((LOWORD(wParam) & MK_SHIFT) != 0);
        if (isHorz) {
            wheelAccumH_ += absDelta;
            if (wheelLockedAxis_ == 0 && wheelAccumH_ >= 12 && wheelAccumH_ >= wheelAccumV_ * 3 / 2) {
                wheelLockedAxis_ = 2;
            }
            return 0;
        }
        wheelAccumV_ += absDelta;
        if (wheelLockedAxis_ == 0 && wheelAccumV_ >= 12 && wheelAccumV_ >= wheelAccumH_ * 3 / 2) {
            wheelLockedAxis_ = 1;
        }
        if (wheelLockedAxis_ == 2 || (wheelLockedAxis_ == 0 && wheelAccumH_ > wheelAccumV_)) {
            return 0;
        }
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        ScreenToClient(hwnd_, &pt);
        if (summaryMode_ != PerfSummaryMode::GraphOnly && PtInRect(&leftPaneRect_, pt)) {
            int stepPx = DpiScale(hwnd_, 42);
            int totalScaled = delta * stepPx + navWheelRemainder_;
            int movePx = totalScaled / WHEEL_DELTA;
            navWheelRemainder_ = totalScaled % WHEEL_DELTA;
            if (movePx != 0) {
                navScrollY_ -= movePx;
                ClampScroll();
                InvalidateRect(hwnd_, &leftPaneRect_, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        lbtnDown_ = true;
        dragStartPt_ = pt;
        HandleLeftClick(pt);
        return 0;
    }
    case WM_LBUTTONUP:
        lbtnDown_ = false;
        return 0;
    case WM_CAPTURECHANGED:
        lbtnDown_ = false;
        return 0;
    case WM_LBUTTONDBLCLK: {
        lbtnDown_ = false;
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        HandleDoubleClick(pt);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        HandleLeftClick(pt);
        ClientToScreen(hwnd_, &pt);
        HandleContextMenu(pt);
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        HandleContextMenu(pt);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && summaryMode_ != PerfSummaryMode::Normal) {
            SetSummaryMode(PerfSummaryMode::Normal, true);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void TabPerformance::HandleLeftClick(POINT pt) {
    if (summaryMode_ != PerfSummaryMode::GraphOnly && PtInRect(&leftPaneRect_, pt)) {
        for (int i = 0; i < static_cast<int>(navItems_.size()); ++i) {
            if (PtInRect(&navItems_[i].cardRect, pt)) {
                if (settings_->perfSelectedItem != i) {
                    settings_->perfSelectedItem = i;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }
    }
}

void TabPerformance::HandleDoubleClick(POINT pt) {
    if (summaryMode_ == PerfSummaryMode::Normal) {
        if (PtInRect(&leftPaneRect_, pt)) {
            HandleLeftClick(pt);
            SetSummaryMode(PerfSummaryMode::SidebarOnly, true);
        } else if (PtInRect(&rightPaneRect_, pt)) {
            SetSummaryMode(PerfSummaryMode::GraphOnly, true);
        }
    } else {
        if (summaryMode_ == PerfSummaryMode::SidebarOnly) {
            HandleLeftClick(pt);
        }
        SetSummaryMode(PerfSummaryMode::Normal, true);
    }
}

void TabPerformance::HandleContextMenu(POINT screenPt) {
    if (!perf_ || navItems_.empty()) return;
    if (screenPt.x == -1 && screenPt.y == -1) {
        RECT rc{};
        if (rightPaneRect_.right > rightPaneRect_.left && rightPaneRect_.bottom > rightPaneRect_.top) {
            POINT pt{ (rightPaneRect_.left + rightPaneRect_.right) / 2,
                      (rightPaneRect_.top + rightPaneRect_.bottom) / 2 };
            ClientToScreen(hwnd_, &pt);
            screenPt = pt;
        } else {
            GetWindowRect(hwnd_, &rc);
            screenPt = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
        }
    }
    const Strings& s = GetStrings(settings_->langMode);
    const NavItem& sel = navItems_[settings_->perfSelectedItem];

    HMENU hMenu = CreatePopupMenu();
    if (sel.kind == NavKind::Cpu) {
        HMENU hSub = CreatePopupMenu();
        AppendMenuW(hSub, MF_STRING | (!settings_->cpuLogicalView ? MF_CHECKED : 0),
                    IDM_CTX_PERF_OVERALL, s.perfOverallUtil);
        AppendMenuW(hSub, MF_STRING | (settings_->cpuLogicalView ? MF_CHECKED : 0),
                    IDM_CTX_PERF_LOGICAL, s.perfLogicalView);
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSub), s.perfChangeGraphTo);
        AppendMenuW(hMenu, MF_STRING | (settings_->showKernelTimes ? MF_CHECKED : 0),
                    IDM_CTX_PERF_KERNEL_TIMES, s.perfShowKernelTimes);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    HMENU hSpan = CreatePopupMenu();
    AppendMenuW(hSpan, MF_STRING | (settings_->updateSpeed == UpdateSpeed::High ? MF_CHECKED : 0),
                IDM_VIEW_SPEED_HIGH, s.menuSpeedHigh);
    AppendMenuW(hSpan, MF_STRING | (settings_->updateSpeed == UpdateSpeed::Normal ? MF_CHECKED : 0),
                IDM_VIEW_SPEED_NORMAL, s.menuSpeedNormal);
    AppendMenuW(hSpan, MF_STRING | (settings_->updateSpeed == UpdateSpeed::Low ? MF_CHECKED : 0),
                IDM_VIEW_SPEED_LOW, s.menuSpeedLow);
    AppendMenuW(hSpan, MF_STRING | (settings_->updateSpeed == UpdateSpeed::Paused ? MF_CHECKED : 0),
                IDM_VIEW_SPEED_PAUSED, s.menuSpeedPaused);
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSpan), s.menuUpdateSpeed);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING | (summaryMode_ != PerfSummaryMode::Normal ? MF_CHECKED : 0),
                IDM_CTX_PERF_SUMMARY_VIEW, s.perfGraphSummaryView);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_PERF_COPY, s.perfCopy);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_CTX_PERF_OVERALL:
        settings_->cpuLogicalView = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case IDM_CTX_PERF_LOGICAL:
        settings_->cpuLogicalView = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case IDM_CTX_PERF_KERNEL_TIMES:
        settings_->showKernelTimes = !settings_->showKernelTimes;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case IDM_VIEW_SPEED_HIGH:
        if (onSpeedChanged) onSpeedChanged(UpdateSpeed::High);
        break;
    case IDM_VIEW_SPEED_NORMAL:
        if (onSpeedChanged) onSpeedChanged(UpdateSpeed::Normal);
        break;
    case IDM_VIEW_SPEED_LOW:
        if (onSpeedChanged) onSpeedChanged(UpdateSpeed::Low);
        break;
    case IDM_VIEW_SPEED_PAUSED:
        if (onSpeedChanged) onSpeedChanged(UpdateSpeed::Paused);
        break;
    case IDM_CTX_PERF_SUMMARY_VIEW:
        SetSummaryMode(summaryMode_ == PerfSummaryMode::Normal ? PerfSummaryMode::GraphOnly : PerfSummaryMode::Normal, true);
        break;
    case IDM_CTX_PERF_COPY:
        CopySelectedSummary();
        break;
    }
}

void TabPerformance::CopySelectedSummary() {
    if (!perf_ || navItems_.empty()) return;
    const Strings& s = GetStrings(settings_->langMode);
    const NavItem& sel = navItems_[settings_->perfSelectedItem];
    std::wstring text;

    if (sel.kind == NavKind::Cpu) {
        const auto& c = perf_->Cpu();
        text = c.name + L"\r\n\r\n" +
               s.perfBaseSpeed + std::wstring(L":\t") + std::to_wstring(c.baseSpeedGhz).substr(0, 4) + L" GHz\r\n" +
               s.perfSockets + L":\t" + std::to_wstring(c.sockets) + L"\r\n" +
               s.perfCores + L":\t" + std::to_wstring(c.cores) + L"\r\n" +
               s.perfLogicalProcs + L":\t" + std::to_wstring(c.logicalProcessors) + L"\r\n" +
               s.perfVirtualization + L":\t" + (c.virtualization ? s.perfEnabled : s.perfDisabled) + L"\r\n" +
               s.perfUtilization + L":\t" + FormatPercent(c.utilization, 0) + L"\r\n" +
               s.perfProcesses + L":\t" + std::to_wstring(c.processes) + L"\r\n" +
               s.perfThreads + L":\t" + std::to_wstring(c.threads) + L"\r\n" +
               s.perfHandles + L":\t" + std::to_wstring(c.handles) + L"\r\n" +
               s.perfUptime + L":\t" + FormatUptime(c.uptimeMs) + L"\r\n";
    } else if (sel.kind == NavKind::Memory) {
        const auto& m = perf_->Memory();
        text = std::wstring(s.perfMemory) + L"\r\n\r\n" +
               s.perfInUse + L":\t" + FormatMemGB(m.inUseBytes) + L"\r\n" +
               s.perfAvailable + L":\t" + FormatMemGB(m.availableBytes) + L"\r\n" +
               s.perfCommitted + L":\t" + FormatMemGB(m.committedBytes) + L" / " + FormatMemGB(m.commitLimitBytes) + L"\r\n" +
               s.perfCached + L":\t" + FormatMemGB(m.cachedBytes) + L"\r\n" +
               s.perfPagedPool + L":\t" + FormatBytesAuto(m.pagedPoolBytes) + L"\r\n" +
               s.perfNonPagedPool + L":\t" + FormatBytesAuto(m.nonPagedPoolBytes) + L"\r\n";
    } else if (sel.kind == NavKind::Disk && sel.index < static_cast<int>(perf_->Disks().size())) {
        const auto& d = perf_->Disks()[sel.index];
        text = d.title + L" - " + d.model + L"\r\n\r\n" +
               s.perfActiveTime + L":\t" + FormatPercent(d.activeTimePct, 0) + L"\r\n" +
               s.perfReadSpeed + L":\t" + FormatDiskSpeed(d.readBytesPerSec) + L"\r\n" +
               s.perfWriteSpeed + L":\t" + FormatDiskSpeed(d.writeBytesPerSec) + L"\r\n" +
               s.perfCapacity + L":\t" + FormatMemGB(d.capacityBytes) + L"\r\n";
    } else if (sel.kind == NavKind::Network && sel.index < static_cast<int>(perf_->Networks().size())) {
        const auto& n = perf_->Networks()[sel.index];
        text = n.title + L" - " + n.adapterName + L"\r\n\r\n" +
               s.perfSend + L":\t" + FormatNetSpeedAuto(n.sendBytesPerSec) + L"\r\n" +
               s.perfReceive + L":\t" + FormatNetSpeedAuto(n.recvBytesPerSec) + L"\r\n" +
               s.perfIpv4 + L":\t" + n.ipv4 + L"\r\n" +
               s.perfIpv6 + L":\t" + n.ipv6 + L"\r\n";
    } else if (sel.kind == NavKind::Gpu && sel.index < static_cast<int>(perf_->Gpus().size())) {
        const auto& g = perf_->Gpus()[sel.index];
        text = g.title + L" - " + g.name + L"\r\n\r\n" +
               s.perfUtilization + L":\t" + FormatPercent(g.utilization, 0) + L"\r\n" +
               s.perfDedGpuMem + L":\t" + FormatMemGB(g.dedicatedUsed) + L" / " + FormatMemGB(g.dedicatedTotal) + L"\r\n" +
               s.perfSharedGpuMem + L":\t" + FormatMemGB(g.sharedUsed) + L" / " + FormatMemGB(g.sharedTotal) + L"\r\n";
    }

    if (text.empty()) return;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void* p = GlobalLock(hMem);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }
}

void TabPerformance::Paint(HDC hdc, const RECT& clientRc) {
    ThemeManager& tm = ThemeManager::Instance();
    const ThemePalette& pal = tm.Palette();
    const Strings& s = GetStrings(settings_->langMode);

    HBRUSH bgBr = CreateSolidBrush(pal.surfaceBg);
    FillRect(hdc, &clientRc, bgBr);
    DeleteObject(bgBr);

    if (!perf_) return;

    const wchar_t* spanLabel = s.perf60Sec;
    const wchar_t* cpuLogLabel = s.perfUtilization60s;
    UpdateSpeed effSpd = perf_->GetEffectiveSpeed();
    if (effSpd == UpdateSpeed::High) {
        spanLabel = s.perf30Sec;
        cpuLogLabel = s.perfUtilization30s;
    } else if (effSpd == UpdateSpeed::Low) {
        spanLabel = s.perf4Min;
        cpuLogLabel = s.perfUtilization4m;
    }

    int totalW = clientRc.right - clientRc.left;
    int totalH = clientRc.bottom - clientRc.top;
    int sidebarW = 0;
    if (summaryMode_ == PerfSummaryMode::SidebarOnly) {
        sidebarW = totalW;
        leftPaneRect_ = { 0, 0, totalW, totalH };
        rightPaneRect_ = { 0, 0, 0, 0 };
    } else if (summaryMode_ == PerfSummaryMode::GraphOnly) {
        sidebarW = 0;
        leftPaneRect_ = { 0, 0, 0, 0 };
        rightPaneRect_ = { 0, 0, totalW, totalH };
    } else {
        sidebarW = std::clamp(GetSidebarPreferredWidth(), DpiScale(hwnd_, 220), (std::max)(DpiScale(hwnd_, 220), totalW * 38 / 100));
        leftPaneRect_ = { 0, 0, sidebarW, totalH };
        rightPaneRect_ = { sidebarW + 1, 0, totalW, totalH };
    }

    SetBkMode(hdc, TRANSPARENT);
    Gdiplus::Graphics gp(hdc);

    // 1. Draw left navigation sidebar
    if (summaryMode_ != PerfSummaryMode::GraphOnly && sidebarW > 0) {
        int cardPad = DpiScale(hwnd_, 6);
        int cardH = DpiScale(hwnd_, 70);
        int cardW = sidebarW - cardPad * 2;
        int sparkW = DpiScale(hwnd_, 72);
        int sparkH = DpiScale(hwnd_, 48);
        int padIn = DpiScale(hwnd_, 8);

        navTotalHeight_ = cardPad + static_cast<int>(navItems_.size()) * (cardH + DpiScale(hwnd_, 2)) + cardPad;
        ClampScroll();

        HRGN clipRgn = CreateRectRgnIndirect(&leftPaneRect_);
        SelectClipRgn(hdc, clipRgn);
        gp.SetClip(Gdiplus::Rect(leftPaneRect_.left, leftPaneRect_.top, sidebarW, totalH));

        int y = cardPad - navScrollY_;
        for (int i = 0; i < static_cast<int>(navItems_.size()); ++i) {
            NavItem& item = navItems_[i];
            item.cardRect = { cardPad, y, cardPad + cardW, y + cardH };

            if (item.cardRect.bottom >= 0 && item.cardRect.top <= totalH) {
                bool selected = (i == settings_->perfSelectedItem);
                bool hovered = (i == hoveredNav_);
                if (selected) {
                    HBRUSH brSel = CreateSolidBrush(pal.rowSelectedBg);
                    FillRect(hdc, &item.cardRect, brSel);
                    DeleteObject(brSel);
                    RECT accRc{ item.cardRect.left, item.cardRect.top, item.cardRect.left + DpiScale(hwnd_, 3), item.cardRect.bottom };
                    HBRUSH brAcc = CreateSolidBrush(pal.accentColor);
                    FillRect(hdc, &accRc, brAcc);
                    DeleteObject(brAcc);
                } else if (hovered) {
                    HBRUSH brHov = CreateSolidBrush(pal.rowHoverBg);
                    FillRect(hdc, &item.cardRect, brHov);
                    DeleteObject(brHov);
                }

                // Miniature sparkline chart
                Gdiplus::RectF sparkRc(
                    static_cast<float>(item.cardRect.left + padIn),
                    static_cast<float>(item.cardRect.top + (cardH - sparkH) / 2),
                    static_cast<float>(sparkW),
                    static_cast<float>(sparkH));
                HBRUSH brSparkBg = CreateSolidBrush(pal.windowBg);
                RECT rSpark{ static_cast<LONG>(sparkRc.X), static_cast<LONG>(sparkRc.Y),
                             static_cast<LONG>(sparkRc.GetRight()), static_cast<LONG>(sparkRc.GetBottom()) };
                FillRect(hdc, &rSpark, brSparkBg);
                DeleteObject(brSparkBg);

                std::wstring cardTitle;
                std::wstring cardSub1;
                std::wstring cardSub2;

                if (item.kind == NavKind::Cpu) {
                    const auto& c = perf_->Cpu();
                    DrawChartSeries(gp, sparkRc, c.history, 100.0, pal.chartCpu, scrollTick_, false);
                    cardTitle = s.perfCpu;
                    wchar_t buf[64];
                    swprintf_s(buf, L"%.0f%% %.2f GHz", c.utilization, c.currentSpeedGhz);
                    cardSub1 = buf;
                } else if (item.kind == NavKind::Memory) {
                    const auto& m = perf_->Memory();
                    DrawChartSeries(gp, sparkRc, m.history, 100.0, pal.chartMem, scrollTick_, false);
                    cardTitle = s.perfMemory;
                    wchar_t buf[64];
                    double uGb = static_cast<double>(m.inUseBytes) / (1024.0 * 1024.0 * 1024.0);
                    double tGb = static_cast<double>(m.totalBytes) / (1024.0 * 1024.0 * 1024.0);
                    swprintf_s(buf, L"%.1f/%.1f GB (%.0f%%)", uGb, tGb, m.utilization);
                    cardSub1 = buf;
                } else if (item.kind == NavKind::Disk && item.index < static_cast<int>(perf_->Disks().size())) {
                    const auto& d = perf_->Disks()[item.index];
                    DrawChartSeries(gp, sparkRc, d.activeHistory, 100.0, pal.chartDisk, scrollTick_, false);
                    cardTitle = d.title;
                    cardSub1 = d.type;
                    cardSub2 = FormatPercent(d.activeTimePct, 0);
                } else if (item.kind == NavKind::Network && item.index < static_cast<int>(perf_->Networks().size())) {
                    const auto& n = perf_->Networks()[item.index];
                    double mx = ComputeNiceMaxScale(SeriesMax(n.recvHistory, &n.sendHistory), 0.1);
                    DrawChartSeries(gp, sparkRc, n.recvHistory, mx, pal.chartNet, scrollTick_, false, false);
                    DrawChartSeries(gp, sparkRc, n.sendHistory, mx, pal.chartNet, scrollTick_, false, true);
                    cardTitle = n.title;
                    cardSub1 = n.connType;
                    wchar_t buf[96];
                    swprintf_s(buf, L"S: %s  R: %s",
                               FormatNetSpeedAuto(n.sendBytesPerSec).c_str(),
                               FormatNetSpeedAuto(n.recvBytesPerSec).c_str());
                    cardSub2 = buf;
                } else if (item.kind == NavKind::Gpu && item.index < static_cast<int>(perf_->Gpus().size())) {
                    const auto& g = perf_->Gpus()[item.index];
                    DrawChartSeries(gp, sparkRc, g.histOverall, 100.0, pal.chartGpu, scrollTick_, false);
                    cardTitle = g.title;
                    cardSub1 = g.name;
                    wchar_t buf[64];
                    if (g.hasTemperature) {
                        swprintf_s(buf, L"%.0f%% (%.0f \x00B0C)", g.utilization, g.temperatureC);
                    } else {
                        swprintf_s(buf, L"%.0f%%", g.utilization);
                    }
                    cardSub2 = buf;
                }

                int textX = static_cast<int>(sparkRc.GetRight()) + DpiScale(hwnd_, 10);
                int textW = item.cardRect.right - textX - DpiScale(hwnd_, 6);
                RECT rcT{ textX, item.cardRect.top + DpiScale(hwnd_, 6), textX + textW, item.cardRect.top + DpiScale(hwnd_, 28) };
                tm.DrawModernText(hdc, hwnd_, 12, false, cardTitle.c_str(), rcT,
                                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textPrimary);

                RECT rcS1{ textX, item.cardRect.top + DpiScale(hwnd_, 28), textX + textW, item.cardRect.top + DpiScale(hwnd_, 47) };
                tm.DrawModernText(hdc, hwnd_, 9, false, cardSub1.c_str(), rcS1,
                                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textSecondary);

                if (!cardSub2.empty()) {
                    RECT rcS2{ textX, item.cardRect.top + DpiScale(hwnd_, 47), textX + textW, item.cardRect.top + DpiScale(hwnd_, 66) };
                    tm.DrawModernText(hdc, hwnd_, 9, false, cardSub2.c_str(), rcS2,
                                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textSecondary);
                }
            }
            y += cardH + DpiScale(hwnd_, 2);
        }

        SelectClipRgn(hdc, nullptr);
        gp.ResetClip();
        DeleteObject(clipRgn);

        if (summaryMode_ == PerfSummaryMode::SidebarOnly) {
            HPEN penOuter = CreatePen(PS_SOLID, 1, pal.borderColor);
            HPEN oldOuter = static_cast<HPEN>(SelectObject(hdc, penOuter));
            HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
            Rectangle(hdc, 0, 0, totalW, totalH);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldOuter);
            DeleteObject(penOuter);
            return;
        }

        // Vertical splitter between sidebar and detail panel
        HPEN penDiv = CreatePen(PS_SOLID, 1, pal.splitterColor);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, penDiv));
        MoveToEx(hdc, sidebarW, 0, nullptr);
        LineTo(hdc, sidebarW, totalH);
        SelectObject(hdc, oldPen);
        DeleteObject(penDiv);
    }

    // 2. Draw right detail panel
    if (navItems_.empty() || summaryMode_ == PerfSummaryMode::SidebarOnly) return;
    const NavItem& sel = navItems_[settings_->perfSelectedItem];
    bool showStatsBlock = (summaryMode_ == PerfSummaryMode::Normal);
    int pad = DpiScale(hwnd_, summaryMode_ == PerfSummaryMode::GraphOnly ? 14 : 20);
    int rx = rightPaneRect_.left + pad;
    int ry = rightPaneRect_.top + DpiScale(hwnd_, summaryMode_ == PerfSummaryMode::GraphOnly ? 10 : 14);
    int rw = std::max<int>(100, (rightPaneRect_.right - rightPaneRect_.left) - pad * 2);
    int rh = std::max<int>(100, (rightPaneRect_.bottom - rightPaneRect_.top) - ry - DpiScale(hwnd_, 12));

    std::wstring mainTitle;
    std::wstring hwModel;
    if (sel.kind == NavKind::Cpu) {
        mainTitle = s.perfCpu;
        hwModel = perf_->Cpu().name;
    } else if (sel.kind == NavKind::Memory) {
        mainTitle = s.perfMemory;
        hwModel = FormatMemGB(perf_->Memory().installedBytes > 0 ? perf_->Memory().installedBytes : perf_->Memory().totalBytes);
    } else if (sel.kind == NavKind::Disk && sel.index < static_cast<int>(perf_->Disks().size())) {
        mainTitle = perf_->Disks()[sel.index].title;
        hwModel = perf_->Disks()[sel.index].model;
    } else if (sel.kind == NavKind::Network && sel.index < static_cast<int>(perf_->Networks().size())) {
        mainTitle = perf_->Networks()[sel.index].title;
        hwModel = perf_->Networks()[sel.index].adapterName;
    } else if (sel.kind == NavKind::Gpu && sel.index < static_cast<int>(perf_->Gpus().size())) {
        mainTitle = perf_->Gpus()[sel.index].title;
        hwModel = perf_->Gpus()[sel.index].name;
    }

    // Top Header (shown in both Normal and compact GraphOnly modes)
    int headerH = DpiScale(hwnd_, 40);
    {
        RECT rcMT{ rx, ry, rx + rw * 42 / 100, ry + headerH };
        tm.DrawModernText(hdc, hwnd_, 22, false, mainTitle.c_str(), rcMT,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textPrimary);

        RECT rcHW{ rx + rw * 38 / 100, ry, rx + rw, ry + headerH };
        tm.DrawModernText(hdc, hwnd_, 12, false, hwModel.c_str(), rcHW,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textPrimary);
    }

    int desiredStatsDp = 164;
    int minStatsDp = 140;
    if (sel.kind == NavKind::Cpu) {
        desiredStatsDp = 192;
    } else if (sel.kind == NavKind::Disk || sel.kind == NavKind::Network) {
        desiredStatsDp = 130;
        minStatsDp = 115;
    }
    int statsAreaH = !showStatsBlock
        ? 0
        : std::clamp(DpiScale(hwnd_, desiredStatsDp), DpiScale(hwnd_, minStatsDp), rh * 46 / 100);
    int topOffset = headerH + DpiScale(hwnd_, 8);
    int graphAreaTop = ry + topOffset;
    int graphAreaH = std::max(DpiScale(hwnd_, 80), rh - topOffset - statsAreaH);
    graphAreaRect_ = { rx, graphAreaTop, rx + rw, graphAreaTop + graphAreaH };

    int lblH = DpiScale(hwnd_, 20);
    auto drawTopBottomLabels = [&](int x, int y, int w, int h, const wchar_t* topL, const wchar_t* topR,
                                   const wchar_t* botL, const wchar_t* botR, RECT& outPlot) {
        RECT rTL{ x, y, x + w * 2 / 3, y + lblH };
        tm.DrawModernText(hdc, hwnd_, 9, false, topL, rTL,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textSecondary);
        RECT rTR{ x + w / 2, y, x + w, y + lblH };
        tm.DrawModernText(hdc, hwnd_, 9, false, topR, rTR,
                          DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX, pal.textSecondary);

        int plotTop = y + lblH + DpiScale(hwnd_, 2);
        int plotBot = y + h - lblH - DpiScale(hwnd_, 2);
        if (plotBot < plotTop + 20) plotBot = plotTop + 20;
        outPlot = { x, plotTop, x + w, plotBot };

        RECT rBL{ x, plotBot + DpiScale(hwnd_, 2), x + w / 2, plotBot + DpiScale(hwnd_, 2) + lblH };
        tm.DrawModernText(hdc, hwnd_, 9, false, botL, rBL,
                          DT_LEFT | DT_SINGLELINE | DT_NOPREFIX, pal.textSecondary);
        RECT rBR{ x + w / 2, plotBot + DpiScale(hwnd_, 2), x + w, plotBot + DpiScale(hwnd_, 2) + lblH };
        tm.DrawModernText(hdc, hwnd_, 9, false, botR, rBR,
                          DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX, pal.textSecondary);
    };

    auto measureStatColW = [&](int valPt, std::initializer_list<const wchar_t*> labels,
                               std::initializer_list<const wchar_t*> values) -> int {
        int w = MeasureMaxTextW(hdc, hwnd_, 9, labels);
        w = (std::max)(w, MeasureMaxTextW(hdc, hwnd_, valPt, values));
        return w + DpiScale(hwnd_, 6);
    };

    // Draw main graphs + stats per kind
    if (sel.kind == NavKind::Cpu) {
        const auto& c = perf_->Cpu();
        RECT plotRc{};
        drawTopBottomLabels(rx, graphAreaTop, rw, graphAreaH,
                            settings_->cpuLogicalView ? cpuLogLabel : s.perfUtilization,
                            s.perf100Pct, spanLabel, L"0", plotRc);

        if (settings_->cpuLogicalView && !c.coreHistories.empty()) {
            int n = static_cast<int>(c.coreHistories.size());
            int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
            if (n == 8) cols = 4;
            else if (n == 12) cols = 4;
            else if (n == 16) cols = 4;
            else if (n == 24) cols = 6;
            else if (n == 32) cols = 8;
            int rows = (n + cols - 1) / cols;
            int gap = DpiScale(hwnd_, 6);
            float cellW = static_cast<float>((plotRc.right - plotRc.left) - gap * (cols - 1)) / cols;
            float cellH = static_cast<float>((plotRc.bottom - plotRc.top) - gap * (rows - 1)) / rows;
            for (int idx = 0; idx < n; ++idx) {
                int r = idx / cols;
                int col = idx % cols;
                Gdiplus::RectF rCell(
                    static_cast<float>(plotRc.left) + col * (cellW + gap),
                    static_cast<float>(plotRc.top) + r * (cellH + gap),
                    cellW, cellH);
                DrawChartSeries(gp, rCell, c.coreHistories[idx], 100.0, pal.chartCpu, scrollTick_, true);
            }
        } else {
            Gdiplus::RectF rPlot(static_cast<float>(plotRc.left), static_cast<float>(plotRc.top),
                                 static_cast<float>(plotRc.right - plotRc.left),
                                 static_cast<float>(plotRc.bottom - plotRc.top));
            const std::vector<double>* kHist = settings_->showKernelTimes ? &c.kernelHistory : nullptr;
            DrawChartSeries(gp, rPlot, c.history, 100.0, pal.chartCpu, scrollTick_, true, false, kHist);
        }

        if (showStatsBlock) {
            int sy = graphAreaTop + graphAreaH + DpiScale(hwnd_, 8);
            int lblSt = DpiScale(hwnd_, 19);
            int valSt = DpiScale(hwnd_, 27);
            int colGap = DpiScale(hwnd_, 20);

            std::wstring utilStr = FormatPercent(c.utilization, 0);
            wchar_t spdBuf[32];
            swprintf_s(spdBuf, L"%.2f GHz", c.currentSpeedGhz);
            std::wstring procStr = std::to_wstring(c.processes);
            std::wstring thrStr = std::to_wstring(c.threads);
            std::wstring hndStr = std::to_wstring(c.handles);
            std::wstring upStr = FormatUptime(c.uptimeMs);

            int w10 = measureStatColW(16, { s.perfUtilization }, { L"100%", utilStr.c_str() });
            int w11 = measureStatColW(16, { s.perfSpeed }, { L"9.99 GHz", spdBuf });
            int w20 = measureStatColW(15, { s.perfProcesses }, { L"9999", procStr.c_str() });
            int w21 = measureStatColW(15, { s.perfThreads }, { L"99999", thrStr.c_str() });
            int w22 = measureStatColW(15, { s.perfHandles }, { L"999999", hndStr.c_str() });
            int col0W = (std::max)(w10, w20);
            int upW = measureStatColW(15, { s.perfUptime }, { L"00:00:00:00", upStr.c_str() });
            int row1Used = col0W + colGap + w11;
            int row2Used = col0W + colGap + w21 + colGap + w22;
            int leftUsedW = (std::max)({ row1Used, row2Used, upW });

            DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                          rx, sy, col0W, lblSt, valSt, s.perfUtilization, utilStr);
            DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy, w11, lblSt, valSt, s.perfSpeed, spdBuf);

            int sy2 = sy + lblSt + valSt + DpiScale(hwnd_, 5);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx, sy2, col0W, lblSt, valSt, s.perfProcesses, procStr);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy2, w21, lblSt, valSt, s.perfThreads, thrStr);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap + w21 + colGap, sy2, w22, lblSt, valSt, s.perfHandles, hndStr);

            int sy3 = sy2 + lblSt + valSt + DpiScale(hwnd_, 5);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx, sy3, leftUsedW, lblSt, valSt, s.perfUptime, upStr);

            int kx = 0, kW = 0, vW = 0;
            ComputeCompactKvRegion(hdc, hwnd_, rx, rw, leftUsedW,
                                   { s.perfBaseSpeed, s.perfSockets, s.perfCores, s.perfLogicalProcs,
                                     s.perfVirtualization, s.perfL1Cache, s.perfL2Cache, s.perfL3Cache },
                                   kx, kW, vW);
            int rH = DpiScale(hwnd_, 23);
            wchar_t baseBuf[32];
            swprintf_s(baseBuf, L"%.2f GHz", c.baseSpeedGhz);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 0, kW, vW, rH, s.perfBaseSpeed, baseBuf);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 1, kW, vW, rH, s.perfSockets, std::to_wstring(c.sockets));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 2, kW, vW, rH, s.perfCores, std::to_wstring(c.cores));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 3, kW, vW, rH, s.perfLogicalProcs, std::to_wstring(c.logicalProcessors));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 4, kW, vW, rH, s.perfVirtualization,
                            c.virtualization ? s.perfEnabled : s.perfDisabled);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 5, kW, vW, rH, s.perfL1Cache, FormatCacheSize(c.l1CacheBytes));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 6, kW, vW, rH, s.perfL2Cache, FormatCacheSize(c.l2CacheBytes));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 7, kW, vW, rH, s.perfL3Cache, FormatCacheSize(c.l3CacheBytes));
        }
    } else if (sel.kind == NavKind::Memory) {
        const auto& m = perf_->Memory();
        int compH = DpiScale(hwnd_, 52);
        int mainH = std::max(60, graphAreaH - compH);

        RECT plotRc{};
        std::wstring memTopR = FormatMemGB(m.totalBytes);
        drawTopBottomLabels(rx, graphAreaTop, rw, mainH, s.perfMemUsage, memTopR.c_str(), spanLabel, L"0", plotRc);
        Gdiplus::RectF rPlot(static_cast<float>(plotRc.left), static_cast<float>(plotRc.top),
                             static_cast<float>(plotRc.right - plotRc.left),
                             static_cast<float>(plotRc.bottom - plotRc.top));
        DrawChartSeries(gp, rPlot, m.history, 100.0, pal.chartMem, scrollTick_, true);

        {
            int cy = graphAreaTop + mainH + DpiScale(hwnd_, 4);
            RECT rCL{ rx, cy, rx + rw, cy + lblH };
            tm.DrawModernText(hdc, hwnd_, 9, false, s.perfMemComposition, rCL,
                              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX, pal.textSecondary);

            int barY = cy + lblH + DpiScale(hwnd_, 2);
            int barH = DpiScale(hwnd_, 24);
            double tot = static_cast<double>(std::max<uint64_t>(1, m.totalBytes));
            int wUse = static_cast<int>(rw * (static_cast<double>(m.inUseBytes) / tot));
            int wMod = static_cast<int>(rw * (static_cast<double>(m.modifiedBytes) / tot));
            int wStb = static_cast<int>(rw * (static_cast<double>(m.standbyBytes) / tot));
            wUse = std::clamp(wUse, 0, rw);
            wMod = std::clamp(wMod, 0, rw - wUse);
            wStb = std::clamp(wStb, 0, rw - wUse - wMod);

            RECT rUse{ rx, barY, rx + wUse, barY + barH };
            RECT rMod{ rUse.right, barY, rUse.right + wMod, barY + barH };
            RECT rStb{ rMod.right, barY, rMod.right + wStb, barY + barH };

            HBRUSH b1 = CreateSolidBrush(pal.chartMem.line);
            HBRUSH b2 = CreateSolidBrush(RGB(230, 145, 56));
            HBRUSH b3 = CreateSolidBrush(RGB(170, 125, 215));
            FillRect(hdc, &rUse, b1);
            FillRect(hdc, &rMod, b2);
            FillRect(hdc, &rStb, b3);
            DeleteObject(b1);
            DeleteObject(b2);
            DeleteObject(b3);

            Gdiplus::Pen bp(ToGpColor(pal.chartMem.border, 255), 1.0f);
            gp.DrawRectangle(&bp, static_cast<float>(rx), static_cast<float>(barY),
                             static_cast<float>(rw), static_cast<float>(barH));
        }

        if (showStatsBlock) {

            int sy = graphAreaTop + graphAreaH + DpiScale(hwnd_, 8);
            int lblSt = DpiScale(hwnd_, 19);
            int valSt = DpiScale(hwnd_, 26);
            int colGap = DpiScale(hwnd_, 22);

            std::wstring inUseStr = FormatMemGB(m.inUseBytes);
            std::wstring availStr = FormatMemGB(m.availableBytes);
            std::wstring commStr = FormatMemPairGB(m.committedBytes, m.commitLimitBytes);
            std::wstring cachedStr = FormatMemGB(m.cachedBytes);
            std::wstring pagedStr = FormatBytesAuto(m.pagedPoolBytes);
            std::wstring nonPagedStr = FormatBytesAuto(m.nonPagedPoolBytes);

            int col0W = measureStatColW(15,
                                      { s.perfInUse, s.perfCommitted, s.perfPagedPool },
                                      { L"99.9/99.9 GB", inUseStr.c_str(), commStr.c_str(), pagedStr.c_str() });
            int col1W = measureStatColW(15,
                                      { s.perfAvailable, s.perfCached, s.perfNonPagedPool },
                                      { L"99.9 GB", availStr.c_str(), cachedStr.c_str(), nonPagedStr.c_str() });
            int leftUsedW = col0W + colGap + col1W;

            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx, sy, col0W, lblSt, valSt, s.perfInUse, inUseStr);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy, col1W, lblSt, valSt, s.perfAvailable, availStr);

            int sy2 = sy + lblSt + valSt + DpiScale(hwnd_, 6);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx, sy2, col0W, lblSt, valSt, s.perfCommitted, commStr);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy2, col1W, lblSt, valSt, s.perfCached, cachedStr);

            int sy3 = sy2 + lblSt + valSt + DpiScale(hwnd_, 6);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx, sy3, col0W, lblSt, valSt, s.perfPagedPool, pagedStr);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy3, col1W, lblSt, valSt, s.perfNonPagedPool, nonPagedStr);

            int kx = 0, kW = 0, vW = 0;
            ComputeCompactKvRegion(hdc, hwnd_, rx, rw, leftUsedW,
                                   { s.perfSpeed, s.perfSlotsUsed, s.perfFormFactor, s.perfHwReserved },
                                   kx, kW, vW);
            int rH = DpiScale(hwnd_, 24);
            std::wstring spdStr = m.speedMhz > 0 ? (std::to_wstring(m.speedMhz) + L" MHz") : L"-";
            std::wstring slotStr = m.slotsTotal > 0
                ? (std::to_wstring(m.slotsUsed) + L" / " + std::to_wstring(m.slotsTotal))
                : L"-";
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 0, kW, vW, rH, s.perfSpeed, spdStr);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 1, kW, vW, rH, s.perfSlotsUsed, slotStr);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 2, kW, vW, rH, s.perfFormFactor,
                            m.formFactor.empty() ? L"-" : m.formFactor);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 3, kW, vW, rH, s.perfHwReserved,
                            FormatBytesAuto(m.hardwareReservedBytes));
        }
    } else if (sel.kind == NavKind::Disk && sel.index < static_cast<int>(perf_->Disks().size())) {
        const auto& d = perf_->Disks()[sel.index];
        int topH = graphAreaH * 60 / 100;
        int gap = DpiScale(hwnd_, 8);
        int botH = std::max(45, graphAreaH - topH - gap);

        RECT plot1{};
        drawTopBottomLabels(rx, graphAreaTop, rw, topH, s.perfActiveTime, s.perf100Pct, spanLabel, L"0", plot1);
        Gdiplus::RectF rP1(static_cast<float>(plot1.left), static_cast<float>(plot1.top),
                           static_cast<float>(plot1.right - plot1.left), static_cast<float>(plot1.bottom - plot1.top));
        DrawChartSeries(gp, rP1, d.activeHistory, 100.0, pal.chartDisk, scrollTick_, true);

        double maxDiskMbs = ComputeNiceMaxScale(SeriesMax(d.readHistory, &d.writeHistory), 0.1);
        std::wstring dScaleStr = FormatScaleDiskMBs(maxDiskMbs);
        RECT plot2{};
        drawTopBottomLabels(rx, graphAreaTop + topH + gap, rw, botH,
                            s.perfDiskTransferRate, dScaleStr.c_str(), spanLabel, L"0", plot2);
        Gdiplus::RectF rP2(static_cast<float>(plot2.left), static_cast<float>(plot2.top),
                           static_cast<float>(plot2.right - plot2.left), static_cast<float>(plot2.bottom - plot2.top));
        DrawChartSeries(gp, rP2, d.readHistory, maxDiskMbs, pal.chartDisk, scrollTick_, true, false);
        DrawChartSeries(gp, rP2, d.writeHistory, maxDiskMbs, pal.chartDisk, scrollTick_, false, true);

        if (showStatsBlock) {
            int sy = graphAreaTop + graphAreaH + DpiScale(hwnd_, 8);
            int lblSt = DpiScale(hwnd_, 19);
            int valSt = DpiScale(hwnd_, 27);
            int colGap = DpiScale(hwnd_, 22);

            std::wstring actStr = FormatPercent(d.activeTimePct, 0);
            wchar_t avgBuf[32];
            swprintf_s(avgBuf, L"%.1f ms", d.avgResponseMs);
            std::wstring readStr = FormatDiskSpeed(d.readBytesPerSec);
            std::wstring writeStr = FormatDiskSpeed(d.writeBytesPerSec);

            int col0W = measureStatColW(16,
                                      { s.perfActiveTime, s.perfReadSpeed },
                                      { L"100%", L"999.9 MB/s", actStr.c_str(), readStr.c_str() });
            int col1W = measureStatColW(16,
                                      { s.perfAvgResponse, s.perfWriteSpeed },
                                      { L"999.9 ms", L"999.9 MB/s", avgBuf, writeStr.c_str() });
            int leftUsedW = col0W + colGap + col1W;

            DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                          rx, sy, col0W, lblSt, valSt, s.perfActiveTime, actStr);
            DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy, col1W, lblSt, valSt, s.perfAvgResponse, avgBuf);

            int sy2 = sy + lblSt + valSt + DpiScale(hwnd_, 7);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx, sy2, col0W, lblSt, valSt, s.perfReadSpeed, readStr);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy2, col1W, lblSt, valSt, s.perfWriteSpeed, writeStr);

            int kx = 0, kW = 0, vW = 0;
            ComputeCompactKvRegion(hdc, hwnd_, rx, rw, leftUsedW,
                                   { s.perfCapacity, s.perfFormatted, s.perfSystemDisk, s.perfPageFile, s.perfType },
                                   kx, kW, vW);
            int rH = DpiScale(hwnd_, 24);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 0, kW, vW, rH, s.perfCapacity, FormatMemGB(d.capacityBytes));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 1, kW, vW, rH, s.perfFormatted, FormatMemGB(d.formattedBytes));
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 2, kW, vW, rH, s.perfSystemDisk, d.isSystemDisk ? s.perfYes : s.perfNo);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 3, kW, vW, rH, s.perfPageFile, d.hasPageFile ? s.perfYes : s.perfNo);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 4, kW, vW, rH, s.perfType, d.type);
        }
    } else if (sel.kind == NavKind::Network && sel.index < static_cast<int>(perf_->Networks().size())) {
        const auto& n = perf_->Networks()[sel.index];
        double maxNetMbps = ComputeNiceMaxScale(SeriesMax(n.recvHistory, &n.sendHistory), 0.1);
        std::wstring nScaleStr = FormatScaleNetMbps(maxNetMbps);
        RECT plotRc{};
        drawTopBottomLabels(rx, graphAreaTop, rw, graphAreaH, s.perfThroughput, nScaleStr.c_str(), spanLabel, L"0", plotRc);
        Gdiplus::RectF rPlot(static_cast<float>(plotRc.left), static_cast<float>(plotRc.top),
                             static_cast<float>(plotRc.right - plotRc.left), static_cast<float>(plotRc.bottom - plotRc.top));
        DrawChartSeries(gp, rPlot, n.recvHistory, maxNetMbps, pal.chartNet, scrollTick_, true, false);
        DrawChartSeries(gp, rPlot, n.sendHistory, maxNetMbps, pal.chartNet, scrollTick_, false, true);

        if (showStatsBlock) {
            int sy = graphAreaTop + graphAreaH + DpiScale(hwnd_, 8);
            int lblSt = DpiScale(hwnd_, 19);
            int valSt = DpiScale(hwnd_, 27);
            int colGap = DpiScale(hwnd_, 22);

            std::wstring sendStr = FormatNetSpeedAuto(n.sendBytesPerSec);
            std::wstring recvStr = FormatNetSpeedAuto(n.recvBytesPerSec);
            int col0W = measureStatColW(15, { s.perfSend }, { L"999.9 Mbps", sendStr.c_str() });
            int col1W = measureStatColW(15, { s.perfReceive }, { L"999.9 Mbps", recvStr.c_str() });
            int leftUsedW = col0W + colGap + col1W;

            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx, sy, col0W, lblSt, valSt, s.perfSend, sendStr);
            DrawStatBlock(hdc, hwnd_, 9, 15, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy, col1W, lblSt, valSt, s.perfReceive, recvStr);

            int kx = 0, kW = 0, vW = 0;
            ComputeCompactKvRegion(hdc, hwnd_, rx, rw, leftUsedW,
                                   { s.perfAdapterName, s.perfDnsName, s.perfConnType, s.perfIpv4, s.perfIpv6 },
                                   kx, kW, vW);
            int rH = DpiScale(hwnd_, 24);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 0, kW, vW, rH, s.perfAdapterName, n.adapterName);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 1, kW, vW, rH, s.perfDnsName, n.dnsName.empty() ? L"-" : n.dnsName);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 2, kW, vW, rH, s.perfConnType, n.connType);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 3, kW, vW, rH, s.perfIpv4, n.ipv4.empty() ? L"-" : n.ipv4);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 4, kW, vW, rH, s.perfIpv6, n.ipv6.empty() ? L"-" : n.ipv6);
        }
    } else if (sel.kind == NavKind::Gpu && sel.index < static_cast<int>(perf_->Gpus().size())) {
        const auto& g = perf_->Gpus()[sel.index];
        int gapX = DpiScale(hwnd_, 12);
        int gapY = DpiScale(hwnd_, 8);
        int memBarAreaH = DpiScale(hwnd_, 54);
        int engAreaH = std::max(80, graphAreaH - memBarAreaH);
        int cellW = (rw - gapX) / 2;
        int cellH = (engAreaH - gapY) / 2;

        struct EngItem { const wchar_t* title; const std::vector<double>* data; } eng[4] = {
            { L"3D", &g.hist3D },
            { L"Copy", &g.histCopy },
            { L"Video Encode", &g.histVideoEncode },
            { L"Video Decode", &g.histVideoDecode }
        };
        for (int i = 0; i < 4; ++i) {
            int col = i % 2;
            int row = i / 2;
            int cx = rx + col * (cellW + gapX);
            int cy = graphAreaTop + row * (cellH + gapY);
            RECT plotRc{};
            drawTopBottomLabels(cx, cy, cellW, cellH, eng[i].title, s.perf100Pct, spanLabel, L"0", plotRc);
            Gdiplus::RectF rP(static_cast<float>(plotRc.left), static_cast<float>(plotRc.top),
                              static_cast<float>(plotRc.right - plotRc.left), static_cast<float>(plotRc.bottom - plotRc.top));
            DrawChartSeries(gp, rP, *eng[i].data, 100.0, pal.chartGpu, scrollTick_, true);
        }

        {
            // Dedicated GPU memory sparkline bar
            int mTop = graphAreaTop + engAreaH + DpiScale(hwnd_, 4);
            RECT plotDed{};
            std::wstring dedTop = FormatMemGB(g.dedicatedTotal);
            drawTopBottomLabels(rx, mTop, rw, memBarAreaH, s.perfDedGpuMem, dedTop.c_str(), L"", L"", plotDed);
            Gdiplus::RectF rDed(static_cast<float>(plotDed.left), static_cast<float>(plotDed.top),
                                static_cast<float>(plotDed.right - plotDed.left), static_cast<float>(plotDed.bottom - plotDed.top));
            DrawChartSeries(gp, rDed, g.histDedMem, 100.0, pal.chartGpu, scrollTick_, false);
        }

        if (showStatsBlock) {

            int sy = graphAreaTop + graphAreaH + DpiScale(hwnd_, 8);
            int lblSt = DpiScale(hwnd_, 19);
            int valSt = DpiScale(hwnd_, 26);
            int colGap = DpiScale(hwnd_, 22);

            std::wstring utilStr = FormatPercent(g.utilization, 0);
            std::wstring dedStr = FormatMemPairGB(g.dedicatedUsed, g.dedicatedTotal);
            std::wstring shdStr = FormatMemPairGB(g.sharedUsed, g.sharedTotal);
            std::wstring totStr = FormatMemPairGB(g.dedicatedUsed + g.sharedUsed, g.dedicatedTotal + g.sharedTotal);

            int col0W = measureStatColW(16,
                                      { s.perfUtilization, s.perfDedGpuMem, s.perfGpuMemory },
                                      { L"100%", L"99.9/99.9 GB", utilStr.c_str(), dedStr.c_str(), totStr.c_str() });
            int col1W = measureStatColW(14,
                                      { s.perfSharedGpuMem, g.hasTemperature ? s.perfGpuTemp : L"" },
                                      { L"99.9/99.9 GB", shdStr.c_str() });
            int leftUsedW = col0W + colGap + col1W;

            DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                          rx, sy, col0W, lblSt, valSt, s.perfUtilization, utilStr);
            if (g.hasTemperature) {
                wchar_t tBuf[32];
                swprintf_s(tBuf, L"%.0f \x00B0C", g.temperatureC);
                DrawStatBlock(hdc, hwnd_, 9, 16, pal.textSecondary, pal.textPrimary,
                              rx + col0W + colGap, sy, col1W, lblSt, valSt, s.perfGpuTemp, tBuf);
            }

            int sy2 = sy + lblSt + valSt + DpiScale(hwnd_, 6);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx, sy2, col0W, lblSt, valSt, s.perfDedGpuMem, dedStr);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx + col0W + colGap, sy2, col1W, lblSt, valSt, s.perfSharedGpuMem, shdStr);

            int sy3 = sy2 + lblSt + valSt + DpiScale(hwnd_, 6);
            DrawStatBlock(hdc, hwnd_, 9, 14, pal.textSecondary, pal.textPrimary,
                          rx, sy3, leftUsedW, lblSt, valSt, s.perfGpuMemory, totStr);

            int kx = 0, kW = 0, vW = 0;
            ComputeCompactKvRegion(hdc, hwnd_, rx, rw, leftUsedW,
                                   { s.perfDriverVer, s.perfDriverDate, s.perfDxVersion, s.perfPhysLocation },
                                   kx, kW, vW);
            int rH = DpiScale(hwnd_, 24);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 0, kW, vW, rH, s.perfDriverVer, g.driverVersion.empty() ? L"-" : g.driverVersion);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 1, kW, vW, rH, s.perfDriverDate, g.driverDate.empty() ? L"-" : g.driverDate);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 2, kW, vW, rH, s.perfDxVersion, g.directXVersion.empty() ? L"-" : g.directXVersion);
            DrawKeyValueRow(hdc, hwnd_, 10, pal.textSecondary, pal.textPrimary, kx, sy + rH * 3, kW, vW, rH, s.perfPhysLocation, g.physLocation.empty() ? L"-" : g.physLocation);
        }
    }

    if (summaryMode_ != PerfSummaryMode::Normal) {
        HPEN penOuter = CreatePen(PS_SOLID, 1, pal.borderColor);
        HPEN oldOuter = static_cast<HPEN>(SelectObject(hdc, penOuter));
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        Rectangle(hdc, 0, 0, totalW, totalH);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldOuter);
        DeleteObject(penOuter);
    }
}

} // namespace taskman

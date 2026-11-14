#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "settings.h"

namespace taskman {

struct ChartColorSet {
    COLORREF line;
    COLORREF fill;      // GDI+ ARGB stored via helper or RGB + alpha
    BYTE     fillAlpha;
    COLORREF kernelLine;
    COLORREF kernelFill;
    BYTE     kernelAlpha;
    COLORREF grid;
    COLORREF border;
};

struct ThemePalette {
    bool isDark;

    COLORREF windowBg;
    COLORREF chromeBg;        // menu bar, tab strip, footer bar
    COLORREF surfaceBg;       // list view / detail panel background
    COLORREF surfaceAltBg;    // subtle alternating / sidebar
    COLORREF borderColor;
    COLORREF splitterColor;

    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textDisabled;
    COLORREF groupHeaderText; // "Apps", "Background processes", "Windows processes"

    COLORREF accentColor;
    COLORREF tabActiveBg;
    COLORREF tabInactiveBg;
    COLORREF tabHoverBg;

    COLORREF rowHoverBg;
    COLORREF rowSelectedBg;
    COLORREF rowSelectedUnfocusedBg;
    COLORREF rowSelectedBorder;
    COLORREF groupExpandedBg;

    COLORREF headerBg;
    COLORREF headerHoverBg;
    COLORREF headerBorder;

    COLORREF menuHoverBg;
    COLORREF menuSelectedBg;

    COLORREF btnBg;
    COLORREF btnHoverBg;
    COLORREF btnPressedBg;
    COLORREF btnBorder;
    COLORREF btnDisabledBg;

    COLORREF editBg;
    COLORREF editBorder;

    COLORREF heat[5];         // 0 = none, 1..4 = low..very high

    ChartColorSet chartCpu;
    ChartColorSet chartMem;
    ChartColorSet chartDisk;
    ChartColorSet chartNet;
    ChartColorSet chartGpu;
};

class ThemeManager {
public:
    static ThemeManager& Instance();

    void Init();
    void Shutdown();

    bool IsSystemDark() const;
    bool ResolveDark(ThemeMode mode) const;
    void ApplyTheme(ThemeMode mode);

    bool IsDark() const { return palette_.isDark; }
    const ThemePalette& Palette() const { return palette_; }

    // Apply Win32 dark mode chrome & controls to a top-level or child window
    void ApplyToWindow(HWND hwnd);
    void ApplyToControl(HWND hCtrl);

    // Handle UAH menubar custom painting on Win10/11
    bool HandleMenuBarMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outResult);

    // Handle WM_CTLCOLOR* for dialogs & controls
    HBRUSH HandleCtlColor(UINT msg, HDC hdc, HWND hCtrl);

    // Shared brushes (owned by ThemeManager, valid until next ApplyTheme/Shutdown)
    HBRUSH WindowBrush() const { return brWindow_; }
    HBRUSH ChromeBrush() const { return brChrome_; }
    HBRUSH SurfaceBrush() const { return brSurface_; }
    HBRUSH EditBrush() const { return brEdit_; }
    HBRUSH BtnBrush() const { return brBtn_; }

    // Cached UI fonts owned by ThemeManager; callers must NOT DeleteObject() them.
    // GetFont() resolves the CJK-capable UI font (e.g. Noto Sans SC DemiLight);
    // GetLatinFont() always returns crisp Segoe UI for English/digits & standard dialogs.
    HFONT GetFont(HWND hwnd, int pointSize, bool bold = false, bool light = false);
    HFONT GetLatinFont(HWND hwnd, int pointSize, bool bold = false, bool light = false);
    const std::wstring& UiFontFace();
    void ClearFontCache();

    // Composite per-script rendering & measurement (Segoe UI for ASCII/Latin + CJK UI font for Hanzi)
    int MeasureTextWidth(HDC hdc, HWND hwnd, int pointSize, bool bold, const wchar_t* text, int len = -1);
    void DrawModernText(HDC hdc, HWND hwnd, int pointSize, bool bold, const wchar_t* text,
                        const RECT& rc, UINT dtFlags, COLORREF color, bool light = false);

private:
    ThemeManager() = default;
    HFONT GetFontInternal(HWND hwnd, int pointSize, bool bold, bool light, bool latinOnly);
    void RebuildBrushes();
    void DestroyBrushes();
    void ResolveUiFontFace();

    ThemePalette palette_{};
    HBRUSH brWindow_ = nullptr;
    HBRUSH brChrome_ = nullptr;
    HBRUSH brSurface_ = nullptr;
    HBRUSH brEdit_ = nullptr;
    HBRUSH brBtn_ = nullptr;

    std::unordered_map<uint64_t, HFONT> fontCache_;
    std::wstring uiFontFace_;
    std::wstring uiCjkBoldFace_;
    int uiCjkWeight_ = FW_NORMAL;
    int uiCjkBoldWeight_ = FW_SEMIBOLD;
};

// DPI scaling helper
inline int DpiScale(HWND hwnd, int px) {
    UINT dpi = 96;
    if (hwnd) {
        using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
        static auto pfn = reinterpret_cast<PFN_GetDpiForWindow>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (pfn) dpi = pfn(hwnd);
    }
    return MulDiv(px, static_cast<int>(dpi), 96);
}

inline int DpiScaleVal(UINT dpi, int px) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

} // namespace taskman

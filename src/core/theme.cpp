#include "theme.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <vector>

namespace taskman {

// Undocumented uxtheme ordinals (Win10 1809+)
enum PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
};

using PFN_AllowDarkModeForApp = bool(WINAPI*)(bool);
using PFN_SetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using PFN_AllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using PFN_FlushMenuThemes = void(WINAPI*)();
using PFN_RefreshImmersiveColorPolicyState = void(WINAPI*)();

static PFN_AllowDarkModeForApp g_AllowDarkModeForApp = nullptr;
static PFN_SetPreferredAppMode g_SetPreferredAppMode = nullptr;
static PFN_AllowDarkModeForWindow g_AllowDarkModeForWindow = nullptr;
static PFN_FlushMenuThemes g_FlushMenuThemes = nullptr;
static PFN_RefreshImmersiveColorPolicyState g_RefreshImmersive = nullptr;

// UAH MenuBar undocumented messages & structs
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU        0x0091
#define WM_UAHDRAWMENUITEM    0x0092
#define WM_UAHMEASUREMENUITEM 0x0094
#endif

typedef union tagUAHMENUITEMMETRICS {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct tagUAHMENUPOPUPMETRICS {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct tagUAHMENU {
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
} UAHMENU;

typedef struct tagUAHMENUITEM {
    int iPosition;
    UAHMENUITEMMETRICS umim;
    UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

typedef struct tagUAHDRAWMENUITEM {
    DRAWITEMSTRUCT dis;
    UAHMENU um;
    UAHMENUITEM umi;
} UAHDRAWMENUITEM;

static ThemePalette MakeLightPalette() {
    ThemePalette p{};
    p.isDark = false;
    p.windowBg = RGB(255, 255, 255);
    p.chromeBg = RGB(240, 240, 240);
    p.surfaceBg = RGB(255, 255, 255);
    p.surfaceAltBg = RGB(248, 248, 248);
    p.borderColor = RGB(217, 217, 217);
    p.splitterColor = RGB(224, 224, 224);

    p.textPrimary = RGB(0, 0, 0);
    p.textSecondary = RGB(96, 96, 96);
    p.textDisabled = RGB(150, 150, 150);
    p.groupHeaderText = RGB(0, 51, 153);

    p.accentColor = RGB(0, 120, 215);
    p.tabActiveBg = RGB(255, 255, 255);
    p.tabInactiveBg = RGB(240, 240, 240);
    p.tabHoverBg = RGB(229, 241, 251);

    p.rowHoverBg = RGB(229, 243, 255);
    p.rowSelectedBg = RGB(205, 232, 255);
    p.rowSelectedUnfocusedBg = RGB(224, 224, 224);
    p.rowSelectedBorder = RGB(153, 209, 255);
    p.groupExpandedBg = RGB(224, 240, 255);

    p.headerBg = RGB(255, 255, 255);
    p.headerHoverBg = RGB(229, 243, 255);
    p.headerBorder = RGB(229, 229, 229);

    p.menuHoverBg = RGB(229, 243, 255);
    p.menuSelectedBg = RGB(205, 232, 255);

    p.btnBg = RGB(225, 225, 225);
    p.btnHoverBg = RGB(229, 241, 251);
    p.btnPressedBg = RGB(204, 228, 247);
    p.btnBorder = RGB(173, 173, 173);
    p.btnDisabledBg = RGB(236, 236, 236);

    p.editBg = RGB(255, 255, 255);
    p.editBorder = RGB(173, 173, 173);

    p.heat[0] = RGB(255, 255, 255);
    p.heat[1] = RGB(255, 244, 196);
    p.heat[2] = RGB(255, 230, 148);
    p.heat[3] = RGB(255, 209, 102);
    p.heat[4] = RGB(244, 162, 97);

    p.chartCpu  = { RGB(17, 125, 187), RGB(17, 125, 187), 48, RGB(17, 125, 187), RGB(17, 125, 187), 115, RGB(217, 234, 244), RGB(17, 125, 187) };
    p.chartMem  = { RGB(139, 18, 174), RGB(139, 18, 174), 45, RGB(139, 18, 174), RGB(139, 18, 174), 95,  RGB(232, 214, 238), RGB(139, 18, 174) };
    p.chartDisk = { RGB(77, 166, 12),  RGB(77, 166, 12),  48, RGB(77, 166, 12),  RGB(77, 166, 12),  95,  RGB(218, 238, 204), RGB(77, 166, 12) };
    p.chartNet  = { RGB(167, 79, 1),   RGB(167, 79, 1),   48, RGB(167, 79, 1),   RGB(167, 79, 1),   95,  RGB(240, 222, 204), RGB(167, 79, 1) };
    p.chartGpu  = { RGB(17, 125, 187), RGB(17, 125, 187), 48, RGB(17, 125, 187), RGB(17, 125, 187), 95,  RGB(217, 234, 244), RGB(17, 125, 187) };
    return p;
}

static ThemePalette MakeDarkPalette() {
    ThemePalette p{};
    p.isDark = true;
    p.windowBg = RGB(25, 25, 25);
    p.chromeBg = RGB(32, 32, 32);
    p.surfaceBg = RGB(25, 25, 25);
    p.surfaceAltBg = RGB(30, 30, 30);
    p.borderColor = RGB(58, 58, 58);
    p.splitterColor = RGB(52, 52, 52);

    p.textPrimary = RGB(242, 242, 242);
    p.textSecondary = RGB(168, 168, 168);
    p.textDisabled = RGB(105, 105, 105);
    p.groupHeaderText = RGB(108, 188, 255);

    p.accentColor = RGB(76, 194, 255);
    p.tabActiveBg = RGB(25, 25, 25);
    p.tabInactiveBg = RGB(32, 32, 32);
    p.tabHoverBg = RGB(45, 45, 45);

    p.rowHoverBg = RGB(42, 42, 42);
    p.rowSelectedBg = RGB(38, 79, 120);
    p.rowSelectedUnfocusedBg = RGB(52, 52, 52);
    p.rowSelectedBorder = RGB(76, 194, 255);
    p.groupExpandedBg = RGB(34, 48, 66);

    p.headerBg = RGB(32, 32, 32);
    p.headerHoverBg = RGB(48, 48, 48);
    p.headerBorder = RGB(55, 55, 55);

    p.menuHoverBg = RGB(52, 52, 52);
    p.menuSelectedBg = RGB(64, 64, 64);

    p.btnBg = RGB(48, 48, 48);
    p.btnHoverBg = RGB(62, 62, 62);
    p.btnPressedBg = RGB(40, 40, 40);
    p.btnBorder = RGB(78, 78, 78);
    p.btnDisabledBg = RGB(36, 36, 36);

    p.editBg = RGB(34, 34, 34);
    p.editBorder = RGB(84, 84, 84);

    p.heat[0] = RGB(25, 25, 25);
    p.heat[1] = RGB(58, 47, 22);
    p.heat[2] = RGB(84, 62, 20);
    p.heat[3] = RGB(114, 74, 18);
    p.heat[4] = RGB(148, 78, 18);

    p.chartCpu  = { RGB(59, 172, 238), RGB(59, 172, 238), 64, RGB(59, 172, 238), RGB(59, 172, 238), 130, RGB(38, 60, 76), RGB(59, 172, 238) };
    p.chartMem  = { RGB(194, 98, 228), RGB(194, 98, 228), 60, RGB(194, 98, 228), RGB(194, 98, 228), 115, RGB(66, 42, 76), RGB(194, 98, 228) };
    p.chartDisk = { RGB(108, 203, 45), RGB(108, 203, 45), 62, RGB(108, 203, 45), RGB(108, 203, 45), 115, RGB(44, 68, 34), RGB(108, 203, 45) };
    p.chartNet  = { RGB(232, 134, 52), RGB(232, 134, 52), 62, RGB(232, 134, 52), RGB(232, 134, 52), 115, RGB(74, 50, 32), RGB(232, 134, 52) };
    p.chartGpu  = { RGB(59, 172, 238), RGB(59, 172, 238), 62, RGB(59, 172, 238), RGB(59, 172, 238), 115, RGB(38, 60, 76), RGB(59, 172, 238) };
    return p;
}

ThemeManager& ThemeManager::Instance() {
    static ThemeManager inst;
    return inst;
}

void ThemeManager::Init() {
    HMODULE hUx = LoadLibraryW(L"uxtheme.dll");
    if (hUx) {
        auto ord135 = GetProcAddress(hUx, MAKEINTRESOURCEA(135));
        g_SetPreferredAppMode = reinterpret_cast<PFN_SetPreferredAppMode>(ord135);
        g_AllowDarkModeForApp = reinterpret_cast<PFN_AllowDarkModeForApp>(ord135);
        g_AllowDarkModeForWindow = reinterpret_cast<PFN_AllowDarkModeForWindow>(
            GetProcAddress(hUx, MAKEINTRESOURCEA(133)));
        g_FlushMenuThemes = reinterpret_cast<PFN_FlushMenuThemes>(
            GetProcAddress(hUx, MAKEINTRESOURCEA(136)));
        g_RefreshImmersive = reinterpret_cast<PFN_RefreshImmersiveColorPolicyState>(
            GetProcAddress(hUx, MAKEINTRESOURCEA(104)));
    }
    ResolveUiFontFace();
    palette_ = MakeLightPalette();
    RebuildBrushes();
}

void ThemeManager::Shutdown() {
    ClearFontCache();
    DestroyBrushes();
}

bool ThemeManager::IsSystemDark() const {
    DWORD val = 1; // 1 = Light, 0 = Dark
    DWORD size = sizeof(val);
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&val), &size);
        RegCloseKey(hKey);
    }
    return (val == 0);
}

bool ThemeManager::ResolveDark(ThemeMode mode) const {
    if (mode == ThemeMode::Dark) return true;
    if (mode == ThemeMode::Light) return false;
    return IsSystemDark();
}

void ThemeManager::ApplyTheme(ThemeMode mode) {
    bool dark = ResolveDark(mode);
    palette_ = dark ? MakeDarkPalette() : MakeLightPalette();
    RebuildBrushes();

    if (g_SetPreferredAppMode) {
        g_SetPreferredAppMode(dark ? ForceDark : ForceLight);
    }
    if (g_RefreshImmersive) g_RefreshImmersive();
    if (g_FlushMenuThemes) g_FlushMenuThemes();
}

void ThemeManager::RebuildBrushes() {
    DestroyBrushes();
    brWindow_ = CreateSolidBrush(palette_.windowBg);
    brChrome_ = CreateSolidBrush(palette_.chromeBg);
    brSurface_ = CreateSolidBrush(palette_.surfaceBg);
    brEdit_ = CreateSolidBrush(palette_.editBg);
    brBtn_ = CreateSolidBrush(palette_.btnBg);
}

void ThemeManager::DestroyBrushes() {
    auto safeDel = [](HBRUSH& b) { if (b) { DeleteObject(b); b = nullptr; } };
    safeDel(brWindow_);
    safeDel(brChrome_);
    safeDel(brSurface_);
    safeDel(brEdit_);
    safeDel(brBtn_);
}

void ThemeManager::ApplyToWindow(HWND hwnd) {
    if (!hwnd) return;
    BOOL useDark = palette_.isDark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 on Win10 2004+/Win11, 19 on Win10 1903/1909
    HRESULT hr = DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
    bool usedLegacy = false;
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
        usedLegacy = true;
    }
    if (g_AllowDarkModeForWindow) {
        g_AllowDarkModeForWindow(hwnd, palette_.isDark);
    }
    if (usedLegacy) {
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

void ThemeManager::ApplyToControl(HWND hCtrl) {
    if (!hCtrl) return;
    if (g_AllowDarkModeForWindow) {
        g_AllowDarkModeForWindow(hCtrl, palette_.isDark);
    }
    wchar_t cls[64] = {};
    GetClassNameW(hCtrl, cls, 64);
    if (_wcsicmp(cls, L"Button") == 0) {
        // DarkMode_Explorer keeps pushbuttons dark on Win10/11
        SetWindowTheme(hCtrl, palette_.isDark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    } else if (_wcsicmp(cls, L"Edit") == 0) {
        SetWindowTheme(hCtrl, palette_.isDark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    } else {
        SetWindowTheme(hCtrl, palette_.isDark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
}

bool ThemeManager::HandleMenuBarMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outResult) {
    switch (msg) {
    case WM_UAHDRAWMENU: {
        auto* pUDM = reinterpret_cast<UAHMENU*>(lParam);
        MENUBARINFO mbi{ sizeof(mbi) };
        if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
            RECT rcWin{};
            GetWindowRect(hwnd, &rcWin);
            RECT rcBar = mbi.rcBar;
            OffsetRect(&rcBar, -rcWin.left, -rcWin.top);
            FillRect(pUDM->hdc, &rcBar, brChrome_);
            // Subtle bottom separator line
            HPEN pen = CreatePen(PS_SOLID, 1, palette_.borderColor);
            HGDIOBJ oldPen = SelectObject(pUDM->hdc, pen);
            MoveToEx(pUDM->hdc, rcBar.left, rcBar.bottom - 1, nullptr);
            LineTo(pUDM->hdc, rcBar.right, rcBar.bottom - 1);
            SelectObject(pUDM->hdc, oldPen);
            DeleteObject(pen);
        }
        outResult = 0;
        return true;
    }
    case WM_UAHDRAWMENUITEM: {
        auto* pUDMI = reinterpret_cast<UAHDRAWMENUITEM*>(lParam);
        wchar_t text[128] = {};
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = 128;
        GetMenuItemInfoW(pUDMI->um.hmenu, pUDMI->umi.iPosition, TRUE, &mii);

        COLORREF bg = palette_.chromeBg;
        COLORREF fg = palette_.textPrimary;
        if (pUDMI->dis.itemState & ODS_DISABLED) {
            fg = palette_.textDisabled;
        } else if (pUDMI->dis.itemState & ODS_SELECTED) {
            bg = palette_.menuSelectedBg;
        } else if (pUDMI->dis.itemState & (ODS_HOTLIGHT | ODS_FOCUS)) {
            bg = palette_.menuHoverBg;
        }

        HBRUSH br = CreateSolidBrush(bg);
        FillRect(pUDMI->dis.hDC, &pUDMI->dis.rcItem, br);
        DeleteObject(br);

        DWORD dtFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (pUDMI->dis.itemState & ODS_NOACCEL) dtFlags |= DT_HIDEPREFIX;

        int oldBk = SetBkMode(pUDMI->dis.hDC, TRANSPARENT);
        DrawModernText(pUDMI->dis.hDC, hwnd, 9, false, text, pUDMI->dis.rcItem, dtFlags, fg);
        SetBkMode(pUDMI->dis.hDC, oldBk);

        outResult = 0;
        return true;
    }
    case WM_UAHMEASUREMENUITEM: {
        outResult = DefWindowProcW(hwnd, msg, wParam, lParam);
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        if (!palette_.isDark) return false;
        LRESULT res = DefWindowProcW(hwnd, msg, wParam, lParam);
        // Paint over the 1px white line left by standard Win32 menu bar below non-client area
        MENUBARINFO mbi{ sizeof(mbi) };
        if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
            RECT rcWin{};
            GetWindowRect(hwnd, &rcWin);
            RECT rcLine = mbi.rcBar;
            OffsetRect(&rcLine, -rcWin.left, -rcWin.top);
            rcLine.top = rcLine.bottom;
            rcLine.bottom = rcLine.top + 1;
            HDC hdc = GetWindowDC(hwnd);
            if (hdc) {
                HBRUSH br = CreateSolidBrush(palette_.borderColor);
                FillRect(hdc, &rcLine, br);
                DeleteObject(br);
                ReleaseDC(hwnd, hdc);
            }
        }
        outResult = res;
        return true;
    }
    }
    return false;
}

HBRUSH ThemeManager::HandleCtlColor(UINT msg, HDC hdc, HWND /*hCtrl*/) {
    switch (msg) {
    case WM_CTLCOLORDLG:
        SetBkColor(hdc, palette_.windowBg);
        SetTextColor(hdc, palette_.textPrimary);
        return brWindow_;
    case WM_CTLCOLORSTATIC:
        SetBkColor(hdc, palette_.windowBg);
        SetTextColor(hdc, palette_.textPrimary);
        return brWindow_;
    case WM_CTLCOLOREDIT:
        SetBkColor(hdc, palette_.editBg);
        SetTextColor(hdc, palette_.textPrimary);
        return brEdit_;
    case WM_CTLCOLORBTN:
        SetBkColor(hdc, palette_.windowBg);
        SetTextColor(hdc, palette_.textPrimary);
        return brWindow_;
    }
    return nullptr;
}

struct FontProbeResult {
    bool found = false;
    int weight = FW_NORMAL;
};

static int CALLBACK FontCheckEnumProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    auto* r = reinterpret_cast<FontProbeResult*>(lp);
    r->found = true;
    r->weight = lf ? lf->lfWeight : FW_NORMAL;
    return 0; // stop immediately on first match
}

static FontProbeResult ProbeFontGdi(HDC hdc, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    FontProbeResult res;
    EnumFontFamiliesExW(hdc, &lf, FontCheckEnumProc, reinterpret_cast<LPARAM>(&res), 0);
    return res;
}

void ThemeManager::ResolveUiFontFace() {
    uiFontFace_ = L"Segoe UI";
    uiCjkBoldFace_.clear();
    uiCjkWeight_ = FW_NORMAL;
    uiCjkBoldWeight_ = FW_SEMIBOLD;

    HDC hdc = GetDC(nullptr);
    if (!hdc) return;

    // Prefer Noto Sans SC DemiLight (weight 350: slightly bolder than Source Han Sans Light 300)
    // and clean Regular/UI faces over hairline Light 300.
    static constexpr const wchar_t* kCandidates[] = {
        L"Noto Sans SC DemiLight",
        L"Noto Sans SC",
        L"\x601D\x6E90\x9ED1\x4F53 Regular",
        L"\x601D\x6E90\x9ED1\x4F53",
        L"Microsoft YaHei UI",
        L"Noto Sans SC Light",
        L"\x601D\x6E90\x9ED1\x4F53 Light",
        L"Source Han Sans SC Light",
        L"Segoe UI",
    };
    for (const wchar_t* face : kCandidates) {
        FontProbeResult pr = ProbeFontGdi(hdc, face);
        if (pr.found) {
            uiFontFace_ = face;
            uiCjkWeight_ = (pr.weight > 0) ? pr.weight : FW_NORMAL;
            break;
        }
    }

    static constexpr const wchar_t* kBoldCandidates[] = {
        L"Noto Sans SC Medium",
        L"Noto Sans SC",
        L"Microsoft YaHei UI",
    };
    for (const wchar_t* face : kBoldCandidates) {
        FontProbeResult pr = ProbeFontGdi(hdc, face);
        if (pr.found) {
            uiCjkBoldFace_ = face;
            uiCjkBoldWeight_ = (pr.weight >= FW_MEDIUM) ? pr.weight : FW_SEMIBOLD;
            break;
        }
    }

    ReleaseDC(nullptr, hdc);
}

const std::wstring& ThemeManager::UiFontFace() {
    if (uiFontFace_.empty()) {
        ResolveUiFontFace();
    }
    return uiFontFace_;
}

void ThemeManager::ClearFontCache() {
    for (auto& kv : fontCache_) {
        if (kv.second) DeleteObject(kv.second);
    }
    fontCache_.clear();
}

HFONT ThemeManager::GetFontInternal(HWND hwnd, int pointSize, bool bold, bool light, bool latinOnly) {
    UINT dpi = 96;
    if (hwnd) {
        using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
        static auto pfn = reinterpret_cast<PFN_GetDpiForWindow>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (pfn) dpi = pfn(hwnd);
    }
    if (dpi == 0) dpi = 96;

    if (uiFontFace_.empty()) ResolveUiFontFace();

    uint64_t key = (static_cast<uint64_t>(dpi) << 20) |
                   (static_cast<uint64_t>(pointSize & 0xFFFF) << 3) |
                   (latinOnly ? 4ULL : 0ULL) |
                   (bold ? 2ULL : 0ULL) |
                   (light ? 1ULL : 0ULL);
    auto it = fontCache_.find(key);
    if (it != fontCache_.end() && it->second) {
        return it->second;
    }

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;

    if (latinOnly) {
        lf.lfWeight = bold ? FW_SEMIBOLD : (light ? FW_LIGHT : FW_NORMAL);
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    } else if (bold && !uiCjkBoldFace_.empty()) {
        lf.lfWeight = uiCjkBoldWeight_;
        wcscpy_s(lf.lfFaceName, uiCjkBoldFace_.c_str());
    } else {
        lf.lfWeight = bold ? FW_SEMIBOLD : (light ? FW_LIGHT : uiCjkWeight_);
        wcscpy_s(lf.lfFaceName, uiFontFace_.c_str());
    }

    HFONT hf = CreateFontIndirectW(&lf);
    if (hf) {
        if (fontCache_.size() > 512) {
            ClearFontCache();
        }
        fontCache_[key] = hf;
    }
    return hf;
}

HFONT ThemeManager::GetFont(HWND hwnd, int pointSize, bool bold, bool light) {
    return GetFontInternal(hwnd, pointSize, bold, light, false);
}

HFONT ThemeManager::GetLatinFont(HWND hwnd, int pointSize, bool bold, bool light) {
    return GetFontInternal(hwnd, pointSize, bold, light, true);
}

static bool IsCjkCodeUnit(wchar_t ch) {
    return (ch >= 0x2E80 && ch <= 0x9FFF) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0x3000 && ch <= 0x303F) ||
           (ch >= 0xFF00 && ch <= 0xFFEF);
}

struct MixedRun {
    bool isCjk = false;
    std::wstring text;
    int width = 0;
    int underlinePos = -1; // character offset inside `text` to underline
};

static std::wstring StripMnemonicForMeasure(const wchar_t* src, int len, bool parsePrefix, int& outAccelIdx) {
    outAccelIdx = -1;
    std::wstring out;
    int n = (len >= 0) ? len : static_cast<int>(wcslen(src));
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (parsePrefix && src[i] == L'&' && i + 1 < n) {
            if (src[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
                continue;
            }
            if (outAccelIdx < 0) outAccelIdx = static_cast<int>(out.size());
            out.push_back(src[i + 1]);
            ++i;
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
}

static void AnalyzeScriptPresence(const wchar_t* text, int len, bool& hasLatin, bool& hasCjk) {
    hasLatin = false;
    hasCjk = false;
    int n = (len >= 0) ? len : static_cast<int>(wcslen(text));
    for (int i = 0; i < n; ++i) {
        wchar_t ch = text[i];
        if (ch == L' ') continue;
        if (IsCjkCodeUnit(ch)) hasCjk = true;
        else hasLatin = true;
        if (hasLatin && hasCjk) break;
    }
}

static std::vector<MixedRun> BuildMixedRuns(HDC hdc, const std::wstring& cleanText, int accelIdx,
                                            HFONT fLat, HFONT fCjk) {
    std::vector<MixedRun> runs;
    if (cleanText.empty()) return runs;

    for (size_t i = 0; i < cleanText.size(); ++i) {
        wchar_t ch = cleanText[i];
        bool cjk = (ch == L' ') ? (!runs.empty() ? runs.back().isCjk : false) : IsCjkCodeUnit(ch);
        if (runs.empty() || runs.back().isCjk != cjk) {
            MixedRun r;
            r.isCjk = cjk;
            runs.push_back(std::move(r));
        }
        auto& cur = runs.back();
        if (static_cast<int>(i) == accelIdx) {
            cur.underlinePos = static_cast<int>(cur.text.size());
        }
        cur.text.push_back(ch);
    }

    HGDIOBJ oldF = SelectObject(hdc, fLat);
    for (auto& r : runs) {
        SelectObject(hdc, r.isCjk ? fCjk : fLat);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, r.text.c_str(), static_cast<int>(r.text.size()), &sz);
        r.width = sz.cx;
    }
    SelectObject(hdc, oldF);
    return runs;
}

int ThemeManager::MeasureTextWidth(HDC hdc, HWND hwnd, int pointSize, bool bold, const wchar_t* text, int len) {
    if (!hdc || !text || !text[0]) return 0;
    int n = (len >= 0) ? len : static_cast<int>(wcslen(text));
    if (n <= 0) return 0;

    bool hasLatin = false, hasCjk = false;
    AnalyzeScriptPresence(text, n, hasLatin, hasCjk);

    HFONT fLat = GetLatinFont(hwnd, pointSize, bold);
    HFONT fCjk = GetFont(hwnd, pointSize, bold);

    if (!hasCjk || !hasLatin) {
        HGDIOBJ oldF = SelectObject(hdc, hasCjk ? fCjk : fLat);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, n, &sz);
        SelectObject(hdc, oldF);
        return sz.cx;
    }

    int accelIdx = -1;
    std::wstring clean = StripMnemonicForMeasure(text, n, false, accelIdx);
    auto runs = BuildMixedRuns(hdc, clean, -1, fLat, fCjk);
    int total = 0;
    for (const auto& r : runs) total += r.width;
    return total;
}

void ThemeManager::DrawModernText(HDC hdc, HWND hwnd, int pointSize, bool bold, const wchar_t* text,
                                  const RECT& rc, UINT dtFlags, COLORREF color, bool light) {
    if (!hdc || !text) return;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    int n = static_cast<int>(wcslen(text));
    if (n == 0) return;

    bool hasLatin = false, hasCjk = false;
    AnalyzeScriptPresence(text, n, hasLatin, hasCjk);

    HFONT fLat = GetLatinFont(hwnd, pointSize, bold, light);
    HFONT fCjk = GetFont(hwnd, pointSize, bold, light);

    COLORREF oldColor = SetTextColor(hdc, color);
    if (!hasCjk || !hasLatin) {
        HGDIOBJ oldF = SelectObject(hdc, hasCjk ? fCjk : fLat);
        RECT rcCopy = rc;
        DrawTextW(hdc, text, n, &rcCopy, dtFlags);
        SelectObject(hdc, oldF);
        SetTextColor(hdc, oldColor);
        return;
    }

    // Mixed Latin + CJK drawing with exact baseline alignment
    bool parsePrefix = (dtFlags & DT_NOPREFIX) == 0;
    bool drawPrefix = parsePrefix && ((dtFlags & DT_HIDEPREFIX) == 0);
    int accelIdx = -1;
    std::wstring clean = StripMnemonicForMeasure(text, n, parsePrefix, accelIdx);
    if (clean.empty()) {
        SetTextColor(hdc, oldColor);
        return;
    }

    std::vector<MixedRun> runs = BuildMixedRuns(hdc, clean, drawPrefix ? accelIdx : -1, fLat, fCjk);
    int totalW = 0;
    for (const auto& r : runs) totalW += r.width;

    int availW = rc.right - rc.left;
    if ((dtFlags & DT_END_ELLIPSIS) && totalW > availW) {
        HGDIOBJ oldF = SelectObject(hdc, fLat);
        SIZE dotsSz{};
        GetTextExtentPoint32W(hdc, L"...", 3, &dotsSz);

        int keepW = (std::max)(0, availW - static_cast<int>(dotsSz.cx));
        std::vector<MixedRun> clipped;
        int accW = 0;
        for (const auto& r : runs) {
            if (accW + r.width <= keepW) {
                clipped.push_back(r);
                accW += r.width;
            } else {
                int rem = keepW - accW;
                if (rem > 0) {
                    SelectObject(hdc, r.isCjk ? fCjk : fLat);
                    int fitCount = 0;
                    SIZE subSz{};
                    for (size_t k = 1; k <= r.text.size(); ++k) {
                        GetTextExtentPoint32W(hdc, r.text.c_str(), static_cast<int>(k), &subSz);
                        if (subSz.cx > rem) break;
                        fitCount = static_cast<int>(k);
                    }
                    if (fitCount > 0) {
                        MixedRun part;
                        part.isCjk = r.isCjk;
                        part.text = r.text.substr(0, static_cast<size_t>(fitCount));
                        GetTextExtentPoint32W(hdc, part.text.c_str(), fitCount, &subSz);
                        part.width = subSz.cx;
                        if (r.underlinePos >= 0 && r.underlinePos < fitCount) {
                            part.underlinePos = r.underlinePos;
                        }
                        clipped.push_back(std::move(part));
                    }
                }
                break;
            }
        }
        MixedRun dots;
        dots.isCjk = false;
        dots.text = L"...";
        dots.width = dotsSz.cx;
        clipped.push_back(std::move(dots));
        runs = std::move(clipped);

        totalW = 0;
        for (const auto& r : runs) totalW += r.width;
        SelectObject(hdc, oldF);
    }

    int curX = rc.left;
    if (dtFlags & DT_CENTER) {
        curX = rc.left + (std::max)(0, (availW - totalW) / 2);
    } else if (dtFlags & DT_RIGHT) {
        curX = (std::max)(static_cast<int>(rc.left), static_cast<int>(rc.right) - totalW);
    }

    TEXTMETRICW tmLat{}, tmCjk{};
    HGDIOBJ oldF = SelectObject(hdc, fLat);
    GetTextMetricsW(hdc, &tmLat);
    SelectObject(hdc, fCjk);
    GetTextMetricsW(hdc, &tmCjk);

    int yLat = rc.top;
    if (dtFlags & DT_VCENTER) {
        yLat = rc.top + ((rc.bottom - rc.top) - tmLat.tmHeight) / 2;
    } else if (dtFlags & DT_BOTTOM) {
        yLat = rc.bottom - tmLat.tmHeight;
    }
    int baselineY = yLat + tmLat.tmAscent;
    int yCjk = baselineY - tmCjk.tmAscent;

    UINT oldAlign = SetTextAlign(hdc, TA_LEFT | TA_TOP);
    for (const auto& r : runs) {
        SelectObject(hdc, r.isCjk ? fCjk : fLat);
        int runY = r.isCjk ? yCjk : yLat;
        ExtTextOutW(hdc, curX, runY, ETO_CLIPPED, &rc,
                    r.text.c_str(), static_cast<UINT>(r.text.size()), nullptr);

        if (r.underlinePos >= 0 && r.underlinePos < static_cast<int>(r.text.size())) {
            SIZE preSz{}, chSz{};
            if (r.underlinePos > 0) {
                GetTextExtentPoint32W(hdc, r.text.c_str(), r.underlinePos, &preSz);
            }
            GetTextExtentPoint32W(hdc, r.text.c_str() + r.underlinePos, 1, &chSz);
            int ux = curX + preSz.cx;
            int uy = baselineY + 2;
            HPEN penU = CreatePen(PS_SOLID, 1, color);
            HGDIOBJ oldP = SelectObject(hdc, penU);
            MoveToEx(hdc, ux, uy, nullptr);
            LineTo(hdc, ux + chSz.cx, uy);
            SelectObject(hdc, oldP);
            DeleteObject(penU);
        }
        curX += r.width;
    }

    SetTextAlign(hdc, oldAlign);
    SelectObject(hdc, oldF);
    SetTextColor(hdc, oldColor);
}

} // namespace taskman

#include "dialogs.h"
#include "../resource.h"
#include "build_version.h"
#include "../core/i18n.h"
#include "../core/theme.h"
#include "../core/format_util.h"
#include "../core/process_monitor.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cwchar>
#include <vector>

namespace taskman {

static BOOL CALLBACK ThemeChildEnumProc(HWND hChild, LPARAM lp) {
    ThemeManager::Instance().ApplyToControl(hChild);
    if (lp) {
        SendMessageW(hChild, WM_SETFONT, static_cast<WPARAM>(lp), FALSE);
    }
    return TRUE;
}

static void ThemeDialogWindow(HWND hwnd) {
    auto& tm = ThemeManager::Instance();
    tm.ApplyToWindow(hwnd);
    HFONT hFont = tm.GetLatinFont(hwnd, 9, false);
    if (hFont) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);
    }
    EnumChildWindows(hwnd, ThemeChildEnumProc, reinterpret_cast<LPARAM>(hFont));
}

// ============================================================================
// Run new task dialog
// ============================================================================
struct RunTaskCtx {
    const AppSettings* settings;
    std::wstring lastCmd;
};

static std::wstring& LastRunCommand() {
    static std::wstring cmd;
    return cmd;
}

static INT_PTR CALLBACK RunTaskDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<RunTaskCtx*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<RunTaskCtx*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        const auto& s = GetStrings(ctx->settings->langMode);
        SetWindowTextW(hwnd, s.dlgRunTitle);
        SetDlgItemTextW(hwnd, IDC_RUN_DESC, s.dlgRunDesc);
        SetDlgItemTextW(hwnd, IDC_RUN_LABEL, s.dlgRunOpen);
        SetDlgItemTextW(hwnd, IDC_RUN_ADMIN_CHK, s.dlgRunAdmin);
        SetDlgItemTextW(hwnd, IDOK, s.dlgOk);
        SetDlgItemTextW(hwnd, IDCANCEL, s.dlgCancel);
        SetDlgItemTextW(hwnd, IDC_RUN_BROWSE, s.dlgBrowse);

        if (!LastRunCommand().empty()) {
            SetDlgItemTextW(hwnd, IDC_RUN_EDIT, LastRunCommand().c_str());
        }

        if (IsCurrentProcessElevated()) {
            CheckDlgButton(hwnd, IDC_RUN_ADMIN_CHK, BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_RUN_ADMIN_CHK), FALSE);
        } else {
            SetDlgItemTextW(hwnd, IDC_RUN_ADMIN_HINT, s.dlgRunAdminHint);
        }

        ThemeDialogWindow(hwnd);
        SetFocus(GetDlgItem(hwnd, IDC_RUN_EDIT));
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HBRUSH br = ThemeManager::Instance().HandleCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        if (br) return reinterpret_cast<INT_PTR>(br);
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDC_RUN_BROWSE) {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Programs (*.exe;*.cmd;*.bat;*.msc)\0*.exe;*.cmd;*.bat;*.msc\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hwnd, IDC_RUN_EDIT, file);
            }
            return TRUE;
        }
        if (id == IDOK) {
            wchar_t buf[1024] = {};
            GetDlgItemTextW(hwnd, IDC_RUN_EDIT, buf, 1024);
            std::wstring raw = buf;
            while (!raw.empty() && iswspace(raw.front())) raw.erase(raw.begin());
            while (!raw.empty() && iswspace(raw.back())) raw.pop_back();
            if (raw.empty()) return TRUE;

            LastRunCommand() = raw;
            bool runAdmin = (IsDlgButtonChecked(hwnd, IDC_RUN_ADMIN_CHK) == BST_CHECKED);

            // Split exe & args if quoted or space-separated
            std::wstring exe, params;
            if (raw.front() == L'"') {
                size_t closeQ = raw.find(L'"', 1);
                if (closeQ != std::wstring::npos) {
                    exe = raw.substr(1, closeQ - 1);
                    params = raw.substr(closeQ + 1);
                } else {
                    exe = raw;
                }
            } else {
                size_t sp = raw.find(L' ');
                if (sp != std::wstring::npos && GetFileAttributesW(raw.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    exe = raw.substr(0, sp);
                    params = raw.substr(sp + 1);
                } else {
                    exe = raw;
                }
            }

            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.hwnd = hwnd;
            sei.lpVerb = runAdmin ? L"runas" : L"open";
            sei.lpFile = exe.c_str();
            sei.lpParameters = params.empty() ? nullptr : params.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) {
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void ShowRunNewTaskDialog(HWND parent, const AppSettings& settings) {
    RunTaskCtx ctx{ &settings, L"" };
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RUN_TASK),
                    parent, RunTaskDlgProc, reinterpret_cast<LPARAM>(&ctx));
}

// ============================================================================
// Processor Affinity Dialog
// ============================================================================
struct AffinityCtx {
    DWORD pid = 0;
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    const AppSettings* settings = nullptr;
    bool updating = false;
};

static INT_PTR CALLBACK AffinityDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AffinityCtx*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<AffinityCtx*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        const auto& s = GetStrings(ctx->settings->langMode);
        SetWindowTextW(hwnd, s.dlgAffinityTitle);
        SetDlgItemTextW(hwnd, IDC_AFFINITY_DESC, s.dlgAffinityDesc);
        SetDlgItemTextW(hwnd, IDC_AFFINITY_ALL, s.dlgAllProcessors);
        SetDlgItemTextW(hwnd, IDOK, s.dlgOk);
        SetDlgItemTextW(hwnd, IDCANCEL, s.dlgCancel);

        HWND hList = GetDlgItem(hwnd, IDC_AFFINITY_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{};
        col.mask = LVCF_WIDTH;
        col.cx = DpiScale(hwnd, 280);
        ListView_InsertColumn(hList, 0, &col);

        ctx->updating = true;
        int totalCores = 0;
        int checkedCores = 0;
        for (int i = 0; i < 64; ++i) {
            DWORD_PTR bit = (static_cast<DWORD_PTR>(1) << i);
            if (!(ctx->sysMask & bit)) continue;
            wchar_t label[64];
            swprintf_s(label, L"CPU %d", i);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = totalCores;
            item.pszText = label;
            item.lParam = static_cast<LPARAM>(i);
            int idx = ListView_InsertItem(hList, &item);
            bool chk = (ctx->procMask & bit) != 0;
            ListView_SetCheckState(hList, idx, chk ? TRUE : FALSE);
            totalCores++;
            if (chk) checkedCores++;
        }
        CheckDlgButton(hwnd, IDC_AFFINITY_ALL, (checkedCores == totalCores) ? BST_CHECKED : BST_UNCHECKED);
        ctx->updating = false;

        ThemeDialogWindow(hwnd);
        const auto& pal = ThemeManager::Instance().Palette();
        ListView_SetBkColor(hList, pal.surfaceBg);
        ListView_SetTextBkColor(hList, pal.surfaceBg);
        ListView_SetTextColor(hList, pal.textPrimary);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HBRUSH br = ThemeManager::Instance().HandleCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        if (br) return reinterpret_cast<INT_PTR>(br);
        break;
    }
    case WM_NOTIFY: {
        if (ctx && !ctx->updating) {
            auto* nm = reinterpret_cast<NMHDR*>(lParam);
            if (nm->idFrom == IDC_AFFINITY_LIST && nm->code == LVN_ITEMCHANGED) {
                auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (nmlv->uChanged & LVIF_STATE) {
                    HWND hList = nm->hwndFrom;
                    int cnt = ListView_GetItemCount(hList);
                    int checked = 0;
                    for (int i = 0; i < cnt; ++i) {
                        if (ListView_GetCheckState(hList, i)) checked++;
                    }
                    ctx->updating = true;
                    CheckDlgButton(hwnd, IDC_AFFINITY_ALL, (checked == cnt) ? BST_CHECKED : BST_UNCHECKED);
                    ctx->updating = false;
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDC_AFFINITY_ALL && ctx && !ctx->updating) {
            bool all = (IsDlgButtonChecked(hwnd, IDC_AFFINITY_ALL) == BST_CHECKED);
            HWND hList = GetDlgItem(hwnd, IDC_AFFINITY_LIST);
            int cnt = ListView_GetItemCount(hList);
            ctx->updating = true;
            for (int i = 0; i < cnt; ++i) {
                ListView_SetCheckState(hList, i, all ? TRUE : FALSE);
            }
            ctx->updating = false;
            return TRUE;
        }
        if (id == IDOK && ctx) {
            HWND hList = GetDlgItem(hwnd, IDC_AFFINITY_LIST);
            int cnt = ListView_GetItemCount(hList);
            DWORD_PTR newMask = 0;
            for (int i = 0; i < cnt; ++i) {
                if (ListView_GetCheckState(hList, i)) {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = i;
                    if (ListView_GetItem(hList, &item)) {
                        newMask |= (static_cast<DWORD_PTR>(1) << static_cast<int>(item.lParam));
                    }
                }
            }
            if (newMask == 0) return TRUE;
            if (!ProcessMonitor::SetAffinity(ctx->pid, newMask)) {
                MessageBoxW(hwnd, GetStrings(ctx->settings->langMode).msgAccessDenied,
                            GetStrings(ctx->settings->langMode).appTitle, MB_ICONERROR);
                return TRUE;
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void ShowAffinityDialog(HWND parent, DWORD pid, const std::wstring& /*procName*/, const AppSettings& settings) {
    DWORD_PTR pMask = 0, sMask = 0;
    if (!ProcessMonitor::GetAffinity(pid, pMask, sMask)) {
        MessageBoxW(parent, GetStrings(settings.langMode).msgAccessDenied,
                    GetStrings(settings.langMode).appTitle, MB_ICONERROR);
        return;
    }
    AffinityCtx ctx{ pid, pMask, sMask, &settings, false };
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_AFFINITY),
                    parent, AffinityDlgProc, reinterpret_cast<LPARAM>(&ctx));
}

// ============================================================================
// Select Columns Dialog
// ============================================================================
struct SelectColsCtx {
    AppSettings* settings = nullptr;
};

static INT_PTR CALLBACK SelectColsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SelectColsCtx*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<SelectColsCtx*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        const auto& s = GetStrings(ctx->settings->langMode);
        SetWindowTextW(hwnd, s.dlgSelectColsTitle);
        SetDlgItemTextW(hwnd, IDC_COLS_DESC, s.dlgSelectColsDesc);
        SetDlgItemTextW(hwnd, IDOK, s.dlgOk);
        SetDlgItemTextW(hwnd, IDCANCEL, s.dlgCancel);

        const wchar_t* names[DCOL_COUNT] = {
            s.colName, s.colPid, s.colStatus, s.colUserName, s.colCpu,
            s.colMemory, s.colWorkingSet, s.colCommitSize, s.colThreads, s.colHandles,
            s.colSessionId, s.colPlatform, s.colElevated, s.colUacVirt,
            s.colCommandLine, s.colDescription
        };

        HWND hList = GetDlgItem(hwnd, IDC_COLS_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{};
        col.mask = LVCF_WIDTH;
        col.cx = DpiScale(hwnd, 280);
        ListView_InsertColumn(hList, 0, &col);

        for (int i = 0; i < DCOL_COUNT; ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = const_cast<LPWSTR>(names[i]);
            ListView_InsertItem(hList, &item);
            bool vis = (ctx->settings->detailsColMask & (1u << i)) != 0;
            ListView_SetCheckState(hList, i, vis ? TRUE : FALSE);
        }

        ThemeDialogWindow(hwnd);
        const auto& pal = ThemeManager::Instance().Palette();
        ListView_SetBkColor(hList, pal.surfaceBg);
        ListView_SetTextBkColor(hList, pal.surfaceBg);
        ListView_SetTextColor(hList, pal.textPrimary);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HBRUSH br = ThemeManager::Instance().HandleCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        if (br) return reinterpret_cast<INT_PTR>(br);
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDOK && ctx) {
            HWND hList = GetDlgItem(hwnd, IDC_COLS_LIST);
            uint32_t mask = (1u << DCOL_NAME); // Always keep Name
            for (int i = 1; i < DCOL_COUNT; ++i) {
                if (ListView_GetCheckState(hList, i)) mask |= (1u << i);
            }
            ctx->settings->detailsColMask = mask;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

bool ShowSelectColumnsDialog(HWND parent, AppSettings& settings) {
    SelectColsCtx ctx{ &settings };
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SELECT_COLUMNS),
                           parent, SelectColsDlgProc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

// ============================================================================
// About Dialog
// ============================================================================
static INT_PTR CALLBACK AboutDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* settings = reinterpret_cast<const AppSettings*>(lParam);
        const auto& s = GetStrings(settings->langMode);
        SetWindowTextW(hwnd, s.dlgAboutTitle);
        wchar_t aboutBuf[512] = {};
        swprintf_s(aboutBuf, s.dlgAboutBody, APP_BUILD_TAG_W);
        SetDlgItemTextW(hwnd, IDC_ABOUT_TEXT, aboutBuf);
        SetDlgItemTextW(hwnd, IDOK, s.dlgOk);
        ThemeDialogWindow(hwnd);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HBRUSH br = ThemeManager::Instance().HandleCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        if (br) return reinterpret_cast<INT_PTR>(br);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowAboutDialog(HWND parent, const AppSettings& settings) {
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT),
                    parent, AboutDlgProc, reinterpret_cast<LPARAM>(&settings));
}

} // namespace taskman

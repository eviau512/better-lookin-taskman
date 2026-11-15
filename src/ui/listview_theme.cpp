#include "listview_theme.h"
#include "../core/theme.h"
#include <algorithm>
#include <cmath>

namespace taskman {

static constexpr UINT_PTR kListSubclassId = 101;
static constexpr UINT_PTR kHeaderSubclassId = 102;
static constexpr UINT_PTR kColSaveTimerId = 103;

void ThemedListView::Attach(HWND hList, bool twoLineHeader) {
    Detach();
    hList_ = hList;
    twoLineHeader_ = twoLineHeader;

    if (hList_) {
        LONG_PTR style = GetWindowLongPtrW(hList_, GWL_STYLE);
        if (!(style & WS_CLIPSIBLINGS)) {
            SetWindowLongPtrW(hList_, GWL_STYLE, style | WS_CLIPSIBLINGS);
            SetWindowPos(hList_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    ListView_SetExtendedListViewStyle(
        hList_,
        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);

    SetWindowSubclass(hList_, ListSubclassProc, kListSubclassId, reinterpret_cast<DWORD_PTR>(this));

    hHeader_ = ListView_GetHeader(hList_);
    bool noColHeader = hList_ && ((GetWindowLongPtrW(hList_, GWL_STYLE) & LVS_NOCOLUMNHEADER) != 0);
    if (hHeader_) {
        SetWindowSubclass(hHeader_, HeaderSubclassProc, kHeaderSubclassId, reinterpret_cast<DWORD_PTR>(this));
        if (noColHeader) {
            ShowWindow(hHeader_, SW_HIDE);
        }
    }

    ApplyTheme();
}

void ThemedListView::Detach() {
    if (hHeader_) {
        RemoveWindowSubclass(hHeader_, HeaderSubclassProc, kHeaderSubclassId);
        hHeader_ = nullptr;
    }
    if (hList_) {
        KillTimer(hList_, kColSaveTimerId);
        ListView_SetImageList(hList_, nullptr, LVSIL_SMALL);
        RemoveWindowSubclass(hList_, ListSubclassProc, kListSubclassId);
        hList_ = nullptr;
    }
    fontMain_ = nullptr;
    fontBold_ = nullptr;
    fontTotal_ = nullptr;
    fontGroup_ = nullptr;
    if (hRowHeightImg_) {
        ImageList_Destroy(hRowHeightImg_);
        hRowHeightImg_ = nullptr;
    }
}

void ThemedListView::ApplyTheme() {
    if (!hList_) return;
    auto& tm = ThemeManager::Instance();
    const auto& pal = tm.Palette();

    tm.ApplyToControl(hList_);
    ListView_SetBkColor(hList_, pal.surfaceBg);
    ListView_SetTextBkColor(hList_, pal.surfaceBg);
    ListView_SetTextColor(hList_, pal.textPrimary);

    if (hHeader_) {
        tm.ApplyToControl(hHeader_);
    }

    fontMain_ = tm.GetFont(hList_, 10, false);
    fontBold_ = tm.GetFont(hList_, 10, true);
    fontTotal_ = tm.GetFont(hList_, 15, false);
    fontGroup_ = tm.GetFont(hList_, 12, false);

    HIMAGELIST newImg = ImageList_Create(1, DpiScale(hList_, 27), ILC_COLOR32, 1, 1);
    if (newImg) {
        ListView_SetImageList(hList_, newImg, LVSIL_SMALL);
        if (hRowHeightImg_) ImageList_Destroy(hRowHeightImg_);
        hRowHeightImg_ = newImg;
    }

    SendMessageW(hList_, WM_SETFONT, reinterpret_cast<WPARAM>(fontMain_), FALSE);
    InvalidateRect(hList_, nullptr, TRUE);
    if (hHeader_) InvalidateRect(hHeader_, nullptr, TRUE);
}

void ThemedListView::SetColumnsMeta(std::vector<HeaderColumnMeta> cols) {
    colsMeta_ = std::move(cols);
    if (hHeader_) InvalidateRect(hHeader_, nullptr, FALSE);
}

void ThemedListView::UpdateHeaderTotals(const std::vector<std::wstring>& totals) {
    for (size_t i = 0; i < colsMeta_.size() && i < totals.size(); ++i) {
        colsMeta_[i].totalText = totals[i];
    }
    if (hHeader_) InvalidateRect(hHeader_, nullptr, FALSE);
}

void ThemedListView::SetSortIndicator(int col, bool ascending) {
    sortCol_ = col;
    sortAsc_ = ascending;
    if (hHeader_) InvalidateRect(hHeader_, nullptr, FALSE);
}

bool ThemedListView::HitTestChevron(int row, POINT clientPt) const {
    if (!hList_ || !cb_.getRowExpandState) return false;
    int exp = cb_.getRowExpandState(row);
    if (exp < 0) return false;

    int indent = cb_.getRowIndent ? cb_.getRowIndent(row) : 1;
    int indentPx = (indent >= 2) ? DpiScale(hList_, 24) : DpiScale(hList_, 4);
    RECT rc{};
    ListView_GetItemRect(hList_, row, &rc, LVIR_BOUNDS);
    int chevLeft = rc.left + indentPx;
    int chevRight = chevLeft + DpiScale(hList_, 22);
    return clientPt.x >= chevLeft && clientPt.x <= chevRight;
}

LRESULT CALLBACK ThemedListView::ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                  UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<ThemedListView*>(dwRefData);
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        LVHITTESTINFO hti{};
        hti.pt = pt;
        int row = ListView_HitTest(hwnd, &hti);
        if (row != self->hoveredRow_) {
            int old = self->hoveredRow_;
            self->hoveredRow_ = row;
            if (old >= 0) ListView_RedrawItems(hwnd, old, old);
            if (row >= 0) ListView_RedrawItems(hwnd, row, row);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        if (self->hoveredRow_ >= 0) {
            int old = self->hoveredRow_;
            self->hoveredRow_ = -1;
            ListView_RedrawItems(hwnd, old, old);
        }
        break;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (LOWORD(wParam) & MK_CONTROL) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int absDelta = std::abs(delta);
        ULONGLONG now = GetTickCount64();
        if (now - self->lastWheelTick_ > 180) {
            self->wheelLockedAxis_ = 0;
            self->wheelAccumV_ = 0;
            self->wheelAccumH_ = 0;
        }
        self->lastWheelTick_ = now;

        const bool isHorz = (msg == WM_MOUSEHWHEEL) || ((LOWORD(wParam) & MK_SHIFT) != 0);
        if (isHorz) {
            self->wheelAccumH_ += absDelta;
        } else {
            self->wheelAccumV_ += absDelta;
        }

        // Determine or update dominant axis lock for touchpad two-finger swipe
        if (self->wheelLockedAxis_ == 0) {
            if (self->wheelAccumV_ + self->wheelAccumH_ >= 12) {
                if (self->wheelAccumV_ >= self->wheelAccumH_ * 3 / 2) {
                    self->wheelLockedAxis_ = 1;
                } else if (self->wheelAccumH_ >= self->wheelAccumV_ * 3 / 2) {
                    self->wheelLockedAxis_ = 2;
                }
            }
        } else if (self->wheelLockedAxis_ == 1 && isHorz && absDelta >= 60 && self->wheelAccumH_ > self->wheelAccumV_ * 2) {
            self->wheelLockedAxis_ = 2;
        } else if (self->wheelLockedAxis_ == 2 && !isHorz && absDelta >= 60 && self->wheelAccumV_ > self->wheelAccumH_ * 2) {
            self->wheelLockedAxis_ = 1;
        }

        // Suppress cross-axis drift during a locked or dominant single-axis swipe
        if (isHorz && (self->wheelLockedAxis_ == 1 || (self->wheelLockedAxis_ == 0 && self->wheelAccumV_ > self->wheelAccumH_))) {
            return 0;
        }
        if (!isHorz && (self->wheelLockedAxis_ == 2 || (self->wheelLockedAxis_ == 0 && self->wheelAccumH_ > self->wheelAccumV_))) {
            return 0;
        }

        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (isHorz) {
            InvalidateRect(hwnd, nullptr, FALSE);
            if (self->hHeader_) InvalidateRect(self->hHeader_, nullptr, FALSE);
        }
        return res;
    }
    case WM_HSCROLL: {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (self->hHeader_) InvalidateRect(self->hHeader_, nullptr, FALSE);
        return res;
    }
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, ThemeManager::Instance().SurfaceBrush());
        return 1;
    }
    case LVM_SETCOLUMNWIDTH: {
        int col = static_cast<int>(wParam);
        int cx = static_cast<int>(static_cast<INT_PTR>(lParam));
        if (cx == LVSCW_AUTOSIZE || cx == LVSCW_AUTOSIZE_USEHEADER) {
            int fitW = self->AutoFitColumnWidth(col, cx == LVSCW_AUTOSIZE_USEHEADER);
            if (fitW > 0) {
                LRESULT res = DefSubclassProc(hwnd, msg, wParam, static_cast<LPARAM>(fitW));
                InvalidateRect(hwnd, nullptr, FALSE);
                if (self->hHeader_) InvalidateRect(self->hHeader_, nullptr, FALSE);
                return res;
            }
        }
        break;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (nm && self->hHeader_ && nm->hwndFrom == self->hHeader_ &&
            (nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_DIVIDERDBLCLICKA)) {
            auto* nmh = reinterpret_cast<const NMHEADERW*>(lParam);
            if (nmh) {
                int fitW = self->AutoFitColumnWidth(nmh->iItem, true);
                if (fitW > 0) {
                    ListView_SetColumnWidth(hwnd, nmh->iItem, fitW);
                    KillTimer(hwnd, kColSaveTimerId);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    InvalidateRect(self->hHeader_, nullptr, FALSE);
                    if (self->cb_.onHeaderTrack) {
                        self->cb_.onHeaderTrack();
                    } else {
                        SendMessageW(GetParent(hwnd), WM_NOTIFY,
                                     static_cast<WPARAM>(GetDlgCtrlID(hwnd)), lParam);
                    }
                }
            }
            return 0;
        }
        if (IsHeaderTrackNotify(nm, self->hHeader_)) {
            KillTimer(hwnd, kColSaveTimerId);
            LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (self->cb_.onHeaderTrack) {
                self->cb_.onHeaderTrack();
            } else {
                SendMessageW(GetParent(hwnd), WM_NOTIFY,
                             static_cast<WPARAM>(GetDlgCtrlID(hwnd)), lParam);
            }
            return res;
        }
        if (nm && self->hHeader_ && nm->hwndFrom == self->hHeader_ &&
            (nm->code == HDN_ITEMCHANGEDW || nm->code == HDN_ITEMCHANGEDA)) {
            auto* nmh = reinterpret_cast<NMHEADERW*>(lParam);
            if (nmh && nmh->pitem && (nmh->pitem->mask & HDI_WIDTH)) {
                LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
                SetTimer(hwnd, kColSaveTimerId, 250, nullptr);
                return res;
            }
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == kColSaveTimerId) {
            KillTimer(hwnd, kColSaveTimerId);
            if (self->cb_.onHeaderTrack) {
                self->cb_.onHeaderTrack();
            }
            return 0;
        }
        break;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ThemedListView::HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<ThemedListView*>(dwRefData);
    switch (msg) {
    case HDM_LAYOUT: {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (self->hList_ && ((GetWindowLongPtrW(self->hList_, GWL_STYLE) & LVS_NOCOLUMNHEADER) != 0)) {
            auto* pLayout = reinterpret_cast<HDLAYOUT*>(lParam);
            pLayout->pwpos->cy = 0;
            pLayout->pwpos->flags |= SWP_HIDEWINDOW;
            pLayout->prc->top = 0;
            return res;
        }
        if (self->twoLineHeader_) {
            auto* pLayout = reinterpret_cast<HDLAYOUT*>(lParam);
            int h = DpiScale(hwnd, 56);
            pLayout->pwpos->cy = h;
            pLayout->prc->top = h;
        } else {
            auto* pLayout = reinterpret_cast<HDLAYOUT*>(lParam);
            int h = DpiScale(hwnd, 30);
            pLayout->pwpos->cy = h;
            pLayout->prc->top = h;
        }
        return res;
    }
    case WM_MOUSEMOVE: {
        HDHITTESTINFO hti{};
        hti.pt.x = static_cast<short>(LOWORD(lParam));
        hti.pt.y = static_cast<short>(HIWORD(lParam));
        int idx = static_cast<int>(SendMessageW(hwnd, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hti)));
        if (idx != self->hoveredHeaderCol_) {
            self->hoveredHeaderCol_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        if (self->hoveredHeaderCol_ >= 0) {
            self->hoveredHeaderCol_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
        HGDIOBJ oldBmp = SelectObject(memDc, bmp);
        self->DrawHeader(memDc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void ThemedListView::DrawHeader(HDC hdc) {
    RECT rcClient{};
    GetClientRect(hHeader_, &rcClient);
    const auto& pal = ThemeManager::Instance().Palette();

    HBRUSH brBg = CreateSolidBrush(pal.headerBg);
    FillRect(hdc, &rcClient, brBg);
    DeleteObject(brBg);

    int count = Header_GetItemCount(hHeader_);
    HPEN sepPen = CreatePen(PS_SOLID, 1, pal.headerBorder);
    HGDIOBJ oldPen = SelectObject(hdc, sepPen);
    int oldBk = SetBkMode(hdc, TRANSPARENT);

    int padX = DpiScale(hHeader_, 6);

    for (int i = 0; i < count; ++i) {
        RECT rcItem{};
        if (!Header_GetItemRect(hHeader_, i, &rcItem)) continue;

        if (i == hoveredHeaderCol_) {
            HBRUSH brHov = CreateSolidBrush(pal.headerHoverBg);
            FillRect(hdc, &rcItem, brHov);
            DeleteObject(brHov);
        }

        // Right divider
        MoveToEx(hdc, rcItem.right - 1, rcItem.top + DpiScale(hHeader_, 3), nullptr);
        LineTo(hdc, rcItem.right - 1, rcItem.bottom - DpiScale(hHeader_, 3));

        std::wstring title;
        std::wstring totalStr;
        bool rightAlign = false;
        if (i < static_cast<int>(colsMeta_.size())) {
            title = colsMeta_[static_cast<size_t>(i)].title;
            totalStr = colsMeta_[static_cast<size_t>(i)].totalText;
            rightAlign = colsMeta_[static_cast<size_t>(i)].rightAlign;
        } else {
            wchar_t buf[128] = {};
            HDITEMW hdi{};
            hdi.mask = HDI_TEXT | HDI_FORMAT;
            hdi.pszText = buf;
            hdi.cchTextMax = 128;
            if (Header_GetItem(hHeader_, i, &hdi)) {
                title = buf;
                rightAlign = (hdi.fmt & HDF_RIGHT) != 0;
            }
        }

        // Draw sort chevron at top center
        if (i == sortCol_) {
            int cx = (rcItem.left + rcItem.right) / 2;
            int cy = rcItem.top + DpiScale(hHeader_, 5);
            int sz = DpiScale(hHeader_, 3);
            HPEN arrowPen = CreatePen(PS_SOLID, 1, pal.textSecondary);
            HGDIOBJ prevP = SelectObject(hdc, arrowPen);
            if (sortAsc_) {
                MoveToEx(hdc, cx - sz, cy + sz, nullptr);
                LineTo(hdc, cx, cy);
                LineTo(hdc, cx + sz + 1, cy + sz + 1);
            } else {
                MoveToEx(hdc, cx - sz, cy, nullptr);
                LineTo(hdc, cx, cy + sz);
                LineTo(hdc, cx + sz + 1, cy - 1);
            }
            SelectObject(hdc, prevP);
            DeleteObject(arrowPen);
        }

        RECT rcText = rcItem;
        rcText.left += padX;
        rcText.right -= padX;
        UINT alignFlag = rightAlign ? DT_RIGHT : DT_LEFT;

        auto& tm = ThemeManager::Instance();
        if (twoLineHeader_) {
            if (!totalStr.empty()) {
                RECT rcTop = rcText;
                rcTop.top += DpiScale(hHeader_, 6);
                rcTop.bottom = rcTop.top + DpiScale(hHeader_, 25);
                tm.DrawModernText(hdc, hHeader_, 15, false, totalStr.c_str(), rcTop,
                                  alignFlag | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                                  pal.textPrimary);
            }
            RECT rcBot = rcText;
            rcBot.top = rcItem.bottom - DpiScale(hHeader_, 22);
            rcBot.bottom = rcItem.bottom - DpiScale(hHeader_, 4);
            tm.DrawModernText(hdc, hHeader_, 10, false, title.c_str(), rcBot,
                              alignFlag | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                              pal.textSecondary);
        } else {
            tm.DrawModernText(hdc, hHeader_, 10, false, title.c_str(), rcText,
                              alignFlag | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                              pal.textSecondary);
        }
    }

    // Bottom horizontal border
    MoveToEx(hdc, rcClient.left, rcClient.bottom - 1, nullptr);
    LineTo(hdc, rcClient.right, rcClient.bottom - 1);

    SetBkMode(hdc, oldBk);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);
}

LRESULT ThemedListView::HandleCustomDraw(NMLVCUSTOMDRAW* cd) {
    if (!cd) return CDRF_DODEFAULT;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        DrawRow(cd);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

void ThemedListView::DrawRow(NMLVCUSTOMDRAW* cd) {
    int row = static_cast<int>(cd->nmcd.dwItemSpec);
    HDC hdc = cd->nmcd.hdc;

    RECT rcRow{};
    ListView_GetItemRect(hList_, row, &rcRow, LVIR_BOUNDS);
    // Keep full row width clipped properly
    const auto& pal = ThemeManager::Instance().Palette();

    bool selected = (ListView_GetItemState(hList_, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    bool focused = (GetFocus() == hList_);
    bool hovered = (row == hoveredRow_);
    bool isCat = cb_.isCategoryHeader ? cb_.isCategoryHeader(row) : false;
    bool inExpGroup = (!isCat && cb_.isExpandedGroupRow) ? cb_.isExpandedGroupRow(row) : false;

    COLORREF baseBg = inExpGroup ? pal.groupExpandedBg : pal.surfaceBg;
    if (selected && !isCat) {
        baseBg = focused ? pal.rowSelectedBg : pal.rowSelectedUnfocusedBg;
    } else if (hovered) {
        baseBg = pal.rowHoverBg;
    }

    HBRUSH brRow = CreateSolidBrush(baseBg);
    FillRect(hdc, &rcRow, brRow);
    DeleteObject(brRow);

    int colCount = Header_GetItemCount(hHeader_);
    int oldBk = SetBkMode(hdc, TRANSPARENT);
    int padX = DpiScale(hList_, 6);

    // Draw column order properly even if columns are dragged
    std::vector<int> order(static_cast<size_t>(colCount));
    if (colCount > 0) ListView_GetColumnOrderArray(hList_, colCount, order.data());

    for (int visIdx = 0; visIdx < colCount; ++visIdx) {
        int col = order[static_cast<size_t>(visIdx)];
        RECT rcSub{};
        if (col == 0) {
            ListView_GetItemRect(hList_, row, &rcSub, LVIR_BOUNDS);
            // Adjust right edge to column 0 width
            LVCOLUMNW lvc{};
            lvc.mask = LVCF_WIDTH;
            ListView_GetColumn(hList_, 0, &lvc);
            rcSub.right = rcSub.left + lvc.cx;
        } else {
            ListView_GetSubItemRect(hList_, row, col, LVIR_BOUNDS, &rcSub);
        }

        if (isCat) {
            if (col == 0) {
                RECT rcFull = rcRow;
                rcFull.left += DpiScale(hList_, 8);
                int exp = cb_.getRowExpandState ? cb_.getRowExpandState(row) : -1;
                if (exp >= 0) {
                    int chevSz = DpiScale(hList_, 5);
                    int cx = rcFull.left + DpiScale(hList_, 6);
                    int cy = (rcFull.top + rcFull.bottom) / 2;
                    HPEN chevPen = CreatePen(PS_SOLID, 1, pal.groupHeaderText);
                    HGDIOBJ oldP = SelectObject(hdc, chevPen);
                    if (exp == 1) {
                        MoveToEx(hdc, cx - chevSz, cy - chevSz / 2, nullptr);
                        LineTo(hdc, cx, cy + chevSz / 2 + 1);
                        LineTo(hdc, cx + chevSz + 1, cy - chevSz / 2 - 1);
                    } else {
                        MoveToEx(hdc, cx - chevSz / 2, cy - chevSz, nullptr);
                        LineTo(hdc, cx + chevSz / 2 + 1, cy);
                        LineTo(hdc, cx - chevSz / 2 - 1, cy + chevSz + 1);
                    }
                    SelectObject(hdc, oldP);
                    DeleteObject(chevPen);
                    rcFull.left += DpiScale(hList_, 19);
                }
                std::wstring text = cb_.getCellText ? cb_.getCellText(row, 0) : L"";
                auto& tm = ThemeManager::Instance();
                tm.DrawModernText(hdc, hList_, 12, false, text.c_str(), rcFull,
                                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                                  pal.groupHeaderText);

                // Subtle separator line extending to the right of category title
                int textW = tm.MeasureTextWidth(hdc, hList_, 12, false, text.c_str(), static_cast<int>(text.size()));
                int lineLeft = rcFull.left + textW + DpiScale(hList_, 10);
                if (lineLeft < rcRow.right - DpiScale(hList_, 8)) {
                    HPEN sepPen = CreatePen(PS_SOLID, 1, pal.borderColor);
                    HGDIOBJ oldP = SelectObject(hdc, sepPen);
                    int lineY = (rcRow.top + rcRow.bottom) / 2;
                    MoveToEx(hdc, lineLeft, lineY, nullptr);
                    LineTo(hdc, rcRow.right - DpiScale(hList_, 8), lineY);
                    SelectObject(hdc, oldP);
                    DeleteObject(sepPen);
                }
            }
            continue;
        }

        // Heatmap cell shading (only when not selected or hovered)
        int heat = (!selected && cb_.getCellHeat) ? cb_.getCellHeat(row, col) : 0;
        if (heat > 0 && heat <= 4) {
            RECT rcHeat = rcSub;
            rcHeat.left += 1;
            rcHeat.bottom -= 1;
            HBRUSH brHeat = CreateSolidBrush(pal.heat[heat]);
            FillRect(hdc, &rcHeat, brHeat);
            DeleteObject(brHeat);
        }

        RECT rcCell = rcSub;
        rcCell.left += padX;
        rcCell.right -= padX;

        if (col == 0) {
            int indent = cb_.getRowIndent ? cb_.getRowIndent(row) : 1;
            if (indent >= 2) rcCell.left += DpiScale(hList_, 20);
            else rcCell.left += DpiScale(hList_, 2);

            int exp = cb_.getRowExpandState ? cb_.getRowExpandState(row) : -1;
            int chevW = DpiScale(hList_, 16);
            if (exp >= 0) {
                int cx = rcCell.left + DpiScale(hList_, 6);
                int cy = (rcCell.top + rcCell.bottom) / 2;
                int sz = DpiScale(hList_, 3);
                HPEN chevPen = CreatePen(PS_SOLID, 1, pal.textSecondary);
                HGDIOBJ oldP = SelectObject(hdc, chevPen);
                if (exp == 1) {
                    MoveToEx(hdc, cx - sz, cy - 1, nullptr);
                    LineTo(hdc, cx, cy + sz);
                    LineTo(hdc, cx + sz + 1, cy - 2);
                } else {
                    MoveToEx(hdc, cx - 1, cy - sz, nullptr);
                    LineTo(hdc, cx + sz, cy);
                    LineTo(hdc, cx - 2, cy + sz + 1);
                }
                SelectObject(hdc, oldP);
                DeleteObject(chevPen);
            }
            rcCell.left += chevW;

            HICON ico = cb_.getRowIcon ? cb_.getRowIcon(row) : nullptr;
            int icoSz = DpiScale(hList_, 16);
            if (ico) {
                int icoY = (rcCell.top + rcCell.bottom - icoSz) / 2;
                DrawIconEx(hdc, rcCell.left, icoY, ico, icoSz, icoSz, 0, nullptr, DI_NORMAL);
            }
            rcCell.left += icoSz + DpiScale(hList_, 6);
        }

        std::wstring text = cb_.getCellText ? cb_.getCellText(row, col) : L"";
        bool overrideColor = false;
        COLORREF fg = pal.textPrimary;
        if (cb_.getCellTextColor) {
            COLORREF customFg = cb_.getCellTextColor(row, col, overrideColor);
            if (overrideColor) fg = customFg;
        }

        bool rightAlign = (col < static_cast<int>(colsMeta_.size())) ? colsMeta_[static_cast<size_t>(col)].rightAlign : false;
        UINT alignFlag = rightAlign ? DT_RIGHT : DT_LEFT;

        if (rcCell.right > rcCell.left) {
            ThemeManager::Instance().DrawModernText(
                hdc, hList_, 10, false, text.c_str(), rcCell,
                alignFlag | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, fg);
        }
    }

    if (selected && !isCat && focused) {
        HPEN selPen = CreatePen(PS_SOLID, 1, pal.rowSelectedBorder);
        HGDIOBJ oldP = SelectObject(hdc, selPen);
        HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rcRow.left, rcRow.top, rcRow.right, rcRow.bottom);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(selPen);
    }

    SetBkMode(hdc, oldBk);
}

int ThemedListView::AutoFitColumnWidth(int col, bool includeHeader) {
    if (!hList_ || col < 0) return 0;
    int colCount = hHeader_ ? Header_GetItemCount(hHeader_) : 0;
    if (colCount > 0 && col >= colCount) return 0;

    HDC hdc = GetDC(hList_);
    if (!hdc) return 0;

    auto& tm = ThemeManager::Instance();
    int padX = DpiScale(hList_, 6);
    int bestW = DpiScale(hList_, 36);

    if (includeHeader) {
        std::wstring title;
        std::wstring totalStr;
        if (col < static_cast<int>(colsMeta_.size())) {
            title = colsMeta_[static_cast<size_t>(col)].title;
            totalStr = colsMeta_[static_cast<size_t>(col)].totalText;
        } else if (hHeader_) {
            wchar_t buf[128] = {};
            HDITEMW hdi{};
            hdi.mask = HDI_TEXT;
            hdi.pszText = buf;
            hdi.cchTextMax = 128;
            if (Header_GetItem(hHeader_, col, &hdi)) title = buf;
        }
        HWND hHdrRef = hHeader_ ? hHeader_ : hList_;
        int hdrTextW = 0;
        if (!title.empty()) {
            hdrTextW = (std::max)(hdrTextW,
                tm.MeasureTextWidth(hdc, hHdrRef, 10, false, title.c_str(), static_cast<int>(title.size())));
        }
        if (twoLineHeader_ && !totalStr.empty()) {
            hdrTextW = (std::max)(hdrTextW,
                tm.MeasureTextWidth(hdc, hHdrRef, 15, false, totalStr.c_str(), static_cast<int>(totalStr.size())));
        }
        if (hdrTextW > 0) {
            bestW = (std::max)(bestW, padX * 2 + hdrTextW + DpiScale(hList_, 6));
        }
    }

    int rowCount = ListView_GetItemCount(hList_);
    wchar_t cellBuf[512]{};
    for (int row = 0; row < rowCount; ++row) {
        if (cb_.isCategoryHeader && cb_.isCategoryHeader(row)) continue;

        std::wstring text;
        if (cb_.getCellText) {
            text = cb_.getCellText(row, col);
        } else {
            cellBuf[0] = L'\0';
            ListView_GetItemText(hList_, row, col, cellBuf, 512);
            text = cellBuf;
        }
        if (text.empty() && col != 0) continue;

        int prefixW = 0;
        if (col == 0) {
            int indent = cb_.getRowIndent ? cb_.getRowIndent(row) : 1;
            prefixW += (indent >= 2) ? DpiScale(hList_, 20) : DpiScale(hList_, 2);
            prefixW += DpiScale(hList_, 16); // chevW
            prefixW += DpiScale(hList_, 16) + DpiScale(hList_, 6); // icoSz + gap
        }

        int textW = text.empty()
            ? 0
            : tm.MeasureTextWidth(hdc, hList_, 10, false, text.c_str(), static_cast<int>(text.size()));
        int rowW = padX * 2 + prefixW + textW + DpiScale(hList_, 6);
        if (rowW > bestW) bestW = rowW;
    }

    ReleaseDC(hList_, hdc);
    return std::clamp(bestW, DpiScale(hList_, 36), DpiScale(hList_, 640));
}

int ThemedListView::WindowDpi(HWND hwnd) {
    int dpi = DpiScale(hwnd, 96);
    return dpi > 0 ? dpi : 96;
}

void ThemedListView::SaveColumnsToArray(HWND hList, const int* colToSlot, int count, int* outWidths, int maxSlots) {
    if (!hList || !outWidths || maxSlots <= 0) return;
    HWND hHdr = ListView_GetHeader(hList);
    int actualCols = hHdr ? Header_GetItemCount(hHdr) : 0;
    int dpi = WindowDpi(hList);
    int limit = (count < actualCols) ? count : actualCols;
    for (int i = 0; i < limit; ++i) {
        int slot = colToSlot ? colToSlot[i] : i;
        if (slot < 0 || slot >= maxSlots) continue;
        int wPx = ListView_GetColumnWidth(hList, i);
        if (wPx > 0) {
            int w96 = MulDiv(wPx, 96, dpi);
            outWidths[slot] = std::clamp(w96, 24, 2400);
        }
    }
}

bool ThemedListView::IsHeaderTrackNotify(const NMHDR* nm, HWND hHeader) {
    if (!nm || !hHeader || nm->hwndFrom != hHeader) return false;
    return nm->code == HDN_ENDTRACKW || nm->code == HDN_ENDTRACKA ||
           nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_DIVIDERDBLCLICKA;
}

} // namespace taskman

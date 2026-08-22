#include "Controls.h"

#include "DpiHelper.h"

#include <algorithm>

namespace od::ui {

namespace {

/// RAII for a GDI object selected into a DC.
class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) : m_dc(dc), m_previous(::SelectObject(dc, object)) {}
    ~SelectedObject() { ::SelectObject(m_dc, m_previous); }

    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;

private:
    HDC m_dc;
    HGDIOBJ m_previous;
};

} // namespace

void Controls::InitializeCommonControls() {
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&common);
}

void Controls::DrawProgressCell(HDC hdc,
                                const RECT& cellRect,
                                double percent,
                                const std::wstring& label,
                                const ProgressCellStyle& style,
                                HFONT font,
                                int dpi) {
    const int cellWidth = cellRect.right - cellRect.left;
    const int cellHeight = cellRect.bottom - cellRect.top;
    if (cellWidth <= 0 || cellHeight <= 0) {
        return;
    }

    HDC memoryDc = ::CreateCompatibleDC(hdc);
    if (memoryDc == nullptr) {
        return;
    }
    HBITMAP bitmap = ::CreateCompatibleBitmap(hdc, cellWidth, cellHeight);
    if (bitmap == nullptr) {
        ::DeleteDC(memoryDc);
        return;
    }
    SelectedObject bitmapScope(memoryDc, bitmap);

    RECT local{0, 0, cellWidth, cellHeight};
    {
        HBRUSH background = ::CreateSolidBrush(style.cellBackground);
        ::FillRect(memoryDc, &local, background);
        ::DeleteObject(background);
    }

    const int paddingX = DpiHelper::Scale(6, static_cast<UINT>(dpi));
    const int barHeight = std::clamp(cellHeight - DpiHelper::Scale(8, static_cast<UINT>(dpi)),
                                     DpiHelper::Scale(10, static_cast<UINT>(dpi)),
                                     DpiHelper::Scale(16, static_cast<UINT>(dpi)));

    RECT bar{
        paddingX,
        (cellHeight - barHeight) / 2,
        cellWidth - paddingX,
        (cellHeight - barHeight) / 2 + barHeight,
    };
    if (bar.right <= bar.left) {
        ::DeleteObject(bitmap);
        ::DeleteDC(memoryDc);
        return;
    }

    const int radius = barHeight;
    const double clamped = std::clamp(percent, 0.0, 100.0);
    const int barWidth = bar.right - bar.left;
    const int fillWidth = static_cast<int>((clamped / 100.0) * barWidth + 0.5);
    const int fillEdge = bar.left + fillWidth;

    // 1. Track.
    {
        HBRUSH trackBrush = ::CreateSolidBrush(style.track);
        HPEN borderPen = ::CreatePen(PS_SOLID, 1, style.border);
        SelectedObject brushScope(memoryDc, trackBrush);
        SelectedObject penScope(memoryDc, borderPen);
        ::RoundRect(memoryDc, bar.left, bar.top, bar.right, bar.bottom, radius, radius);
        ::DeleteObject(trackBrush);
        ::DeleteObject(borderPen);
    }

    // 2. Filled part, clipped so the pill keeps its rounded ends.
    if (fillWidth > 0) {
        HRGN clip = ::CreateRectRgn(bar.left, bar.top, fillEdge, bar.bottom);
        ::SelectClipRgn(memoryDc, clip);

        HBRUSH fillBrush = ::CreateSolidBrush(style.fill);
        HPEN fillPen = ::CreatePen(PS_SOLID, 1, style.fill);
        {
            SelectedObject brushScope(memoryDc, fillBrush);
            SelectedObject penScope(memoryDc, fillPen);
            ::RoundRect(memoryDc, bar.left, bar.top, bar.right, bar.bottom, radius, radius);
        }
        ::DeleteObject(fillBrush);
        ::DeleteObject(fillPen);

        ::SelectClipRgn(memoryDc, nullptr);
        ::DeleteObject(clip);
    }

    // 3. Label, drawn once per side of the fill boundary.
    if (!label.empty()) {
        SelectedObject fontScope(memoryDc, font);
        ::SetBkMode(memoryDc, TRANSPARENT);

        RECT textRect = bar;
        if (fillWidth < barWidth) {
            HRGN clip = ::CreateRectRgn(fillEdge, bar.top, bar.right, bar.bottom);
            ::SelectClipRgn(memoryDc, clip);
            ::SetTextColor(memoryDc, style.textOnTrack);
            ::DrawTextW(memoryDc, label.c_str(), -1, &textRect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectClipRgn(memoryDc, nullptr);
            ::DeleteObject(clip);
        }
        if (fillWidth > 0) {
            HRGN clip = ::CreateRectRgn(bar.left, bar.top, fillEdge, bar.bottom);
            ::SelectClipRgn(memoryDc, clip);
            ::SetTextColor(memoryDc, style.textOnFill);
            ::DrawTextW(memoryDc, label.c_str(), -1, &textRect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectClipRgn(memoryDc, nullptr);
            ::DeleteObject(clip);
        }
    }

    ::BitBlt(hdc, cellRect.left, cellRect.top, cellWidth, cellHeight, memoryDc, 0, 0, SRCCOPY);

    ::DeleteObject(bitmap);
    ::DeleteDC(memoryDc);
}

void Controls::InsertListColumn(HWND hList, int colIndex, const wchar_t* text, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = width;
    column.pszText = const_cast<LPWSTR>(text);
    column.iSubItem = colIndex;
    ::SendMessageW(hList, LVM_INSERTCOLUMNW, static_cast<WPARAM>(colIndex),
                   reinterpret_cast<LPARAM>(&column));
}

int Controls::InsertListItem(HWND hList, int itemIndex, const wchar_t* text, LPARAM lParam) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = itemIndex;
    item.iSubItem = 0;
    item.pszText = const_cast<LPWSTR>(text);
    item.lParam = lParam;
    return static_cast<int>(::SendMessageW(hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
}

void Controls::SetListSubItem(HWND hList, int itemIndex, int subItemIndex, const wchar_t* text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = itemIndex;
    item.iSubItem = subItemIndex;
    item.pszText = const_cast<LPWSTR>(text);
    ::SendMessageW(hList, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex),
                   reinterpret_cast<LPARAM>(&item));
}

} // namespace od::ui

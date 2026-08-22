#pragma once

#include <windows.h>

#include <commctrl.h>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace od::ui {

/// Colours used to render one progress cell of the download table.
struct ProgressCellStyle {
    COLORREF cellBackground{};
    COLORREF track{};
    COLORREF fill{};
    COLORREF border{};
    COLORREF textOnTrack{};
    COLORREF textOnFill{};
};

/**
 * @brief Owner-drawn pieces of the download table and thin ListView helpers.
 */
class Controls {
public:
    static void InitializeCommonControls();

    /**
     * @brief Draw a rounded progress pill with a centred percentage label.
     *
     * The label is drawn twice under complementary clip regions - once over the
     * unfilled track, once over the filled part - so it stays readable no matter
     * where the boundary falls. Everything is composed in a memory DC and
     * blitted once, which is what keeps a 250 ms refresh flicker-free.
     */
    static void DrawProgressCell(HDC hdc,
                                 const RECT& cellRect,
                                 double percent,
                                 const std::wstring& label,
                                 const ProgressCellStyle& style,
                                 HFONT font,
                                 int dpi);

    static void InsertListColumn(HWND hList, int colIndex, const wchar_t* text, int width);
    static int InsertListItem(HWND hList, int itemIndex, const wchar_t* text, LPARAM lParam = 0);
    static void SetListSubItem(HWND hList, int itemIndex, int subItemIndex, const wchar_t* text);
};

} // namespace od::ui

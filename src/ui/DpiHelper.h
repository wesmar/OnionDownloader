#pragma once

#include <windows.h>

namespace od::ui {

/**
 * @brief High-DPI scaling helper implementing Per-Monitor V2 awareness.
 * 
 * DESIGN RATIONALE:
 * In mixed multi-monitor setups (e.g. 4K 150% laptop screen + 1080p 100% external monitor),
 * standard GDI apps become blurry or misaligned. DpiHelper dynamically computes scale factors
 * and recreates DPI-proportional GDI fonts and control rectangles on `WM_DPICHANGED`.
 */
class DpiHelper {
public:
    /// Get current DPI for the specified window
    [[nodiscard]] static UINT GetDpi(HWND hwnd);

    /// Scale a pixel value based on the DPI (standard DPI is 96 = 100%)
    [[nodiscard]] static int Scale(int value, UINT dpi) noexcept;

    /// Create a scaled UI font (Segoe UI or system font)
    [[nodiscard]] static HFONT CreateScaledFont(UINT dpi, int pointSize = 9, int weight = FW_NORMAL);
};

} // namespace od::ui

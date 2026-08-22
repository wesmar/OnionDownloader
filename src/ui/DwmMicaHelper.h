#pragma once

#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace od::ui {

/**
 * @brief Manages Windows 11 Mica / Acrylic materials and Dark Mode integration.
 * 
 * DESIGN RATIONALE:
 * Windows 11 introduced Mica backdrop materials (`DWMWA_SYSTEMBACKDROP_TYPE`) that sample
 * desktop wallpaper with subtle blur and tint. DwmMicaHelper dynamically detects Windows build
 * capabilities and applies native immersive dark mode titlebars, dark Explorer control themes,
 * and backdrop rendering with graceful fallback on Windows 10.
 */
class DwmMicaHelper {
public:
    /// Check whether system-wide dark mode is enabled for applications
    [[nodiscard]] static bool IsSystemDarkModeEnabled();

    /// Apply the Mica backdrop and immersive dark mode to the target window.
    /// Returns true when the system actually granted a Mica backdrop, in which
    /// case the client area must be erased with black so DWM can show through.
    static bool ApplyMicaAndDarkMode(HWND hwnd, bool darkMode = true);

    /// Apply modern dark styling to list/edit style common controls.
    /// Only valid for controls that understand the "Explorer" theme class -
    /// ListView, TreeView, Edit and scroll bars.
    static void ApplyControlDarkTheme(HWND hControl, bool darkMode = true);

    /// Theme a push button. Buttons have no light-mode theme class of their own:
    /// forcing "Explorer" on them paints dark-mode text on a light face, which is
    /// why the default theme has to be restored explicitly outside dark mode.
    static void ApplyButtonTheme(HWND hButton, bool darkMode);

    /// Color palette definitions for dark and light modes
    struct Palette {
        COLORREF background;
        COLORREF surface;
        COLORREF border;
        COLORREF textPrimary;
        COLORREF textSecondary;
        COLORREF accent;
        COLORREF progressTrack;   ///< empty part of the progress pill
        COLORREF progressFill;    ///< completed part, maximum contrast against the track
    };

    [[nodiscard]] static const Palette& GetPalette(bool darkMode);
};

} // namespace od::ui

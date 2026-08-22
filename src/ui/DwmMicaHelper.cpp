#include "DwmMicaHelper.h"
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

namespace od::ui {

bool DwmMicaHelper::IsSystemDarkModeEnabled() {
    HKEY hKey;
    if (::RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0,
            KEY_READ,
            &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 1;
    DWORD size = sizeof(value);
    ::RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(hKey);

    return (value == 0); // 0 = Dark Mode, 1 = Light Mode
}

bool DwmMicaHelper::ApplyMicaAndDarkMode(HWND hwnd, bool darkMode) {
    if (!hwnd) return false;

    BOOL useDark = darkMode ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    ::DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));   // pre-20H1 attribute id

    DWORD backdropType = DWMSBT_MAINWINDOW;
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));

    return true;
}

void DwmMicaHelper::ApplyControlDarkTheme(HWND hControl, bool darkMode) {
    if (!hControl) return;

    if (darkMode) {
        ::SetWindowTheme(hControl, L"DarkMode_Explorer", nullptr);
    } else {
        ::SetWindowTheme(hControl, L"Explorer", nullptr);
    }
}

void DwmMicaHelper::ApplyButtonTheme(HWND hButton, bool darkMode) {
    if (!hButton) return;
    ::SetWindowTheme(hButton, darkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
}

const DwmMicaHelper::Palette& DwmMicaHelper::GetPalette(bool darkMode) {
    static const Palette s_dark = {
        RGB(32, 32, 34),     // Background (Rich dark)
        RGB(42, 42, 46),     // Surface (Control surface)
        RGB(65, 65, 70),     // Border
        RGB(255, 255, 255),  // Text Primary (Crisp pure white)
        RGB(170, 170, 175),  // Text Secondary
        RGB(92, 92, 98),     // Neutral accent
        RGB(55, 55, 60),     // Progress Track
        RGB(142, 142, 148)   // Progress Fill
    };

    static const Palette s_light = {
        RGB(240, 240, 243),  // Background
        RGB(255, 255, 255),  // Surface
        RGB(215, 215, 220),  // Border
        RGB(20, 20, 25),     // Text Primary (Deep dark)
        RGB(100, 100, 105),  // Text Secondary
        RGB(145, 145, 150),  // Neutral accent
        RGB(225, 225, 230),  // Progress Track
        RGB(125, 125, 130)   // Progress Fill
    };

    return darkMode ? s_dark : s_light;
}

} // namespace od::ui

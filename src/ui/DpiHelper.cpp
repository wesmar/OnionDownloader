#include "DpiHelper.h"

namespace od::ui {

UINT DpiHelper::GetDpi(HWND hwnd) {
    // Dynamic runtime link for GetDpiForWindow available on Windows 10 Anniversary Update+
    using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
    static auto pfnGetDpiForWindow = reinterpret_cast<PFN_GetDpiForWindow>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")
    );

    if (pfnGetDpiForWindow && hwnd) {
        return pfnGetDpiForWindow(hwnd);
    }

    HDC hdc = ::GetDC(hwnd);
    if (hdc) {
        int dpiX = ::GetDeviceCaps(hdc, LOGPIXELSX);
        ::ReleaseDC(hwnd, hdc);
        return static_cast<UINT>(dpiX);
    }

    return 96; // Standard 100% fallback
}

int DpiHelper::Scale(int value, UINT dpi) noexcept {
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT DpiHelper::CreateScaledFont(UINT dpi, int pointSize, int weight) {
    const int height = -::MulDiv(pointSize, static_cast<int>(dpi), 72);

    return ::CreateFontW(
        height,
        0, 0, 0,
        weight,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
}

} // namespace od::ui

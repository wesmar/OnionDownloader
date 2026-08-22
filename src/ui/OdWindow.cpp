#include "OdWindow.h"

#include "Controls.h"
#include "DpiHelper.h"
#include "DwmMicaHelper.h"
#include "Resource.h"
#include "core/StringUtils.h"

#include <windows.h>

#include <algorithm>
#include <commctrl.h>
#include <iomanip>
#include <iterator>
#include <memory>
#include <ole2.h>
#include <shellapi.h>
#include <sstream>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace od::ui {

namespace {

constexpr UINT_PTR kRefreshTimerId = 1001;
constexpr int kProgressColumn = 2;

WNDPROC s_originalEditProc = nullptr;
WNDPROC s_originalListProc = nullptr;
WNDPROC s_originalButtonProc = nullptr;
HWND s_hoveredButton = nullptr;

class UrlDropTarget final : public IDropTarget {
public:
    explicit UrlDropTarget(HWND edit) : m_edit(edit) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        return Check(data, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = m_canDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { m_canDrop = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = DROPEFFECT_NONE;
        FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (data == nullptr || FAILED(data->GetData(&format, &medium))) return S_OK;
        const auto* text = static_cast<const wchar_t*>(::GlobalLock(medium.hGlobal));
        if (text != nullptr) {
            ::SetWindowTextW(m_edit, text);
            ::SendMessageW(m_edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
            ::SetFocus(m_edit);
            ::GlobalUnlock(medium.hGlobal);
            if (effect) *effect = DROPEFFECT_COPY;
        }
        ::ReleaseStgMedium(&medium);
        m_canDrop = false;
        return S_OK;
    }

private:
    HRESULT Check(IDataObject* data, DWORD* effect) {
        FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        m_canDrop = data != nullptr && SUCCEEDED(data->QueryGetData(&format));
        if (effect) *effect = m_canDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    std::atomic<ULONG> m_refs{1};
    HWND m_edit;
    bool m_canDrop{false};
};

/// Tracks the pointer over a button so the owner-draw handler can render a hover
/// state. All four buttons share the BUTTON class, hence one saved proc.
LRESULT CALLBACK ButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_MOUSEMOVE:
            if (s_hoveredButton != hWnd) {
                s_hoveredButton = hWnd;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hWnd, 0};
                ::TrackMouseEvent(&track);
                ::InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (s_hoveredButton == hWnd) {
                s_hoveredButton = nullptr;
                ::InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        default:
            break;
    }
    return ::CallWindowProcW(s_originalButtonProc, hWnd, uMsg, wParam, lParam);
}

/// Enter in the URL box behaves like pressing "Download".
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND parent = ::GetParent(hWnd);
        if (parent != nullptr) {
            ::SendMessageW(parent, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED),
                           reinterpret_cast<LPARAM>(hWnd));
            return 0;
        }
    }
    const LRESULT result = ::CallWindowProcW(s_originalEditProc, hWnd, uMsg, wParam, lParam);
    if (uMsg == WM_PAINT && ::GetWindowTextLengthW(hWnd) == 0) {
        auto* window = reinterpret_cast<OdWindow*>(
            ::GetWindowLongPtrW(::GetParent(hWnd), GWLP_USERDATA));
        HDC dc = ::GetDC(hWnd);
        if (window != nullptr && dc != nullptr) window->PaintUrlCue(hWnd, dc);
        if (dc != nullptr) ::ReleaseDC(hWnd, dc);
    }
    return result;
}

/**
 * A report-view header sends its NM_CUSTOMDRAW to the ListView, not to the
 * frame window, so the only way to theme it is to sit in between.
 */
LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_NOTIFY) {
        auto* notification = reinterpret_cast<LPNMHDR>(lParam);
        if (notification != nullptr && notification->code == NM_CUSTOMDRAW) {
            auto* window = reinterpret_cast<OdWindow*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (window != nullptr) {
                return window->OnHeaderCustomDraw(reinterpret_cast<LPNMCUSTOMDRAW>(lParam));
            }
        }
    }
    return ::CallWindowProcW(s_originalListProc, hWnd, uMsg, wParam, lParam);
}

COLORREF Blend(COLORREF a, COLORREF b, double weightOfB) {
    const auto mix = [weightOfB](int x, int y) {
        return static_cast<int>(x * (1.0 - weightOfB) + y * weightOfB + 0.5);
    };
    return RGB(mix(GetRValue(a), GetRValue(b)),
               mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

/// Pick black or white text for maximum legibility on the given background,
/// using the ITU-R BT.601 luma approximation.
COLORREF ContrastText(COLORREF background) {
    const int luma = (GetRValue(background) * 299 +
                      GetGValue(background) * 587 +
                      GetBValue(background) * 114) / 1000;
    return (luma > 150) ? RGB(20, 20, 22) : RGB(250, 250, 252);
}

} // namespace

OdWindow::OdWindow(const cli::CliOptions& options)
    : m_options(options),
      m_engine(m_torPool) {
    m_darkMode = DwmMicaHelper::IsSystemDarkModeEnabled();
}

OdWindow::~OdWindow() {
    if (m_dropTarget != nullptr) {
        if (m_hWnd != nullptr) ::RevokeDragDrop(m_hWnd);
        m_dropTarget->Release();
    }
    if (m_oleInitialized) ::OleUninitialize();
    if (m_hRowSpacer) ::ImageList_Destroy(m_hRowSpacer);
    if (m_hFontNormal) ::DeleteObject(m_hFontNormal);
    if (m_hBgBrush) ::DeleteObject(m_hBgBrush);
    if (m_hSurfaceBrush) ::DeleteObject(m_hSurfaceBrush);
}

bool OdWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;
    Controls::InitializeCommonControls();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProcSetup;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"OdMainWindowClass";
    ::RegisterClassExW(&windowClass);

    m_hWnd = ::CreateWindowExW(
        WS_EX_ACCEPTFILES,
        windowClass.lpszClassName,
        L"Onion Downloader - Multi-Circuit Tor Engine v 1.0.0 - © Marek Wesołowski - (2026)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1020, 620,
        nullptr, nullptr, hInstance, this);

    if (m_hWnd == nullptr) {
        return false;
    }

    ::DragAcceptFiles(m_hWnd, TRUE);
    m_currentDpi = DpiHelper::GetDpi(m_hWnd);

    CreateLayout();
    const HRESULT oleResult = ::OleInitialize(nullptr);
    m_oleInitialized = SUCCEEDED(oleResult);
    if (m_oleInitialized) {
        m_dropTarget = new UrlDropTarget(m_hEditUrl);
        if (FAILED(::RegisterDragDrop(m_hWnd, m_dropTarget))) {
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
    }
    UpdateDpiScaling(m_currentDpi);
    UpdateTheme(m_darkMode);

    RECT client{};
    ::GetClientRect(m_hWnd, &client);
    LayoutControls(client.right - client.left, client.bottom - client.top);

    RefreshTaskView();

    ::ShowWindow(m_hWnd, nCmdShow);
    ::UpdateWindow(m_hWnd);
    ::SetFocus(m_hEditUrl);
    ::SetTimer(m_hWnd, kRefreshTimerId, 250, nullptr);

    m_engine.SetUpdateCallback([this](const core::DownloadTask&) {
        if (m_hWnd != nullptr) {
            ::PostMessageW(m_hWnd, WM_APP_TASK_UPDATE, 0, 0);
        }
    });
    m_engine.SetLogCallback([this](const std::wstring& message) {
        PostStatusText(2, message);
    });

    if (!m_options.url.empty()) {
        ::SetWindowTextW(m_hEditUrl, m_options.url.c_str());
    }

    StartBackend();
    return true;
}

void OdWindow::StartBackend() {
    // Bootstrapping the Tor pool blocks for seconds to minutes on a cold
    // profile, so it happens off the UI thread and reports through messages.
    std::thread([this]() {
        const bool ready = m_torPool.Start(m_options.EffectiveTorInstances(),
                                           [this](const std::wstring& status) {
                                               PostStatusText(0, status);
                                           });
        m_backendReady = ready;
        PostStatusText(1, ready ? L"Engine: ready" : L"Engine: Tor unavailable");

        if (ready && !m_options.url.empty()) {
            m_engine.AddDownload(m_options.url, m_options.outputDir, m_options.workers);
        }
    }).detach();
}

void OdWindow::PostStatusText(int part, const std::wstring& text) {
    if (m_hWnd == nullptr) {
        return;
    }
    auto* payload = new std::wstring(text);
    if (!::PostMessageW(m_hWnd, WM_APP_STATUS_TEXT, static_cast<WPARAM>(part),
                        reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void SetWindowTextIfChanged(HWND window, const wchar_t* text) {
    wchar_t current[64] = {};
    ::GetWindowTextW(window, current, static_cast<int>(std::size(current)));
    if (::wcscmp(current, text) != 0) {
        ::SetWindowTextW(window, text);
    }
}

void EnableWindowIfChanged(HWND window, bool enabled) {
    if ((::IsWindowEnabled(window) != FALSE) != enabled) {
        ::EnableWindow(window, enabled ? TRUE : FALSE);
    }
}

void OdWindow::PasteUrlFromClipboard() {
    if (!::OpenClipboard(m_hWnd)) {
        return;
    }
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        const auto* text = static_cast<const wchar_t*>(::GlobalLock(data));
        if (text != nullptr) {
            ::SetWindowTextW(m_hEditUrl, text);
            ::SendMessageW(m_hEditUrl, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    ::SetFocus(m_hEditUrl);
}

int OdWindow::RunMessageLoop() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0)) {
        if (message.message == WM_KEYDOWN && message.wParam == 'V' &&
            (::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (::GetKeyState(VK_MENU) & 0x8000) == 0) {
            PasteUrlFromClipboard();
            continue;
        }
        if (!::IsDialogMessageW(m_hWnd, &message)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK OdWindow::WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* window = static_cast<OdWindow*>(createStruct->lpCreateParams);
        window->m_hWnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcThunk));
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK OdWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* window = reinterpret_cast<OdWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return (window != nullptr) ? window->HandleMessage(msg, wParam, lParam)
                               : ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OdWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DPICHANGED: {
            m_currentDpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(m_hWnd, nullptr,
                           suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateDpiScaling(m_currentDpi);
            return 0;
        }

        case WM_SETTINGCHANGE: {
            // Follow the system when the user flips light/dark while we run.
            const auto* section = reinterpret_cast<const wchar_t*>(lParam);
            if (section != nullptr && ::wcscmp(section, L"ImmersiveColorSet") == 0) {
                UpdateTheme(DwmMicaHelper::IsSystemDarkModeEnabled());
            }
            return 0;
        }

        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED && m_hEditUrl != nullptr) {
                LayoutControls(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = DpiHelper::Scale(760, m_currentDpi);
            info->ptMinTrackSize.y = DpiHelper::Scale(320, m_currentDpi);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kRefreshTimerId) {
                RefreshTaskView();
            }
            return 0;

        case WM_APP_TASK_UPDATE:
            RefreshTaskView();
            return 0;

        case WM_APP_STATUS_TEXT: {
            const std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            if (m_hStatusBar != nullptr && text) {
                ::SendMessageW(m_hStatusBar, SB_SETTEXTW, static_cast<WPARAM>(wParam),
                               reinterpret_cast<LPARAM>(text->c_str()));
            }
            return 0;
        }

        case WM_NOTIFY: {
            auto* notification = reinterpret_cast<LPNMHDR>(lParam);
            if (notification != nullptr && notification->hwndFrom == m_hListView &&
                notification->code == NM_DBLCLK) {
                OnToggleSelected();
                return 0;
            }
            if (notification != nullptr && notification->hwndFrom == m_hListView &&
                notification->code == LVN_ITEMCHANGED) {
                core::DownloadTask selected;
                const bool hasSelection = TryGetSelectedTask(selected);
                EnableWindowIfChanged(m_hBtnPause, hasSelection && selected.status != core::TaskStatus::Completed);
                EnableWindowIfChanged(m_hBtnRemove, hasSelection);
                if (hasSelection) {
                    SetWindowTextIfChanged(m_hBtnPause,
                        selected.status == core::TaskStatus::Active ? L"Pause" : L"Resume");
                }
            }
            if (notification != nullptr && notification->hwndFrom == m_hListView &&
                notification->code == NM_CUSTOMDRAW) {
                return OnListCustomDraw(reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam));
            }
            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_BTN_START:
                case IDM_FILE_ADD:
                case IDM_DOWNLOAD_START:
                    OnAddDownloadClicked();
                    break;

                case IDC_BTN_PASTE: {
                    PasteUrlFromClipboard();
                    break;
                }

                case IDC_BTN_PAUSE:
                case IDM_DOWNLOAD_PAUSE:
                    OnToggleSelected();
                    break;

                case IDC_BTN_DELETE:
                case IDM_DOWNLOAD_REMOVE:
                    OnRemoveSelected();
                    break;

                case IDM_VIEW_DARK_MODE:
                    UpdateTheme(!m_darkMode);
                    break;

                case IDM_HELP_ABOUT:
                    ::MessageBoxW(
                        m_hWnd,
                        L"Onion Downloader (od.exe) v2.0\n\n"
                        L"Native multi-circuit Tor downloader.\n"
                        L"Every worker owns a private SOCKS5 isolation credential on its own\n"
                        L"Tor daemon, so parallel range requests travel disjoint paths.\n\n"
                        L"C++23 / MSVC v145 / WinAPI / Mica / Per-Monitor V2 DPI",
                        L"About Onion Downloader",
                        MB_OK | MB_ICONINFORMATION);
                    break;

                case IDM_FILE_EXIT:
                    ::SendMessageW(m_hWnd, WM_CLOSE, 0, 0);
                    break;

                default:
                    break;
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw->CtlType == ODT_BUTTON) {
                DrawButton(*draw);
                return TRUE;
            }
            break;
        }

        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wParam);
            wchar_t path[MAX_PATH] = {};
            if (::DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
                ::SetWindowTextW(m_hEditUrl, path);
            }
            ::DragFinish(drop);
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
            ::SetTextColor(reinterpret_cast<HDC>(wParam), palette.textPrimary);
            ::SetBkColor(reinterpret_cast<HDC>(wParam), palette.surface);
            return reinterpret_cast<LRESULT>(m_hSurfaceBrush);
        }

        case WM_CTLCOLORSTATIC: {
            const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
            auto dc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(dc, palette.textPrimary);
            ::SetBkColor(dc, palette.background);
            return reinterpret_cast<LRESULT>(m_hBgBrush);
        }

        case WM_ERASEBKGND: {
            RECT client{};
            ::GetClientRect(m_hWnd, &client);
            ::FillRect(reinterpret_cast<HDC>(wParam), &client, m_hBgBrush);
            return 1;
        }

        case WM_CLOSE:
            ::KillTimer(m_hWnd, kRefreshTimerId);
            m_engine.Shutdown();
            m_torPool.Shutdown();
            ::DestroyWindow(m_hWnd);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

LRESULT OdWindow::OnListCustomDraw(LPNMLVCUSTOMDRAW customDraw) {
    switch (customDraw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;

        case CDDS_POSTPAINT:
            DrawEmptyState(customDraw->nmcd.hdc);
            return CDRF_DODEFAULT;

        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;

        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            if (customDraw->iSubItem != kProgressColumn) {
                return CDRF_DODEFAULT;
            }

            const auto row = static_cast<int>(customDraw->nmcd.dwItemSpec);
            if (row < 0 || row >= static_cast<int>(m_rowProgress.size())) {
                return CDRF_DODEFAULT;
            }

            RECT cell{};
            if (!ListView_GetSubItemRect(m_hListView, row, kProgressColumn, LVIR_BOUNDS, &cell)) {
                return CDRF_DODEFAULT;
            }

            const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
            const bool selected =
                (ListView_GetItemState(m_hListView, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            const double percent = m_rowProgress[static_cast<size_t>(row)];

            ProgressCellStyle style{};
            style.cellBackground = selected ? Blend(palette.surface, palette.border, 0.35)
                                             : palette.surface;
            style.track = selected ? Blend(palette.progressTrack, style.cellBackground, 0.35)
                                   : palette.progressTrack;
            style.fill = percent >= 100.0
                ? (m_darkMode ? RGB(82, 82, 87) : RGB(105, 105, 110))
                : palette.progressFill;
            style.border = palette.border;
            style.textOnTrack = ContrastText(style.track);
            style.textOnFill = ContrastText(style.fill);

            std::wostringstream label;
            label << std::fixed << std::setprecision(1) << percent << L"%";

            Controls::DrawProgressCell(customDraw->nmcd.hdc, cell, percent, label.str(),
                                       style, m_hFontNormal, static_cast<int>(m_currentDpi));
            return CDRF_SKIPDEFAULT;
        }

        default:
            return CDRF_DODEFAULT;
    }
}

LRESULT OdWindow::OnHeaderCustomDraw(LPNMCUSTOMDRAW customDraw) {
    // Comctl32 renders a perfectly good light header on its own; the only reason
    // to take over is that it has no dark variant. Outside dark mode, stay out.
    if (!m_darkMode || customDraw->hdr.hwndFrom != m_hListHeader) {
        return CDRF_DODEFAULT;
    }

    switch (customDraw->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;

        case CDDS_POSTPAINT: {
            // The header has no item above the vertical scrollbar. Comctl32
            // otherwise leaves that small strip in the system (white) color.
            RECT client{};
            ::GetClientRect(m_hListHeader, &client);
            const int count = Header_GetItemCount(m_hListHeader);
            RECT last{};
            if (count > 0 && Header_GetItemRect(m_hListHeader, count - 1, &last)) {
                client.left = std::min(last.right, client.right);
            }
            const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
            HBRUSH background = ::CreateSolidBrush(Blend(palette.surface, palette.background, 0.6));
            ::FillRect(customDraw->hdc, &client, background);
            ::DeleteObject(background);
            return CDRF_DODEFAULT;
        }

        case CDDS_ITEMPREPAINT: {
            const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
            const COLORREF headerBackground = Blend(palette.surface, palette.background, 0.6);

            HBRUSH background = ::CreateSolidBrush(headerBackground);
            ::FillRect(customDraw->hdc, &customDraw->rc, background);
            ::DeleteObject(background);

            // Column separator and the rule under the whole header.
            HPEN rulePen = ::CreatePen(PS_SOLID, 1, palette.border);
            HGDIOBJ previousPen = ::SelectObject(customDraw->hdc, rulePen);
            ::MoveToEx(customDraw->hdc, customDraw->rc.right - 1, customDraw->rc.top + 4, nullptr);
            ::LineTo(customDraw->hdc, customDraw->rc.right - 1, customDraw->rc.bottom - 4);
            ::MoveToEx(customDraw->hdc, customDraw->rc.left, customDraw->rc.bottom - 1, nullptr);
            ::LineTo(customDraw->hdc, customDraw->rc.right, customDraw->rc.bottom - 1);
            ::SelectObject(customDraw->hdc, previousPen);
            ::DeleteObject(rulePen);

            wchar_t caption[128] = {};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = caption;
            item.cchTextMax = static_cast<int>(std::size(caption));
            Header_GetItem(m_hListHeader, static_cast<int>(customDraw->dwItemSpec), &item);

            RECT textRect = customDraw->rc;
            textRect.left += DpiHelper::Scale(8, m_currentDpi);
            textRect.right -= DpiHelper::Scale(6, m_currentDpi);

            HGDIOBJ previousFont = ::SelectObject(customDraw->hdc, m_hFontNormal);
            ::SetBkMode(customDraw->hdc, TRANSPARENT);
            ::SetTextColor(customDraw->hdc, palette.textSecondary);
            ::DrawTextW(customDraw->hdc, caption, -1, &textRect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            ::SelectObject(customDraw->hdc, previousFont);

            return CDRF_SKIPDEFAULT;
        }

        default:
            return CDRF_DODEFAULT;
    }
}

void OdWindow::CreateLayout() {
    m_hEditUrl = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_URL), m_hInstance, nullptr);
    s_originalEditProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(m_hEditUrl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc)));

    struct ButtonSpec {
        const wchar_t* caption;
        int id;
        DWORD style;
        HWND* target;
    };

    // Owner-drawn so the look is identical and correct in both themes; the
    // system button theme paints unreadable text under Mica and dark mode.
    const ButtonSpec buttons[] = {
        {L"Paste",    IDC_BTN_PASTE,  BS_OWNERDRAW,                     &m_hBtnPaste},
        {L"Download", IDC_BTN_START,  BS_OWNERDRAW,                     &m_hBtnAdd},
        {L"Pause",    IDC_BTN_PAUSE,  BS_OWNERDRAW,                     &m_hBtnPause},
        {L"Remove",   IDC_BTN_DELETE, BS_OWNERDRAW,                     &m_hBtnRemove},
    };

    for (const auto& spec : buttons) {
        *spec.target = ::CreateWindowExW(
            0, L"BUTTON", spec.caption,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | spec.style,
            0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)),
            m_hInstance, nullptr);

        WNDPROC previous = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(*spec.target, GWLP_WNDPROC,
                                reinterpret_cast<LONG_PTR>(ButtonSubclassProc)));
        if (s_originalButtonProc == nullptr) {
            s_originalButtonProc = previous;
        }
    }

    m_hListView = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_LIST_DOWNLOADS), m_hInstance, nullptr);

    ListView_SetExtendedListViewStyle(m_hListView,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ApplyRowHeight();

    Controls::InsertListColumn(m_hListView, 0, L"File", DpiHelper::Scale(300, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 1, L"Size", DpiHelper::Scale(95, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 2, L"Progress", DpiHelper::Scale(130, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 3, L"Speed", DpiHelper::Scale(100, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 4, L"Circuits", DpiHelper::Scale(70, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 5, L"ETA", DpiHelper::Scale(80, m_currentDpi));
    Controls::InsertListColumn(m_hListView, 6, L"Status", DpiHelper::Scale(220, m_currentDpi));

    // Sit between the ListView and its header so the header can be themed.
    m_hListHeader = ListView_GetHeader(m_hListView);
    ::SetWindowLongPtrW(m_hListView, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    s_originalListProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(m_hListView, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListSubclassProc)));

    m_hStatusBar = ::CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_STATUS_BAR), m_hInstance, nullptr);

    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Tor: starting..."));
    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L"Engine: idle"));
    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(L""));
    ::EnableWindow(m_hBtnPause, FALSE);
    ::EnableWindow(m_hBtnRemove, FALSE);
}

void OdWindow::PaintUrlCue(HWND edit, HDC dc) const {
    RECT rect{};
    ::GetClientRect(edit, &rect);
    rect.left += DpiHelper::Scale(7, m_currentDpi);
    rect.right -= DpiHelper::Scale(7, m_currentDpi);
    const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
    HGDIOBJ previousFont = ::SelectObject(dc, m_hFontNormal);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, palette.textSecondary);
    ::DrawTextW(dc, L"Paste an onion URL, e.g. http://....onion/file.dat", -1, &rect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    ::SelectObject(dc, previousFont);
}

void OdWindow::ApplyRowHeight() {
    if (m_hListView == nullptr) {
        return;
    }
    // A report view takes its row height from the small image list, so a
    // one-pixel-wide list of the desired height is the sanctioned way to get
    // roomier rows without owner-drawing every cell.
    if (m_hRowSpacer != nullptr) {
        ListView_SetImageList(m_hListView, nullptr, LVSIL_SMALL);
        ::ImageList_Destroy(m_hRowSpacer);
    }
    m_hRowSpacer = ::ImageList_Create(1, DpiHelper::Scale(26, m_currentDpi), ILC_COLOR32, 1, 1);
    ListView_SetImageList(m_hListView, m_hRowSpacer, LVSIL_SMALL);
}

void OdWindow::StretchLastColumn(int listWidth) {
    if (m_hListView == nullptr || listWidth <= 0) {
        return;
    }

    constexpr int kLastColumn = 6;
    int used = 0;
    for (int column = 0; column < kLastColumn; ++column) {
        used += ListView_GetColumnWidth(m_hListView, column);
    }

    const int reserved = ::GetSystemMetrics(SM_CXVSCROLL) + DpiHelper::Scale(8, m_currentDpi);
    const int remaining = listWidth - used - reserved;
    ListView_SetColumnWidth(m_hListView, kLastColumn,
                            std::max(DpiHelper::Scale(140, m_currentDpi), remaining));
}

void OdWindow::DrawButton(const DRAWITEMSTRUCT& draw) const {
    const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);

    const bool isDefault = false;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool hovered = (draw.hwndItem == s_hoveredButton) && !disabled;

    COLORREF face;
    COLORREF border;
    COLORREF textColor;

    if (isDefault) {
        // Accent button (Download)
        if (disabled) {
            face = m_darkMode ? RGB(45, 45, 50) : RGB(220, 220, 225);
            border = m_darkMode ? RGB(60, 60, 65) : RGB(200, 200, 205);
            textColor = m_darkMode ? RGB(110, 110, 115) : RGB(140, 140, 145);
        } else if (pressed) {
            face = RGB(0, 95, 175);
            border = RGB(0, 95, 175);
            textColor = RGB(255, 255, 255);
        } else if (hovered) {
            face = RGB(26, 138, 230);
            border = RGB(26, 138, 230);
            textColor = RGB(255, 255, 255);
        } else {
            face = palette.accent;
            border = palette.accent;
            textColor = RGB(255, 255, 255);
        }
    } else {
        // Standard buttons (Paste, Pause, Remove)
        if (m_darkMode) {
            if (disabled) {
                face = RGB(36, 36, 38);
                border = RGB(50, 50, 55);
                textColor = RGB(110, 110, 115);
            } else if (pressed) {
                face = RGB(36, 36, 40);
                border = RGB(85, 85, 90);
                textColor = RGB(255, 255, 255);
            } else if (hovered) {
                face = RGB(60, 60, 66);
                border = RGB(95, 95, 100);
                textColor = RGB(255, 255, 255);
            } else {
                face = RGB(46, 46, 50);
                border = RGB(70, 70, 75);
                textColor = RGB(245, 245, 250);
            }
        } else {
            if (disabled) {
                face = RGB(245, 245, 248);
                border = RGB(225, 225, 230);
                textColor = RGB(160, 160, 165);
            } else if (pressed) {
                face = RGB(225, 225, 230);
                border = RGB(180, 180, 185);
                textColor = RGB(15, 15, 20);
            } else if (hovered) {
                face = RGB(240, 240, 245);
                border = RGB(190, 190, 195);
                textColor = RGB(15, 15, 20);
            } else {
                face = RGB(255, 255, 255);
                border = RGB(205, 205, 210);
                textColor = RGB(20, 20, 25);
            }
        }
    }

    HDC hdc = draw.hDC;
    RECT rc = draw.rcItem;

    // Fill button background with window background brush
    ::FillRect(hdc, &rc, m_hBgBrush);

    const int radius = DpiHelper::Scale(6, m_currentDpi);
    HBRUSH faceBrush = ::CreateSolidBrush(face);
    HPEN borderPen = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ previousBrush = ::SelectObject(hdc, faceBrush);
    HGDIOBJ previousPen = ::SelectObject(hdc, borderPen);
    ::RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    ::SelectObject(hdc, previousBrush);
    ::SelectObject(hdc, previousPen);
    ::DeleteObject(faceBrush);
    ::DeleteObject(borderPen);

    wchar_t caption[64] = {};
    ::GetWindowTextW(draw.hwndItem, caption, static_cast<int>(std::size(caption)));

    HGDIOBJ previousFont = ::SelectObject(hdc, m_hFontNormal);
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, textColor);
    ::DrawTextW(hdc, caption, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(hdc, previousFont);

    if (focused && !pressed) {
        RECT ring = rc;
        ::InflateRect(&ring, -DpiHelper::Scale(3, m_currentDpi), -DpiHelper::Scale(3, m_currentDpi));
        ::SetTextColor(hdc, textColor);
        ::DrawFocusRect(hdc, &ring);
    }
}

void OdWindow::DrawEmptyState(HDC hdc) const {
    if (!m_rowProgress.empty() || m_hListView == nullptr) {
        return;
    }

    RECT area{};
    ::GetClientRect(m_hListView, &area);

    if (m_hListHeader != nullptr) {
        RECT header{};
        ::GetWindowRect(m_hListHeader, &header);
        area.top += header.bottom - header.top;
    }
    if (area.bottom <= area.top) {
        return;
    }

    static constexpr const wchar_t* kMessage =
        L"No downloads yet\n\n"
        L"Paste an onion URL above and press Download.\n\n"
        L"Ctrl+V anywhere to paste a link.\n"
        L"Interrupted transfers resume automatically.";

    const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
    HGDIOBJ previousFont = ::SelectObject(hdc, m_hFontNormal);
    const int previousMode = ::SetBkMode(hdc, TRANSPARENT);
    const COLORREF previousColor = ::SetTextColor(hdc, palette.textSecondary);

    // DT_VCENTER only applies to single-line text, so the block is measured
    // first and then positioned by hand.
    RECT measured = area;
    ::DrawTextW(hdc, kMessage, -1, &measured, DT_CENTER | DT_NOPREFIX | DT_CALCRECT);

    RECT target = area;
    const LONG blockHeight = measured.bottom - measured.top;
    target.top = area.top + std::max<LONG>(0, ((area.bottom - area.top) - blockHeight) / 2);

    ::DrawTextW(hdc, kMessage, -1, &target, DT_CENTER | DT_NOPREFIX);

    ::SetTextColor(hdc, previousColor);
    ::SetBkMode(hdc, previousMode);
    ::SelectObject(hdc, previousFont);
}

void OdWindow::LayoutControls(int width, int height) {
    const int margin = DpiHelper::Scale(12, m_currentDpi);
    const int gap = DpiHelper::Scale(8, m_currentDpi);
    const int rowHeight = DpiHelper::Scale(30, m_currentDpi);
    const int statusHeight = DpiHelper::Scale(24, m_currentDpi);

    // All four buttons share one width so the action row reads as a single
    // aligned group. The width shrinks when the window is narrow (or DPI is
    // high) so the row always fits instead of spilling past the right edge.
    constexpr int kButtonCount = 4;
    const int minimumEditWidth = DpiHelper::Scale(120, m_currentDpi);
    const int preferredButton = DpiHelper::Scale(104, m_currentDpi);
    const int minimumButton = DpiHelper::Scale(62, m_currentDpi);

    const int rowInterior = width - (margin * 2) - gap - (kButtonCount - 1) * gap;
    const int buttonBudget = rowInterior - minimumEditWidth;
    int buttonWidth = std::clamp(buttonBudget / kButtonCount, minimumButton, preferredButton);

    const int buttonsWidth = kButtonCount * buttonWidth + (kButtonCount - 1) * gap;
    const int editWidth = std::max(minimumEditWidth, width - (margin * 2) - gap - buttonsWidth);

    int x = margin;
    ::MoveWindow(m_hEditUrl, x, margin, editWidth, rowHeight, TRUE);
    x += editWidth + gap;

    HWND buttons[kButtonCount] = {m_hBtnPaste, m_hBtnAdd, m_hBtnPause, m_hBtnRemove};
    for (HWND button : buttons) {
        ::MoveWindow(button, x, margin, buttonWidth, rowHeight, TRUE);
        x += buttonWidth + gap;
    }

    const int listTop = margin + rowHeight + margin;
    const int listHeight = height - listTop - statusHeight - margin;
    if (listHeight > DpiHelper::Scale(60, m_currentDpi)) {
        ::MoveWindow(m_hListView, margin, listTop, width - margin * 2, listHeight, TRUE);
    }

    StretchLastColumn(width - margin * 2);

    const int statusParts[] = {
        DpiHelper::Scale(280, m_currentDpi),
        DpiHelper::Scale(440, m_currentDpi),
        -1,
    };
    ::SendMessageW(m_hStatusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(statusParts));
    ::SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
}

void OdWindow::UpdateDpiScaling(UINT newDpi) {
    if (m_hFontNormal) {
        ::DeleteObject(m_hFontNormal);
    }
    m_hFontNormal = DpiHelper::CreateScaledFont(newDpi, 10, FW_NORMAL);

    HWND controls[] = {m_hEditUrl, m_hBtnPaste, m_hBtnAdd, m_hBtnPause, m_hBtnRemove,
                       m_hListView, m_hStatusBar};
    for (HWND control : controls) {
        if (control != nullptr) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontNormal), TRUE);
        }
    }

    // The header keeps its own font; without this it stays at the old DPI.
    if (m_hListHeader != nullptr) {
        ::SendMessageW(m_hListHeader, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontNormal), TRUE);
    }
    ApplyRowHeight();

    RECT client{};
    ::GetClientRect(m_hWnd, &client);
    LayoutControls(client.right - client.left, client.bottom - client.top);
}

void OdWindow::UpdateTheme(bool darkMode) {
    m_darkMode = darkMode;
    m_micaActive = DwmMicaHelper::ApplyMicaAndDarkMode(m_hWnd, m_darkMode);

    if (m_hBgBrush) ::DeleteObject(m_hBgBrush);
    if (m_hSurfaceBrush) ::DeleteObject(m_hSurfaceBrush);

    const auto& palette = DwmMicaHelper::GetPalette(m_darkMode);
    m_hBgBrush = ::CreateSolidBrush(palette.background);
    m_hSurfaceBrush = ::CreateSolidBrush(palette.surface);

    DwmMicaHelper::ApplyControlDarkTheme(m_hListView, m_darkMode);
    DwmMicaHelper::ApplyControlDarkTheme(m_hEditUrl, m_darkMode);

    if (m_darkMode) {
        ListView_SetBkColor(m_hListView, palette.surface);
        ListView_SetTextBkColor(m_hListView, palette.surface);
        ListView_SetTextColor(m_hListView, palette.textPrimary);
        if (m_hListHeader) ::SetWindowTheme(m_hListHeader, L"DarkMode_Header", nullptr);
        if (m_hStatusBar) {
            ::SetWindowTheme(m_hStatusBar, L"DarkMode_Explorer", nullptr);
            ::SendMessageW(m_hStatusBar, SB_SETBKCOLOR, 0, static_cast<LPARAM>(palette.surface));
        }
    } else {
        ListView_SetBkColor(m_hListView, CLR_NONE);
        ListView_SetTextBkColor(m_hListView, CLR_NONE);
        ListView_SetTextColor(m_hListView, static_cast<COLORREF>(-1));
        if (m_hListHeader) ::SetWindowTheme(m_hListHeader, L"Header", nullptr);
        if (m_hStatusBar) {
            ::SetWindowTheme(m_hStatusBar, L"Explorer", nullptr);
            ::SendMessageW(m_hStatusBar, SB_SETBKCOLOR, 0, static_cast<LPARAM>(CLR_DEFAULT));
        }
    }

    HMENU hMenu = ::GetMenu(m_hWnd);
    if (hMenu) {
        ::CheckMenuItem(hMenu, IDM_VIEW_DARK_MODE,
                        MF_BYCOMMAND | (m_darkMode ? MF_CHECKED : MF_UNCHECKED));
    }

    ::RedrawWindow(m_hWnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_UPDATENOW);
}

bool OdWindow::TryGetSelectedTask(core::DownloadTask& out) const {
    const int selected = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (selected < 0) {
        return false;
    }
    const auto tasks = m_engine.GetAllTasks();
    if (selected >= static_cast<int>(tasks.size())) {
        return false;
    }
    out = tasks[static_cast<size_t>(selected)];
    return true;
}

void OdWindow::OnAddDownloadClicked() {
    wchar_t buffer[2048] = {};
    ::GetWindowTextW(m_hEditUrl, buffer, static_cast<int>(std::size(buffer)));
    const std::wstring url = core::StringUtils::Trim(buffer);

    if (url.empty()) {
        ::MessageBoxW(m_hWnd, L"Paste a URL first.", L"Nothing to download", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!m_backendReady) {
        ::MessageBoxW(m_hWnd, L"Tor is still starting. Try again once the status bar reports ready.",
                      L"Please wait", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (m_engine.AddDownload(url, m_options.outputDir, m_options.workers)) {
        ::SetWindowTextW(m_hEditUrl, L"");
        RefreshTaskView();
    } else {
        ::MessageBoxW(m_hWnd, L"That URL is not a supported http:// address.",
                      L"Invalid URL", MB_OK | MB_ICONERROR);
    }
}

void OdWindow::OnToggleSelected() {
    core::DownloadTask task;
    if (!TryGetSelectedTask(task)) {
        return;
    }
    if (task.status == core::TaskStatus::Active) {
        m_engine.Pause(task.gid);
    } else if (task.status != core::TaskStatus::Completed) {
        m_engine.Resume(task.gid);
    }
    RefreshTaskView();
}

void OdWindow::OnRemoveSelected() {
    core::DownloadTask task;
    if (!TryGetSelectedTask(task)) {
        return;
    }

    int deletePartial = IDNO;
    if (task.status != core::TaskStatus::Completed) {
        deletePartial = ::MessageBoxW(
            m_hWnd,
            L"Also delete the partially downloaded data?\n\n"
            L"Choosing No keeps the .odpart file so the download can be resumed later.",
            L"Remove download", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (deletePartial == IDCANCEL) {
            return;
        }
    }

    m_engine.Remove(task.gid, deletePartial == IDYES);
    ListView_DeleteAllItems(m_hListView);
    m_rowProgress.clear();
    RefreshTaskView();
}

void OdWindow::RefreshTaskView() {
    const auto tasks = m_engine.GetAllTasks();

    core::DownloadTask selected;
    if (TryGetSelectedTask(selected)) {
        SetWindowTextIfChanged(m_hBtnPause,
                         selected.status == core::TaskStatus::Active ? L"Pause" : L"Resume");
    }
    const bool hasSelection = TryGetSelectedTask(selected);
    EnableWindowIfChanged(m_hBtnPause, hasSelection && selected.status != core::TaskStatus::Completed);
    EnableWindowIfChanged(m_hBtnRemove, hasSelection);

    while (ListView_GetItemCount(m_hListView) > static_cast<int>(tasks.size())) {
        ListView_DeleteItem(m_hListView, ListView_GetItemCount(m_hListView) - 1);
    }

    m_rowProgress.assign(tasks.size(), 0.0);

    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        const int row = static_cast<int>(i);
        const std::wstring& name = task.fileName.empty() ? task.url : task.fileName;

        if (row >= ListView_GetItemCount(m_hListView)) {
            Controls::InsertListItem(m_hListView, row, name.c_str());
        } else {
            Controls::SetListSubItem(m_hListView, row, 0, name.c_str());
        }

        const bool active = (task.status == core::TaskStatus::Active);
        m_rowProgress[i] = (task.status == core::TaskStatus::Completed) ? 100.0
                                                                       : task.GetProgressPercent();

        Controls::SetListSubItem(m_hListView, row, 1,
            task.totalBytes > 0 ? core::StringUtils::FormatBytes(task.totalBytes).c_str() : L"--");
        Controls::SetListSubItem(m_hListView, row, 2, L"");   // owner-drawn progress pill
        Controls::SetListSubItem(m_hListView, row, 3,
            active ? core::StringUtils::FormatSpeed(task.downloadSpeed).c_str() : L"--");
        Controls::SetListSubItem(m_hListView, row, 4,
            active ? std::to_wstring(task.activeConnections).c_str() : L"--");
        Controls::SetListSubItem(m_hListView, row, 5,
            active ? core::StringUtils::FormatDuration(task.GetRemainingSeconds()).c_str() : L"--");
        const std::wstring status = task.status == core::TaskStatus::Paused
            ? L"Paused — double-click or select Resume"
            : task.GetStatusString();
        Controls::SetListSubItem(m_hListView, row, 6, status.c_str());
    }
}

} // namespace od::ui

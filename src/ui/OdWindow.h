#pragma once

#include "../cli/CliParser.h"
#include "../core/DownloadEngine.h"
#include "../core/TorPool.h"

#include <windows.h>

#include <atomic>
#include <commctrl.h>
#include <oleidl.h>
#include <string>
#include <vector>

#define WM_APP_TASK_UPDATE (WM_APP + 101)
#define WM_APP_STATUS_TEXT (WM_APP + 102)

namespace od::ui {

/**
 * @brief Main window: Mica backdrop, Per-Monitor V2 DPI, live download table.
 *
 * The window owns the Tor pool and the download engine. Startup is asynchronous
 * - bootstrapping Tor takes seconds, and the interface must be interactive
 * immediately - so all backend progress arrives as posted messages rather than
 * through cross-thread control access.
 *
 * Both the progress column and the list header are owner-drawn. Comctl32 has no
 * dark theme for a report-view header, and no built-in way to put a progress
 * indicator inside a cell, so both are painted from the application palette.
 */
class OdWindow {
public:
    explicit OdWindow(const cli::CliOptions& options);
    ~OdWindow();

    OdWindow(const OdWindow&) = delete;
    OdWindow& operator=(const OdWindow&) = delete;

    bool Create(HINSTANCE hInstance, int nCmdShow = SW_SHOWDEFAULT);
    int RunMessageLoop();

    /// Called from the ListView subclass when its header asks to be drawn.
    LRESULT OnHeaderCustomDraw(LPNMCUSTOMDRAW customDraw);
    void PaintUrlCue(HWND edit, HDC dc) const;

private:
    static LRESULT CALLBACK WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnListCustomDraw(LPNMLVCUSTOMDRAW customDraw);

    void CreateLayout();
    void LayoutControls(int width, int height);
    void UpdateDpiScaling(UINT newDpi);
    void UpdateTheme(bool darkMode);
    void StartBackend();
    void OnAddDownloadClicked();
    void OnToggleSelected();
    void OnRemoveSelected();
    void RefreshTaskView();
    void PostStatusText(int part, const std::wstring& text);
    void PasteUrlFromClipboard();

    /// Give table rows enough height for the progress pill to breathe.
    void ApplyRowHeight();

    /// Absorb the leftover width into the trailing column so the table never
    /// grows a horizontal scroll bar.
    void StretchLastColumn(int listWidth);

    /// Owner-draw one push button from the palette (flat chip, hover/press).
    void DrawButton(const DRAWITEMSTRUCT& draw) const;

    /// Explain what the empty table is waiting for instead of showing a void.
    void DrawEmptyState(HDC hdc) const;

    [[nodiscard]] bool TryGetSelectedTask(core::DownloadTask& out) const;

    cli::CliOptions m_options;

    HINSTANCE m_hInstance{nullptr};
    HWND m_hWnd{nullptr};

    HWND m_hEditUrl{nullptr};
    HWND m_hBtnPaste{nullptr};
    HWND m_hBtnAdd{nullptr};
    HWND m_hBtnPause{nullptr};
    HWND m_hBtnRemove{nullptr};
    HWND m_hListView{nullptr};
    HWND m_hListHeader{nullptr};
    HWND m_hStatusBar{nullptr};

    HFONT m_hFontNormal{nullptr};
    /// One-pixel-wide image list used purely to give table rows breathing room;
    /// comctl32 derives row height from the small image list, nothing else.
    HIMAGELIST m_hRowSpacer{nullptr};
    HBRUSH m_hBgBrush{nullptr};
    HBRUSH m_hSurfaceBrush{nullptr};

    UINT m_currentDpi{96};
    bool m_darkMode{true};
    bool m_micaActive{false};
    std::atomic<bool> m_backendReady{false};

    /// Percentage per visible row, refreshed with the table and read back by the
    /// custom-draw handler (both run on the UI thread, so no locking is needed).
    std::vector<double> m_rowProgress;

    IDropTarget* m_dropTarget{nullptr};
    bool m_oleInitialized{false};

    // Declaration order matters: the engine holds a reference to the pool.
    core::TorPool m_torPool;
    core::DownloadEngine m_engine;
};

} // namespace od::ui

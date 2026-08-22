#include "ConsoleManager.h"
#include "../core/StringUtils.h"
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <iomanip>

namespace od::cli {

bool ConsoleManager::InitializeConsole(bool forceAllocate) {
    HANDLE hStdOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const bool isPiped = (hStdOut != nullptr && hStdOut != INVALID_HANDLE_VALUE && ::GetFileType(hStdOut) != FILE_TYPE_UNKNOWN);

    bool attached = false;
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        attached = true;
    } else if (forceAllocate && ::AllocConsole()) {
        attached = true;
    }

    if (attached) {
        FILE* fp;
        if (!isPiped) {
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        freopen_s(&fp, "CONIN$", "r", stdin);

        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
        return true;
    }

    return isPiped;
}

void ConsoleManager::DetachConsole() {
    fflush(stdout);
    fflush(stderr);
    ::FreeConsole();
}

void ConsoleManager::Write(std::wstring_view text) {
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (::GetConsoleMode(hOut, &mode)) {
            DWORD written = 0;
            ::WriteConsoleW(hOut, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            return;
        } else {
            // Piped output - convert to UTF-8
            std::string u8 = core::StringUtils::WideToUtf8(text);
            DWORD written = 0;
            ::WriteFile(hOut, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
            return;
        }
    }
    wprintf(L"%.*s", static_cast<int>(text.size()), text.data());
}

void ConsoleManager::WriteLine(std::wstring_view text) {
    Write(text);
    Write(L"\n");
}

void ConsoleManager::RenderProgressBar(double percent, uint64_t currentBytes, uint64_t totalBytes, uint64_t speedBytesPerSec, uint32_t connections) {
    constexpr int barWidth = 30;
    int progress = static_cast<int>((percent / 100.0) * barWidth);
    if (progress > barWidth) progress = barWidth;

    std::wstring bar;
    bar.reserve(barWidth + 2);
    bar.push_back(L'[');
    for (int i = 0; i < barWidth; ++i) {
        if (i < progress) bar.push_back(L'=');
        else if (i == progress) bar.push_back(L'>');
        else bar.push_back(L' ');
    }
    bar.push_back(L']');

    std::wstring curStr = core::StringUtils::FormatBytes(currentBytes);
    std::wstring totStr = core::StringUtils::FormatBytes(totalBytes);
    std::wstring spdStr = core::StringUtils::FormatSpeed(speedBytesPerSec);

    // Use carriage return \r to redraw the progress line in place
    wprintf(L"\r%s %5.1f%% %s/%s [%s] (%u streams)    ",
        bar.c_str(),
        percent,
        curStr.c_str(),
        totStr.c_str(),
        spdStr.c_str(),
        connections
    );
    fflush(stdout);
}

[[noreturn]] void ConsoleManager::ExitNow(int exitCode) {
    // Nudge cmd.exe / PowerShell to redraw its prompt immediately by simulating an Enter key
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != nullptr && hIn != INVALID_HANDLE_VALUE) {
        INPUT_RECORD ir[2]{};
        ir[0].EventType = KEY_EVENT;
        ir[0].Event.KeyEvent.bKeyDown = TRUE;
        ir[0].Event.KeyEvent.wRepeatCount = 1;
        ir[0].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        ir[0].Event.KeyEvent.wVirtualScanCode = 0x1C;
        ir[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
        ir[1] = ir[0];
        ir[1].Event.KeyEvent.bKeyDown = FALSE;
        DWORD written = 0;
        ::WriteConsoleInputW(hIn, ir, 2, &written);
    }

    fflush(stdout);
    fflush(stderr);
    ::FreeConsole();
    ::ExitProcess(static_cast<UINT>(exitCode));
}

} // namespace od::cli

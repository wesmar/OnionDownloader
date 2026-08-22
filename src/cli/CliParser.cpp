#include "CliParser.h"

#include "../core/StringUtils.h"

#include <algorithm>
#include <cstdlib>

namespace od::cli {

namespace {

bool Matches(const std::wstring& argument, std::initializer_list<const wchar_t*> forms) {
    return std::any_of(forms.begin(), forms.end(), [&](const wchar_t* form) {
        return core::StringUtils::EqualsIgnoreCase(argument, form);
    });
}

uint32_t ParseCount(const wchar_t* text, uint32_t fallback, uint32_t maximum) {
    const long parsed = ::wcstol(text, nullptr, 10);
    if (parsed <= 0) {
        return fallback;
    }
    return std::min<uint32_t>(static_cast<uint32_t>(parsed), maximum);
}

} // namespace

CliOptions CliParser::Parse(int argc, wchar_t** argv) {
    CliOptions options;
    if (argc <= 1) {
        options.guiMode = true;   // double-clicked from Explorer
        return options;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];

        if (Matches(argument, {L"-h", L"--help", L"/?"})) {
            options.showHelp = true;
            return options;
        }
        if (Matches(argument, {L"-v", L"--version"})) {
            options.showVersion = true;
            return options;
        }
        if (Matches(argument, {L"-g", L"--gui"})) {
            options.guiMode = true;
            continue;
        }
        if (Matches(argument, {L"-o", L"--out", L"--dir"}) && i + 1 < argc) {
            options.outputDir = argv[++i];
            continue;
        }
        if (Matches(argument, {L"-s", L"--split", L"--workers"}) && i + 1 < argc) {
            options.workers = ParseCount(argv[++i], 10, 64);
            continue;
        }
        if (Matches(argument, {L"-c", L"--circuits", L"--tor-instances"}) && i + 1 < argc) {
            options.torInstances = ParseCount(argv[++i], 0, 32);
            continue;
        }

        if (argument.rfind(L"http://", 0) == 0 ||
            argument.rfind(L"https://", 0) == 0 ||
            argument.find(L".onion") != std::wstring::npos) {
            options.url = argument;
            continue;
        }

        options.invalidArguments = true;
        options.argumentError = L"Unknown option or argument: " + argument;
        return options;
    }

    return options;
}

std::wstring CliParser::GetHelpText() {
    return
        L"\n"
        L"===============================================================================\n"
        L" Onion Downloader - Multi-Circuit Tor Engine v1.0.0\n"
        L" (C) Marek Wesolowski (2026)\n"
        L"===============================================================================\n\n"
        L"USAGE:\n"
        L"  od.exe [options] <URL>\n\n"
        L"OPTIONS:\n"
        L"  <URL>                  Target http:// URL, normally an .onion mirror.\n"
        L"  -s, --split <N>        Parallel workers, one Tor circuit each (default: 10).\n"
        L"  -c, --circuits <N>     Independent Tor daemons (default: auto, 2-8).\n"
        L"  -o, --out <dir>        Destination directory (default: user Downloads).\n"
        L"  -g, --gui              Launch GUI (default with no arguments).\n"
        L"  -h, --help             Show this help and exit.\n"
        L"  -v, --version          Show version information.\n\n"
        L"NOTES:\n"
        L"  Interrupted downloads resume automatically: progress is kept in a\n"
        L"  <file>.odpart / <file>.odmeta pair next to the destination.\n\n"
        L"EXAMPLES:\n"
        L"  od.exe http://example.onion/dataset.zip\n"
        L"  od.exe -s 20 -c 8 -o D:\\Data http://example.onion/bigfile.bin\n"
        L"  od.exe --gui\n";
}

std::wstring CliParser::GetVersionText() {
    return
        L"\n"
        L"Onion Downloader - Multi-Circuit Tor Engine v1.0.0\n"
        L"(C) Marek Wesolowski (2026)\n"
        L"Windows x64 / C++23 / MSVC v145 / WinAPI\n";
}

} // namespace od::cli

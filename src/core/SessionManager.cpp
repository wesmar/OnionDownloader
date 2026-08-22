#include "SessionManager.h"
#include "StringUtils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <mutex>
#include <windows.h>

namespace od::core {

namespace {

std::string EscapeJsonString(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

std::string ExtractField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos += needle.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        std::string value;
        bool escaped = false;
        for (size_t i = pos + 1; i < json.size(); ++i) {
            const char c = json[i];
            if (escaped) {
                switch (c) {
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    default: value.push_back(c); break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return value;
            } else {
                value.push_back(c);
            }
        }
        return "";
    } else {
        size_t end = json.find_first_of(",}\n\r", pos);
        if (end == std::string::npos) end = json.size();
        return json.substr(pos, end - pos);
    }
}

} // namespace

std::wstring SessionManager::GetDefaultSessionPath() {
    wchar_t exePath[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path currentExeDir = std::filesystem::path(exePath).parent_path();
        return (currentExeDir / L"session.json").wstring();
    }
    return L"session.json";
}

bool SessionManager::SaveSession(const std::vector<DownloadTask>& tasks, const std::wstring& filePath) {
    static std::mutex sessionWriteMutex;
    std::lock_guard<std::mutex> writeLock(sessionWriteMutex);
    try {
        const std::filesystem::path target(filePath);
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        std::ofstream file(temporary, std::ios::out | std::ios::trunc);
        if (!file.is_open()) return false;

        file << "[\n";
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto& t = tasks[i];
            file << "  {\n";
            file << "    \"gid\": \"" << EscapeJsonString(StringUtils::WideToUtf8(t.gid)) << "\",\n";
            file << "    \"url\": \"" << EscapeJsonString(StringUtils::WideToUtf8(t.url)) << "\",\n";
            file << "    \"fileName\": \"" << EscapeJsonString(StringUtils::WideToUtf8(t.fileName)) << "\",\n";
            file << "    \"destinationPath\": \"" << EscapeJsonString(StringUtils::WideToUtf8(t.destinationPath)) << "\",\n";
            file << "    \"totalBytes\": " << t.totalBytes << ",\n";
            file << "    \"completedBytes\": " << t.completedBytes << ",\n";
            file << "    \"totalSplits\": " << t.totalSplits << ",\n";
            file << "    \"status\": " << static_cast<int>(t.status) << "\n";
            file << "  }" << (i + 1 < tasks.size() ? "," : "") << "\n";
        }
        file << "]\n";
        file.flush();
        if (!file.good()) return false;
        file.close();

        if (::ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                           REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            return true;
        }
        if (::GetLastError() == ERROR_FILE_NOT_FOUND &&
            ::MoveFileExW(temporary.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        return false;
    } catch (...) {
        return false;
    }
}

std::vector<DownloadTask> SessionManager::LoadSession(const std::wstring& filePath) {
    std::vector<DownloadTask> tasks;
    if (!std::filesystem::exists(filePath)) return tasks;

    try {
        std::ifstream file(filePath);
        if (!file.is_open()) return tasks;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t pos = 0;

        while ((pos = content.find('{', pos)) != std::string::npos) {
            size_t endPos = content.find('}', pos);
            if (endPos == std::string::npos) break;

            std::string block = content.substr(pos, endPos - pos + 1);
            pos = endPos + 1;

            DownloadTask task;
            task.gid = StringUtils::Utf8ToWide(ExtractField(block, "gid"));
            task.url = StringUtils::Utf8ToWide(ExtractField(block, "url"));
            task.fileName = StringUtils::Utf8ToWide(ExtractField(block, "fileName"));
            task.destinationPath = StringUtils::Utf8ToWide(ExtractField(block, "destinationPath"));
            
            std::string totalStr = ExtractField(block, "totalBytes");
            std::string compStr = ExtractField(block, "completedBytes");
            std::string splitsStr = ExtractField(block, "totalSplits");
            std::string statusStr = ExtractField(block, "status");

            if (!totalStr.empty()) task.totalBytes = std::strtoull(totalStr.c_str(), nullptr, 10);
            if (!compStr.empty()) task.completedBytes = std::strtoull(compStr.c_str(), nullptr, 10);
            if (!splitsStr.empty()) task.totalSplits = static_cast<uint32_t>(std::strtoul(splitsStr.c_str(), nullptr, 10));
            
            int statusInt = statusStr.empty() ? 2 : std::atoi(statusStr.c_str());
            if (statusInt == 3) {
                task.status = TaskStatus::Completed;
            } else if (task.totalBytes > 0 && task.completedBytes >= task.totalBytes) {
                task.status = TaskStatus::Completed;
            } else {
                task.status = TaskStatus::Paused; // Load active/pending as Paused so user can resume
            }

            if (!task.url.empty()) {
                tasks.push_back(task);
            }
        }
    } catch (...) {
    }

    return tasks;
}

} // namespace od::core

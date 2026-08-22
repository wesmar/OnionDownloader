#pragma once

#include "DownloadTask.h"
#include <vector>
#include <string>

namespace od::core {

class SessionManager {
public:
    static bool SaveSession(const std::vector<DownloadTask>& tasks, const std::wstring& filePath);
    static std::vector<DownloadTask> LoadSession(const std::wstring& filePath);
    static std::wstring GetDefaultSessionPath();
};

} // namespace od::core

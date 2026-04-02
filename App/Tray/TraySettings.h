#pragma once

#include <string>

struct TraySettings
{
    bool debugMode = false;
    bool logToFile = false;
    std::wstring logFilePath;

    static TraySettings load(const std::wstring& iniPath);
    static void save(const std::wstring& iniPath, const TraySettings& settings);
    static void createDefaultIfMissing(const std::wstring& iniPath);
};

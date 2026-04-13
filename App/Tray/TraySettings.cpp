#include "pch.h"
#include "App/Tray/TraySettings.h"

#include <fstream>
#include <sstream>

namespace
{
    std::wstring trim(const std::wstring& s)
    {
        size_t start = s.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos)
        {
            return L"";
        }
        size_t end = s.find_last_not_of(L" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string narrowPath(const std::wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (const wchar_t ch : ws)
        {
            out.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        return out;
    }

    std::wstring widenString(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }
}

TraySettings TraySettings::load(const std::wstring& iniPath)
{
    TraySettings settings;

    std::ifstream file(narrowPath(iniPath));
    if (!file.is_open())
    {
        return settings;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
        {
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        // Trim whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        {
            key.pop_back();
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
            value.erase(0, 1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        {
            value.pop_back();
        }

        if (key == "debug")
        {
            settings.debugMode = (value == "true" || value == "1" || value == "yes");
        }
        else if (key == "log_to_file")
        {
            settings.logToFile = (value == "true" || value == "1" || value == "yes");
        }
        else if (key == "log_file_path")
        {
            settings.logFilePath = widenString(value);
        }
    }

    return settings;
}

void TraySettings::save(const std::wstring& iniPath, const TraySettings& settings)
{
    std::ofstream file(narrowPath(iniPath));
    if (!file.is_open())
    {
        return;
    }

    file << "[Pixels]\n";
    file << "; Set debug=true to see all log messages (default: false shows only roll results)\n";
    file << "debug=" << (settings.debugMode ? "true" : "false") << "\n";
    file << "\n";
    file << "; Set log_to_file=true to save logs to a file\n";
    file << "log_to_file=" << (settings.logToFile ? "true" : "false") << "\n";
    file << "; Path for log file (leave empty for default: pixels_log.txt in app folder)\n";
    file << "log_file_path=" << narrowPath(settings.logFilePath) << "\n";
}

void TraySettings::createDefaultIfMissing(const std::wstring& iniPath)
{
    std::ifstream test(narrowPath(iniPath));
    if (test.is_open())
    {
        test.close();
        return;
    }

    TraySettings defaults;
    save(iniPath, defaults);
}

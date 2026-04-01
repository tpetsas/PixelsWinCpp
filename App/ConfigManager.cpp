#include "pch.h"
#include "App/ConfigManager.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace
{
    // Keep config parsing permissive for whitespace, but strict on values.
    std::string trimCopy(const std::string& s)
    {
        const auto begin = s.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            return "";
        }

        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(begin, end - begin + 1);
    }

    bool parsePixelIdToken(const std::string& token, uint32_t& outValue)
    {
        const std::string trimmed = trimCopy(token);
        if (trimmed.empty())
        {
            return false;
        }

        size_t parsedCount = 0;
        unsigned long value = 0;
        try
        {
            value = std::stoul(trimmed, &parsedCount, 0);
        }
        catch (...)
        {
            return false;
        }

        if (parsedCount != trimmed.size())
        {
            return false;
        }

        outValue = static_cast<uint32_t>(value);
        return true;
    }

    bool validateConfig(const PixelsConfig& config, std::string& outError)
    {
        if (config.pixelIds.empty() || config.pixelIds.size() > 2)
        {
            outError = "Config must contain exactly 1 or 2 pixel IDs.";
            return false;
        }

        std::set<uint32_t> seen;
        for (const uint32_t id : config.pixelIds)
        {
            if (!seen.insert(id).second)
            {
                outError = "Config contains duplicate pixel IDs.";
                return false;
            }
        }

        return true;
    }
}

bool ConfigManager::load(const std::string& path, PixelsConfig& outConfig, std::string& outError)
{
    outConfig.pixelIds.clear();
    outError.clear();

    std::ifstream in(path);
    if (!in)
    {
        outError = "Could not open config file: " + path;
        return false;
    }

    // File format:
    //   version=1
    //   dice=0xAABBCCDD,0x11223344
    std::string line;
    int version = 0;
    bool hasVersion = false;
    bool hasDice = false;

    while (std::getline(in, line))
    {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }

        const auto eqPos = trimmed.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }

        const std::string key = trimCopy(trimmed.substr(0, eqPos));
        const std::string value = trimCopy(trimmed.substr(eqPos + 1));

        if (key == "version")
        {
            try
            {
                version = std::stoi(value);
                hasVersion = true;
            }
            catch (...)
            {
                outError = "Invalid config version.";
                return false;
            }
        }
        else if (key == "dice")
        {
            hasDice = true;

            std::string normalized = value;
            std::replace(normalized.begin(), normalized.end(), ';', ',');

            std::stringstream ss(normalized);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                uint32_t parsedId = 0;
                if (!parsePixelIdToken(token, parsedId))
                {
                    outError = "Invalid pixel ID in config: " + trimCopy(token);
                    return false;
                }
                outConfig.pixelIds.push_back(parsedId);
            }
        }
    }

    if (!hasVersion)
    {
        outError = "Missing config version.";
        return false;
    }

    if (version != 1)
    {
        outError = "Unsupported config version: " + std::to_string(version);
        return false;
    }

    if (!hasDice)
    {
        outError = "Missing 'dice' entry in config.";
        return false;
    }

    return validateConfig(outConfig, outError);
}

bool ConfigManager::save(const std::string& path, const PixelsConfig& config, std::string& outError)
{
    outError.clear();

    if (!validateConfig(config, outError))
    {
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        outError = "Could not write config file: " + path;
        return false;
    }

    // Always write the canonical format expected by load().
    out << "version=1\n";
    out << "dice=";
    for (size_t i = 0; i < config.pixelIds.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        out << formatPixelId(config.pixelIds[i]);
    }
    out << "\n";

    if (!out)
    {
        outError = "Failed while writing config file: " + path;
        return false;
    }

    return true;
}

std::string ConfigManager::formatPixelId(uint32_t pixelId)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << pixelId;
    return oss.str();
}

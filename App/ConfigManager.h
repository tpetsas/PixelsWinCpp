#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PixelsConfig
{
    std::vector<uint32_t> pixelIds;
};

class ConfigManager
{
public:
    static bool load(const std::string& path, PixelsConfig& outConfig, std::string& outError);
    static bool save(const std::string& path, const PixelsConfig& config, std::string& outError);

    static std::string formatPixelId(uint32_t pixelId);
};

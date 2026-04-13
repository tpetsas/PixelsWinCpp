#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "Systemic/Pixels/Pixel.h"

struct DieStatusSnapshot
{
    uint32_t targetPixelId = 0;
    std::string label;

    bool selected = false;
    bool connecting = false;
    bool connectedOnce = false;
    bool hasPixel = false;

    Systemic::Pixels::PixelStatus status = Systemic::Pixels::PixelStatus::Disconnected;
    int batteryLevel = 0;
    int currentFace = 0;
    bool isCharging = false;

    bool hasLastRoll = false;
    int lastRollFace = 0;
    std::chrono::system_clock::time_point lastRollAt{};
    std::vector<int> recentRollFaces;
};

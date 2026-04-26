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
    Systemic::Pixels::PixelRollState rollState = Systemic::Pixels::PixelRollState::Unknown;
    int batteryLevel = 0;
    int currentFace = 0;
    bool isCharging = false;

    bool hasLastRoll = false;
    int lastRollFace = 0;
    std::chrono::system_clock::time_point lastRollAt{};
    std::vector<int> recentRollFaces;

    // In-progress advertisement debounce state (non-zero only during a disconnect event
    // where the die was rolling; resets to 0 after each advert-reported result)
    int advertSettledFace = 0;
    int advertSettledCount = 0;

    // True if the die was physically rolling (rollState == Rolling/Handling) when BLE dropped.
    // currentFace is unreliable in this case — it's wherever the die was mid-tumble, not
    // the final landed face. Do not use it as a fallback result when this is true.
    bool wasRollingAtDisconnect = false;
};

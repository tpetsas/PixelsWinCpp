#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "App/DieConnection.h"

namespace Systemic::Pixels
{
    class PixelScanner;
    class ScannedPixel;
}

class DiceManager
{
public:
    using Logger = std::function<void(const std::string&)>;

    DiceManager(const std::vector<uint32_t>& targetPixelIds, Logger logger);
    ~DiceManager();

    DiceManager(const DiceManager&) = delete;
    DiceManager& operator=(const DiceManager&) = delete;

    void runUntilEnterPressed();

private:
    void startScanner();
    void stopScanner();
    bool allDiceSelected() const;
    std::string configuredDiceText() const;

    Logger logger_;

    std::vector<std::unique_ptr<DieConnection>> dice_;
    std::unique_ptr<Systemic::Pixels::PixelScanner> scanner_;

    std::atomic<bool> inputDone_ = false;
    std::atomic<bool> stopScanRequested_ = false;

    std::thread inputThread_;
    std::thread pollerThread_;
    std::thread watchdogThread_;
};

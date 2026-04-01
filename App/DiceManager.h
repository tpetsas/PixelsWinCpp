#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "App/DieConnection.h"
#include "App/Runtime/RuntimeModels.h"

namespace Systemic::Pixels
{
    class PixelScanner;
    class ScannedPixel;
}

class DiceManager
{
public:
    using Logger = std::function<void(const std::string&)>;
    using StateObserver = std::function<void()>;

    DiceManager(const std::vector<uint32_t>& targetPixelIds, Logger logger, StateObserver stateObserver = nullptr);
    ~DiceManager();

    DiceManager(const DiceManager&) = delete;
    DiceManager& operator=(const DiceManager&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    void runUntilEnterPressed();
    std::vector<DieStatusSnapshot> snapshotDice() const;
    bool isScanning() const;

private:
    void startScanner();
    void stopScanner();
    bool allDiceSelected() const;
    std::string configuredDiceText() const;

    Logger logger_;
    StateObserver stateObserver_;

    std::vector<std::unique_ptr<DieConnection>> dice_;
    std::unique_ptr<Systemic::Pixels::PixelScanner> scanner_;
    mutable std::mutex scannerMutex_;

    std::atomic<bool> inputDone_ = false;
    std::atomic<bool> stopScanRequested_ = false;

    std::thread inputThread_;
    std::thread maintenanceThread_;
    std::thread pollerThread_;
    std::thread watchdogThread_;

    std::atomic<bool> running_ = false;
};

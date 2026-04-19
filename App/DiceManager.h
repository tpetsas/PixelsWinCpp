#pragma once

#include <atomic>
#include <chrono>
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
    using RollObserver = std::function<void(const std::string& label, int face)>;

    DiceManager(const std::vector<uint32_t>& targetPixelIds, Logger logger,
               StateObserver stateObserver = nullptr, RollObserver rollObserver = nullptr);
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
    void checkDisconnectedDieScan();
    void checkRecoveryScan();
    void checkAdapterContention();
    void checkFullBleReset();
    bool allDiceSelected() const;
    bool anyDieDisconnected() const;
    std::string configuredDiceText() const;

    Logger logger_;
    StateObserver stateObserver_;
    RollObserver rollObserver_;

    std::vector<std::unique_ptr<DieConnection>> dice_;
    std::unique_ptr<Systemic::Pixels::PixelScanner> scanner_;
    mutable std::mutex scannerMutex_;

    std::atomic<bool> inputDone_ = false;
    std::atomic<bool> stopScanRequested_ = false;

    // Adapter contention — release a connected die when another is failing repeatedly
    std::chrono::steady_clock::time_point lastAdapterRelease_ = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    static constexpr int kAdapterContentionThreshold = 2;
    static constexpr int kAdapterReleaseCooldownSeconds = 30;
    static constexpr int kAdapterReleaseGracePeriodSeconds = 15;

    // Full BLE reset — disconnect everything when a die is hopelessly stuck
    std::chrono::steady_clock::time_point lastFullBleReset_ = std::chrono::steady_clock::now() - std::chrono::seconds(120);
    static constexpr int kFullBleResetCooldownSeconds = 60;
    static constexpr int kFullBleResetDelaySeconds = 3;

    // Recovery scanning
    bool recoveryScanActive_ = false;
    std::chrono::steady_clock::time_point recoveryScanStartTime_{};
    std::chrono::steady_clock::time_point lastRecoveryScanEnd_ = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    static constexpr int kRecoveryScanDurationSeconds = 10;
    static constexpr int kRecoveryScanCooldownSeconds = 20;

    std::thread inputThread_;
    std::thread maintenanceThread_;
    std::thread pollerThread_;
    std::thread watchdogThread_;

    std::atomic<bool> running_ = false;
};

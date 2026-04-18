#include "pch.h"
#include "App/DiceManager.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <sstream>

#include "App/ConfigManager.h"
#include "Systemic/Pixels/PixelScanner.h"

using namespace Systemic::Pixels;
using namespace std::chrono_literals;

DiceManager::DiceManager(const std::vector<uint32_t>& targetPixelIds, Logger logger, StateObserver stateObserver)
    : logger_(std::move(logger)),
      stateObserver_(std::move(stateObserver))
{
    for (size_t i = 0; i < targetPixelIds.size(); ++i)
    {
        const std::string label = "die " + std::to_string(i + 1);
        dice_.emplace_back(std::make_unique<DieConnection>(targetPixelIds[i], label, logger_, stateObserver_));
    }
}

DiceManager::~DiceManager()
{
    stop();
    if (inputThread_.joinable())
    {
        inputThread_.join();
    }
}

void DiceManager::start()
{
    if (running_)
    {
        return;
    }

    inputDone_ = false;
    stopScanRequested_ = false;
    running_ = true;

    startScanner();

    maintenanceThread_ = std::thread([this]()
    {
        while (!inputDone_)
        {
            if (stopScanRequested_)
            {
                stopScanner();
                stopScanRequested_ = false;
                recoveryScanActive_ = false;
                if (logger_)
                {
                    logger_("\nStopped scanning after selecting configured dice.");
                }
            }

            checkDisconnectedDieScan();
            checkRecoveryScan();
            // Contention disabled — releasing a healthy die when the adapter is
            // poisoned just makes both dice stuck.  Full BLE reset is the
            // correct recovery mechanism.
            // checkAdapterContention();
            checkFullBleReset();

            std::this_thread::sleep_for(200ms);
        }
    });

    pollerThread_ = std::thread([this]()
    {
        try
        {
            while (!inputDone_)
            {
                std::this_thread::sleep_for(5s);
                for (auto& die : dice_)
                {
                    die->tickPoll();
                }
            }
        }
        catch (const std::exception& ex)
        {
            if (logger_)
            {
                logger_(std::string("\nPoller exception: ") + ex.what());
            }
        }
        catch (...)
        {
            if (logger_)
            {
                logger_("\nPoller unknown exception.");
            }
        }
    });

    watchdogThread_ = std::thread([this]()
    {
        try
        {
            while (!inputDone_)
            {
                std::this_thread::sleep_for(3s);
                for (auto& die : dice_)
                {
                    die->tickWatchdog();
                }
            }
        }
        catch (const std::exception& ex)
        {
            if (logger_)
            {
                logger_(std::string("\nWatchdog exception: ") + ex.what());
            }
        }
        catch (...)
        {
            if (logger_)
            {
                logger_("\nWatchdog unknown exception.");
            }
        }
    });
}

void DiceManager::stop()
{
    if (!running_)
    {
        return;
    }

    inputDone_ = true;

    stopScanner();
    for (auto& die : dice_)
    {
        die->shutdown();
    }

    if (maintenanceThread_.joinable())
    {
        maintenanceThread_.join();
    }

    if (watchdogThread_.joinable())
    {
        watchdogThread_.join();
    }

    if (pollerThread_.joinable())
    {
        pollerThread_.join();
    }

    running_ = false;
}

bool DiceManager::isRunning() const
{
    return running_;
}

void DiceManager::runUntilEnterPressed()
{
    if (logger_)
    {
        logger_("Scanning for configured Pixel dice...\nConfigured IDs: " + configuredDiceText() + "\nPress Enter to exit.");
    }

    start();

    if (inputThread_.joinable())
    {
        inputThread_.join();
    }

    inputThread_ = std::thread([this]()
    {
        try
        {
            std::string line;
            std::getline(std::cin, line);
        }
        catch (...)
        {
        }
        inputDone_ = true;
    });

    while (!inputDone_)
    {
        std::this_thread::sleep_for(200ms);
    }

    stop();

    if (inputThread_.joinable())
    {
        inputThread_.join();
    }
}

std::vector<DieStatusSnapshot> DiceManager::snapshotDice() const
{
    std::vector<DieStatusSnapshot> snapshots;
    snapshots.reserve(dice_.size());
    for (const auto& die : dice_)
    {
        snapshots.push_back(die->snapshot());
    }
    return snapshots;
}

bool DiceManager::isScanning() const
{
    std::lock_guard<std::mutex> lock(scannerMutex_);
    return scanner_ != nullptr;
}

void DiceManager::startScanner()
{
    PixelScanner::ScannedPixelListener listener = [this](const std::shared_ptr<const ScannedPixel>& scannedPixel)
    {
        if (!scannedPixel)
        {
            return;
        }

        bool selectedAny = false;
        for (auto& die : dice_)
        {
            if (die->trySelectScannedPixel(scannedPixel))
            {
                selectedAny = true;
            }

            // Feed advertisement data to disconnected dice for fast roll recovery
            die->processAdvertisement(scannedPixel);
        }

        // Do not stop scanner directly from its callback thread.
        // Request stop and let runUntilEnterPressed() stop it safely.
        // But keep scanner running if any die is disconnected (for advert-based roll recovery).
        if (selectedAny && allDiceSelected() && !anyDieDisconnected())
        {
            stopScanRequested_ = true;
        }
    };

    std::lock_guard<std::mutex> lock(scannerMutex_);
    scanner_ = std::make_unique<PixelScanner>(listener);
    scanner_->start();

    if (stateObserver_)
    {
        stateObserver_();
    }
}

void DiceManager::stopScanner()
{
    std::lock_guard<std::mutex> lock(scannerMutex_);

    if (!scanner_)
    {
        return;
    }

    try
    {
        scanner_->stop();
    }
    catch (...)
    {
    }

    scanner_.reset();

    if (stateObserver_)
    {
        stateObserver_();
    }
}

void DiceManager::checkRecoveryScan()
{
    const auto now = std::chrono::steady_clock::now();

    // If a recovery scan is active, check if we should stop it
    if (recoveryScanActive_)
    {
        bool stillNeedsRecovery = false;
        for (const auto& die : dice_)
        {
            if (die->needsRecoveryScan())
            {
                stillNeedsRecovery = true;
                break;
            }
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - recoveryScanStartTime_).count();

        if (!stillNeedsRecovery)
        {
            if (logger_)
            {
                logger_("\n[recovery] Recovery scan: die re-discovered, stopping scanner.");
            }
            stopScanner();
            recoveryScanActive_ = false;
            lastRecoveryScanEnd_ = now;
        }
        else if (elapsed >= kRecoveryScanDurationSeconds)
        {
            if (logger_)
            {
                logger_("\n[recovery] Recovery scan timeout (" + std::to_string(elapsed) + "s), stopping scanner.");
            }
            stopScanner();
            recoveryScanActive_ = false;
            lastRecoveryScanEnd_ = now;
        }

        return;
    }

    // Check if any die needs a recovery scan
    if (isScanning())
    {
        return;  // Initial scan still running
    }

    bool needsRecovery = false;
    for (const auto& die : dice_)
    {
        if (die->needsRecoveryScan())
        {
            needsRecovery = true;
            break;
        }
    }

    if (!needsRecovery)
    {
        return;
    }

    // Cooldown: don't start recovery scans too frequently
    const auto cooldown = std::chrono::duration_cast<std::chrono::seconds>(now - lastRecoveryScanEnd_).count();
    if (cooldown < kRecoveryScanCooldownSeconds)
    {
        return;
    }

    if (logger_)
    {
        logger_("\n[recovery] Starting recovery scan for disconnected dice...");
    }

    startScanner();
    recoveryScanActive_ = true;
    recoveryScanStartTime_ = now;
}

void DiceManager::checkAdapterContention()
{
    // Look for a die that is failing repeatedly while another is connected.
    // This pattern suggests the BLE adapter can't handle the load, so we
    // temporarily release the healthy die to free adapter resources.
    DieConnection* failingDie = nullptr;
    DieConnection* connectedDie = nullptr;

    for (auto& die : dice_)
    {
        const auto state = die->connectionState();
        const int failures = die->consecutiveFailures();

        if (failures >= kAdapterContentionThreshold && state == ConnectionState::Selected)
        {
            failingDie = die.get();
        }
        else if (state == ConnectionState::Ready)
        {
            connectedDie = die.get();
        }
    }

    if (!failingDie || !connectedDie)
    {
        return;
    }

    // If the failing die qualifies for recovery scanning, the problem is
    // deeper than adapter contention — stop releasing the healthy die
    // and let recovery scanning find fresh advertisement data instead
    if (failingDie->needsRecoveryScan())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAdapterRelease_).count();
    if (elapsed < kAdapterReleaseCooldownSeconds)
    {
        return;
    }

    // Don't release a die that just reconnected — prevents yo-yo where
    // contention immediately tears down a die that just recovered
    const auto connectedFor = std::chrono::duration_cast<std::chrono::seconds>(
        now - connectedDie->lastSuccessfulConnect()).count();
    if (connectedFor < kAdapterReleaseGracePeriodSeconds)
    {
        return;
    }

    if (logger_)
    {
        logger_("\n[contention] " + failingDie->label() + " has " +
                std::to_string(failingDie->consecutiveFailures()) +
                " failures while " + connectedDie->label() +
                " is connected (" + std::to_string(connectedFor) +
                "s) — releasing to free BLE adapter");
    }

    connectedDie->forceReleaseConnection();
    failingDie->requestPriorityReconnect();
    lastAdapterRelease_ = now;
}

void DiceManager::checkFullBleReset()
{
    // When a die has been failing for a very long time (5+ failures),
    // normal reconnection and recovery scanning haven't helped.
    // The Windows BLE stack has likely cached bad state for this device.
    // Nuclear option: disconnect ALL dice, wait, and restart one at a time.
    bool needsReset = false;
    for (const auto& die : dice_)
    {
        if (die->needsFullBleReset())
        {
            needsReset = true;
            break;
        }
    }

    if (!needsReset)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastFullBleReset_).count();
    if (elapsed < kFullBleResetCooldownSeconds)
    {
        return;
    }

    if (logger_)
    {
        logger_("\n[RESET] BLE adapter appears stuck — disconnecting ALL dice and restarting connections");
    }

    // CRITICAL: Suspend ALL watchdog reconnection for the duration of the reset + stagger.
    // Without this, the watchdog starts reconnecting during the 3s cleanup window.
    const auto totalSuspendTime = std::chrono::seconds(kFullBleResetDelaySeconds + 10);
    for (auto& die : dice_)
    {
        die->suspendReconnectUntil(std::chrono::steady_clock::now() + totalSuspendTime);
    }

    // Force-release every die (connected or not)
    for (auto& die : dice_)
    {
        die->forceReleaseConnection();
    }

    if (logger_)
    {
        logger_("[RESET] Waiting " + std::to_string(kFullBleResetDelaySeconds) +
                "s for BLE adapter cleanup...");
    }

    // Wait for BLE adapter to fully clean up
    std::this_thread::sleep_for(std::chrono::seconds(kFullBleResetDelaySeconds));

    if (logger_)
    {
        logger_("[RESET] BLE cleanup done — resuming dice with staggered timing");
    }

    // Stagger reconnection: resume dice one at a time with 8s gaps.
    // Simultaneous connection attempts compete for the BLE adapter and both fail.
    const auto resumeNow = std::chrono::steady_clock::now();
    for (size_t i = 0; i < dice_.size(); ++i)
    {
        auto delay = std::chrono::seconds(i * 8);
        dice_[i]->suspendReconnectUntil(resumeNow + delay);
        if (logger_)
        {
            logger_("[RESET] " + dice_[i]->label() + " will resume in " +
                    std::to_string(i * 8) + "s");
        }
    }

    lastFullBleReset_ = std::chrono::steady_clock::now();
}

bool DiceManager::allDiceSelected() const
{
    for (const auto& die : dice_)
    {
        if (!die->isSelected())
        {
            return false;
        }
    }
    return true;
}

bool DiceManager::anyDieDisconnected() const
{
    for (const auto& die : dice_)
    {
        const auto state = die->connectionState();
        if (state == ConnectionState::Selected || state == ConnectionState::Reconnecting)
        {
            return true;
        }
    }
    return false;
}

void DiceManager::checkDisconnectedDieScan()
{
    const bool disconnected = anyDieDisconnected();

    // If scanner is running (not as a recovery scan) and all dice are back, stop it
    if (isScanning() && !recoveryScanActive_ && !disconnected && allDiceSelected())
    {
        if (logger_)
        {
            logger_("\n[advert-scan] All dice reconnected, stopping advertisement scanner.");
        }
        stopScanner();
        return;
    }

    // If scanner is already running or recovery scan is active, nothing to do
    if (isScanning() || recoveryScanActive_)
    {
        return;
    }

    // Start scanner if any die is disconnected so we can receive advertisements
    // for fast missed-roll recovery, regardless of failure count
    if (!disconnected)
    {
        return;
    }

    if (logger_)
    {
        logger_("\n[advert-scan] Starting scanner for disconnected die advertisement recovery...");
    }

    startScanner();
}

std::string DiceManager::configuredDiceText() const
{
    std::ostringstream oss;

    for (size_t i = 0; i < dice_.size(); ++i)
    {
        if (i > 0)
        {
            oss << ", ";
        }
        oss << ConfigManager::formatPixelId(dice_[i]->targetPixelId());
    }

    return oss.str();
}

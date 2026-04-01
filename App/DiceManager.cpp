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

DiceManager::DiceManager(const std::vector<uint32_t>& targetPixelIds, Logger logger)
    : logger_(std::move(logger))
{
    for (size_t i = 0; i < targetPixelIds.size(); ++i)
    {
        const std::string label = "die " + std::to_string(i + 1);
        dice_.emplace_back(std::make_unique<DieConnection>(targetPixelIds[i], label, logger_));
    }
}

DiceManager::~DiceManager()
{
    inputDone_ = true;

    stopScanner();
    for (auto& die : dice_)
    {
        die->shutdown();
    }

    if (watchdogThread_.joinable())
    {
        watchdogThread_.join();
    }

    if (pollerThread_.joinable())
    {
        pollerThread_.join();
    }

    if (inputThread_.joinable())
    {
        inputThread_.join();
    }
}

void DiceManager::runUntilEnterPressed()
{
    if (logger_)
    {
        logger_("Scanning for configured Pixel dice...\nConfigured IDs: " + configuredDiceText() + "\nPress Enter to exit.");
    }

    startScanner();

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

    while (!inputDone_)
    {
        if (stopScanRequested_)
        {
            stopScanner();
            stopScanRequested_ = false;
            if (logger_)
            {
                logger_("\nStopped scanning after selecting configured dice.");
            }
        }
        std::this_thread::sleep_for(200ms);
    }

    stopScanner();
    for (auto& die : dice_)
    {
        die->shutdown();
    }

    if (watchdogThread_.joinable())
    {
        watchdogThread_.join();
    }

    if (pollerThread_.joinable())
    {
        pollerThread_.join();
    }

    if (inputThread_.joinable())
    {
        inputThread_.join();
    }
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
        }

        // Do not stop scanner directly from its callback thread.
        // Request stop and let runUntilEnterPressed() stop it safely.
        if (selectedAny && allDiceSelected())
        {
            stopScanRequested_ = true;
        }
    };

    scanner_ = std::make_unique<PixelScanner>(listener);
    scanner_->start();
}

void DiceManager::stopScanner()
{
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

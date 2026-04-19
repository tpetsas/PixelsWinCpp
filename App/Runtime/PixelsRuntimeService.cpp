#include "pch.h"
#include "App/Runtime/PixelsRuntimeService.h"

#include "App/DiceManager.h"

PixelsRuntimeService::PixelsRuntimeService(Logger logger, StateObserver stateObserver, RollObserver rollObserver)
    : logger_(std::move(logger)),
      stateObserver_(std::move(stateObserver)),
      rollObserver_(std::move(rollObserver))
{
}

PixelsRuntimeService::~PixelsRuntimeService()
{
    stop();
}

bool PixelsRuntimeService::loadConfig(const std::string& configPath, std::string& outError)
{
    PixelsConfig loaded;
    if (!ConfigManager::load(configPath, loaded, outError))
    {
        return false;
    }

    config_ = loaded;
    return true;
}

bool PixelsRuntimeService::start()
{
    if (config_.pixelIds.empty())
    {
        return false;
    }

    if (manager_)
    {
        manager_->stop();
        manager_.reset();
    }

    manager_ = std::make_unique<DiceManager>(config_.pixelIds, logger_, stateObserver_, rollObserver_);
    manager_->start();
    return true;
}

void PixelsRuntimeService::stop()
{
    if (!manager_)
    {
        return;
    }

    manager_->stop();
    manager_.reset();
}

void PixelsRuntimeService::runUntilEnterPressed()
{
    if (config_.pixelIds.empty())
    {
        return;
    }

    if (manager_)
    {
        manager_->stop();
        manager_.reset();
    }

    manager_ = std::make_unique<DiceManager>(config_.pixelIds, logger_, stateObserver_, rollObserver_);
    manager_->runUntilEnterPressed();
    manager_.reset();
}

std::vector<DieStatusSnapshot> PixelsRuntimeService::snapshotDice() const
{
    if (!manager_)
    {
        return {};
    }

    return manager_->snapshotDice();
}

bool PixelsRuntimeService::isRunning() const
{
    return manager_ && manager_->isRunning();
}

bool PixelsRuntimeService::isScanning() const
{
    return manager_ && manager_->isScanning();
}

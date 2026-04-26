#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "App/ConfigManager.h"
#include "App/Runtime/RuntimeModels.h"

class DiceManager;

class PixelsRuntimeService
{
public:
    using Logger = std::function<void(const std::string&)>;
    using StateObserver = std::function<void()>;
    using RollObserver = std::function<void(const std::string& label, int face)>;

    explicit PixelsRuntimeService(Logger logger = nullptr, StateObserver stateObserver = nullptr,
                                  RollObserver rollObserver = nullptr);
    ~PixelsRuntimeService();

    PixelsRuntimeService(const PixelsRuntimeService&) = delete;
    PixelsRuntimeService& operator=(const PixelsRuntimeService&) = delete;

    bool loadConfig(const std::string& configPath, std::string& outError);
    bool start();
    void stop();

    void runUntilEnterPressed();

    std::vector<DieStatusSnapshot> snapshotDice() const;
    bool isRunning() const;
    bool isScanning() const;
    void suspendReconnects(std::chrono::seconds duration);

private:
    Logger logger_;
    StateObserver stateObserver_;
    RollObserver rollObserver_;

    PixelsConfig config_;
    std::unique_ptr<DiceManager> manager_;
};

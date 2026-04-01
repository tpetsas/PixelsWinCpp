#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "App/Runtime/RuntimeModels.h"
#include "Systemic/Pixels/Pixel.h"
#include "Systemic/Pixels/ScannedPixel.h"

class DieConnection
{
public:
    using Logger = std::function<void(const std::string&)>;
    using StateObserver = std::function<void()>;

    DieConnection(uint32_t targetPixelId, std::string label, Logger logger, StateObserver stateObserver = nullptr);
    ~DieConnection();

    DieConnection(const DieConnection&) = delete;
    DieConnection& operator=(const DieConnection&) = delete;

    bool trySelectScannedPixel(const std::shared_ptr<const Systemic::Pixels::ScannedPixel>& scannedPixel);

    void tickWatchdog();
    void tickPoll();

    void shutdown();

    bool isSelected() const;
    uint32_t targetPixelId() const;
    const std::string& label() const;
    DieStatusSnapshot snapshot() const;

private:
    class Delegate;

    std::future<Systemic::Pixels::Pixel::ConnectResult> connectAndInitialize(const std::shared_ptr<Systemic::Pixels::Pixel>& pixel);
    bool reconnectPixel();
    void startConnectThread();
    void markAnyMessage();
    void markRollEvent();
    void markRollResult(int face);
    void notifyStateChanged() const;

    std::string prefix() const;
    void log(const std::string& message) const;

    mutable std::mutex mutex_;

    const uint32_t targetPixelId_;
    const std::string label_;
    const Logger logger_;
    const StateObserver stateObserver_;

    std::shared_ptr<Systemic::Pixels::Pixel> pixel_;
    std::shared_ptr<Delegate> delegate_;

    bool shuttingDown_ = false;
    bool connecting_ = false;
    bool connectedOnce_ = false;
    bool selected_ = false;
    std::chrono::steady_clock::time_point lastConnectAttempt_ = std::chrono::steady_clock::now() - std::chrono::seconds(30);

    std::chrono::steady_clock::time_point lastRollEvent_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnyMessage_ = std::chrono::steady_clock::now();
    bool hasLastRoll_ = false;
    int lastRollFace_ = 0;
    std::chrono::system_clock::time_point lastRollAt_{};

    std::thread connectThread_;
};

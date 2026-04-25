#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <cmath>

#include "App/Runtime/RuntimeModels.h"

class RollServer
{
public:
    using Logger = std::function<void(const std::string&)>;
    using DiceSnapshotProvider = std::function<std::vector<DieStatusSnapshot>()>;
    // Called when a multi-die request starts and any die is not Ready.
    // Suspends watchdog reconnect attempts so the BLE scanner has a clear
    // channel to receive advertisements from the disconnected die.
    using DiceReconnectSuspender = std::function<void(std::chrono::seconds)>;

    explicit RollServer(Logger logger = nullptr);
    ~RollServer();

    RollServer(const RollServer&) = delete;
    RollServer& operator=(const RollServer&) = delete;

    void start(DiceSnapshotProvider snapshotProvider,
               DiceReconnectSuspender reconnectSuspender = nullptr);
    void stop();

    // Called by DieConnection::markRollResult via the RollObserver chain
    void onRoll(const std::string& label, int face);

    static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";

private:
    void serverThread();
    void handleClient(HANDLE pipe);
    std::string processRequest(const std::string& json);
    std::string waitForRolls(const std::string& mode, uint32_t generation);
    void showRollRegisteredPopup();
    void handleReadyCommand();

    Logger logger_;
    DiceSnapshotProvider snapshotProvider_;
    DiceReconnectSuspender reconnectSuspender_;

    std::thread serverThread_;
    std::atomic<bool> running_ = false;
    HANDLE stopEvent_ = nullptr;  // Signaled to wake up the pipe wait

    // Pending roll request state
    struct PendingRequest
    {
        bool active = false;
        std::string mode;
        uint32_t generation = 0;
        int rollsNeeded = 0;
        std::vector<int> collectedRolls;
        std::vector<std::string> collectedLabels;  // which die reported each roll
        std::chrono::system_clock::time_point startedAt{};
    };

    std::mutex rollMutex_;
    std::condition_variable rollCv_;
    PendingRequest pendingRequest_;

    void log(const std::string& message) const;
};

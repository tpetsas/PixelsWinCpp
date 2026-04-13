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
#include <vector>

#include "App/Runtime/RuntimeModels.h"
#include "Systemic/Pixels/Pixel.h"
#include "Systemic/Pixels/ScannedPixel.h"

// Explicit app-level connection state (distinct from library PixelStatus)
enum class ConnectionState
{
    Unselected,     // Die not yet selected from scan
    Selected,       // Die selected, waiting for first connection
    Connecting,     // Connection attempt in progress
    Ready,          // Connected and operational
    Stale,          // Connection appears dead (no messages)
    Reconnecting,   // Attempting to reconnect after failure
    Shutdown        // Shutting down, no more operations
};

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
    bool needsRecoveryScan() const;
    bool needsFullBleReset() const;
    uint32_t targetPixelId() const;
    const std::string& label() const;
    ConnectionState connectionState() const;
    int consecutiveFailures() const;
    std::chrono::steady_clock::time_point lastSuccessfulConnect() const;
    DieStatusSnapshot snapshot() const;

    // Adapter contention management
    void forceReleaseConnection();
    void requestPriorityReconnect();
    void suspendReconnectUntil(std::chrono::steady_clock::time_point until);

private:
    class Delegate;

    std::future<Systemic::Pixels::Pixel::ConnectResult> connectAndInitialize(const std::shared_ptr<Systemic::Pixels::Pixel>& pixel);
    void onPixelDisconnected();
    void checkMissedRoll(const std::shared_ptr<Systemic::Pixels::Pixel>& pixel);
    bool reconnectPixel();
    bool reconstructiveReconnect();
    void startConnectThread();
    void markAnyMessage();
    void markRollEvent();
    void markRollResult(int face);
    void notifyStateChanged() const;
    void setConnectionState(ConnectionState newState);

    std::string prefix() const;
    void log(const std::string& message) const;

    static const char* connectionStateToString(ConnectionState state);

    mutable std::mutex mutex_;
    std::mutex bleOpMutex_;  // Serializes BLE operations (connect/disconnect/reconnect)

    const uint32_t targetPixelId_;
    const std::string label_;
    const Logger logger_;
    const StateObserver stateObserver_;

    std::shared_ptr<Systemic::Pixels::Pixel> pixel_;
    std::shared_ptr<Delegate> delegate_;

    // Store ScannedPixel data for reconstructive reconnect
    std::shared_ptr<const Systemic::Pixels::ScannedPixel> lastScannedPixel_;

    // Explicit app-level state
    ConnectionState connectionState_ = ConnectionState::Unselected;

    bool shuttingDown_ = false;
    std::chrono::steady_clock::time_point lastConnectAttempt_ = std::chrono::steady_clock::now() - std::chrono::seconds(30);

    // Exponential backoff for reconnection
    int consecutiveFailures_ = 0;
    static constexpr int kMaxBackoffSeconds = 15;
    static constexpr int kBaseBackoffSeconds = 2;
    static constexpr int kRecoveryScanThreshold = 4;  // Failures before requesting recovery scan
    static constexpr int kFullBleResetThreshold = 5;   // Failures before full BLE reset (disconnect all dice)
    std::atomic<bool> immediateReconnectRequested_{false};
    std::chrono::steady_clock::time_point reconnectSuspendedUntil_{};  // DiceManager sets this during BLE reset

    // Stale detection parameters
    static constexpr int kStaleTimeoutSeconds = 20;      // Time without messages before considered stale
    static constexpr int kStaleGracePeriodSeconds = 10;  // Grace period after reconnect before checking stale

    // Message timing for stale detection
    std::chrono::steady_clock::time_point lastRollEvent_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnyMessage_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastSuccessfulConnect_ = std::chrono::steady_clock::now();

    // Roll tracking
    int faceBeforeDisconnect_ = 0;  // Saved on disconnect, used to detect missed rolls after reconnect
    Systemic::Pixels::PixelRollState rollStateBeforeDisconnect_ = Systemic::Pixels::PixelRollState::Unknown;  // Saved on disconnect
    bool hasLastRoll_ = false;
    int lastRollFace_ = 0;
    std::chrono::system_clock::time_point lastRollAt_{};
    std::vector<int> recentRollFaces_;

    std::thread connectThread_;
};

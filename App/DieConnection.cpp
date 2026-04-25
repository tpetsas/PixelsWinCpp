#include "pch.h"
#include "App/DieConnection.h"

#include <exception>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace Systemic::Pixels;
using namespace Systemic::Pixels::Messages;

namespace
{
    std::string serializeTimePoint(const std::chrono::system_clock::time_point& time, const std::string& format)
    {
        std::time_t tt = std::chrono::system_clock::to_time_t(time);
        std::tm tm{};
        localtime_s(&tm, &tt);

        std::stringstream ss;
        ss << std::put_time(&tm, format.c_str());
        return ss.str();
    }

    std::string narrow(const std::wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (const wchar_t ch : ws)
        {
            out.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        return out;
    }

    std::string toStatusString(PixelStatus status)
    {
        switch (status)
        {
        case PixelStatus::Disconnected:  return "disconnected";
        case PixelStatus::Connecting:    return "connecting";
        case PixelStatus::Identifying:   return "identifying";
        case PixelStatus::Ready:         return "ready";
        case PixelStatus::Disconnecting: return "disconnecting";
        default:                         return "unknown";
        }
    }
}

const char* DieConnection::connectionStateToString(ConnectionState state)
{
    switch (state)
    {
    case ConnectionState::Unselected:   return "Unselected";
    case ConnectionState::Selected:     return "Selected";
    case ConnectionState::Connecting:   return "Connecting";
    case ConnectionState::Ready:        return "Ready";
    case ConnectionState::Stale:        return "Stale";
    case ConnectionState::Reconnecting: return "Reconnecting";
    case ConnectionState::Shutdown:     return "Shutdown";
    default:                            return "Unknown";
    }
}

class DieConnection::Delegate : public PixelDelegate
{
public:
    explicit Delegate(DieConnection* owner) : owner_(owner)
    {
    }

    void onStatusChanged(std::shared_ptr<Pixel> /*pixel*/, PixelStatus status) override
    {
        owner_->log("Status changed to " + toStatusString(status));
        owner_->markAnyMessage();

        // Event-driven reconnect: immediately flag when the die disconnects
        // so the watchdog can act on the next tick without waiting for backoff
        if (status == PixelStatus::Disconnected)
        {
            owner_->onPixelDisconnected();
        }
    }

    void onFirmwareDateChanged(std::shared_ptr<Pixel> /*pixel*/, std::chrono::system_clock::time_point firmwareDate) override
    {
        owner_->log("Firmware date changed to " + serializeTimePoint(firmwareDate, "%Y-%m-%d %H:%M:%S"));
        owner_->markAnyMessage();
    }

    void onRssiChanged(std::shared_ptr<Pixel> /*pixel*/, int rssi) override
    {
        owner_->log("RSSI changed to " + std::to_string(rssi) + "dBm");
        owner_->markAnyMessage();
    }

    void onBatteryLevelChanged(std::shared_ptr<Pixel> /*pixel*/, int batteryLevel) override
    {
        owner_->log("Battery level changed to " + std::to_string(batteryLevel) + "%");
        owner_->markAnyMessage();
    }

    void onChargingStateChanged(std::shared_ptr<Pixel> /*pixel*/, bool isCharging) override
    {
        owner_->log(std::string("Battery is") + (isCharging ? "" : " not") + " charging");
        owner_->markAnyMessage();
    }

    void onRollStateChanged(std::shared_ptr<Pixel> /*pixel*/, PixelRollState state, int face) override
    {
        owner_->log("Roll state changed to " + std::to_string(static_cast<int>(state)) + " with face " + std::to_string(face) + " up");
        owner_->markRollEvent();
    }

    void onRolled(std::shared_ptr<Pixel> /*pixel*/, int face) override
    {
        owner_->markRollResult(face);
        owner_->log("Rolled on face " + std::to_string(face));
        owner_->markRollEvent();
    }

    void onMessageReceived(std::shared_ptr<Pixel> /*pixel*/, std::shared_ptr<const PixelMessage> message) override
    {
        if (message)
        {
            owner_->log("[raw] message type = " + std::string(getMessageName(message->type)));
        }
        owner_->markAnyMessage();
    }

private:
    DieConnection* owner_;
};

DieConnection::DieConnection(uint32_t targetPixelId, std::string label, Logger logger,
                             StateObserver stateObserver, RollObserver rollObserver)
    : targetPixelId_(targetPixelId),
      label_(std::move(label)),
      logger_(std::move(logger)),
      stateObserver_(std::move(stateObserver)),
      rollObserver_(std::move(rollObserver)),
      delegate_(std::make_shared<Delegate>(this))
{
}

DieConnection::~DieConnection()
{
    shutdown();
}

bool DieConnection::trySelectScannedPixel(const std::shared_ptr<const ScannedPixel>& scannedPixel)
{
    if (!scannedPixel)
    {
        return false;
    }

    const uint32_t id = static_cast<uint32_t>(scannedPixel->pixelId());
    if (id != targetPixelId_)
    {
        return false;
    }

    bool shouldConnect = false;
    bool firstSelection = false;
    bool recoveryRediscovery = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shuttingDown_)
        {
            return false;
        }

        // Always update ScannedPixel data when ID matches,
        // even during Reconnecting — the next reconnect attempt will use fresh data
        lastScannedPixel_ = scannedPixel;

        // Seed missed-roll state from advert if we've never connected via GATT.
        // Without this, processAdvertisement() ignores all adverts because
        // faceBeforeDisconnect_ stays 0 until the first GATT disconnect event.
        if (faceBeforeDisconnect_ == 0)
        {
            faceBeforeDisconnect_ = static_cast<int>(scannedPixel->currentFace());
            rollStateBeforeDisconnect_ = scannedPixel->rollState();
        }

        if (connectionState_ == ConnectionState::Connecting ||
            connectionState_ == ConnectionState::Reconnecting)
        {
            return false;
        }

        if (connectionState_ == ConnectionState::Unselected)
        {
            setConnectionState(ConnectionState::Selected);
            firstSelection = true;
        }

        // Recovery re-discovery: die was already selected but disconnected with many failures.
        // Request immediate reconnect with the fresh ScannedPixel data.
        if (connectionState_ == ConnectionState::Selected && pixel_ && consecutiveFailures_ >= 2)
        {
            recoveryRediscovery = true;
            immediateReconnectRequested_.store(true);
        }

        if (!pixel_)
        {
            setConnectionState(ConnectionState::Connecting);
            lastConnectAttempt_ = std::chrono::steady_clock::now();
            pixel_ = Pixel::create(*scannedPixel, delegate_);
            lastAnyMessage_ = std::chrono::steady_clock::now();
            lastRollEvent_ = lastAnyMessage_;
            shouldConnect = true;
        }
    }

    if (recoveryRediscovery)
    {
        log("[recovery] Re-discovered via scan (RSSI: " + std::to_string(scannedPixel->rssi()) +
            "), requesting immediate reconnect with fresh data");
    }

    if (firstSelection)
    {
        log("Chosen Pixel: " + narrow(scannedPixel->name()));

        std::stringstream ss;
        ss << "Id: 0x" << std::uppercase << std::hex << id << std::dec;
        ss << ", RSSI: " << scannedPixel->rssi();
        ss << ", Battery: " << static_cast<int>(scannedPixel->batteryLevel()) << "%";
        ss << ", Charging: " << (scannedPixel->isCharging() ? "yes" : "no");
        ss << ", Face: " << static_cast<int>(scannedPixel->currentFace());
        ss << ", Firmware: " << serializeTimePoint(scannedPixel->firmwareDate(), "%Y-%m-%d %H:%M:%S");
        log(ss.str());
    }

    // Connection runs on a dedicated thread so scanner callbacks stay fast.
    if (shouldConnect)
    {
        startConnectThread();
    }

    notifyStateChanged();

    return shouldConnect;
}

bool DieConnection::needsRecoveryScan() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_ == ConnectionState::Selected &&
           consecutiveFailures_ >= kRecoveryScanThreshold;
}

bool DieConnection::needsFullBleReset() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_ == ConnectionState::Selected &&
           consecutiveFailures_ >= kFullBleResetThreshold;
}

void DieConnection::tickWatchdog()
{
    std::shared_ptr<Pixel> localPixel;
    ConnectionState currentConnState = ConnectionState::Unselected;
    PixelStatus currentStatus = PixelStatus::Disconnected;
    bool shouldTryReconnect = false;
    int backoffSeconds = kBaseBackoffSeconds;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentConnState = connectionState_;
        localPixel = pixel_;
        if (localPixel)
        {
            currentStatus = localPixel->status();
        }

        // Don't do anything if we're already connecting/reconnecting or shutdown
        if (currentConnState == ConnectionState::Connecting ||
            currentConnState == ConnectionState::Reconnecting ||
            currentConnState == ConnectionState::Shutdown ||
            currentConnState == ConnectionState::Unselected)
        {
            return;
        }

        // Reconnection suspended by DiceManager (e.g., during full BLE reset)
        if (std::chrono::steady_clock::now() < reconnectSuspendedUntil_)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto connectAge = std::chrono::duration_cast<std::chrono::seconds>(now - lastConnectAttempt_).count();

        // Calculate exponential backoff: 2s, 4s, 8s, 15s (max)
        backoffSeconds = std::min(kMaxBackoffSeconds, kBaseBackoffSeconds * (1 << consecutiveFailures_));

        // Handle explicit disconnection
        if (localPixel && currentStatus == PixelStatus::Disconnected)
        {
            const bool immediate = immediateReconnectRequested_.exchange(false);

            log("[watchdog] Disconnected, elapsed=" + std::to_string(connectAge) +
                "s since last attempt (backoff=" + std::to_string(backoffSeconds) +
                "s, failures=" + std::to_string(consecutiveFailures_) +
                (immediate ? ", immediate" : "") + ")");

            if (immediate || connectAge >= backoffSeconds)
            {
                shouldTryReconnect = true;
            }
        }
    }

    if (!localPixel || !shouldTryReconnect)
    {
        return;
    }

    // After the first failed reconnectPixel (which already tried 3× internally),
    // switch to reconstructive reconnect — destroy and recreate Pixel to clear
    // BLE stack cached state. This saves ~30s of futile regular retries.
    if (consecutiveFailures_ >= 1)
    {
        log("[watchdog] Prior failure, trying reconstructive reconnect...");
        reconstructiveReconnect();
    }
    else
    {
        log("[watchdog] Trying to reconnect...");
        reconnectPixel();
    }
}

void DieConnection::tickPoll()
{
    std::shared_ptr<Pixel> localPixel;
    ConnectionState currentConnState = ConnectionState::Unselected;
    bool shouldReconnect = false;
    long long secondsSinceLastMessage = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        localPixel = pixel_;
        currentConnState = connectionState_;
    }

    if (!localPixel)
    {
        return;
    }

    const int currentRssi = localPixel->rssi();
    const int currentFace = localPixel->currentFace();
    const int currentBattery = localPixel->batteryLevel();
    const auto currentStatus = localPixel->status();

    // Stale connection detection based on message timing
    // This is more reliable than comparing poll values
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentConnState == ConnectionState::Ready && currentStatus == PixelStatus::Ready)
        {
            const auto now = std::chrono::steady_clock::now();
            secondsSinceLastMessage = std::chrono::duration_cast<std::chrono::seconds>(now - lastAnyMessage_).count();
            const auto secondsSinceConnect = std::chrono::duration_cast<std::chrono::seconds>(now - lastSuccessfulConnect_).count();

            // Don't check for stale during grace period after reconnect
            // This prevents false positives when BLE stack is still stabilizing
            if (secondsSinceConnect < kStaleGracePeriodSeconds)
            {
                // Still in grace period, don't check stale
            }
            else if (secondsSinceLastMessage >= kStaleTimeoutSeconds)
            {
                // If no messages for 20+ seconds, connection is likely stale
                // (RSSI updates should come every ~5 seconds when connection is healthy)
                log("[poll] Stale connection detected (no messages for " + std::to_string(secondsSinceLastMessage) + "s)");
                setConnectionState(ConnectionState::Stale);
                shouldReconnect = true;
            }
        }
    }

    log("[poll] state=" + std::string(connectionStateToString(currentConnState)) +
        ", status=" + toStatusString(currentStatus) +
        ", face=" + std::to_string(currentFace) +
        ", rollState=" + std::to_string(static_cast<int>(localPixel->rollState())) +
        ", battery=" + std::to_string(currentBattery) +
        ", rssi=" + std::to_string(currentRssi) +
        ", lastMsg=" + std::to_string(secondsSinceLastMessage) + "s ago");

    if (shouldReconnect)
    {
        log("[poll] Forcing reconnection due to stale connection...");
        // Save face and roll state before stale reconnect for missed-roll detection
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (localPixel)
            {
                faceBeforeDisconnect_ = localPixel->currentFace();
                rollStateBeforeDisconnect_ = localPixel->rollState();
            }
        }
        // Use reconstructive reconnect for stale connections
        reconstructiveReconnect();
    }
}

void DieConnection::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        setConnectionState(ConnectionState::Shutdown);
    }

    // Try to acquire the BLE op lock with a timeout. A WinRT connect/identify
    // call can block for many seconds; we don't want shutdown to hang the UI.
    bool gotLock = bleOpMutex_.try_lock_for(std::chrono::seconds(3));

    if (gotLock)
    {
        std::shared_ptr<Pixel> localPixel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localPixel = pixel_;
        }
        if (localPixel)
        {
            try { localPixel->disconnect(); } catch (...) {}
        }
        bleOpMutex_.unlock();
    }

    if (connectThread_.joinable())
    {
        if (gotLock)
            connectThread_.join();
        else
            connectThread_.detach();  // BLE op still in flight; abandon it
    }
}

bool DieConnection::isSelected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_ != ConnectionState::Unselected;
}

ConnectionState DieConnection::connectionState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_;
}

int DieConnection::consecutiveFailures() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return consecutiveFailures_;
}

std::chrono::steady_clock::time_point DieConnection::lastSuccessfulConnect() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSuccessfulConnect_;
}

void DieConnection::forceReleaseConnection()
{
    std::unique_lock<std::timed_mutex> bleLock(bleOpMutex_, std::try_to_lock);
    if (!bleLock.owns_lock())
    {
        log("[adapter] BLE operation in progress, cannot release");
        return;
    }

    std::shared_ptr<Pixel> localPixel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connectionState_ == ConnectionState::Ready ||
            connectionState_ == ConnectionState::Stale)
        {
            localPixel = pixel_;
            // Save state for missed-roll detection
            if (localPixel)
            {
                faceBeforeDisconnect_ = localPixel->currentFace();
                rollStateBeforeDisconnect_ = localPixel->rollState();
            }
            setConnectionState(ConnectionState::Selected);
            // Give this die a small backoff so the failing die gets priority
            lastConnectAttempt_ = std::chrono::steady_clock::now();
            consecutiveFailures_ = 1;
        }
        else if (connectionState_ == ConnectionState::Selected)
        {
            // Already disconnected but stuck — reset failures for fresh start
            // Keep pixel_ alive so the watchdog can still attempt reconnection
            consecutiveFailures_ = 0;
            lastConnectAttempt_ = std::chrono::steady_clock::now();
            immediateReconnectRequested_.store(true);
        }
        else
        {
            return;
        }
    }

    log("[adapter] Releasing connection to free BLE adapter (face=" +
        std::to_string(faceBeforeDisconnect_) + ")");

    if (localPixel)
    {
        try
        {
            localPixel->disconnect();
        }
        catch (...)
        {
        }
    }

    notifyStateChanged();
}

void DieConnection::requestPriorityReconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    immediateReconnectRequested_.store(true);
    log("[adapter] Priority reconnect requested (failures=" +
        std::to_string(consecutiveFailures_) + ")");
}

void DieConnection::suspendReconnectUntil(std::chrono::steady_clock::time_point until)
{
    std::lock_guard<std::mutex> lock(mutex_);
    reconnectSuspendedUntil_ = until;
}

DieStatusSnapshot DieConnection::snapshot() const
{
    DieStatusSnapshot snapshot;
    snapshot.targetPixelId = targetPixelId_;
    snapshot.label = label_;

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.selected = (connectionState_ != ConnectionState::Unselected);
    snapshot.connecting = (connectionState_ == ConnectionState::Connecting ||
                           connectionState_ == ConnectionState::Reconnecting);
    snapshot.connectedOnce = (connectionState_ == ConnectionState::Ready ||
                              connectionState_ == ConnectionState::Stale ||
                              connectionState_ == ConnectionState::Reconnecting);
    snapshot.hasPixel = (pixel_ != nullptr);
    snapshot.hasLastRoll = hasLastRoll_;
    snapshot.lastRollFace = lastRollFace_;
    snapshot.lastRollAt = lastRollAt_;
    snapshot.recentRollFaces = recentRollFaces_;
    snapshot.advertSettledFace = advertSettledFace_;
    snapshot.advertSettledCount = advertSettledCount_;
    snapshot.wasRollingAtDisconnect = (rollStateBeforeDisconnect_ == PixelRollState::Rolling ||
                                       rollStateBeforeDisconnect_ == PixelRollState::Handling);

    if (pixel_)
    {
        snapshot.status = pixel_->status();
        snapshot.batteryLevel = pixel_->batteryLevel();
        snapshot.currentFace = pixel_->currentFace();
        snapshot.isCharging = pixel_->isCharging();
    }

    return snapshot;
}

uint32_t DieConnection::targetPixelId() const
{
    return targetPixelId_;
}

const std::string& DieConnection::label() const
{
    return label_;
}

std::future<Pixel::ConnectResult> DieConnection::connectAndInitialize(const std::shared_ptr<Pixel>& pixel)
{
    log("Connecting...");

    // Use maintainConnection=false so link-loss fires a proper Disconnected event
    // instead of silently losing GATT subscriptions during WinRT auto-reconnect.
    // Our own watchdog / event-driven reconnect handles recovery explicitly.
    auto result = co_await pixel->connectAsync(false);
    if (result != Pixel::ConnectResult::Success)
    {
        log("First connection attempt failed, retrying...");
        result = co_await pixel->connectAsync(false);
    }

    if (result == Pixel::ConnectResult::Success)
    {
        log("Connected and ready to use!");

        // Enable RSSI reporting so we get live RSSI updates for stale connection detection
        try
        {
            co_await pixel->reportRssiAsync(true);
            log("RSSI reporting enabled");
        }
        catch (...)
        {
            log("Failed to enable RSSI reporting");
        }

        log("Roll die to see results...");
    }
    else
    {
        log("Connection error: " + std::to_string(static_cast<int>(result)));
    }

    co_return result;
}

void DieConnection::onPixelDisconnected()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Only act if we were in a connected state
    if (connectionState_ != ConnectionState::Ready &&
        connectionState_ != ConnectionState::Stale)
    {
        return;
    }

    // Save current face and roll state so we can detect missed rolls after reconnect
    if (pixel_)
    {
        faceBeforeDisconnect_ = pixel_->currentFace();
        rollStateBeforeDisconnect_ = pixel_->rollState();
    }

    // Reset advert recovery debounce
    advertSettledFace_ = 0;
    advertSettledCount_ = 0;
    advertSawRolling_ = false;

    log("[event] Disconnect detected — requesting immediate reconnect (face=" +
        std::to_string(faceBeforeDisconnect_) + ", rollState=" +
        std::to_string(static_cast<int>(rollStateBeforeDisconnect_)) + ")");
    setConnectionState(ConnectionState::Selected);
    immediateReconnectRequested_.store(true);
}

void DieConnection::checkMissedRoll(const std::shared_ptr<Pixel>& pixel)
{
    int savedFace;
    PixelRollState savedRollState;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        savedFace = faceBeforeDisconnect_;
        savedRollState = rollStateBeforeDisconnect_;
        // Do NOT clear here — keep rollStateBeforeDisconnect_ set until after
        // markRollResult, so that snapshot() correctly reports wasRollingAtDisconnect
        // during the RollServer fallback window. Cleared at the end of this function.
    }

    if (!pixel || savedFace == 0)
    {
        return;
    }

    const int newFace = pixel->currentFace();
    const auto currentRollState = pixel->rollState();

    const bool wasRolling = (savedRollState == PixelRollState::Rolling ||
                             savedRollState == PixelRollState::Handling);
    const bool isAtRest = (currentRollState != PixelRollState::Rolling);

    // Case 1: Die was mid-roll at disconnect time. Any settled face after
    // reconnect is the result of that roll, even if it coincidentally matches
    // the transient face we last saw while tumbling.
    if (wasRolling && isAtRest)
    {
        log("[missed-roll] Die was rolling at disconnect (face=" + std::to_string(savedFace) +
            ", rollState=" + std::to_string(static_cast<int>(savedRollState)) +
            ") -> landed on " + std::to_string(newFace) +
            " (rollState=" + std::to_string(static_cast<int>(currentRollState)) + ")");
        markRollResult(newFace, /*isMissedRoll=*/false);
        // Clear after markRollResult so wasRollingAtDisconnect stays accurate in snapshot
        {
            std::lock_guard<std::mutex> lock(mutex_);
            faceBeforeDisconnect_ = 0;
            rollStateBeforeDisconnect_ = PixelRollState::Unknown;
        }
        return;
    }

    // Case 2: Die was at rest at disconnect time but face changed —
    // it was rolled and settled during the disconnect window.
    if (!wasRolling && newFace != savedFace && isAtRest)
    {
        log("[missed-roll] Face changed from " + std::to_string(savedFace) +
            " to " + std::to_string(newFace) + " during disconnect (rollState=" +
            std::to_string(static_cast<int>(currentRollState)) + ")");
        markRollResult(newFace, /*isMissedRoll=*/false);
    }

    // Clear after markRollResult to prevent double-counting on next reconnect
    {
        std::lock_guard<std::mutex> lock(mutex_);
        faceBeforeDisconnect_ = 0;
        rollStateBeforeDisconnect_ = PixelRollState::Unknown;
    }
}

void DieConnection::processAdvertisement(const std::shared_ptr<const ScannedPixel>& scannedPixel)
{
    if (!scannedPixel)
    {
        return;
    }

    const uint32_t id = static_cast<uint32_t>(scannedPixel->pixelId());
    if (id != targetPixelId_)
    {
        return;
    }

    int reportFace = 0;
    bool reportWasRolling = false;  // Track if roll came from a genuinely rolling die

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Only process while disconnected and waiting for reconnect
        if (connectionState_ != ConnectionState::Selected &&
            connectionState_ != ConnectionState::Reconnecting)
        {
            return;
        }

        // Already recovered the roll via advert (or no disconnect state saved)
        if (faceBeforeDisconnect_ == 0)
        {
            return;
        }

        const int advFace = scannedPixel->currentFace();
        const auto advRollState = scannedPixel->rollState();

        // Debug: log every advertisement received while waiting for recovery
        log("[advert-debug] face=" + std::to_string(advFace) +
            ", rollState=" + std::to_string(static_cast<int>(advRollState)) +
            ", settledFace=" + std::to_string(advertSettledFace_) +
            ", settledCount=" + std::to_string(advertSettledCount_));

        // Die still rolling — not settled yet, reset debounce and keep listening
        if (advRollState == PixelRollState::Rolling ||
            advRollState == PixelRollState::Handling)
        {
            advertSettledFace_ = 0;
            advertSettledCount_ = 0;
            advertSawRolling_ = true;
            log("[advert-debug] Still rolling, reset debounce");
            return;
        }

        // Die is settled (OnFace or Crooked). Check if this is a missed roll.
        const bool wasRolling = advertSawRolling_ ||
                                (rollStateBeforeDisconnect_ == PixelRollState::Rolling ||
                                 rollStateBeforeDisconnect_ == PixelRollState::Handling);

        if (wasRolling)
        {
            // Debounce: require consecutive settled adverts on the same face
            // to avoid false positives from transient settle readings mid-tumble
            if (advFace == advertSettledFace_)
            {
                advertSettledCount_++;
                log("[advert-debug] Same face, count now " + std::to_string(advertSettledCount_));
            }
            else
            {
                advertSettledFace_ = advFace;
                advertSettledCount_ = 1;
                log("[advert-debug] Face changed to " + std::to_string(advFace) + ", reset count to 1");
            }

            if (advertSettledCount_ < kAdvertSettledThreshold)
            {
                log("[advert-debug] Waiting for " + std::to_string(kAdvertSettledThreshold) + " consecutive, have " + std::to_string(advertSettledCount_));
                return;
            }

            log("[advert-recovery] Roll detected via advertisement (debounced): face=" +
                std::to_string(advFace) + " (was face=" +
                std::to_string(faceBeforeDisconnect_) + ", rollState=" +
                std::to_string(static_cast<int>(rollStateBeforeDisconnect_)) +
                ", settled count=" + std::to_string(advertSettledCount_) + ")");

            // Keep monitoring: set to recovered face so further changes are detected
            faceBeforeDisconnect_ = advFace;
            rollStateBeforeDisconnect_ = advRollState;
            advertSettledFace_ = 0;
            advertSettledCount_ = 0;
            advertSawRolling_ = false;
            reportFace = advFace;
            reportWasRolling = true;
        }
        else if (advFace != faceBeforeDisconnect_)
        {
            // Face changed while die was at rest. Only consider OnFace/Crooked as
            // potential rolls — rollState=None(5) means just moved/handled.
            // Apply debouncing here too: advertisement data can show stale rollState
            // values, so a single advert should never trigger a roll report.
            if (advRollState == PixelRollState::OnFace ||
                advRollState == PixelRollState::Crooked)
            {
                if (advFace == advertSettledFace_)
                {
                    advertSettledCount_++;
                    log("[advert-debug] Same face, count now " + std::to_string(advertSettledCount_));
                }
                else
                {
                    advertSettledFace_ = advFace;
                    advertSettledCount_ = 1;
                    log("[advert-debug] Face changed to " + std::to_string(advFace) + ", reset count to 1");
                }

                if (advertSettledCount_ < kAdvertSettledThreshold)
                {
                    log("[advert-debug] Waiting for " + std::to_string(kAdvertSettledThreshold) + " consecutive, have " + std::to_string(advertSettledCount_));
                    return;
                }

                log("[advert-recovery] Roll detected via advertisement (debounced): face=" +
                    std::to_string(advFace) + " (was face=" +
                    std::to_string(faceBeforeDisconnect_) + ", rollState=" +
                    std::to_string(static_cast<int>(rollStateBeforeDisconnect_)) +
                    ", settled count=" + std::to_string(advertSettledCount_) + ")");

                faceBeforeDisconnect_ = advFace;
                rollStateBeforeDisconnect_ = advRollState;
                advertSettledFace_ = 0;
                advertSettledCount_ = 0;
                reportFace = advFace;
                reportWasRolling = true;  // 3-consecutive settled adverts on a new face is a real roll
            }
            else
            {
                log("[advert-debug] Face changed to " + std::to_string(advFace) +
                    " (rollState=" + std::to_string(static_cast<int>(advRollState)) +
                    ") — not a roll, tracking only");
            }
        }
    }

    if (reportFace > 0)
    {
        markRollResult(reportFace, /*isMissedRoll=*/!reportWasRolling);
    }
}

bool DieConnection::reconnectPixel()
{
    std::unique_lock<std::timed_mutex> bleLock(bleOpMutex_, std::try_to_lock);
    if (!bleLock.owns_lock())
    {
        log("[reconnect] BLE operation already in progress, skipping");
        return false;
    }

    std::shared_ptr<Pixel> localPixel;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_ || !pixel_ ||
            connectionState_ == ConnectionState::Connecting ||
            connectionState_ == ConnectionState::Reconnecting)
        {
            return false;
        }
        setConnectionState(ConnectionState::Reconnecting);
        lastConnectAttempt_ = std::chrono::steady_clock::now();
        localPixel = pixel_;
    }

    // Retry loop: try up to 3 times with 2s delays between attempts.
    // This is much faster than going back to the watchdog (which adds 4-15s backoff each time).
    static constexpr int kReconnectRetries = 3;
    static constexpr int kRetryDelaySeconds = 2;

    bool ok = false;
    for (int attempt = 0; attempt < kReconnectRetries && !ok; ++attempt)
    {
        if (attempt == 0)
        {
            log("[reconnect] Disconnecting and reconnecting same Pixel object...");
        }
        else
        {
            log("[reconnect] Retry " + std::to_string(attempt) + "/" +
                std::to_string(kReconnectRetries - 1) + " after " +
                std::to_string(kRetryDelaySeconds) + "s delay...");
            std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySeconds));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return false;
        }

        try
        {
            localPixel->disconnect();
        }
        catch (...)
        {
        }

        try
        {
            const auto result = connectAndInitialize(localPixel).get();
            ok = (result == Pixel::ConnectResult::Success);
        }
        catch (const std::exception& ex)
        {
            log(std::string("[reconnect] Exception: ") + ex.what());
        }
        catch (...)
        {
            log("[reconnect] Unknown exception.");
        }
    }

    if (ok)
    {
        checkMissedRoll(localPixel);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok)
        {
            setConnectionState(ConnectionState::Ready);
            consecutiveFailures_ = 0;
            lastSuccessfulConnect_ = std::chrono::steady_clock::now();
        }
        else
        {
            setConnectionState(ConnectionState::Selected);
            consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 10);
        }
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

    notifyStateChanged();

    return ok;
}

bool DieConnection::reconstructiveReconnect()
{
    std::unique_lock<std::timed_mutex> bleLock(bleOpMutex_, std::try_to_lock);
    if (!bleLock.owns_lock())
    {
        log("[reconstructive] BLE operation already in progress, skipping");
        return false;
    }

    std::shared_ptr<Pixel> oldPixel;
    std::shared_ptr<const ScannedPixel> scannedPixel;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_ || !lastScannedPixel_ ||
            connectionState_ == ConnectionState::Connecting ||
            connectionState_ == ConnectionState::Reconnecting)
        {
            return false;
        }
        setConnectionState(ConnectionState::Reconnecting);
        lastConnectAttempt_ = std::chrono::steady_clock::now();
        oldPixel = pixel_;
        scannedPixel = lastScannedPixel_;
    }

    log("[reconstructive] Destroying old Pixel and creating new one from ScannedPixel...");

    // Disconnect and destroy old Pixel
    if (oldPixel)
    {
        try
        {
            oldPixel->disconnect();
        }
        catch (...)
        {
        }
        oldPixel.reset();
    }

    // Give BLE stack time to clean up cached state for this address.
    // Also allow the advertisement scanner to receive adverts from this die
    // for missed-roll recovery (the die can only advertise when no connection
    // attempt is active).
    log("[reconstructive] Waiting 3s for BLE cleanup + advertisement scan window...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) return false;
        // Re-read ScannedPixel — scanner may have received fresh data during the wait
        if (lastScannedPixel_)
        {
            scannedPixel = lastScannedPixel_;
        }
    }

    // Create new Pixel from stored ScannedPixel data
    auto newPixel = Pixel::create(*scannedPixel, delegate_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pixel_ = newPixel;
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

    // Retry loop: try up to 3 times with 2s delays, same as reconnectPixel.
    static constexpr int kReconstructiveRetries = 3;
    static constexpr int kReconstructiveRetryDelaySeconds = 2;

    bool ok = false;
    for (int attempt = 0; attempt < kReconstructiveRetries && !ok; ++attempt)
    {
        if (attempt > 0)
        {
            log("[reconstructive] Retry " + std::to_string(attempt) + "/" +
                std::to_string(kReconstructiveRetries - 1) + " after " +
                std::to_string(kReconstructiveRetryDelaySeconds) + "s delay...");
            std::this_thread::sleep_for(std::chrono::seconds(kReconstructiveRetryDelaySeconds));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return false;
        }

        try
        {
            if (attempt > 0)
            {
                newPixel->disconnect();
            }
        }
        catch (...)
        {
        }

        try
        {
            const auto result = connectAndInitialize(newPixel).get();
            ok = (result == Pixel::ConnectResult::Success);
        }
        catch (const std::exception& ex)
        {
            log(std::string("[reconstructive] Exception: ") + ex.what());
        }
        catch (...)
        {
            log("[reconstructive] Unknown exception.");
        }
    }

    if (ok)
    {
        checkMissedRoll(newPixel);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok)
        {
            setConnectionState(ConnectionState::Ready);
            consecutiveFailures_ = 0;
            lastSuccessfulConnect_ = std::chrono::steady_clock::now();
            log("[reconstructive] Successfully reconnected with new Pixel object!");
        }
        else
        {
            setConnectionState(ConnectionState::Selected);
            consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 10);
        }
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

    notifyStateChanged();

    return ok;
}

void DieConnection::startConnectThread()
{
    if (connectThread_.joinable())
    {
        connectThread_.join();
    }

    connectThread_ = std::thread([this]()
    {
        std::unique_lock<std::timed_mutex> bleLock(bleOpMutex_, std::try_to_lock);
        if (!bleLock.owns_lock())
        {
            log("[connect] BLE operation already in progress, skipping");
            return;
        }

        bool ok = false;

        try
        {
            std::shared_ptr<Pixel> localPixel;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                localPixel = pixel_;
            }

            if (localPixel)
            {
                const auto result = connectAndInitialize(localPixel).get();
                ok = (result == Pixel::ConnectResult::Success);

                if (ok)
                {
                    checkMissedRoll(localPixel);
                }
            }
        }
        catch (const std::exception& ex)
        {
            log(std::string("Connect thread exception: ") + ex.what());
        }
        catch (...)
        {
            log("Connect thread unknown exception.");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok)
            {
                setConnectionState(ConnectionState::Ready);
                consecutiveFailures_ = 0;
                lastSuccessfulConnect_ = std::chrono::steady_clock::now();
            }
            else
            {
                setConnectionState(ConnectionState::Selected);
                consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 4);
            }
            lastAnyMessage_ = std::chrono::steady_clock::now();
            lastRollEvent_ = lastAnyMessage_;
        }

        notifyStateChanged();
    });
}

void DieConnection::markAnyMessage()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastAnyMessage_ = std::chrono::steady_clock::now();
}

void DieConnection::markRollEvent()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastRollEvent_ = std::chrono::steady_clock::now();
    lastAnyMessage_ = lastRollEvent_;
}

void DieConnection::markRollResult(int face, bool isMissedRoll)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hasLastRoll_ = true;
        lastRollFace_ = face;
        lastRollAt_ = std::chrono::system_clock::now();
        recentRollFaces_.push_back(face);
        if (recentRollFaces_.size() > 8)
        {
            recentRollFaces_.erase(recentRollFaces_.begin());
        }
    }
    notifyStateChanged();
    // Only notify the roll server for real physical rolls, not missed-roll
    // recovery after reconnect (those are face-change artifacts, not real rolls)
    if (rollObserver_ && !isMissedRoll)
    {
        rollObserver_(label_, face);
    }
}

void DieConnection::notifyStateChanged() const
{
    if (stateObserver_)
    {
        stateObserver_();
    }
}

void DieConnection::setConnectionState(ConnectionState newState)
{
    // Note: caller must hold mutex_
    if (connectionState_ == ConnectionState::Shutdown && newState != ConnectionState::Shutdown)
    {
        return;  // Never leave Shutdown state
    }
    if (connectionState_ != newState)
    {
        log("[state] " + std::string(connectionStateToString(connectionState_)) +
            " -> " + std::string(connectionStateToString(newState)));
        connectionState_ = newState;
    }
}

std::string DieConnection::prefix() const
{
    return "[" + label_ + "] ";
}

void DieConnection::log(const std::string& message) const
{
    if (logger_)
    {
        logger_("\n" + prefix() + message);
    }

    if (message.find("Status changed to") != std::string::npos ||
        message.find("Battery level changed") != std::string::npos ||
        message.find("Connecting...") != std::string::npos ||
        message.find("Connected and ready") != std::string::npos ||
        message.find("Connection error") != std::string::npos)
    {
        notifyStateChanged();
    }
}

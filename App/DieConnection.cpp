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

DieConnection::DieConnection(uint32_t targetPixelId, std::string label, Logger logger, StateObserver stateObserver)
    : targetPixelId_(targetPixelId),
      label_(std::move(label)),
      logger_(std::move(logger)),
      stateObserver_(std::move(stateObserver)),
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

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shuttingDown_ || connectionState_ == ConnectionState::Connecting ||
            connectionState_ == ConnectionState::Reconnecting)
        {
            return false;
        }

        // Store ScannedPixel for potential reconstructive reconnect later
        lastScannedPixel_ = scannedPixel;

        if (connectionState_ == ConnectionState::Unselected)
        {
            setConnectionState(ConnectionState::Selected);
            firstSelection = true;
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

        const auto now = std::chrono::steady_clock::now();
        const auto connectAge = std::chrono::duration_cast<std::chrono::seconds>(now - lastConnectAttempt_).count();

        // Calculate exponential backoff: 2s, 4s, 8s, 16s, 32s, 60s (max)
        backoffSeconds = std::min(kMaxBackoffSeconds, kBaseBackoffSeconds * (1 << consecutiveFailures_));

        // Handle explicit disconnection
        if (localPixel && currentStatus == PixelStatus::Disconnected)
        {
            log("[watchdog] Disconnected, elapsed=" + std::to_string(connectAge) +
                "s since last attempt (backoff=" + std::to_string(backoffSeconds) +
                "s, failures=" + std::to_string(consecutiveFailures_) + ")");

            if (connectAge >= backoffSeconds)
            {
                shouldTryReconnect = true;
            }
        }
    }

    if (!localPixel || !shouldTryReconnect)
    {
        return;
    }

    // After multiple failures, try reconstructive reconnect (destroy and recreate Pixel)
    if (consecutiveFailures_ >= 2)
    {
        log("[watchdog] Multiple failures, trying reconstructive reconnect...");
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
        // Use reconstructive reconnect for stale connections
        reconstructiveReconnect();
    }
}

void DieConnection::shutdown()
{
    std::shared_ptr<Pixel> localPixel;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        setConnectionState(ConnectionState::Shutdown);
        localPixel = pixel_;
    }

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

    if (connectThread_.joinable())
    {
        connectThread_.join();
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

    // Use maintainConnection=true for automatic reconnection after unexpected disconnection
    auto result = co_await pixel->connectAsync(true);
    if (result != Pixel::ConnectResult::Success)
    {
        log("First connection attempt failed, retrying...");
        result = co_await pixel->connectAsync(true);
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

bool DieConnection::reconnectPixel()
{
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

    log("[reconnect] Disconnecting and reconnecting same Pixel object...");

    try
    {
        localPixel->disconnect();
    }
    catch (...)
    {
    }

    bool ok = false;
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
            consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 5); // Cap at 5 (64s max backoff)
        }
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

    notifyStateChanged();

    return ok;
}

bool DieConnection::reconstructiveReconnect()
{
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
    }

    // Create new Pixel from stored ScannedPixel data
    auto newPixel = Pixel::create(*scannedPixel, delegate_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pixel_ = newPixel;
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

    bool ok = false;
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
            consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 5);
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
                consecutiveFailures_ = std::min(consecutiveFailures_ + 1, 5);
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

void DieConnection::markRollResult(int face)
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

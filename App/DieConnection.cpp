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

DieConnection::DieConnection(uint32_t targetPixelId, std::string label, Logger logger)
    : targetPixelId_(targetPixelId),
      label_(std::move(label)),
      logger_(std::move(logger)),
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

        if (shuttingDown_ || connecting_)
        {
            return false;
        }

        if (!selected_)
        {
            selected_ = true;
            firstSelection = true;
        }

        if (!pixel_)
        {
            connecting_ = true;
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

    return shouldConnect;
}

void DieConnection::tickWatchdog()
{
    std::shared_ptr<Pixel> localPixel;
    bool connecting = false;
    PixelStatus currentStatus = PixelStatus::Disconnected;
    bool shouldTryReconnect = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connecting = connecting_;
        localPixel = pixel_;
        if (localPixel)
        {
            currentStatus = localPixel->status();
        }

        if (localPixel && !connecting_ && currentStatus == PixelStatus::Disconnected)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastConnectAttempt_ >= std::chrono::seconds(5))
            {
                shouldTryReconnect = true;
            }
        }
    }

    if (!localPixel || connecting || !shouldTryReconnect)
    {
        return;
    }

    log("[watchdog] Trying to connect/reconnect...");
    reconnectPixel();
}

void DieConnection::tickPoll()
{
    std::shared_ptr<Pixel> localPixel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        localPixel = pixel_;
    }

    if (!localPixel)
    {
        return;
    }

    log("[poll] status=" + toStatusString(localPixel->status()) +
        ", face=" + std::to_string(localPixel->currentFace()) +
        ", rollState=" + std::to_string(static_cast<int>(localPixel->rollState())) +
        ", battery=" + std::to_string(localPixel->batteryLevel()));
}

void DieConnection::shutdown()
{
    std::shared_ptr<Pixel> localPixel;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
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
    return selected_;
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

    auto result = co_await pixel->connectAsync();
    if (result != Pixel::ConnectResult::Success)
    {
        log("First connection attempt failed, retrying...");
        result = co_await pixel->connectAsync();
    }

    if (result == Pixel::ConnectResult::Success)
    {
        log("Connected and ready to use!");
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
        if (shuttingDown_ || connecting_ || !pixel_)
        {
            return false;
        }
        connecting_ = true;
        lastConnectAttempt_ = std::chrono::steady_clock::now();
        localPixel = pixel_;
    }

    // Preserve old behavior: explicit disconnect before retrying connect.
    log("[watchdog] Reconnecting...");

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
        log(std::string("[watchdog] Reconnect exception: ") + ex.what());
    }
    catch (...)
    {
        log("[watchdog] Reconnect unknown exception.");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connecting_ = false;
        connectedOnce_ = ok;
        lastAnyMessage_ = std::chrono::steady_clock::now();
        lastRollEvent_ = lastAnyMessage_;
    }

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
            connecting_ = false;
            connectedOnce_ = ok;
            lastAnyMessage_ = std::chrono::steady_clock::now();
            lastRollEvent_ = lastAnyMessage_;
        }
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
}

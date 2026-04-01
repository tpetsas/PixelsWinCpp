#include "pch.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "Systemic/Pixels/Pixel.h"
#include "Systemic/Pixels/PixelScanner.h"

using namespace Systemic::Pixels;
using namespace Systemic::Pixels::Messages;
using namespace std::chrono_literals;

namespace
{
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

    std::string serializeTimePoint(const std::chrono::system_clock::time_point& time, const std::string& format)
    {
        std::time_t tt = std::chrono::system_clock::to_time_t(time);
        std::tm tm{};
        localtime_s(&tm, &tt);

        std::stringstream ss;
        ss << std::put_time(&tm, format.c_str());
        return ss.str();
    }

    // Minimal delegate used by this original sample entry point.
    struct SimpleDelegate : PixelDelegate
    {
        void onStatusChanged(std::shared_ptr<Pixel> /*pixel*/, PixelStatus status) override
        {
            std::cout << "\nStatus changed to " << toStatusString(status);
        }

        void onRssiChanged(std::shared_ptr<Pixel> /*pixel*/, int rssi) override
        {
            std::cout << "\nRSSI changed to " << rssi << "dBm";
        }

        void onRolled(std::shared_ptr<Pixel> /*pixel*/, int face) override
        {
            std::cout << "\nRolled on face " << face;
        }
    };

    // Connect helper kept intentionally simple for quick testing.
    std::future<void> connectAndBlink(const std::shared_ptr<Pixel>& pixel)
    {
        std::cout << "\nConnecting...";

        auto result = co_await pixel->connectAsync();
        if (result != Pixel::ConnectResult::Success)
        {
            result = co_await pixel->connectAsync();
        }

        if (result == Pixel::ConnectResult::Success)
        {
            std::cout << "\nConnected and ready to use!";
            co_await pixel->reportRssiAsync();
            co_await pixel->blinkAsync(3s, 0xFF0000, 3);
            std::cout << "\nRoll die to see results...";
        }
        else
        {
            std::cout << "\nConnection error: " << static_cast<int>(result);
        }
    }
}

// Original single-die sample entry point from the upstream-style flow.
// The config-based multi-die app now lives in `app_main.cpp`.
int main()
{
    winrt::init_apartment();

    std::mutex mutex;
    std::shared_ptr<Pixel> pixel;
    std::shared_ptr<SimpleDelegate> delegate;
    std::thread connectThread;

    std::cout << "Scanning for Pixels dice...\n";
    std::cout << "Once a Pixel is found, it will attempt to connect to it.\n";
    std::cout << "Press Enter to exit.\n";

    PixelScanner scanner{ [&mutex, &pixel, &delegate, &connectThread](const std::shared_ptr<const ScannedPixel>& scannedPixel)
    {
        if (!scannedPixel)
        {
            return;
        }

        std::wcout << L"\nScanned Pixel: " << scannedPixel->name().c_str();
        std::stringstream ss;
        ss << "\nId: 0x" << std::hex << static_cast<uint32_t>(scannedPixel->pixelId()) << std::dec;
        ss << "\nRSSI: " << scannedPixel->rssi();
        ss << "\nFirmware: " << serializeTimePoint(scannedPixel->firmwareDate(), "%Y-%m-%d %H:%M:%S");
        ss << "\nRoll state: " << static_cast<int>(scannedPixel->rollState());
        ss << "\nCurrent face: " << static_cast<int>(scannedPixel->currentFace());
        ss << "\nBattery level: " << static_cast<int>(scannedPixel->batteryLevel()) << "%";
        ss << "\nCharging: " << (scannedPixel->isCharging() ? "yes" : "no");
        std::cout << ss.str();

        std::lock_guard<std::mutex> lock{ mutex };
        if (pixel)
        {
            return;
        }

        delegate = std::make_shared<SimpleDelegate>();
        pixel = Pixel::create(*scannedPixel, delegate);

        if (connectThread.joinable())
        {
            connectThread.join();
        }
        connectThread = std::thread([pixel]() { connectAndBlink(pixel).get(); });
    } };

    scanner.start();

    std::string line;
    std::getline(std::cin, line);

    try
    {
        scanner.stop();
    }
    catch (...)
    {
    }

    if (connectThread.joinable())
    {
        connectThread.join();
    }

    std::cout << "\nBye!\n";
    return 0;
}

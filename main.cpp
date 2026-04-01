#include "pch.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <future>
#include <string>
#include <chrono>
#include <atomic>
#include <exception>
#include <cstdlib>

#include "Systemic/Pixels/PixelScanner.h"
#include "Systemic/Pixels/Pixel.h"

using namespace Systemic::Pixels;
using namespace Systemic::Pixels::Messages;
using namespace std::chrono_literals;

// Change this to the die you want to test.
constexpr uint32_t TARGET_PIXEL_ID = 0xbb55091c;

std::mutex g_logMutex;

void logLine(const std::string& s)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << s << std::flush;
}

void installTerminateHandler()
{
    std::set_terminate([]()
    {
        try
        {
            auto eptr = std::current_exception();
            if (eptr)
            {
                std::rethrow_exception(eptr);
            }
            logLine("\n[FATAL] std::terminate called with no current exception.\n");
        }
        catch (const std::exception& ex)
        {
            logLine(std::string("\n[FATAL] Unhandled exception: ") + ex.what() + "\n");
        }
        catch (...)
        {
            logLine("\n[FATAL] Unhandled non-standard exception.\n");
        }
        std::abort();
    });
}

std::string to_string(PixelStatus status)
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

struct SharedState
{
    std::mutex mutex;
    std::shared_ptr<Pixel> pixel;

    bool shuttingDown = false;
    bool connecting = false;
    bool connectedOnce = false;
    bool selected = false;

    std::chrono::steady_clock::time_point lastRollEvent = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnyMessage = std::chrono::steady_clock::now();
};

struct MyDelegate : PixelDelegate
{
    SharedState* state = nullptr;

    explicit MyDelegate(SharedState* s) : state(s) {}

    void markAnyMessage()
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->lastAnyMessage = std::chrono::steady_clock::now();
    }

    void markRollEvent()
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->lastRollEvent = std::chrono::steady_clock::now();
        state->lastAnyMessage = state->lastRollEvent;
    }

    void onStatusChanged(std::shared_ptr<Pixel> /*pixel*/, PixelStatus status) override
    {
        logLine("\nStatus changed to " + to_string(status));
        markAnyMessage();
    }

    void onFirmwareDateChanged(std::shared_ptr<Pixel> /*pixel*/, std::chrono::system_clock::time_point firmwareDate) override
    {
        logLine("\nFirmware date changed to " + serializeTimePoint(firmwareDate, "%Y-%m-%d %H:%M:%S"));
        markAnyMessage();
    }

    void onRssiChanged(std::shared_ptr<Pixel> /*pixel*/, int rssi) override
    {
        logLine("\nRSSI changed to " + std::to_string(rssi) + "dBm");
        markAnyMessage();
    }

    void onBatteryLevelChanged(std::shared_ptr<Pixel> /*pixel*/, int batteryLevel) override
    {
        logLine("\nBattery level changed to " + std::to_string(batteryLevel) + "%");
        markAnyMessage();
    }

    void onChargingStateChanged(std::shared_ptr<Pixel> /*pixel*/, bool isCharging) override
    {
        logLine(std::string("\nBattery is") + (isCharging ? "" : " not") + " charging");
        markAnyMessage();
    }

    void onRollStateChanged(std::shared_ptr<Pixel> /*pixel*/, PixelRollState stateValue, int face) override
    {
        logLine("\nRoll state changed to " + std::to_string((int)stateValue) + " with face " + std::to_string(face) + " up");
        markRollEvent();
    }

    void onRolled(std::shared_ptr<Pixel> /*pixel*/, int face) override
    {
        logLine("\nRolled on face " + std::to_string(face));
        markRollEvent();
    }

    void onMessageReceived(std::shared_ptr<Pixel> /*pixel*/, std::shared_ptr<const PixelMessage> message) override
    {
        if (message)
        {
            logLine("\n[raw] message type = " + std::string(getMessageName(message->type)));
        }
        markAnyMessage();
    }
};

std::future<Pixel::ConnectResult> connectAndInitialize(std::shared_ptr<Pixel> pixel)
{
    logLine("\nConnecting...");

    auto result = co_await pixel->connectAsync();
    if (result != Pixel::ConnectResult::Success)
    {
        logLine("\nFirst connection attempt failed, retrying...");
        result = co_await pixel->connectAsync();
    }

    if (result == Pixel::ConnectResult::Success)
    {
        logLine("\nConnected and ready to use!");
        logLine("\nRoll die to see results...");
    }
    else
    {
        logLine("\nConnection error: " + std::to_string((int)result));
    }

    co_return result;
}

bool reconnectPixel(SharedState& state)
{
    std::shared_ptr<Pixel> localPixel;

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.shuttingDown || state.connecting || !state.pixel)
        {
            return false;
        }
        state.connecting = true;
        localPixel = state.pixel;
    }

    logLine("\n[watchdog] Reconnecting...");

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
        logLine(std::string("\n[watchdog] Reconnect exception: ") + ex.what());
    }
    catch (...)
    {
        logLine("\n[watchdog] Reconnect unknown exception.");
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.connecting = false;
        state.connectedOnce = ok;
        state.lastAnyMessage = std::chrono::steady_clock::now();
        state.lastRollEvent = state.lastAnyMessage;
    }

    return ok;
}

int main()
{
    installTerminateHandler();
    winrt::init_apartment();

    SharedState state;
    auto delegate = std::make_shared<MyDelegate>(&state);

    std::stringstream header;
    header << "Scanning for target Pixel die...\n";
    header << "Target pixel id: 0x" << std::hex << TARGET_PIXEL_ID << std::dec << "\n";
    header << "Press Enter to exit.\n";
    logLine(header.str());

    PixelScanner* scannerPtr = nullptr;
    std::thread connectThread;

    PixelScanner::ScannedPixelListener listener =
        [&state, &delegate, &scannerPtr, &connectThread](const std::shared_ptr<const ScannedPixel>& scannedPixel)
        {
            if (!scannedPixel)
            {
                return;
            }

            const uint32_t id = (uint32_t)scannedPixel->pixelId();
            if (id != TARGET_PIXEL_ID)
            {
                return;
            }

            bool shouldConnect = false;
            bool firstSelection = false;

            {
                std::lock_guard<std::mutex> lock(state.mutex);

                if (state.shuttingDown || state.connecting)
                {
                    return;
                }

                if (!state.selected)
                {
                    state.selected = true;
                    firstSelection = true;
                }

                if (!state.pixel)
                {
                    state.connecting = true;
                    state.pixel = Pixel::create(*scannedPixel, delegate);
                    state.lastAnyMessage = std::chrono::steady_clock::now();
                    state.lastRollEvent = state.lastAnyMessage;
                    shouldConnect = true;
                }
            }

            if (!shouldConnect)
            {
                return;
            }

            if (firstSelection)
            {
                {
                    std::lock_guard<std::mutex> lock(g_logMutex);
                    std::wcout << L"\nChosen Pixel: " << scannedPixel->name().c_str() << std::flush;
                }

                std::stringstream ss;
                ss << "\nId: " << std::hex << id << std::dec;
                ss << "\nRSSI: " << scannedPixel->rssi();
                ss << "\nFirmware: " << serializeTimePoint(scannedPixel->firmwareDate(), "%Y-%m-%d %H:%M:%S");
                ss << "\nRoll state: " << (int)scannedPixel->rollState();
                ss << "\nCurrent face: " << (int)scannedPixel->currentFace();
                ss << "\nBattery level: " << (int)scannedPixel->batteryLevel() << "%";
                ss << "\nCharging: " << (scannedPixel->isCharging() ? "yes" : "no");
                logLine(ss.str());
            }

            if (scannerPtr)
            {
                try
                {
                    scannerPtr->stop();
                    logLine("\nStopped scanning after selecting target die.");
                }
                catch (...)
                {
                }
            }

            if (connectThread.joinable())
            {
                connectThread.join();
            }

            connectThread = std::thread([&state]()
            {
                bool ok = false;

                try
                {
                    std::shared_ptr<Pixel> localPixel;
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        localPixel = state.pixel;
                    }

                    if (localPixel)
                    {
                        const auto result = connectAndInitialize(localPixel).get();
                        ok = (result == Pixel::ConnectResult::Success);
                    }
                }
                catch (const std::exception& ex)
                {
                    logLine(std::string("\nConnect thread exception: ") + ex.what());
                }
                catch (...)
                {
                    logLine("\nConnect thread unknown exception.");
                }

                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.connecting = false;
                    state.connectedOnce = ok;
                    state.lastAnyMessage = std::chrono::steady_clock::now();
                    state.lastRollEvent = state.lastAnyMessage;
                }
            });
        };

    PixelScanner scanner(listener);
    scannerPtr = &scanner;
    scanner.start();

    std::atomic<bool> inputDone = false;

    std::thread inputThread([&]()
    {
        try
        {
            std::string line;
            std::getline(std::cin, line);
        }
        catch (...)
        {
        }
        inputDone = true;
    });

    std::thread poller([&state, &inputDone]()
    {
        try
        {
            while (!inputDone)
            {
                std::this_thread::sleep_for(5s);

                std::shared_ptr<Pixel> localPixel;
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    localPixel = state.pixel;
                }

                if (localPixel)
                {
                    logLine("\n[poll] status=" + to_string(localPixel->status()) +
                            ", face=" + std::to_string(localPixel->currentFace()) +
                            ", rollState=" + std::to_string((int)localPixel->rollState()) +
                            ", battery=" + std::to_string(localPixel->batteryLevel()));
                }
            }
        }
        catch (const std::exception& ex)
        {
            logLine(std::string("\nPoller exception: ") + ex.what());
        }
        catch (...)
        {
            logLine("\nPoller unknown exception.");
        }
    });

    std::thread watchdog([&state, &inputDone]()
    {
        try
        {
            while (!inputDone)
            {
                std::this_thread::sleep_for(3s);

                std::shared_ptr<Pixel> localPixel;
                bool connecting = false;
                bool connectedOnce = false;
                PixelStatus currentStatus = PixelStatus::Disconnected;

                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    connecting = state.connecting;
                    connectedOnce = state.connectedOnce;
                    localPixel = state.pixel;
                    if (localPixel)
                    {
                        currentStatus = localPixel->status();
                    }
                }

                if (!localPixel || connecting || !connectedOnce)
                {
                    continue;
                }

                if (currentStatus == PixelStatus::Disconnected)
                {
                    logLine("\n[watchdog] Detected disconnect.");
                    reconnectPixel(state);
                }
            }
        }
        catch (const std::exception& ex)
        {
            logLine(std::string("\nWatchdog exception: ") + ex.what());
        }
        catch (...)
        {
            logLine("\nWatchdog unknown exception.");
        }
    });

    while (!inputDone)
    {
        std::this_thread::sleep_for(200ms);
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.shuttingDown = true;
        if (state.pixel)
        {
            try
            {
                state.pixel->disconnect();
            }
            catch (...)
            {
            }
        }
    }

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

    if (watchdog.joinable())
    {
        watchdog.join();
    }

    if (poller.joinable())
    {
        poller.join();
    }

    if (inputThread.joinable())
    {
        inputThread.join();
    }

    logLine("\nBye!\n");
    return 0;
}

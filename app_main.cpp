#include "pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "App/CliOptions.h"
#include "App/ConfigManager.h"
#include "App/DiceManager.h"
#include "Systemic/Pixels/PixelScanner.h"

using namespace Systemic::Pixels;

namespace
{
    std::mutex g_logMutex;

    struct DiscoveredDie
    {
        uint32_t pixelId = 0;
        std::wstring name;
        int rssi = 0;
        int batteryLevel = 0;
        bool isCharging = false;
        int currentFace = 0;
        PixelRollState rollState = PixelRollState::Unknown;
        std::chrono::system_clock::time_point firmwareDate{};
    };

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

    std::vector<DiscoveredDie> scanUntilEnter(bool printDiscoveries)
    {
        std::mutex diceMutex;
        std::map<uint32_t, DiscoveredDie> discoveredById;

        // Keep latest advertisement snapshot per pixel ID while scan is running.
        PixelScanner::ScannedPixelListener listener =
            [&diceMutex, &discoveredById, printDiscoveries](const std::shared_ptr<const ScannedPixel>& scannedPixel)
            {
                if (!scannedPixel)
                {
                    return;
                }

                const uint32_t id = static_cast<uint32_t>(scannedPixel->pixelId());
                bool isNew = false;

                {
                    std::lock_guard<std::mutex> lock(diceMutex);

                    auto it = discoveredById.find(id);
                    if (it == discoveredById.end())
                    {
                        DiscoveredDie die;
                        die.pixelId = id;
                        die.name = scannedPixel->name();
                        die.rssi = scannedPixel->rssi();
                        die.batteryLevel = scannedPixel->batteryLevel();
                        die.isCharging = scannedPixel->isCharging();
                        die.currentFace = scannedPixel->currentFace();
                        die.rollState = scannedPixel->rollState();
                        die.firmwareDate = scannedPixel->firmwareDate();
                        discoveredById.emplace(id, die);
                        isNew = true;
                    }
                    else
                    {
                        it->second.name = scannedPixel->name();
                        it->second.rssi = scannedPixel->rssi();
                        it->second.batteryLevel = scannedPixel->batteryLevel();
                        it->second.isCharging = scannedPixel->isCharging();
                        it->second.currentFace = scannedPixel->currentFace();
                        it->second.rollState = scannedPixel->rollState();
                        it->second.firmwareDate = scannedPixel->firmwareDate();
                    }
                }

                if (printDiscoveries && isNew)
                {
                    logLine("\n[scan] Found " + ConfigManager::formatPixelId(id) + " (" + narrow(scannedPixel->name()) + ")");
                }
            };

        PixelScanner scanner(listener);
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

        std::vector<DiscoveredDie> out;
        {
            std::lock_guard<std::mutex> lock(diceMutex);
            out.reserve(discoveredById.size());
            for (const auto& kvp : discoveredById)
            {
                out.push_back(kvp.second);
            }
        }

        std::sort(out.begin(), out.end(), [](const DiscoveredDie& a, const DiscoveredDie& b)
        {
            return a.pixelId < b.pixelId;
        });

        return out;
    }

    void printDiscoveredDice(const std::vector<DiscoveredDie>& dice)
    {
        if (dice.empty())
        {
            logLine("\nNo Pixels dice discovered.\n");
            return;
        }

        for (size_t i = 0; i < dice.size(); ++i)
        {
            const auto& die = dice[i];
            std::stringstream ss;
            ss << "\n[" << (i + 1) << "] "
               << ConfigManager::formatPixelId(die.pixelId)
               << " name='" << narrow(die.name) << "'"
               << " rssi=" << die.rssi
               << " battery=" << die.batteryLevel << "%"
               << " charging=" << (die.isCharging ? "yes" : "no")
               << " face=" << die.currentFace
               << " rollState=" << static_cast<int>(die.rollState)
               << " firmware=" << serializeTimePoint(die.firmwareDate, "%Y-%m-%d %H:%M:%S");
            logLine(ss.str());
        }

        logLine("\n");
    }

    bool parseSelectionLine(const std::string& line, size_t maxIndex, std::vector<size_t>& outIndices)
    {
        outIndices.clear();

        std::string normalized = line;
        for (char& c : normalized)
        {
            if (c == ',' || c == ';')
            {
                c = ' ';
            }
        }

        std::istringstream iss(normalized);
        std::set<size_t> uniqueIndices;

        size_t index = 0;
        while (iss >> index)
        {
            if (index < 1 || index > maxIndex)
            {
                return false;
            }
            uniqueIndices.insert(index - 1);
        }

        if (uniqueIndices.empty() || uniqueIndices.size() > 2)
        {
            return false;
        }

        outIndices.assign(uniqueIndices.begin(), uniqueIndices.end());
        return true;
    }

    int runListMode()
    {
        logLine("Scanning for nearby Pixels dice. Press Enter to stop and print results...\n");
        const auto discovered = scanUntilEnter(true);
        logLine("\nScan complete.\n");
        printDiscoveredDice(discovered);
        return 0;
    }

    int runSetupMode()
    {
        logLine("Setup mode: scan for Pixels dice and pick 1 or 2 to save.\n");
        logLine("Press Enter when you are ready to choose from discovered dice...\n");

        const auto discovered = scanUntilEnter(true);
        if (discovered.empty())
        {
            logLine("\nNo dice discovered. Try --setup again closer to your dice.\n");
            return 1;
        }

        logLine("\nDiscovered dice:\n");
        printDiscoveredDice(discovered);

        std::vector<size_t> pickedIndices;
        while (true)
        {
            logLine("Select 1 or 2 dice by index (example: 1 or 1,2): ");

            std::string line;
            std::getline(std::cin, line);

            if (parseSelectionLine(line, discovered.size(), pickedIndices))
            {
                break;
            }

            logLine("\nInvalid selection. Please choose one or two valid indices.\n");
        }

        PixelsConfig config;
        for (const size_t idx : pickedIndices)
        {
            config.pixelIds.push_back(discovered[idx].pixelId);
        }

        std::string error;
        if (!ConfigManager::save("pixels.cfg", config, error))
        {
            logLine("\nFailed to save pixels.cfg: " + error + "\n");
            return 1;
        }

        std::stringstream ss;
        ss << "\nSaved pixels.cfg with " << config.pixelIds.size() << " die/dice: ";
        for (size_t i = 0; i < config.pixelIds.size(); ++i)
        {
            if (i > 0)
            {
                ss << ", ";
            }
            ss << ConfigManager::formatPixelId(config.pixelIds[i]);
        }
        ss << "\n";
        logLine(ss.str());

        return 0;
    }

    int runNormalMode(const CliOptions& options)
    {
        PixelsConfig config;
        std::string error;
        if (!ConfigManager::load("pixels.cfg", config, error))
        {
            logLine("Failed to load pixels.cfg: " + error + "\n");
            logLine("Run with --setup to create pixels.cfg.\n");
            return 1;
        }

        auto runtimeLogger = [&options](const std::string& message)
        {
            if (!options.rollsOnlyLogs)
            {
                logLine(message);
                return;
            }

            if (message.find("Rolled on face") != std::string::npos)
            {
                logLine(message);
            }
        };

        // Runtime manager keeps the connect/watchdog behavior for each configured die.
        DiceManager manager(config.pixelIds, runtimeLogger);
        manager.runUntilEnterPressed();

        logLine("\nBye!\n");
        return 0;
    }

    void printHelp()
    {
        logLine(
            "PixelsWinCpp usage:\n"
            "  --list   Scan and print nearby Pixels dice\n"
            "  --setup  Scan, choose 1 or 2 dice, save pixels.cfg\n"
            "  (no args) Load pixels.cfg and connect to configured dice\n"
            "  --rolls-only  In normal mode, print only roll result lines\n");
    }
}

int main(int argc, char** argv)
{
    installTerminateHandler();
    winrt::init_apartment();

    const CliOptions options = parseCliOptions(argc, argv);
    if (!options.valid)
    {
        logLine("Error: " + options.error + "\n\n");
        printHelp();
        return 1;
    }

    switch (options.mode)
    {
    case AppMode::List:
        return runListMode();
    case AppMode::Setup:
        return runSetupMode();
    case AppMode::Help:
        printHelp();
        return 0;
    case AppMode::Run:
    default:
        return runNormalMode(options);
    }
}

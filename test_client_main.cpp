#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <chrono>
#include <ctime>
#include <iomanip>

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";

// Writes a line to both stdout and the log file.
static std::ofstream g_logFile;

static void tee(const std::string& msg)
{
    std::cout << msg << std::endl;
    if (g_logFile.is_open())
        g_logFile << msg << "\n";
}

static std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

HANDLE connectToPipe()
{
    tee("Connecting to pipe...");

    while (true)
    {
        HANDLE pipe = CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (pipe != INVALID_HANDLE_VALUE)
        {
            tee("Connected!");
            return pipe;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY)
        {
            tee("Pipe busy, waiting...");
            if (!WaitNamedPipeW(kPipeName, 5000))
            {
                tee("Timeout waiting for pipe.");
                return INVALID_HANDLE_VALUE;
            }
        }
        else if (err == ERROR_FILE_NOT_FOUND)
        {
            tee("Pipe not found. Is PixelsTray running?");
            return INVALID_HANDLE_VALUE;
        }
        else
        {
            tee("Failed to connect, error: " + std::to_string(err));
            return INVALID_HANDLE_VALUE;
        }
    }
}

bool sendRequest(HANDLE pipe, const std::string& json)
{
    std::string msg = json + "\n";
    DWORD written = 0;
    if (!WriteFile(pipe, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr))
    {
        tee("WriteFile failed, error: " + std::to_string(GetLastError()));
        return false;
    }
    FlushFileBuffers(pipe);
    return true;
}

std::string readResponse(HANDLE pipe)
{
    std::string result;
    char buf[4096];

    while (true)
    {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            break;
        buf[bytesRead] = '\0';
        result += buf;
        if (result.find('\n') != std::string::npos)
            break;
    }

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result;
}

void printHelp()
{
    tee("\n=== Pixels Roll Server Test Client ===");
    tee("Commands:");
    tee("  normal               - Send normal roll (auto-increments generation)");
    tee("  advantage            - Send advantage roll (auto-increments generation)");
    tee("  disadvantage         - Send disadvantage roll (auto-increments generation)");
    tee("  normal <gen>         - Send normal roll with specific generation");
    tee("  advantage <gen>      - Send advantage roll with specific generation");
    tee("  disadvantage <gen>   - Send disadvantage roll with specific generation");
    tee("  ready                - Send ready command (BG3 click)");
    tee("  raw <json>           - Send raw JSON string");
    tee("  reset                - Reset all generation counters to 1");
    tee("  help                 - Show this help");
    tee("  quit                 - Exit");
    tee("");
}

int main()
{
    g_logFile.open("client_log.txt", std::ios::app);
    if (g_logFile.is_open())
    {
        g_logFile << "\n--- Session started: " << timestamp() << " ---\n";
        g_logFile.flush();
    }

    HANDLE pipe = connectToPipe();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        tee("Press Enter to exit...");
        std::cin.get();
        return 1;
    }

    printHelp();

    // Per-mode auto-incrementing generation counters
    std::map<std::string, int> generations = {
        {"normal", 1}, {"advantage", 1}, {"disadvantage", 1}
    };

    std::string line;
    while (true)
    {
        std::cout << "> ";
        if (g_logFile.is_open()) g_logFile << "> " << std::flush;

        if (!std::getline(std::cin, line))
            break;

        if (g_logFile.is_open()) g_logFile << line << "\n";

        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        std::string json;

        if (cmd == "quit" || cmd == "exit" || cmd == "q")
        {
            break;
        }
        else if (cmd == "help" || cmd == "h" || cmd == "?")
        {
            printHelp();
            continue;
        }
        else if (cmd == "reset")
        {
            for (auto& kv : generations) kv.second = 1;
            tee("Generation counters reset to 1.");
            continue;
        }
        else if (cmd == "ready")
        {
            json = "{\"mode\": \"ready\"}";
        }
        else if (cmd == "normal" || cmd == "advantage" || cmd == "disadvantage")
        {
            int gen = 0;
            if (iss >> gen)
            {
                // Explicit generation — also sync the counter
                generations[cmd] = gen;
            }
            else
            {
                gen = generations[cmd];
            }
            json = "{\"mode\": \"" + cmd + "\", \"generation\": " + std::to_string(gen) + "}";
        }
        else if (cmd == "raw")
        {
            json = line.substr(4);
            auto pos = json.find_first_not_of(" \t");
            if (pos != std::string::npos)
                json = json.substr(pos);
            else
            {
                tee("Usage: raw <json>");
                continue;
            }
        }
        else
        {
            tee("Unknown command: " + cmd + " (type 'help')");
            continue;
        }

        tee("Sending: " + json);

        auto t0 = std::chrono::steady_clock::now();

        if (!sendRequest(pipe, json))
        {
            tee("Send failed, pipe may be broken.");
            break;
        }

        tee("Waiting for response...");
        std::string response = readResponse(pipe);

        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (response.empty())
        {
            tee("No response (pipe closed?).");
            break;
        }

        tee("Response: " + response);
        tee("Time: " + std::to_string(ms) + " ms");

        // Auto-increment generation for roll commands (not ready/raw)
        if (cmd == "normal" || cmd == "advantage" || cmd == "disadvantage")
            generations[cmd]++;
    }

    CloseHandle(pipe);
    tee("Disconnected.");

    if (g_logFile.is_open())
    {
        g_logFile << "--- Session ended: " << timestamp() << " ---\n";
        g_logFile.close();
    }

    return 0;
}

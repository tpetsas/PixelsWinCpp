#include <windows.h>
#include <iostream>
#include <string>
#include <sstream>

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";

HANDLE connectToPipe()
{
    std::cout << "Connecting to pipe..." << std::endl;

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
            std::cout << "Connected!" << std::endl;
            return pipe;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY)
        {
            std::cout << "Pipe busy, waiting..." << std::endl;
            if (!WaitNamedPipeW(kPipeName, 5000))
            {
                std::cerr << "Timeout waiting for pipe." << std::endl;
                return INVALID_HANDLE_VALUE;
            }
        }
        else if (err == ERROR_FILE_NOT_FOUND)
        {
            std::cerr << "Pipe not found. Is PixelsTray running?" << std::endl;
            return INVALID_HANDLE_VALUE;
        }
        else
        {
            std::cerr << "Failed to connect, error: " << err << std::endl;
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
        std::cerr << "WriteFile failed, error: " << GetLastError() << std::endl;
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
        {
            break;
        }
        buf[bytesRead] = '\0';
        result += buf;

        if (result.find('\n') != std::string::npos)
        {
            break;
        }
    }

    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    {
        result.pop_back();
    }

    return result;
}

void printHelp()
{
    std::cout << "\n=== Pixels Roll Server Test Client ===" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  normal <gen>         - Send normal roll request" << std::endl;
    std::cout << "  advantage <gen>      - Send advantage roll request" << std::endl;
    std::cout << "  disadvantage <gen>   - Send disadvantage roll request" << std::endl;
    std::cout << "  ready                - Send ready command (BG3 click)" << std::endl;
    std::cout << "  raw <json>           - Send raw JSON string" << std::endl;
    std::cout << "  help                 - Show this help" << std::endl;
    std::cout << "  quit                 - Exit" << std::endl;
    std::cout << std::endl;
}

int main()
{
    HANDLE pipe = connectToPipe();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    printHelp();

    std::string line;
    while (true)
    {
        std::cout << "> ";
        if (!std::getline(std::cin, line))
        {
            break;
        }

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
        else if (cmd == "ready")
        {
            json = "{\"mode\": \"ready\"}";
        }
        else if (cmd == "normal" || cmd == "advantage" || cmd == "disadvantage")
        {
            int gen = 1;
            iss >> gen;
            json = "{\"mode\": \"" + cmd + "\", \"generation\": " + std::to_string(gen) + "}";
        }
        else if (cmd == "raw")
        {
            json = line.substr(4);
            // Trim leading whitespace
            auto pos = json.find_first_not_of(" \t");
            if (pos != std::string::npos)
                json = json.substr(pos);
            else
            {
                std::cerr << "Usage: raw <json>" << std::endl;
                continue;
            }
        }
        else
        {
            std::cerr << "Unknown command: " << cmd << " (type 'help')" << std::endl;
            continue;
        }

        std::cout << "Sending: " << json << std::endl;

        if (!sendRequest(pipe, json))
        {
            std::cerr << "Send failed, pipe may be broken." << std::endl;
            break;
        }

        std::cout << "Waiting for response..." << std::endl;
        std::string response = readResponse(pipe);

        if (response.empty())
        {
            std::cerr << "No response (pipe closed?)." << std::endl;
            break;
        }

        std::cout << "Response: " << response << std::endl;
    }

    CloseHandle(pipe);
    std::cout << "Disconnected." << std::endl;
    return 0;
}

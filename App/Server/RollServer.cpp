#include "pch.h"
#include "App/Server/RollServer.h"
#include "App/DieConnection.h"

#include <shellapi.h>
#include <sstream>
#include <algorithm>

// Minimal JSON helpers — no external dependency needed for this simple protocol.
// We parse/emit flat JSON objects with string and integer values only.

namespace
{
    // Trim whitespace from both ends
    std::string trim(const std::string& s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // Extract a string value for a given key from a flat JSON object
    std::string jsonString(const std::string& json, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    // Extract an integer value for a given key from a flat JSON object
    int jsonInt(const std::string& json, const std::string& key, int defaultVal = 0)
    {
        const std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return defaultVal;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return defaultVal;
        // Skip whitespace after colon
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        try { return std::stoi(json.substr(pos)); }
        catch (...) { return defaultVal; }
    }

    // Find BG3 window
    HWND g_bg3Window = nullptr;

    BOOL CALLBACK FindBG3WindowCallback(HWND hwnd, LPARAM /*lParam*/)
    {
        wchar_t title[256];
        GetWindowTextW(hwnd, title, 256);
        std::wstring titleStr(title);
        if (titleStr.find(L"Baldur's Gate 3") != std::wstring::npos)
        {
            g_bg3Window = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindBG3Window()
    {
        g_bg3Window = nullptr;
        EnumWindows(FindBG3WindowCallback, 0);
        return g_bg3Window;
    }
#if 0
    void ClickWindowCenter(HWND hwnd)
    {
        if (!hwnd) return;

        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) return;

        int clickX = rect.left + (rect.right - rect.left) / 2;
        int clickY = rect.top + (rect.bottom - rect.top) / 2;

        // Convert pixel coords to absolute (0-65535) for MOUSEEVENTF_ABSOLUTE
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        LONG absX = static_cast<LONG>((clickX * 65536) / screenW);
        LONG absY = static_cast<LONG>((clickY * 65536) / screenH);

        // Ensure BG3 has focus
        SetForegroundWindow(hwnd);
        Sleep(100);

        // Move cursor and click
        INPUT inputs[3] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = absX;
        inputs[0].mi.dy = absY;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

        inputs[2].type = INPUT_MOUSE;
        inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;

        SendInput(3, inputs, sizeof(INPUT));
    }
#endif
}

bool GetClientPointOnScreen(HWND hwnd, float normX, float normY, POINT& outPt)
{
    if (!hwnd || !IsWindow(hwnd)) return false;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;

    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    POINT pt{};
    pt.x = static_cast<LONG>(std::lround(clientW * normX));
    pt.y = static_cast<LONG>(std::lround(clientH * normY));

    if (!ClientToScreen(hwnd, &pt)) return false;

    outPt = pt;
    return true;
}

// Move cursor using SendInput so it generates raw-input events.
// BG3 uses raw input to detect mouse activity and switch from
// controller mode to mouse mode; SetCursorPos alone won't do it.
void MoveMouseAbsolute(int screenX, int screenY)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(std::lround(screenX * 65535.0 / (sw - 1)));
    input.mi.dy = static_cast<LONG>(std::lround(screenY * 65535.0 / (sh - 1)));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(INPUT));
}

void LeftClickScreenPoint(POINT pt)
{
    MoveMouseAbsolute(pt.x, pt.y);

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SendInput(2, inputs, sizeof(INPUT));
}

void ClickClientNormalized(HWND hwnd, float normX, float normY)
{
    if (!hwnd) return;

    POINT pt{};
    if (!GetClientPointOnScreen(hwnd, normX, normY, pt)) return;

    SetForegroundWindow(hwnd);
    Sleep(75);

    LeftClickScreenPoint(pt);
}

RollServer::RollServer(Logger logger)
    : logger_(std::move(logger))
{
}

RollServer::~RollServer()
{
    stop();
}

void RollServer::start(DiceSnapshotProvider snapshotProvider,
                       DiceReconnectSuspender reconnectSuspender)
{
    if (running_) return;

    snapshotProvider_ = std::move(snapshotProvider);
    reconnectSuspender_ = std::move(reconnectSuspender);
    running_ = true;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    serverThread_ = std::thread([this]() { serverThread(); });

    log("[RollServer] Started on pipe: \\\\.\\pipe\\PixelsDiceRoll");
}

void RollServer::stop()
{
    if (!running_) return;

    running_ = false;

    // Signal the stop event to unblock ConnectNamedPipe
    if (stopEvent_)
    {
        SetEvent(stopEvent_);
    }

    // Wake up any thread waiting on rollCv_
    rollCv_.notify_all();

    if (serverThread_.joinable())
    {
        serverThread_.join();
    }

    if (stopEvent_)
    {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    log("[RollServer] Stopped");
}

void RollServer::onRoll(const std::string& label, int face)
{
    std::lock_guard<std::mutex> lock(rollMutex_);
    if (!pendingRequest_.active) return;

    // Deduplicate: if we already have a result from this die (e.g. advert path fired
    // then GATT reconnect also fires), ignore the later duplicate.
    for (const auto& existingLabel : pendingRequest_.collectedLabels)
    {
        if (existingLabel == label) return;
    }

    pendingRequest_.collectedRolls.push_back(face);
    pendingRequest_.collectedLabels.push_back(label);
    log("[RollServer] Roll received: " + label + " -> " + std::to_string(face) +
        " (" + std::to_string(pendingRequest_.collectedRolls.size()) + "/" +
        std::to_string(pendingRequest_.rollsNeeded) + ")");

    rollCv_.notify_all();
}

void RollServer::serverThread()
{
    while (running_)
    {
        // Create the named pipe instance
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,          // Max instances
            4096,       // Out buffer
            4096,       // In buffer
            0,          // Default timeout
            nullptr     // Default security
        );

        if (pipe == INVALID_HANDLE_VALUE)
        {
            log("[RollServer] Failed to create pipe, error: " + std::to_string(GetLastError()));
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Use overlapped ConnectNamedPipe so we can cancel via stopEvent_
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(pipe, &overlapped);
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                // Wait for either a client to connect or the stop event
                HANDLE waitHandles[2] = { overlapped.hEvent, stopEvent_ };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    // Stop requested
                    CancelIo(pipe);
                    CloseHandle(overlapped.hEvent);
                    CloseHandle(pipe);
                    break;
                }

                // Client connected
                DWORD transferred = 0;
                GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
            }
            else if (err != ERROR_PIPE_CONNECTED)
            {
                log("[RollServer] ConnectNamedPipe failed, error: " + std::to_string(err));
                CloseHandle(overlapped.hEvent);
                CloseHandle(pipe);
                continue;
            }
        }

        CloseHandle(overlapped.hEvent);

        if (!running_)
        {
            CloseHandle(pipe);
            break;
        }

        log("[RollServer] Client connected");
        handleClient(pipe);
        log("[RollServer] Client disconnected");

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void RollServer::handleClient(HANDLE pipe)
{
    // Read newline-delimited JSON requests, respond with JSON
    std::string buffer;
    char readBuf[4096];

    while (running_)
    {
        DWORD bytesRead = 0;
        BOOL success = ReadFile(pipe, readBuf, sizeof(readBuf) - 1, &bytesRead, nullptr);
        if (!success || bytesRead == 0)
        {
            break;  // Client disconnected or error
        }

        readBuf[bytesRead] = '\0';
        buffer += readBuf;

        // Process complete lines
        size_t newlinePos;
        while ((newlinePos = buffer.find('\n')) != std::string::npos)
        {
            std::string line = trim(buffer.substr(0, newlinePos));
            buffer = buffer.substr(newlinePos + 1);

            if (line.empty()) continue;

            log("[RollServer] Request: " + line);

            std::string response = processRequest(line);
            response += "\n";

            log("[RollServer] Response: " + trim(response));

            DWORD bytesWritten = 0;
            WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.size()), &bytesWritten, nullptr);
            FlushFileBuffers(pipe);
        }
    }
}

std::string RollServer::processRequest(const std::string& json)
{
    const std::string mode = jsonString(json, "mode");

    if (mode == "ready")
    {
        handleReadyCommand();
        return "{\"mode\": \"ready\", \"status\": \"ok\"}";
    }

    if (mode == "normal" || mode == "advantage" || mode == "disadvantage")
    {
        const uint32_t generation = static_cast<uint32_t>(jsonInt(json, "generation"));
        return waitForRolls(mode, generation);
    }

    return "{\"error\": \"unknown mode\"}";
}

std::string RollServer::waitForRolls(const std::string& mode, uint32_t generation)
{
    const int rollsNeeded = (mode == "normal") ? 1 : 2;

    // Determine configured and connected dice counts
    int configuredDice = 0;
    int connectedDice = 0;
    if (snapshotProvider_)
    {
        auto snapshots = snapshotProvider_();
        configuredDice = static_cast<int>(snapshots.size());
        for (const auto& s : snapshots)
        {
            if (s.status == Systemic::Pixels::PixelStatus::Ready)
            {
                connectedDice++;
            }
        }
    }

    // Set up the pending request
    {
        std::lock_guard<std::mutex> lock(rollMutex_);
        pendingRequest_.active = true;
        pendingRequest_.mode = mode;
        pendingRequest_.generation = generation;
        pendingRequest_.rollsNeeded = rollsNeeded;
        pendingRequest_.collectedRolls.clear();
        pendingRequest_.collectedLabels.clear();
        pendingRequest_.startedAt = std::chrono::system_clock::now();
    }

    log("[RollServer] Waiting for " + std::to_string(rollsNeeded) + " roll(s), mode=" +
        mode + ", gen=" + std::to_string(generation) +
        ", configuredDice=" + std::to_string(configuredDice) +
        ", connectedDice=" + std::to_string(connectedDice));

    // Only show "roll again" popup when the user has a single die configured,
    // not when one of two configured dice is temporarily disconnected.
    bool needsPopup = (rollsNeeded == 2 && configuredDice <= 1);

    // 25s gives the user ample time to roll while capping the worst-case failure
    // (both GATT and advert paths dead) at a tolerable delay. The advert fast-path
    // fires in <1s when a die disconnects mid-roll, so the 25s only burns in a
    // complete radio blackout — far better than the old 60s.
    static constexpr auto kFirstRollTimeout = std::chrono::seconds(25);
    // Windows BLE takes ~10-12s to start delivering advertisements from a freshly-
    // disconnected device (radio/stack arbitration delay). The advert path fires
    // reliably once adverts flow, but needs this window to survive the delay.
    // 15s provides ~3-5s of headroom past the typical 10-12s Windows advert delay.
    static constexpr auto kSecondRollTimeout = std::chrono::seconds(15);

    // Suspend watchdog reconnect attempts so the BLE scanner has a clear channel
    // for advert-based roll recovery during this request window.
    // For 2-die requests, pre-arm for the full first-roll timeout so that a Ready
    // die which disconnects mid-request is also covered (Fix A.1+A.2: the watchdog
    // checks reconnectSuspendedUntil_ before firing, so even a die that just
    // disconnected won't start reconnecting during the request window).
    if (reconnectSuspender_)
    {
        if (rollsNeeded == 2)
        {
            log("[RollServer] Pre-arming reconnect suspension for 2-die request (25s)");
            reconnectSuspender_(kFirstRollTimeout);
        }
        else if (connectedDice < configuredDice)
        {
            log("[RollServer] Suspending reconnects for disconnected dice (10s) to unblock advert path");
            reconnectSuspender_(std::chrono::seconds(10));
        }
    }

    {
        std::unique_lock<std::mutex> lock(rollMutex_);

        // Wait for first roll (with timeout)
        bool gotFirst = rollCv_.wait_for(lock, kFirstRollTimeout, [this]()
        {
            return !pendingRequest_.collectedRolls.empty() || !running_;
        });

        if (!running_)
        {
            pendingRequest_.active = false;
            return "{\"error\": \"server stopping\"}";
        }

        if (!gotFirst)
        {
            log("[RollServer] Timeout waiting for first roll");
            pendingRequest_.active = false;
            return "{\"error\": \"timeout\"}";
        }

        if (rollsNeeded == 2 && pendingRequest_.collectedRolls.size() == 1)
        {
            // Mid-request: unconditionally refresh suspension after the first roll arrives.
            // The pre-arm at request start covers most cases; this handles the narrow race
            // where die 2 disconnects exactly as die 1 fires its roll notification.
            if (reconnectSuspender_)
            {
                lock.unlock();
                log("[RollServer] Mid-request: refreshing reconnect suspension (" +
                    std::to_string(kSecondRollTimeout.count()) + "s) for second die");
                reconnectSuspender_(kSecondRollTimeout);
                lock.lock();
            }

            if (needsPopup)
            {
                // Unlock before showing popup (it has a timer)
                lock.unlock();
                showRollRegisteredPopup();
                lock.lock();
            }

            // Snapshot pre-roll face for the second die so we can detect a face change
            // even when the GATT onRolled notification is dropped by Windows.
            std::string firstLabel = pendingRequest_.collectedLabels.empty()
                ? "" : pendingRequest_.collectedLabels[0];
            int otherDiePreFace = 0;
            std::string otherDieLabel;
            if (snapshotProvider_)
            {
                lock.unlock();
                for (const auto& s : snapshotProvider_())
                {
                    if (s.label != firstLabel)
                    {
                        otherDiePreFace = s.currentFace;
                        otherDieLabel = s.label;
                        break;
                    }
                }
                lock.lock();
            }

            // Wait for second roll. Poll every 500ms so we can detect a face change on a
            // connected-but-silent die (Windows may drop GATT notifications; currentFace is
            // still updated by every RollState heartbeat, including rollState=5 frames).
            const auto secondDeadline = std::chrono::steady_clock::now() + kSecondRollTimeout;
            bool gotSecond = false;
            while (running_ && !gotSecond && std::chrono::steady_clock::now() < secondDeadline)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    secondDeadline - std::chrono::steady_clock::now());
                const auto slice = std::min(remaining, std::chrono::milliseconds(500));

                gotSecond = rollCv_.wait_for(lock, slice, [this]()
                {
                    return pendingRequest_.collectedRolls.size() >= 2 || !running_;
                });

                if (gotSecond || !running_) break;
                if (std::chrono::steady_clock::now() >= secondDeadline) break;

                // Snapshot poll: if the other die is still Ready but its face changed,
                // the notification was dropped — accept the face-change as the roll result.
                if (!otherDieLabel.empty() && snapshotProvider_ && otherDiePreFace > 0)
                {
                    const auto msElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now() - pendingRequest_.startedAt).count();
                    if (msElapsed >= 1500)
                    {
                        lock.unlock();
                        auto snaps = snapshotProvider_();
                        lock.lock();

                        if (pendingRequest_.collectedRolls.size() >= 2)
                        {
                            gotSecond = true;
                            break;
                        }
                        for (const auto& s : snaps)
                        {
                            if (s.label != otherDieLabel) continue;
                            if (s.status != Systemic::Pixels::PixelStatus::Ready) continue;
                            if (s.rollState == Systemic::Pixels::PixelRollState::Rolling ||
                                s.rollState == Systemic::Pixels::PixelRollState::Handling) continue;
                            if (s.currentFace == 0 || s.currentFace == otherDiePreFace) continue;

                            log("[RollServer] Second-roll snapshot recovery: " + s.label +
                                " face " + std::to_string(otherDiePreFace) +
                                " -> " + std::to_string(s.currentFace) +
                                " (GATT notification likely dropped)");
                            pendingRequest_.collectedRolls.push_back(s.currentFace);
                            pendingRequest_.collectedLabels.push_back(s.label + "-snapshot");
                            gotSecond = true;
                            break;
                        }
                    }
                }
            }

            if (!running_)
            {
                pendingRequest_.active = false;
                return "{\"error\": \"server stopping\"}";
            }

            if (!gotSecond)
            {
                // The second die likely disconnected mid-roll.
                // Read its current face from the snapshot as the fallback result —
                // the die has physically settled by now even if BLE dropped.
                log("[RollServer] Second roll timed out, attempting face-snapshot fallback...");

                std::string reportedLabel = pendingRequest_.collectedLabels.empty()
                    ? "" : pendingRequest_.collectedLabels[0];
                auto requestStartedAt = pendingRequest_.startedAt;
                int fallbackFace = 0;
                std::string fallbackLabelSuffix = "fallback";

                // Helper lambda that runs P1/P2/P3 against a snapshot vector.
                // Returns the face if any priority matched, else 0. Also captures
                // wasRollingAtDisconnect / currentFace from the missing die for the
                // caller to inspect (set unconditionally even on a hit).
                auto tryAdvertFallbacks = [&](const std::vector<DieStatusSnapshot>& snaps,
                                              bool& outMissingDieWasRolling,
                                              int& outMissingDieFace,
                                              std::string& outMissingDieLabel) -> int
                {
                    outMissingDieWasRolling = false;
                    outMissingDieFace = 0;
                    outMissingDieLabel.clear();

                    for (const auto& s : snaps)
                    {
                        if (s.label == reportedLabel)
                            continue;

                        outMissingDieWasRolling = s.wasRollingAtDisconnect;
                        outMissingDieFace = s.currentFace;
                        outMissingDieLabel = s.label;

                        // Priority 1: advert debounce has reached the threshold for this
                        // disconnect event. advertSettledFace resets after each advert-reported
                        // result, so non-zero here means it is tracking the current roll.
                        // Require the full threshold (not just >= 1) to avoid mid-tumble false positives.
                        if (s.advertSettledFace > 0 &&
                            s.advertSettledCount >= DieConnection::kAdvertSettledThreshold)
                        {
                            log("[RollServer] Fallback (P1-debounced-advert): using " + s.label +
                                " advertSettledFace=" + std::to_string(s.advertSettledFace) +
                                " (count=" + std::to_string(s.advertSettledCount) + "/" +
                                std::to_string(DieConnection::kAdvertSettledThreshold) + ")");
                            return s.advertSettledFace;
                        }

                        // Priority 2: last confirmed roll face — only if it happened
                        // after this request started (guards against previous-generation faces).
                        if (!s.recentRollFaces.empty() && s.hasLastRoll &&
                            s.lastRollAt >= requestStartedAt)
                        {
                            log("[RollServer] Fallback (P2-recentRollFace): using " + s.label +
                                " recentRollFace=" + std::to_string(s.recentRollFaces.back()));
                            return s.recentRollFaces.back();
                        }

                        // Priority 3: single advert below the normal debounce threshold.
                        // By the time this fallback runs (~18s total), rolling is long over.
                        // The 2-packet debounce guards against mid-tumble false positives during
                        // active rolling — irrelevant here. One advert at timeout is reliable.
                        if (s.advertSettledFace > 0)
                        {
                            log("[RollServer] Fallback (P3-single-advert): using " + s.label +
                                " advertSettledFace=" + std::to_string(s.advertSettledFace) +
                                " (count=" + std::to_string(s.advertSettledCount) + "/" +
                                std::to_string(DieConnection::kAdvertSettledThreshold) +
                                " — below threshold but accepted at timeout)");
                            return s.advertSettledFace;
                        }

                        break;  // only consider the first non-reporting die
                    }
                    return 0;
                };

                bool missingDieWasRolling = false;
                int missingDieFace = 0;
                std::string missingDieLabel;

                // First pass: P1/P2/P3 with current snapshot data.
                lock.unlock();
                if (snapshotProvider_)
                {
                    auto snapshots = snapshotProvider_();
                    fallbackFace = tryAdvertFallbacks(
                        snapshots, missingDieWasRolling, missingDieFace, missingDieLabel);
                }
                lock.lock();

                if (fallbackFace == 0)
                {
                    if (!missingDieWasRolling && missingDieFace > 0)
                    {
                        // Die was idle at disconnect — currentFace is the last GATT-reported
                        // settled face (or post-roll face from a brief reconnect). Safe to use.
                        fallbackFace = missingDieFace;
                        log("[RollServer] Fallback (P4-currentFace): using " + missingDieLabel +
                            " currentFace=" + std::to_string(fallbackFace) +
                            " (last known settled face — die was not rolling at disconnect)");
                    }
                    else if (missingDieWasRolling)
                    {
                        // Die was mid-tumble at disconnect: currentFace is known stale.
                        // Run an extension wait — Windows BLE may suppress adverts from a
                        // freshly-disconnected device for 10-20+ seconds. A polling window
                        // converts a wrong stale face into the correct settled face once
                        // the OS finally lets adverts through.
                        // (Session 10 Fix 2: widened from 4s to 8s — observed advert
                        // suppression up to ~19s post-disconnect in real sessions, exceeding
                        // the prior 15s + 4s = 19s budget. 15s + 8s = 23s catches more.)
                        static constexpr auto kExtensionWindow = std::chrono::seconds(8);
                        const auto extDeadline = std::chrono::steady_clock::now() + kExtensionWindow;

                        log("[RollServer] Extension wait: " + missingDieLabel +
                            " was rolling at disconnect (currentFace=" +
                            std::to_string(missingDieFace) +
                            " is stale); waiting up to 8s for advert data");

                        // Refresh reconnect suspension so the watchdog doesn't start
                        // reconnecting and blocking adverts during the critical extension.
                        if (reconnectSuspender_)
                        {
                            lock.unlock();
                            reconnectSuspender_(std::chrono::duration_cast<std::chrono::seconds>(
                                kExtensionWindow + std::chrono::seconds(1)));
                            lock.lock();
                        }

                        bool gotAnyResult = false;
                        while (running_ && !gotAnyResult &&
                               std::chrono::steady_clock::now() < extDeadline)
                        {
                            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                extDeadline - std::chrono::steady_clock::now());
                            const auto slice = std::min(remaining, std::chrono::milliseconds(500));

                            // Wait briefly on rollCv_ in case advert path fires markRollResult.
                            const bool gotRoll = rollCv_.wait_for(lock, slice, [this]()
                            {
                                return pendingRequest_.collectedRolls.size() >= 2 || !running_;
                            });

                            if (!running_) break;

                            if (gotRoll && pendingRequest_.collectedRolls.size() >= 2)
                            {
                                // Roll arrived via rollCv_ — second die was already added.
                                log("[RollServer] Extension wait: roll arrived via rollCv_");
                                gotAnyResult = true;
                                gotSecond = true;
                                break;
                            }

                            // Re-poll the snapshot for advert data.
                            if (snapshotProvider_)
                            {
                                lock.unlock();
                                auto snaps = snapshotProvider_();
                                int extFace = tryAdvertFallbacks(
                                    snaps, missingDieWasRolling, missingDieFace, missingDieLabel);
                                lock.lock();

                                if (pendingRequest_.collectedRolls.size() >= 2)
                                {
                                    // rollCv_ raced with us; roll was already added
                                    gotAnyResult = true;
                                    gotSecond = true;
                                    break;
                                }
                                if (extFace > 0)
                                {
                                    fallbackFace = extFace;
                                    fallbackLabelSuffix = "fallback-extension";
                                    gotAnyResult = true;
                                    log("[RollServer] Extension wait: advert data arrived, using face=" +
                                        std::to_string(fallbackFace));
                                    break;
                                }
                            }
                        }

                        if (!running_)
                        {
                            pendingRequest_.active = false;
                            return "{\"error\": \"server stopping\"}";
                        }

                        // Session 11: extension yielded no advert data. Before falling back to
                        // the stale P4 currentFace, lift the reconnect suspension so the
                        // watchdog can immediately reconnect the missing die. A successful
                        // GATT reconnect triggers checkMissedRoll, which calls
                        // onRoll(missingDieLabel, settledFace) → rollCv_.notify_all(). This
                        // converts the wrong stale-face answer into the correct settled face
                        // in cases where Windows BLE advert suppression outlasts the
                        // 15s + 8s = 23s P1/P2/P3/extension budget.
                        if (fallbackFace == 0 && !gotSecond && missingDieWasRolling)
                        {
                            log("[RollServer] Extension yielded no advert for " + missingDieLabel +
                                "; lifting suspension to allow GATT reconnect + checkMissedRoll (up to 10s)");

                            if (reconnectSuspender_)
                            {
                                lock.unlock();
                                // Pass 0s: reconnectSuspendedUntil_ = now + 0 = now, which the
                                // watchdog interprets as "no longer suspended" on its next tick.
                                // immediateReconnectRequested_ was already set on disconnect, so
                                // the watchdog fires immediately.
                                reconnectSuspender_(std::chrono::seconds(0));
                                lock.lock();
                            }

                            static constexpr auto kGattReconnectWindow = std::chrono::seconds(10);
                            const auto reconnectDeadline =
                                std::chrono::steady_clock::now() + kGattReconnectWindow;

                            while (running_ && !gotSecond && fallbackFace == 0 &&
                                   std::chrono::steady_clock::now() < reconnectDeadline)
                            {
                                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    reconnectDeadline - std::chrono::steady_clock::now());
                                const auto slice = std::min(remaining, std::chrono::milliseconds(500));

                                gotSecond = rollCv_.wait_for(lock, slice, [this]()
                                {
                                    return pendingRequest_.collectedRolls.size() >= 2 || !running_;
                                });

                                if (gotSecond || !running_) break;

                                // Snapshot poll: die may have reconnected and updated currentFace
                                // without firing onRoll (edge case). Accept if face changed from
                                // the stale value captured at disconnect.
                                if (snapshotProvider_ && !missingDieLabel.empty())
                                {
                                    lock.unlock();
                                    auto snaps = snapshotProvider_();
                                    lock.lock();

                                    if (pendingRequest_.collectedRolls.size() >= 2)
                                    {
                                        gotSecond = true;
                                        break;
                                    }

                                    for (const auto& s : snaps)
                                    {
                                        if (s.label != missingDieLabel) continue;
                                        if (s.status != Systemic::Pixels::PixelStatus::Ready) continue;
                                        if (s.currentFace > 0 && s.currentFace != missingDieFace)
                                        {
                                            fallbackFace = s.currentFace;
                                            fallbackLabelSuffix = "fallback-reconnect";
                                            log("[RollServer] GATT reconnect: " + missingDieLabel +
                                                " reconnected, face=" + std::to_string(fallbackFace) +
                                                " (was stale " + std::to_string(missingDieFace) + ")");
                                        }
                                        break;
                                    }
                                }
                            }

                            if (!running_)
                            {
                                pendingRequest_.active = false;
                                return "{\"error\": \"server stopping\"}";
                            }
                        }

                        // Extension + GATT reconnect both yielded nothing — true last resort:
                        // stale currentFace.
                        if (fallbackFace == 0 && !gotSecond && missingDieFace > 0)
                        {
                            fallbackFace = missingDieFace;
                            log("[RollServer] Fallback (P4-currentFace): using " + missingDieLabel +
                                " currentFace=" + std::to_string(fallbackFace) +
                                " (uncertain — was rolling at disconnect, face may be stale; "
                                "extension wait and GATT reconnect both yielded no data)");
                        }
                    }
                }

                if (gotSecond)
                {
                    // Roll arrived via rollCv_ during extension; nothing to push.
                }
                else if (fallbackFace > 0)
                {
                    pendingRequest_.collectedRolls.push_back(fallbackFace);
                    pendingRequest_.collectedLabels.push_back(fallbackLabelSuffix);
                }
                else
                {
                    log("[RollServer] Fallback failed — no face available for missing die");
                    pendingRequest_.active = false;
                    return "{\"error\": \"timeout\"}";
                }
            }
        }
    }

    // Build response
    int die1 = 0, die2 = 0;
    {
        std::lock_guard<std::mutex> lock(rollMutex_);
        if (!pendingRequest_.collectedRolls.empty())
            die1 = pendingRequest_.collectedRolls[0];
        if (pendingRequest_.collectedRolls.size() >= 2)
            die2 = pendingRequest_.collectedRolls[1];
        pendingRequest_.active = false;
    }

    std::ostringstream oss;
    oss << "{\"mode\": \"" << mode << "\", \"generation\": " << generation
        << ", \"die1\": " << die1;
    if (rollsNeeded == 2)
    {
        oss << ", \"die2\": " << die2;
    }
    oss << "}";

    return oss.str();
}

void RollServer::showRollRegisteredPopup()
{
    log("[RollServer] First roll registered, showing notification for second roll");

    // Show a non-intrusive tray balloon notification that doesn't steal focus.
    // Create a temporary hidden NOTIFYICONDATA just for the balloon.
    std::thread([]()
    {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = nullptr;  // no window needed for balloon-only
        nid.uID = 9999;      // unique ID for this temp icon
        nid.uFlags = NIF_INFO | NIF_ICON;
        nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
        nid.hIcon = LoadIcon(nullptr, IDI_INFORMATION);
        wcscpy_s(nid.szInfoTitle, L"Pixels Dice");
        wcscpy_s(nid.szInfo, L"First roll registered!\nRoll again for the second die.");
        nid.uTimeout = 3000;

        Shell_NotifyIconW(NIM_ADD, &nid);
        Shell_NotifyIconW(NIM_MODIFY, &nid);

        Sleep(4000);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();
}

void RollServer::handleReadyCommand()
{
    log("[RollServer] Ready command received, looking for BG3 window...");

    HWND bg3Window = FindBG3Window();
    if (!bg3Window)
    {
        log("[RollServer] BG3 window not found");
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground != bg3Window)
    {
        log("[RollServer] BG3 not in foreground, skipping click");
        return;
    }

    RECT rect;
    GetWindowRect(bg3Window, &rect);
    int cx = rect.left + (rect.right - rect.left) / 2;
    int cy = rect.top + (rect.bottom - rect.top) / 2;
    log("[RollServer] Clicking BG3 window center at (" + std::to_string(cx) + ", " + std::to_string(cy) +
        ") window=(" + std::to_string(rect.left) + "," + std::to_string(rect.top) + ")-(" +
        std::to_string(rect.right) + "," + std::to_string(rect.bottom) + ")");
    // When using a controller (e.g. DualSense), BG3 is in controller mode
    // and ignores mouse clicks until it detects real mouse activity via raw
    // input. Move the cursor over the dice using SendInput (which generates
    // raw-input events) to trigger the controller->mouse mode switch AND
    // register the hover, then click once.
    POINT pt{};
    if (!GetClientPointOnScreen(bg3Window, 0.50f, 0.43f, pt)) return;

    SetForegroundWindow(bg3Window);
    Sleep(100);
    MoveMouseAbsolute(pt.x, pt.y);   // raw-input move → triggers mode switch
    Sleep(1000);                       // let BG3 switch to mouse mode + hover
    LeftClickScreenPoint(pt);          // single click on the dice
}

void RollServer::log(const std::string& message) const
{
    if (logger_)
    {
        logger_(message);
    }
}

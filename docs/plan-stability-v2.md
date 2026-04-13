# Pixels Windows Stability Plan v2 — Deep Dive Findings & Implementation

## Context

This plan supersedes the high-level gap analysis in `plan-revised.md` with **concrete findings
from a deep-dive into the Android app (`pixels-js`) and dice firmware (`DiceFirmware`) repositories**,
cross-referenced against our current codebase on `polishing-tray-app`.

The analysis was done on the following source material:

- **Android app**: `PixelsCentral.ts`, `PixelScheduler.ts`, `ScanRequester.ts`, `PixelConnect.ts`,
  `Pixel.ts`, `Central.ts`
- **Dice firmware**: `bluetooth_stack.cpp`, `bluetooth_message_service.cpp`
- **Windows app**: `Peripheral.cpp`, `Peripheral.h`, `Pixel.cpp`, `DieConnection.cpp/.h`,
  `DiceManager.cpp/.h`

---

## Root Cause Analysis

### The Stale Connection Problem

The most frequently observed instability pattern is:

1. Die is connected and working (getting RSSI, roll events, etc.)
2. Messages silently stop flowing
3. After 20 seconds our stale detection kicks in
4. Full reconnect succeeds
5. Cycle repeats

### Why This Happens: `MaintainConnection` Silently Loses GATT Subscriptions

The root cause lives in our BLE library layer (`Peripheral.cpp`).

When `MaintainConnection=true` on the `GattSession` and a **link loss** occurs (brief radio
interference, die momentarily out of range, etc.):

```
Peripheral::onDeviceConnectionStatusChanged()
  → device is Disconnected
  → calls internalDisconnect(LinkLoss, fromDevice=true)

Peripheral::internalDisconnect():
  → sees fromDevice=true && session.MaintainConnection()=true
  → returns early WITHOUT cleaning up device/session/services
  → NO Disconnected event is emitted to the Pixel layer
```

The WinRT `GattSession` then attempts an **automatic transport-level reconnect**. When it succeeds:

```
Peripheral::onDeviceConnectionStatusChanged()
  → device is Connected
  → else branch: does nothing (comment says "handled in connectAsync")
```

**The problem**: After a transport-level reconnect, GATT characteristic notification subscriptions
may not survive. But our app never knows — it still thinks the die is `Ready`. No messages flow,
and after 20 seconds we detect staleness.

**The Android app never has this problem** because it **never relies on auto-reconnect**. It always
detects the disconnect event explicitly and performs a full teardown → reconnect → rediscover
services → resubscribe cycle.

### Firmware Context

From `bluetooth_stack.cpp`:

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `CONN_SUP_TIMEOUT` | 3000ms | If central doesn't communicate for 3s, firmware drops connection |
| `RSSI_NOTIFY_MIN_INTERVAL` | 1000ms | Firmware throttles RSSI updates to 1/second |
| `MIN_CONN_INTERVAL` | 15ms | Minimum connection interval |
| `MAX_CONN_INTERVAL` | 30ms | Maximum connection interval |
| `disconnect_on_fail` | true | Firmware disconnects if connection param negotiation fails |
| `MAX_CONN_PARAMS_UPDATE_COUNT` | 3 | Max renegotiation attempts before disconnect |

The 3-second supervision timeout means any radio interference lasting >3s triggers a firmware-side
disconnect. This is normal BLE behavior — the question is how cleanly we recover.

---

## What the Android App Does That We Don't

### 1. Event-Driven Reconnection

**Android** (`PixelsCentral._onConnectionStatus`):
```typescript
if (isConnected(lastStatus) && !isConnected(status)) {
    this._scheduleConnectIfQueued(pixelId, data, reconnectionDelay.short); // 1s delay
}
```

When a connected die transitions to disconnected, Android **immediately** schedules a reconnect
with a 1-second delay through the operation scheduler.

**Our app** (`DieConnection::Delegate::onStatusChanged`):
```cpp
void onStatusChanged(std::shared_ptr<Pixel>, PixelStatus status) override {
    owner_->log("Status changed to " + toStatusString(status));
    owner_->markAnyMessage();
}
```

We only log and update the message timestamp. Disconnects are detected by the **watchdog polling
every 3 seconds**, meaning up to 3+ seconds of latency before we even notice, plus backoff delay.

### 2. Serialized Per-Die Operation Queue

**Android** (`PixelScheduler`):
- Infinite async loop processes one operation at a time per die
- Priority ordering: connect > updateFirmware > resetSettings > rename > blink > programProfile > turnOff > disconnect
- All BLE operations go through the queue — no direct calls
- Cancel support for in-progress connect operations

**Our app**:
- Watchdog thread (3s interval) and poller thread (5s interval) both detect issues independently
- Both can trigger reconnects simultaneously (mutex protects state, but not operation sequencing)
- `startConnectThread()`, `reconnectPixel()`, `reconstructiveReconnect()` can race

### 3. Recovery Scanning

**Android** (`PixelsCentral._scheduleConnectOrScan`):
- If a Pixel object doesn't exist for a die, starts scanning to discover it
- `waitForScannedPixelAsync()` with 20s timeout
- On scan hit, immediately schedules connection
- `ScanRequester` handles Android's 5-scans-per-30-seconds BLE limitation with delayed stops

**Our app** (`DiceManager`):
- Scanner stops after selecting all configured dice and **never restarts**
- If a die reboots or BLE address changes, we have no way to re-discover it
- The stored `lastScannedPixel_` data may become stale

### 4. Connection Limit / GATT Error Handling

**Android** (`PixelsCentral._onConnectOperationFailed`):
- Tracks `lastConnectError` per die (timestamp + error type)
- If 2 GATT errors occur within 30 seconds → assumes BLE adapter connection limit reached
- Disconnects a lower-priority die to free a connection slot
- Uses high/low priority queue for connection ordering

**Our app**:
- No GATT error classification or tracking
- No connection limit awareness
- No ability to disconnect one die to reconnect another

### 5. Timing Constants Comparison

| Concept | Android | Windows (Ours) |
|---------|---------|----------------|
| Reconnect delay (short) | 1s | 2s base + exponential backoff |
| Reconnect delay (long) | 30s | 60s max backoff |
| Stale/keepalive timeout | 8s (`keepAliveDuration`) | 20s (`kStaleTimeoutSeconds`) |
| Grace period after connect | 5s (`connectionRelease.gracePeriod`) | 10s (`kStaleGracePeriodSeconds`) |
| Connection release interval | 7s | N/A |
| Watchdog poll interval | Event-driven | 3s poll |
| Stale check interval | Event-driven | 5s poll |
| Scan stop delay | 9s | Immediate |

---

## Implementation Plan

### Priority 1 — Stop Relying on `MaintainConnection` for Auto-Reconnect

**Impact**: Critical — this is the most likely root cause of stale connections.

**Current behavior**: We pass `maintainConnection=true` to `Pixel::connectAsync()`, which sets
`GattSession.MaintainConnection(true)`. When link loss occurs, the WinRT stack silently reconnects
at the transport level but GATT subscriptions are lost.

**Target behavior**: Always detect disconnects explicitly. Perform a full disconnect → reconnect →
rediscover → resubscribe cycle (matching Android).

**Implementation steps**:

1. **Change `connectAndInitialize()` to pass `maintainConnection=false`**:
   ```cpp
   // DieConnection.cpp, connectAndInitialize()
   auto result = co_await pixel->connectAsync(false);  // was: true
   ```
   This ensures that when a link loss occurs, the Peripheral layer fires a proper `Disconnected`
   event (with `Timeout` reason) instead of silently swallowing it.

2. **Verify Peripheral behavior with `maintainConnection=false`**:
   In `Peripheral::onDeviceConnectionStatusChanged`, when `MaintainConnection` is false, a device
   disconnect fires `internalDisconnect(Timeout, true)` which:
   - Increments `_connectCounter` (cancels in-progress operations)
   - Queues `Disconnected` event with reason
   - Cleans up device/session/services
   - Notifies user code

   This means the `Pixel` layer will transition to `PixelStatus::Disconnected`, which our watchdog
   already handles. But we should also add event-driven detection (see Priority 2).

3. **No changes needed to `Peripheral.cpp` or `Pixel.cpp`** — the existing code already handles
   `maintainConnection=false` correctly. We just need to stop passing `true`.

**Risk**: Slightly slower reconnect (we no longer get "free" transport-level auto-reconnects).
Mitigated by Priority 2 (event-driven reconnection).

**Test criteria**: After this change, link loss should produce a visible `Disconnected` status
change in logs, followed by our reconnect logic. No more "silent stale" pattern.

---

### Priority 2 — Event-Driven Disconnect Detection

**Impact**: High — reduces reconnect latency from 3-5 seconds (poll) to ~1 second.

**Current behavior**: `DieConnection::Delegate::onStatusChanged` only logs. Disconnects are
detected by the watchdog polling every 3 seconds.

**Target behavior**: When the delegate receives `PixelStatus::Disconnected` while we're in
`ConnectionState::Ready`, immediately schedule a reconnect (matching Android's 1-second delay).

**Implementation steps**:

1. **Add a `scheduleReconnect()` method to `DieConnection`**:
   ```cpp
   void DieConnection::scheduleReconnect()
   {
       // Called from delegate callback (potentially on BLE thread), so
       // we must not block. Spawn a reconnect on a background thread.
       std::lock_guard<std::mutex> lock(mutex_);
       if (shuttingDown_ ||
           connectionState_ == ConnectionState::Connecting ||
           connectionState_ == ConnectionState::Reconnecting ||
           connectionState_ == ConnectionState::Shutdown ||
           connectionState_ == ConnectionState::Unselected)
       {
           return;
       }

       const auto now = std::chrono::steady_clock::now();
       const auto connectAge = std::chrono::duration_cast<std::chrono::seconds>(
           now - lastConnectAttempt_).count();

       int backoffSeconds = std::min(kMaxBackoffSeconds,
           kBaseBackoffSeconds * (1 << consecutiveFailures_));

       if (connectAge < backoffSeconds)
       {
           return; // Too soon, let watchdog handle it
       }

       setConnectionState(ConnectionState::Reconnecting);
       lastConnectAttempt_ = now;

       // Spawn reconnect thread (non-blocking)
       if (connectThread_.joinable()) connectThread_.join();
       connectThread_ = std::thread([this]() {
           if (consecutiveFailures_ >= 2)
               reconstructiveReconnect();
           else
               reconnectPixel();
       });
   }
   ```

2. **Update `Delegate::onStatusChanged` to trigger reconnect**:
   ```cpp
   void onStatusChanged(std::shared_ptr<Pixel>, PixelStatus status) override {
       owner_->log("Status changed to " + toStatusString(status));
       owner_->markAnyMessage();
       if (status == PixelStatus::Disconnected) {
           owner_->scheduleReconnect();
       }
   }
   ```

3. **Update `tickWatchdog()` to serve as a fallback only**:
   The watchdog still runs as a safety net for cases where the event-driven path misses
   something, but the primary reconnect trigger is now the delegate callback.

**Risk**: The delegate callback runs on the BLE thread. Must not block or do heavy work
directly — spawning a thread is fine (as we already do with `startConnectThread()`).

**Test criteria**: Logs should show "Status changed to disconnected" followed almost immediately
by a reconnect attempt, without waiting for the next watchdog poll.

---

### Priority 3 — Recovery Scanning

**Impact**: High — handles cases where the die has rebooted, been firmware-updated, or gone
out of range and returned.

**Current behavior**: `DiceManager` stops scanning after selecting all configured dice. If a die
becomes undiscoverable (e.g., after reboot), we can only reconnect using the stored
`lastScannedPixel_` data, which may be stale (especially the BLE address, though Pixels dice
use static addresses).

**Target behavior**: When a die has been disconnected for a configurable duration (e.g., 30
seconds), restart the scanner to re-discover it. When found, update `lastScannedPixel_` and
attempt a fresh reconstructive reconnect.

**Implementation steps**:

1. **Add `requestRescan()` method to `DiceManager`**:
   ```cpp
   void DiceManager::requestRescan(uint32_t pixelId)
   {
       std::lock_guard<std::mutex> lock(scannerMutex_);
       if (!scanner_) {
           startScanner();
       }
       // Scanner will call trySelectScannedPixel which updates lastScannedPixel_
   }
   ```

2. **Add rescan trigger to `DieConnection`**:
   Add a `RescanRequester` callback (similar to the existing `Logger` and `StateObserver`):
   ```cpp
   using RescanRequester = std::function<void(uint32_t pixelId)>;
   ```

3. **Trigger rescan after prolonged disconnection**:
   In `tickWatchdog()`, if the die has been disconnected for >30 seconds and the last
   reconstructive reconnect also failed, request a rescan:
   ```cpp
   if (consecutiveFailures_ >= 3 && connectAge >= 30) {
       log("[watchdog] Requesting rescan after prolonged disconnection");
       rescanRequester_(targetPixelId_);
   }
   ```

4. **Update `trySelectScannedPixel` to handle re-selection**:
   Currently it returns `false` if already connecting/reconnecting. Add a path for updating
   the `lastScannedPixel_` even when the die is already selected but disconnected, so a
   reconstructive reconnect uses fresh scan data.

5. **Auto-stop scanner after rescan window**:
   Start a timer (e.g., 20 seconds) after requesting a rescan. If the die is found and
   reconnected, stop immediately. Otherwise stop after timeout to conserve resources.

**Risk**: Must not start/stop scanner too frequently. Windows BLE doesn't have the same
5-scans-per-30-seconds limit as Android, but excessive scanning wastes battery and CPU.

**Test criteria**: After multiple failed reconnects, logs should show a rescan being
requested. When the die is in range, it should be discovered and a fresh connection made.

---

### Priority 4 — Serialize BLE Operations Per Die

**Impact**: Medium — prevents race conditions between watchdog, poller, and event-driven
reconnects.

**Current behavior**: Watchdog (3s), poller (5s), and event-driven reconnects can all trigger
`reconnectPixel()` or `reconstructiveReconnect()` concurrently. The mutex protects shared state,
but multiple BLE operations can be in-flight simultaneously for the same die.

**Target behavior**: Only one BLE operation (connect, disconnect, reconnect) can be in progress
per die at any time. Additional requests are queued or skipped.

**Implementation steps**:

1. **Add an `std::atomic<bool> operationInProgress_` flag to `DieConnection`**:
   ```cpp
   std::atomic<bool> operationInProgress_{false};
   ```

2. **Guard all reconnect paths with the flag**:
   ```cpp
   bool DieConnection::reconnectPixel() {
       if (operationInProgress_.exchange(true)) {
           log("[reconnect] Operation already in progress, skipping");
           return false;
       }
       // ... existing logic ...
       operationInProgress_ = false;
       return ok;
   }
   ```

3. **Apply the same guard to `reconstructiveReconnect()` and `startConnectThread()`**.

4. **Future enhancement**: Replace the boolean flag with a proper operation queue if we ever
   need to support queued operations (blink, rename, profile programming). For now, a simple
   guard is sufficient since we only do connect/disconnect/reconnect.

**Risk**: Minimal. The existing mutex already prevents most races. This adds an explicit
"one operation at a time" semantic.

**Test criteria**: Logs should never show overlapping reconnect attempts. If the watchdog and
poller both detect an issue simultaneously, only one reconnect should proceed.

---

### Priority 5 — GATT Error Tracking and Connection Limit Handling

**Impact**: Lower (for single die) / Medium (for multi-die) — handles edge cases where
the Windows BLE adapter hits its connection limit.

**Current behavior**: No error classification. All connection failures increment
`consecutiveFailures_` uniformly.

**Target behavior**: Track GATT errors specifically. When two GATT errors occur within a
short window, assume the BLE adapter connection limit is reached.

**Implementation**: Deferred until multi-die scenarios are more common. The current exponential
backoff and reconstructive reconnect handle most single-die cases adequately.

**Outline for future implementation**:
- Add `lastConnectError_` tracking (timestamp + error type) to `DieConnection`
- Classify `ConnectResult` values into GATT errors vs other errors
- If 2 GATT errors in <30s, request `DiceManager` to temporarily disconnect another die
- Re-queue the failed connection attempt

---

## Summary Table

| # | Fix | Impact | Complexity | Files Changed |
|---|-----|--------|------------|---------------|
| 1 | Stop using `MaintainConnection` | Critical | Low | `DieConnection.cpp` (1 line) |
| 2 | Event-driven disconnect detection | High | Medium | `DieConnection.cpp`, `DieConnection.h` |
| 3 | Recovery scanning | High | Medium | `DiceManager.cpp/.h`, `DieConnection.cpp/.h` |
| 4 | Serialize BLE operations | Medium | Low | `DieConnection.cpp`, `DieConnection.h` |
| 5 | GATT error tracking | Lower | Medium | Deferred |

---

## Implementation Order

The fixes are designed to be **incremental and independently testable**:

1. **Priority 1** alone should dramatically reduce stale connections
2. **Priority 2** builds on Priority 1 — makes reconnects faster
3. **Priority 3** is independent — adds resilience for prolonged outages
4. **Priority 4** is a safety improvement — prevents edge-case races
5. **Priority 5** is deferred until multi-die testing reveals a need

After each priority, a test cycle with debug logging should confirm the improvement before
moving to the next.

---

## Appendix: Key Android Source References

| File | What it does |
|------|-------------|
| `PixelsCentral.ts` | Central orchestrator: scanning, connection queue, reconnect scheduling, error handling |
| `PixelScheduler.ts` | Per-die operation serializer: priority queue, async processing loop |
| `ScanRequester.ts` | Managed scanning: reference-counted, delayed stop, BLE rate limiting |
| `PixelConnect.ts` | Base Pixel class: status machine, message serialization, connect/identify/ready flow |
| `Pixel.ts` | Concrete die class: RSSI/battery/roll events, message handling |
| `Central.ts` | Low-level BLE: scan management, peripheral connection, characteristic operations |
| `bluetooth_stack.cpp` | Firmware BLE config: advertising, connection params, supervision timeout, RSSI throttle |
| `bluetooth_message_service.cpp` | Firmware messaging: GATT service, TX/RX characteristics, message queuing |

## Appendix: Key Firmware Constants

| Constant | Value | Relevance |
|----------|-------|-----------|
| `CONN_SUP_TIMEOUT` | 3000ms | Radio silence >3s = firmware drops connection |
| `RSSI_NOTIFY_MIN_INTERVAL` | 1000ms | Max 1 RSSI update/second from firmware |
| `APP_ADV_INTERVAL` | 187.5ms | How often die advertises when not connected |
| `MIN_CONN_INTERVAL` | 15ms | Fastest possible data exchange rate |
| `MAX_CONN_INTERVAL` | 30ms | Slowest data exchange rate (still fast) |
| `FIRST_CONN_PARAMS_UPDATE_DELAY` | 5000ms | Firmware requests param update 5s after connect |
| `NEXT_CONN_PARAMS_UPDATE_DELAY` | 30000ms | Subsequent param update attempts every 30s |
| `MAX_CONN_PARAMS_UPDATE_COUNT` | 3 | Disconnect if param negotiation fails 3 times |
| `disconnect_on_fail` | true | Failed param negotiation = forced disconnect |

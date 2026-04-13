# Advertisement-Based Missed Roll Recovery

## Problem

When a die disconnects during or before a roll, the current system must complete a full
GATT reconnection before it can detect the roll result via `checkMissedRoll()`. In practice
this means roll results can be delayed **10–60+ seconds** while reconnection attempts fail
and retry.

Example from logs: Die 2 disconnects with `rollState=1` (at rest), face=5. During a ~60s
reconnection struggle, it's rolled and lands on face 8. The app can't report that roll
until reconnection finally succeeds.

## Key Insight

Once a die disconnects, its firmware **immediately restarts BLE advertising**. The
advertisement packets include `rollState` and `currentFace` — the exact data needed to
detect a missed roll. We can read these from the advertisement without needing a full
GATT connection.

Confirmed in firmware (`bluetooth_stack.cpp`):
- `BLE_GAP_EVT_CONNECTED` → `CustomAdvertisingDataHandler::stop()`
- `BLE_GAP_EVT_DISCONNECTED` → advertising restarts automatically via Nordic SDK

## Architecture

### New Method: `DieConnection::processAdvertisement()`

A new public method on `DieConnection` that receives scan results while the die is
disconnected and uses them to detect missed rolls from advertisement data alone.

### Flow

```
Die Connected (Ready)
        |
    [Disconnect event]
        |
        v
onPixelDisconnected() saves faceBeforeDisconnect_, rollStateBeforeDisconnect_
        |
        v
State = Selected, reconnection begins
        |
        +--> DiceManager starts/ensures scanner is running
        |
        +--> Scanner receives advertisement packets
        |       |
        |       v
        |    processAdvertisement(scannedPixel)
        |       |
        |       +--> Is rollState == OnFace (settled)?
        |       |       |
        |       |     Yes: Was it rolling before, or face changed?
        |       |       |
        |       |     Yes: --> markRollResult(face) <-- INSTANT RECOVERY
        |       |              Clear faceBeforeDisconnect_ so
        |       |              checkMissedRoll() won't double-count
        |       |
        |       +--> Is rollState == Rolling/Handling?
        |               |
        |             Yes: Update tracking state
        |                  (die is still in motion, keep listening)
        |
        v
    [Reconnection succeeds]
        |
        v
checkMissedRoll() — already has the roll, skips (faceBeforeDisconnect_ == 0)
```

### Changes by File

#### `DieConnection.h`

```cpp
// New public method
void processAdvertisement(
    const std::shared_ptr<const Systemic::Pixels::ScannedPixel>& scannedPixel);

// New private member — tracks whether we already recovered a roll via advertisement
bool missedRollRecoveredViaAdvert_ = false;
```

#### `DieConnection.cpp` — New method `processAdvertisement()`

Called by DiceManager's scanner listener when a disconnected die's advertisement
is received:

```cpp
void DieConnection::processAdvertisement(const ScannedPixel& scannedPixel)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Only process while disconnected and waiting for reconnect
    if (connectionState_ != ConnectionState::Selected &&
        connectionState_ != ConnectionState::Reconnecting)
        return;

    // Already recovered the roll via advert — nothing more to do
    if (faceBeforeDisconnect_ == 0)
        return;

    const int advFace = scannedPixel.currentFace();
    const auto advRollState = scannedPixel.rollState();

    // Die still rolling — not settled yet, keep waiting
    if (advRollState == PixelRollState::Rolling ||
        advRollState == PixelRollState::Handling)
        return;

    // Die is settled (OnFace or Crooked). Check if this is a missed roll.
    const bool wasRolling =
        (rollStateBeforeDisconnect_ == PixelRollState::Rolling ||
         rollStateBeforeDisconnect_ == PixelRollState::Handling);

    if (wasRolling || advFace != faceBeforeDisconnect_)
    {
        // Report the roll immediately from advertisement data
        log("[advert-recovery] Roll detected via advertisement: face=" +
            std::to_string(advFace) +
            " (was " + std::to_string(faceBeforeDisconnect_) + ")");

        // Clear saved state so checkMissedRoll() won't double-count
        faceBeforeDisconnect_ = 0;
        rollStateBeforeDisconnect_ = PixelRollState::Unknown;

        // markRollResult outside the lock (it acquires mutex_ internally)
    }
}
```

#### `DiceManager.cpp` — Scanner listener modification

The existing scanner listener in `startScanner()` already iterates over all dice.
Add a call to `processAdvertisement()` for all known dice:

```cpp
// In startScanner() listener lambda:
for (auto& die : dice_)
{
    // Existing: try to select new dice
    if (die->trySelectScannedPixel(scannedPixel))
    {
        selectedAny = true;
    }

    // NEW: feed advertisement data to disconnected dice for fast roll recovery
    die->processAdvertisement(scannedPixel);
}
```

#### `DiceManager.cpp` — Ensure scanner runs during reconnection

Currently the scanner only runs during initial discovery and recovery scans.
We need it running whenever any die is disconnected. Options:

- In the maintenance loop, if any die is in `Selected` state, ensure scanner is running
- Lightweight: piggyback on existing `checkRecoveryScan()` by starting the scanner
  immediately (at failure count 0) when any die is disconnected

### Timing Analysis

| Metric                          | Value           |
|---------------------------------|-----------------|
| Advertisement interval (fast)   | ~100-200ms      |
| Time to first advert after disc | < 1 second      |
| Time to detect settled roll     | < 1s after rest |
| **Current recovery time**       | **10-60+ sec**  |
| **New recovery time**           | **< 1 second**  |

### Edge Cases

| Case | Handling |
|------|----------|
| Die rolled and settled before we start scanning | First advert has `OnFace` + new face → immediate detection |
| Die still rolling when we start scanning | `rollState=Rolling` → keep listening → detect when it settles |
| Die not rolled (just disconnected) | Face unchanged + was `OnFace` → no action |
| Roll recovered via advert, then GATT reconnect completes | `faceBeforeDisconnect_ == 0` → `checkMissedRoll()` skips → no double-count |
| Scanner not running when disconnect happens | DiceManager starts scanner on disconnect |
| Multiple disconnects while scanning | Each die tracked independently via its own `faceBeforeDisconnect_` |

### What Does NOT Change

- **Reconnection logic** — completely untouched, runs in parallel
- **`checkMissedRoll()`** — still works as fallback if advert recovery didn't fire
- **`markRollResult()`** — same function, just called earlier
- **UI/snapshot** — no changes needed, `markRollResult()` already calls `notifyStateChanged()`

### Risk Assessment

- **Low risk**: Only reading advertisement data, not interfering with reconnection
- **Scanner resource**: `BluetoothLEAdvertisementWatcher` is lightweight and independent
  from GATT operations
- **Concurrent operation**: Scanner + GATT connect use different WinRT code paths
  (advertisement vs connection). Already proven to work during recovery scans.

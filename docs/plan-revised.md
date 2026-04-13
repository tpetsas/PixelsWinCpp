# Pixels Windows Stability Plan — `tpetsas/PixelsWinCpp@polishing-tray-app`

## Scope

This document revises the original Android-to-Windows migration plan so it applies specifically to:

- repository: `tpetsas/PixelsWinCpp`
- branch: `polishing-tray-app`
- target app: the tray app and its runtime model

It is a **gap analysis** against the Android-inspired plan, not a greenfield proposal.

---

## Executive summary

The tray-app branch is **already significantly ahead** of upstream `PixelsWinCpp`.

Compared with the original plan, the branch already has these major pieces in place:

1. `maintainConnection` has been exposed through `Pixel::connectAsync(bool maintainConnection)` and is used by the tray app.
2. A real multi-die manager exists (`DiceManager`) instead of a single one-off connection flow.
3. There is already a watchdog loop for reconnecting explicitly disconnected dice.
4. There is already a poller loop for detecting stale sessions.
5. The tray app already uses RSSI reporting and message callbacks as liveness signals.
6. Dice are already remembered through `pixels.cfg` and auto-connected at runtime.

So the branch is **not missing the whole plan**. It already implements a meaningful native equivalent of the Foundry “heartbeat reconnect” idea and some early Android-style orchestration.

The biggest remaining gaps are:

- reconnect currently reuses the same `Pixel` object instead of rebuilding it from scratch,
- there is no true serialized operation scheduler like Android’s `PixelScheduler`,
- scan/connect coordination is still simpler than Android’s `PixelsCentral`,
- stale detection is good but still heuristic-heavy,
- there is no single explicit state machine layer that unifies all reconnect logic.

---

## Reference architecture target

The Android app is stable mainly because it has:

- a central manager (`PixelsCentral`),
- explicit reconnect timing and backoff,
- scan coordination,
- serialized per-die operations,
- remembered dice and startup reconnect,
- strong separation between “transport discovered” and “functionally ready”.

The Windows tray branch already implements **parts** of this architecture, but not all of it.

The goal is therefore:

> Evolve the current tray-app branch from a good watchdog-based implementation into a Windows-native equivalent of Android’s orchestration model.

---

## Current branch assessment

## 1. Library-level connection support

### Status: **implemented**

The original plan called for exposing the lower BLE layer’s `maintainConnection` flag through `Pixel::connectAsync`.

This is already done in the branch.

### Evidence in branch

- `Pixel::connectAsync(bool maintainConnection)` exists.
- It forwards `maintainConnection` into `_peripheral->connectAsync({ PixelBleUuids::service }, maintainConnection)`.
- The lower BLE layer still treats `GattSession.MaintainConnection()` specially when link loss occurs.

### Conclusion

This is one of the most important upstream library changes, and it is already complete in the branch.

### Recommendation

Keep this behavior. Do not regress it.

---

## 2. Tray-app connection orchestration layer

### Status: **implemented, but simpler than Android**

The original plan called for a Windows-side central manager. The branch already has a real manager in `DiceManager`.

### Evidence in branch

- `DiceManager` owns multiple `DieConnection` objects.
- It starts and stops a scanner.
- It runs dedicated background threads:
  - maintenance thread,
  - poller thread,
  - watchdog thread.
- It loads configured dice and connects to them automatically in normal runtime.

### What this means

This is already much closer to Android’s `PixelsCentral` than upstream `PixelsWinCpp`.

### Gap versus Android

Android’s `PixelsCentral` is more advanced because it also handles:

- priority queueing,
- richer reconnect scheduling,
- scan requester lifecycle,
- connection release / capacity management,
- long-lived scan coordination.

### Recommendation

Treat `DiceManager` as the seed of `PixelsCentralWin`, not as something to replace.

---

## 3. Dedicated connection thread per die

### Status: **implemented**

The original plan called for avoiding long BLE work inside scan callbacks and keeping connect work isolated.

### Evidence in branch

- `DieConnection::trySelectScannedPixel(...)` creates the `Pixel` and then starts connection on a dedicated thread.
- The code explicitly comments that connection runs on a dedicated thread so scanner callbacks stay fast.

### Conclusion

This is a solid design choice and aligns with the Android idea of not letting discovery and connect logic race on the same execution path.

### Recommendation

Keep it.

---

## 4. Explicit watchdog reconnect for disconnected state

### Status: **implemented**

The original plan called for a watchdog that treats transport disconnect as recoverable and reconnects automatically.

### Evidence in branch

`DieConnection::tickWatchdog()`:

- checks `pixel_->status()`,
- looks for `PixelStatus::Disconnected`,
- enforces a minimum delay since the previous attempt,
- then calls `reconnectPixel()`.

### Current behavior

- reconnect attempts happen after about 5 seconds since the last attempt,
- reconnect is not spammed continuously,
- reconnect work is separated from the scan callback path.

### Conclusion

This is already the native equivalent of the coarse reconnect timer idea used by the Foundry fork and the upstream Foundry module.

### Gap versus Android

Android’s reconnect behavior is more policy-driven and integrated with the central queue; Windows is currently more direct and local to each die.

### Recommendation

Keep this, but eventually fold it into a more explicit state machine.

---

## 5. Stale-session detection

### Status: **implemented, but heuristic**

The original plan called for detecting functionally dead sessions, not just explicit disconnects.

The branch already does this.

### Evidence in branch

`DieConnection::tickPoll()`:

- polls current RSSI, face, battery, status, and roll state,
- compares current RSSI / face / battery against previous poll values,
- increments `identicalPollCount_` if data remains identical,
- after 3 identical polls, treats the connection as stale,
- forces reconnection.

### Strengths

- It addresses exactly the failure mode you observed: sessions that still look connected but stop delivering live events.
- It is much better than relying on `status()` alone.

### Weaknesses

- It assumes unchanged data implies stale connection.
- That can be wrong if the die is truly idle and the data naturally does not change.
- It can also miss edge cases where values change but the notification stream is still unhealthy.

### Recommendation

Keep the current heuristic, but improve it with context:

1. **short timeout** when a roll is expected,
2. **longer timeout** when the die is simply idle,
3. optional “fresh object rebuild” after repeated stale detections.

---

## 6. Liveness signals from delegate callbacks

### Status: **implemented**

The original plan called for tracking actual die activity, not only transport state.

### Evidence in branch

The `DieConnection::Delegate` calls `markAnyMessage()` on:

- `onStatusChanged`,
- `onFirmwareDateChanged`,
- `onRssiChanged`,
- `onBatteryLevelChanged`,
- and other message-driven callbacks.

The branch also separately tracks roll activity through roll-related callbacks.

### Conclusion

This is already one of the most valuable parts of the design.

### Recommendation

Preserve this design and make it more central to stale detection.

---

## 7. RSSI reporting as telemetry

### Status: **implemented**

The original plan suggested RSSI as supporting telemetry, not as the only heartbeat.

### Evidence in branch

After a successful connect, `connectAndInitialize(...)`:

- calls `pixel->reportRssiAsync(true)`,
- logs that RSSI reporting is enabled,
- uses those changes as one of the liveness signals.

### Conclusion

This is already exactly the right use of RSSI:

- useful telemetry,
- useful stale-connection hint,
- not the sole liveness mechanism.

### Recommendation

Keep it.

---

## 8. Connect retry during initialization

### Status: **implemented**

The original plan called for not giving up after the first failed connect/setup attempt.

### Evidence in branch

`connectAndInitialize(...)`:

- calls `pixel->connectAsync(true)`,
- if it fails, logs and retries once immediately,
- then enables RSSI reporting on success.

### Conclusion

This is a practical and already useful improvement over the upstream wrapper flow.

### Recommendation

Keep the one immediate retry, but later replace it with a small reconnect policy object so the retry rules are configurable.

---

## 9. Remembered dice / startup auto-connect

### Status: **implemented**

The original plan called for remembered dice and startup reconnect.

### Evidence in branch

The branch supports:

- `--setup` mode,
- persistent `pixels.cfg`,
- normal runtime loading configured dice,
- automatic connection to configured dice.

### Conclusion

This part of the Android-inspired plan already exists in practical form.

### Recommendation

Keep it and extend it with richer per-die metadata only if needed later.

---

## 10. Reconnect implementation strategy

### Status: **partially implemented**

The branch reconnects automatically, but currently reconnects the **existing `Pixel` object**.

### Evidence in branch

`reconnectPixel()`:

- grabs `pixel_`,
- calls `localPixel->disconnect()`,
- then calls `connectAndInitialize(localPixel)`.

### Why this matters

This re-runs functional setup, which is good.

But it does **not** recreate the `Pixel` object from the last known `ScannedPixel` / die identity.

If the `Pixel` instance itself is in a bad state, reusing it may preserve subtle problems.

### Gap versus target

A more Android-like recovery model would be:

1. disconnect,
2. destroy old `Pixel`,
3. rebuild `Pixel` from known die identity,
4. reconnect and reinitialize from scratch.

### Recommendation

This should be the **next highest-priority change**.

#### Proposed rule

- first stale/disconnect recovery: try current reconnect path,
- repeated failure or repeated stale detection: rebuild the `Pixel` object from scratch.

That gives you a low-risk incremental step before making all reconnects reconstructive.

---

## 11. Operation serialization / scheduler

### Status: **missing**

This is the biggest conceptual gap versus Android.

Android uses a per-die scheduler (`PixelScheduler`) to serialize operations such as:

- connect,
- disconnect,
- blink,
- rename,
- turn off,
- profile operations,
- firmware update.

### Current branch state

The tray app already avoids some races by using threads and a per-die object, but there is no explicit operation queue abstraction.

### Why it matters

BLE stacks are sensitive to overlapping work. A scheduler gives you:

- one source of truth for “what this die is doing now,”
- less risk of racing disconnect/reconnect/poll operations,
- a clean place for backoff, cancellation, and timeouts.

### Recommendation

Add a per-die serialized operation queue.

#### Suggested abstraction

```text
PixelOperationScheduler
  - enqueueConnect()
  - enqueueReconnect(reason)
  - enqueueDisconnect()
  - enqueueBlink(...)
  - enqueueTurnOff()
  - enqueueProfileWrite(...)
```

The queue should guarantee one active BLE operation at a time per die.

This is the **most Android-like improvement still missing**.

---

## 12. Unified explicit state machine

### Status: **partially implemented, but implicit**

The branch already has state in practice, but it is spread across:

- `PixelStatus`,
- `connecting_`,
- `connectedOnce_`,
- `selected_`,
- `shuttingDown_`,
- timestamps,
- stale-poll counters.

### Gap

There is no single explicit app-level state enum like:

```text
Unselected
Selected
Connecting
Ready
WaitingForRoll
Stale
Reconnecting
Shutdown
```

### Why it matters

A unified state machine would make:

- logs easier to interpret,
- reconnect policy easier to maintain,
- UI state easier to display in the tray app,
- testing much simpler.

### Recommendation

Add an app-level connection state enum inside `DieConnection` and drive watchdog/poll behavior from it.

This is a strong candidate for the second major refactor after reconstructive reconnect.

---

## 13. Scan coordination / central policy

### Status: **partially implemented**

The branch already has scan-driven die selection and scanner shutdown after all configured dice are selected.

### Strengths

- scanning is coordinated centrally by `DiceManager`,
- scanner stop is deferred safely instead of stopping inside the callback thread,
- configured dice are matched cleanly.

### Gap versus Android

Android’s `PixelsCentral`/`ScanRequester` logic is richer because it also:

- treats scanning as a constrained system resource,
- keeps scan intent separate from scanner lifetime,
- can coordinate reconnects with discovery over longer periods,
- supports more dynamic recovery policy.

### Recommendation

Do **not** rush to copy Android’s scan complexity immediately.

Instead:

1. keep current scan-selection behavior,
2. add optional long-lived low-rate recovery scanning for disconnected dice,
3. only then consider a fuller scan requester abstraction.

This should be a **later-phase** enhancement.

---

## 14. Multi-die capacity / connection pressure policy

### Status: **missing**

Android has explicit logic related to connection limits and scan failures under load.

The branch does not yet appear to have equivalent policy.

### Is this urgent?

Only if you plan to support many simultaneous dice and start seeing:

- scan registration failures,
- connect starvation,
- or adapter instability with multiple dice.

### Recommendation

Defer this unless you actually hit multi-die scale limits.

For 1–2 dice, it is probably not the first bottleneck.

---

## Prioritized implementation roadmap for this branch

## Phase 0 — keep what is already good

Do **not** remove or regress:

- `maintainConnection=true`,
- immediate connect retry,
- RSSI reporting enablement,
- watchdog reconnect,
- stale poll detection,
- message-based activity tracking,
- remembered dice / `pixels.cfg` flow.

---

## Phase 1 — highest-value next changes

### 1. Rebuild `Pixel` on repeated reconnect failure

Add a fallback reconnect path that reconstructs the `Pixel` object instead of reusing it.

#### Suggested behavior

- first recovery: current reconnect path
- second consecutive recovery failure, or second consecutive stale detection: rebuild from scratch

### 2. Add explicit app-level state enum

Introduce a state enum that reflects app-level lifecycle, not just library `PixelStatus`.

### 3. Make stale detection context-aware

Use different thresholds for:

- passive idle monitoring,
- actively waiting for a roll.

---

## Phase 2 — Android-style structure improvements

### 4. Add per-die operation scheduler

Create a small serialized work queue so all per-die BLE work happens through one execution path.

### 5. Move reconnect logic into the scheduler

Make watchdog/poll request reconnect by queueing an operation instead of calling connect/disconnect paths directly.

### 6. Promote `DiceManager` into a clearer central policy layer

Rename or refactor toward a `PixelsCentralWin` style abstraction once scheduler and state-machine pieces exist.

---

## Phase 3 — optional advanced parity work

### 7. Add optional recovery scanning for disconnected dice

Use scanning not only for initial selection, but also to help reacquire disconnected dice after longer outages.

### 8. Add reconnect backoff profiles

Example:

- fast retry for the first failure,
- medium retry after repeated failures,
- slow retry after persistent failure.

### 9. Add richer metrics and diagnostics

Track:

- connect attempts,
- connect success rate,
- stale detections,
- reconnect success rate,
- average ready time,
- average roll-to-event latency.

This will make future tuning much easier.

---

## Recommended concrete next task list

If I were implementing on this branch next, I would do exactly this order:

1. **Add app-level `ConnectionState` enum to `DieConnection`.**
2. **Add reconstructive reconnect fallback** (recreate `Pixel` after repeated failures/stale detections).
3. **Separate “idle stale timeout” from “awaiting roll timeout”.**
4. **Extract connect/reconnect/disconnect into a serialized per-die scheduler.**
5. **Refactor `DiceManager` into a clearer central-policy object once the scheduler exists.**

---

## Final assessment

### Already implemented well

- wrapper `maintainConnection`
- native watchdog reconnect
- stale-session detection
- RSSI telemetry
- connect retry
- remembered dice and startup auto-connect
- per-die dedicated connection thread

### Partially implemented

- central orchestration
- state machine
- scan/connect coordination
- reconnect policy sophistication

### Still missing

- reconstructive reconnect as a first-class strategy
- explicit per-die operation scheduler
- Android-like central queue/policy abstraction
- advanced multi-die capacity handling

---

## Bottom line

The `polishing-tray-app` branch is **already implementing a meaningful subset of the Android-inspired plan**.

It is not a prototype anymore; it already contains real Windows-native resilience work.

The right next move is **not** to rewrite everything in the image of Android.

The right move is to:

1. preserve the good watchdog/poll architecture already present,
2. harden reconnect by rebuilding the `Pixel` object when needed,
3. add an explicit state machine,
4. then introduce an operation scheduler.

That path gives the highest chance of improving stability without destabilizing the branch.

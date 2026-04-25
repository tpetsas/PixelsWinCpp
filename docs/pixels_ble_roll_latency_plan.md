# Pixels Windows BLE Roll-Latency Stabilization Plan

**Date:** 2026-04-24  
**Last updated:** 2026-04-25 (Session 6 — kSecondRollTimeout extended past Windows BLE advert delay)  
**Branch context:** `server-mode` branch, with uploaded `diff.txt` and `pixels_log-big-session.txt.txt`  
**Primary goal:** after the user physically rolls a Pixel die, return the roll result as fast as possible, ideally within a few seconds, even when Windows BLE/GATT is flaky.

---

## Implementation Status

### ✅ Done (commit `fe87353` + follow-up sessions)

- **Fallback priority fix** — `RollServer` fallback now uses a 3-tier priority:
  1. `advertSettledFace` (in-progress advert debounce for current disconnect event)
  2. `recentRollFaces.back()` only if `lastRollAt >= requestStartedAt` (guards against previous-generation faces)
  3. `currentFace` as last resort, logged explicitly as stale
- **`advertSettledFace/Count` exposed in snapshot** — partial advert debounce state flows to `RollServer` so the fallback can use a face seen 1-2 times (not yet at the 3-packet threshold) rather than a stale pre-roll face.
- **Shutdown hang fixed** — `bleOpMutex_` promoted to `std::timed_mutex`; `shutdown()` times out after 3 s and detaches the BLE thread instead of blocking the UI forever.
- ~~**`RequestPreferredConnectionParameters(ThroughputOptimized)`**~~ — **REVERTED**. Caused severe regressions with 2 simultaneous dice: repeated `Connection error: 1` on die 2, multiple 60 s timeouts, both dice failing concurrently. High-frequency connection parameters appear to saturate the Windows BLE adapter when managing 2 dice. See FW-4 below.
- **`RollTestClient` improvements** — auto-increment generation per mode, request-to-response timing in ms, all output teed to `client_log.txt`.
- **C++20 fix** — `TrayApp::writeLogToFile` wchar_t iterator incompatibility resolved.
- **`wasRollingAtDisconnect` snapshot field** — snapshot exposes whether the die was physically rolling (rollState=Rolling/Handling) when BLE dropped. `RollServer` priority-3 fallback skips `currentFace` when this is true (mid-tumble face is meaningless).
- **Fix A — advert path silenced for face-changed-at-rest case** — `processAdvertisement` else-if branch now sets `reportWasRolling=true` after debounce threshold, so `markRollResult` calls `rollObserver_` and the server is notified.
- **Fix C — adverts ignored for never-GATT-connected dice** — `trySelectScannedPixel` seeds `faceBeforeDisconnect_` from the first scan so dies that never reached GATT-Ready can still have their adverts processed.
- **Fix D — 60s first-roll timeout when connectedDice=0** — `kFirstRollTimeout` reduced from 60s to 20s when no dice are connected.
- **Dedupe in `RollServer::onRoll`** — rejects duplicate label so GATT and advert path can't double-report the same die.
- **`kSecondRollTimeout` extended** — 10s → 20s to allow mid-roll disconnect + missed-roll reconnect (typically 5–15s).
- **`checkMissedRoll` Case 2 `isMissedRoll` fix** — face-changed-at-rest case now calls `markRollResult(face, false)` so `rollObserver_` is invoked (parallel fix to Fix A).

### ✅ Done (Session 2 — 2026-04-25, post deep-dive)

- **Suspend reconnects during multi-die request (Fix 1 / B architecture)** — added `DiceReconnectSuspender` callback to `RollServer`. When a 2-die request starts and any die is not `Ready`, `suspendReconnectUntil(now + 10s)` is called on all non-Ready dice via `DiceManager::suspendReconnects`. This stops the watchdog from competing with the BLE scanner for the radio, allowing the disconnected die to advertise freely. The advert path then fires in ~400–600ms (2 settled adverts × ~200–300ms intervals) instead of timing out after 20s. Wired through `PixelsRuntimeService::suspendReconnects` → `DiceManager::suspendReconnects` → `DieConnection::suspendReconnectUntil`. `TrayApp` passes the callback into `rollServer_->start()`.
- **Remove priority-3 stale-`currentFace` fallback (Fix B4)** — `RollServer` second-roll fallback no longer silently uses `currentFace` as a valid result. If priorities 1 and 2 yield nothing, we log the situation and return `{"error":"timeout"}`. A timeout is always better than a plausible-looking wrong face.
- **Fix B1 — `checkMissedRoll` clears state too early** — `faceBeforeDisconnect_` / `rollStateBeforeDisconnect_` are now cleared AFTER `markRollResult` completes (not before). This ensures `snapshot().wasRollingAtDisconnect` stays accurate during the RollServer fallback window so the guard can still work.
- **Fix B2 — priority-1 fallback accepted a single advert** — changed `advertSettledCount >= 1` to `>= kAdvertSettledThreshold` in the RollServer priority-1 fallback. A single advert bypassed the debounce designed to filter mid-tumble false positives.
- **Lower `kAdvertSettledThreshold` 3 → 2** (`DieConnection.h`) — with reconnects suspended the scanner gets clean, uninterrupted advert delivery; 2 consecutive settled adverts (~400ms) is robust and fast.
- **Lower `kSecondRollTimeout` 20s → 10s** (`RollServer.cpp`) — with the above fixes the fast path completes in 1–3s; 10s is a generous safety margin and keeps the user-visible failure under the 10s target.

### ✅ Done (Session 3 — 2026-04-25, post-fix log analysis)

Opus 4 analysis of `pixels_log.txt` (Session 2 actual gameplay) identified three new failure scenarios that survived the Session 2 fixes. All four fixes below are implemented.

#### Three new failure scenarios

**Scenario A — mid-request disconnect, suspend never triggered (e.g. advantage gen1 → 12s timeout)**
- Both dice Ready at request start → suspend condition (`rollsNeeded > 1 && connectedDice < rollsNeeded`) evaluates false
- die 2 delivers first roll via GATT; die 1 disconnects immediately after
- Watchdog fires ~3s later, GATT reconnect blocks the radio for die 1's adverts
- `kSecondRollTimeout = 10s` fires before advert path can accumulate 2 packets

**Scenario B — single-die disconnect, 60s timeout (normal gen5 → 60s timeout)**
- Die disconnects mid-roll while rolling state was confirmed via GATT
- Only ONE advert arrives (face=14, rollState=5) before the next reconnect attempt suppresses adverts again
- `kAdvertSettledThreshold=2` rejects the single credible packet
- `kFirstRollTimeout = 60s` (die was connected at start) → full 60s burned

**Scenario C — disconnect cascade with in-flight reconnect ignoring suspend (disadvantage gen1+2 → consecutive timeouts)**
- `suspendReconnects` issued at request start, but die 1 had already entered the `reconnectPixel` retry loop
- The loop's 2s-sleep + `connectAsync` cycles ignore `reconnectSuspendedUntil_` entirely
- Radio stays blocked for all 3 retry attempts (~6–8s) even though suspend was set

#### Fixes implemented

- **Fix S1 — Extend suspend to ALL request types and to mid-request disconnects** (`RollServer.cpp`):
  - Suspend condition changed from `rollsNeeded > 1 && connectedDice < rollsNeeded` → `connectedDice < configuredDice`. Now fires for single-die requests too when any configured die is absent.
  - After the first roll arrives in a 2-die request, snapshot dice and call `reconnectSuspender_(10s)` if any die is now disconnected. Eliminates Scenario A (mid-request disconnect case).

- **Fix S2 — Lower `kFirstRollTimeout` to 25s unconditionally** (`RollServer.cpp`):
  - Was: 20s when `connectedDice=0`, 60s otherwise. Now: always 25s.
  - 25s is more than enough for user roll time in BG3. Caps worst-case complete-radio-blackout failures at 25s instead of 60s.

- **Fix S3 — Advert fast-path: 1 packet when GATT-confirmed rolling → settled** (`DieConnection.cpp:processAdvertisement`):
  - When `rollStateBeforeDisconnect_ ∈ {Rolling, Handling}` AND advert `rollState != Crooked` AND `advFace != faceBeforeDisconnect_`, accept result on the first packet.
  - This combination is unambiguous: a die that was actively tumbling when GATT dropped, now reporting a different face in a non-ambiguous state, has definitively settled. One packet is sufficient.
  - Ambiguous cases (same face as tumble face, Crooked state, only `advertSawRolling_` set) still use the 2-packet debounce.
  - Directly fixes Scenario B: the single advert (rollState=5, face=14 ≠ faceBeforeDisconnect_=20) would have been accepted immediately.

- **Fix S4 — Honor `reconnectSuspendedUntil_` inside retry loops** (`DieConnection.cpp:reconnectPixel` and `reconstructiveReconnect`):
  - Added suspend check at the top of each retry iteration (after the optional sleep, before `connectAsync`). If `now < reconnectSuspendedUntil_`, sets `suspendedBySuspend=true` and breaks out of the loop.
  - The post-loop cleanup block skips incrementing `consecutiveFailures_` for a suspend-abort (voluntary, not a failure).
  - Eliminates Scenario C: a suspend issued mid-loop now takes effect at the next retry boundary (~2s delay in worst case) instead of being ignored for the entire 3-retry chain (~8–25s).

- **Fix S5 — Reset advert debounce in `forceReleaseConnection`** (`DieConnection.cpp`):
  - Clears `advertSettledFace_`, `advertSettledCount_`, `advertSawRolling_` when a die is force-released (adapter reset path). Prevents stale debounce counts from a previous disconnect bleeding into the next one (bug B3 guard).

### ✅ Done (Session 4 — 2026-04-25, post-fix log analysis)

Opus 4 analysis of the Session 3 build's `pixels_log.txt` and `client_log.txt` identified four root-cause bugs that survived the Session 3 fixes.  All seven fixes below are implemented and build cleanly.

#### Four root-cause bugs identified

**Bug 1 — `suspendReconnects` skips Ready dice (gen 1 race)**
- `DiceManager::suspendReconnects` only set `reconnectSuspendedUntil_` on dice where `status != Ready`.
- For a 2-die request where both dice are Ready at start time, NO suspension was issued.
- If die 2 disconnects in the narrow window between roll-start and the mid-request snapshot check, the watchdog fires before `RollServer` can issue a suspend, and reconnect blocks the radio for die 2's adverts.
- The mid-request snapshot block was also racy (TOCTOU): die 2 might still show `Ready` at snapshot time even though it just disconnected.

**Bug 2 — rollState=5 silently discarded (PRIMARY cause of gen 2–4 timeouts)**
- The at-rest face-change branch in `processAdvertisement` only accepted `PixelRollState::OnFace(1)` or `Crooked(4)`.
- Current Pixel firmware reports `rollState=5` (not in the C++ SDK enum) as its normal "at rest / idle" post-roll state.
- Log evidence: `[die 2] [advert-debug] Face changed to 15 (rollState=5) — not a roll, tracking only` repeated for every advert after rolling.
- Result: all gen 2–4 disadvantage rolls timed out at 12–13s because every advert from the settled die was silently discarded.

**Bug 3 — `consecutiveFailures_` not decayed on suspend abort**
- When a reconnect was aborted by a suspend, `consecutiveFailures_` was not incremented (correct) but was also not decremented.
- After multiple suspend aborts accumulated from prior sessions, the die stays at `consecutiveFailures_ >= 1`, causing the watchdog to always use `reconstructiveReconnect` (heavyweight, includes a 3s blocking sleep) instead of the lighter `reconnectPixel`.
- The 3s sleep in `reconstructiveReconnect` was also a solid blocking call — it could not be interrupted by a suspension, so the radio was blocked for at least 3s even when the user was actively rolling.

**Bug 4 — `immediateReconnectRequested_` set during suspend window**
- In `trySelectScannedPixel`, the `recoveryRediscovery` block set `immediateReconnectRequested_ = true` even during an active suspend window.
- This caused reconnect attempts to fire the moment the suspension expired, potentially re-blocking the radio during the second-roll wait window.

#### Fixes implemented (Session 4)

- **Fix A.1 — Suspend ALL dice unconditionally** (`DiceManager.cpp`):
  - Removed the `if (snap.status != Ready)` guard from `DiceManager::suspendReconnects`.
  - Now `suspendReconnectUntil` is called on every configured die, including those currently `Ready`.
  - Closes the gen 1 race: if a Ready die disconnects mid-request, its `reconnectSuspendedUntil_` is already set and the watchdog skips reconnect for the duration.

- **Fix A.2 — Pre-arm 25s suspension for all 2-die requests** (`RollServer.cpp`):
  - Changed the suspend condition from `connectedDice < configuredDice` to always issue a 25s suspension (`kFirstRollTimeout`) at the start of every advantage/disadvantage request.
  - Single-die requests still use the 10s conditional suspend (only when a die is already disconnected at request start).
  - Eliminates the TOCTOU race entirely: suspension is set before any disconnect can happen.

- **Fix B — Accept rollState=5 (firmware idle) as settled state** (`DieConnection.cpp:processAdvertisement`):
  - Changed the at-rest face-change branch from `if (advRollState == OnFace || Crooked)` to `if (advRollState != Rolling && advRollState != Handling)`.
  - Any state that is not actively rolling is now treated as settled and runs through the 2-packet debounce.
  - Directly fixes gen 2–4 timeouts: the `rollState=5` adverts are now accepted and will debounce to a result in ~400–600ms.

- **Fix C.1 — Guard `recoveryRediscovery` against suspend windows** (`DieConnection.cpp:trySelectScannedPixel`):
  - Added `&& std::chrono::steady_clock::now() >= reconnectSuspendedUntil_` to the `recoveryRediscovery` condition.
  - `immediateReconnectRequested_` is no longer set during an active suspension, preventing reconnects from firing the instant the suspension expires.

- **Fix C.2 — Decay `consecutiveFailures_` on suspend abort** (`DieConnection.cpp`):
  - Both `reconnectPixel` and `reconstructiveReconnect` cleanup blocks now call `consecutiveFailures_ = std::max(0, consecutiveFailures_ - 1)` when a suspend abort was the reason for exiting the retry loop.
  - Previously: abort skipped the increment but left the counter unchanged.
  - Now: abort actively decrements, keeping the die in the lighter `reconnectPixel` path rather than escalating to `reconstructiveReconnect` after multiple suspend aborts.

- **Fix C.3 — Interruptible 3s cleanup sleep in `reconstructiveReconnect`** (`DieConnection.cpp`):
  - Replaced the blocking `std::this_thread::sleep_for(seconds(3))` with a 250ms-slice loop that checks `reconnectSuspendedUntil_` on each tick.
  - If a suspension is detected during the wait, sets `suspendedBySuspend = true` and breaks immediately.
  - The `Pixel::create` call and the entire retry loop are guarded by `!suspendedBySuspend` and are skipped entirely on abort.
  - Worst-case radio-blocking time on a suspension event drops from 3s to ≤250ms.

- **Fix F — Unconditional mid-request suspension refresh** (`RollServer.cpp`):
  - Simplified the mid-request suspend block (fires after die 1's roll arrives in a 2-die request): removed the snapshot iteration and `anyDisconnected` TOCTOU check.
  - Now always calls `reconnectSuspender_(10s)` unconditionally after the first roll.
  - Since Fix A.2 pre-armed 25s at request start, this refresh is a safety net for the rare race where die 2 disconnects in the exact moment die 1 fires its roll notification.

#### Expected impact

- **Gen 2–4 disadvantage timeouts** (12–13s): should drop to ~400–600ms (2 advert packets × ~200–300ms interval) once Fix B allows rollState=5 adverts to be processed.
- **Gen 1 disadvantage timeout** (TOCTOU race): eliminated by Fix A.1 + A.2 pre-arming suspension for all dice.
- **Post-suspend reconnect storms**: mitigated by Fix C.1 + C.2, keeping dice in the lighter reconnect path.
- **3s dead radio window in reconstructive reconnect**: capped at ≤250ms by Fix C.3.

### ✅ Done (Session 5 — 2026-04-25, root-cause + firmware-state fix)

Opus 4 analysis of the Session 4 client_log identified the true root cause that all previous sessions had missed.

#### Root cause identified

**`Pixel::processMessage` only fires `onRolled` when `rollState == OnFace(1)`.** Current Pixel firmware sometimes reports the post-roll settled state as `rollState=5` (an out-of-enum "idle" value) instead of `OnFace=1`. When die 2 happens to settle via rollState=5 on a given roll, `onRolled` is never called, `markRollResult` is never called, and `RollServer` never receives the roll. The die remains `Ready` (connected), so the advert fallback path (which requires `Selected` or `Reconnecting`) is also useless. After `kSecondRollTimeout=10s` the request times out.

**Why this explains the exact 12–13s pattern:** die 1 delivers its roll at ~2–3s via GATT (it happened to send rollState=1). Die 2 sends only rollState=5, which is silently ignored. `kSecondRollTimeout=10s` fires, giving total 12–13s.

**Why it's intermittent (5/28 two-die rolls):** whether a given physical roll settles to rollState=1 or rollState=5 is nondeterministic — it depends on firmware state-machine timing. Most rolls send at least one rollState=1 frame; some don't.

**Why Android is stable:** the Pixels Android SDK (open source) treats any non-rolling, non-handling state as a roll completion. The Windows C++ SDK has a stricter `== OnFace` check that is a porting bug.

#### Fixes implemented (Session 5)

- **Fix 1 (PRIMARY) — Accept rollState=5 as a valid roll completion in the GATT path** (`Pixel.cpp`):
  - Added `prevState` tracking in `processMessage`: save `_data.rollState` before updating it.
  - Changed the `onRolled` trigger from `roll.state == OnFace` to:
    - Always fire for `OnFace=1` (existing behavior, unchanged).
    - Also fire for any `wasMoving (Rolling|Handling) → isSettled (!Rolling && !Handling && !Unknown && !Crooked)` transition.
  - This catches `rollState=5` (firmware idle) and any other future out-of-enum settled states.
  - **This is the single most impactful fix across all sessions** — it repairs the broken GATT path at source.

- **Fix 2 — Snapshot-polling fallback for genuine GATT notification drops** (`RollServer.cpp`):
  - Replaced the single `rollCv_.wait_for(kSecondRollTimeout)` call with a 500ms-slice polling loop.
  - Before waiting, captures the other die's label and pre-request `currentFace` from a snapshot.
  - Every 500ms, if no roll notification has arrived yet and at least 1.5s has elapsed since request start, takes a snapshot: if the other die is still `Ready` and its `currentFace` changed from the pre-request value, accepts the new face as the roll result with a log message.
  - `currentFace` in `_data` is updated by every `RollState` GATT message regardless of state, so even if `onRolled` never fires (e.g., genuine Windows notification drop), `currentFace` reflects the die's true face.
  - This is belt-and-suspenders on top of Fix 1, covering the truly-dropped-notification case.

#### Expected impact after Session 5

- **The 5/28 two-die timeouts** (all at ~12–13s = 10s second-roll timeout + ~2–3s first-roll latency): should drop to near zero. Fix 1 directly closes the rollState=5 gap in the GATT path. Fix 2 catches any remaining notification drops via polling.
- **Normal rolls** (1 die): unchanged, already working.
- **Android parity**: the GATT path now matches Android SDK semantics for rollState interpretation.

### ✅ Done (Session 6 — 2026-04-25, Windows BLE advert delay analysis)

Deep analysis of `pixels_log.txt` from the Session 5 build identified the surviving timeout root cause.

#### Root cause identified

**Windows BLE takes ~10-12 seconds to start delivering advertisements from a freshly-disconnected device.** After a GATT disconnect, the Windows BLE stack processes the disconnect event and suppresses advertisement delivery from that address for ~10-12s (radio/stack arbitration). The `kSecondRollTimeout=10s` was consistently shorter than this delay.

**Evidence from log (advantage gen1, lines 964-1060):**
- Die 2 disconnects at request start (line 972)
- Advertisement scanner starts immediately (line 978)
- **Zero die 2 advert-debug lines for the next ~11 seconds** — Windows is suppressing adverts
- Die 1 rolls and delivers face=4 at line 1009 (~7s into request)
- Second-roll 10s window starts; deadline = t=17s
- Line 1031: `kSecondRollTimeout` fires — timeout
- Line 1036: **first die 2 advert arrives** (`face=17, rollState=1`) — 1-2s AFTER the timeout
- Lines 1056-1060: second die 2 advert confirms (count=2); advert-recovery fires correctly — but `pendingRequest_.active = false`

The advert path IS correct and would have delivered the right answer. The window was simply too narrow.

**Pattern confirmed across sessions:** all advantage/disadvantage timeouts cluster at ~12-13s (`kSecondRollTimeout=10s` + first-roll time of 2-3s), consistent with the advert arriving just after the deadline.

#### Fixes implemented (Session 6)

- **Extend `kSecondRollTimeout` from 10s to 15s** (`RollServer.cpp`):
  - Provides ~3-5s of headroom past the typical 10-12s Windows BLE advert-delivery delay after disconnect.
  - In the advantage gen1 case: advert arrives at ~t=17s from request start; new deadline is t=22s (7s first roll + 15s second window). Advert now within deadline. ✓
  - Worst-case genuine second-die timeout: 15s instead of 10s (acceptable UX trade-off vs. constant timeout failures).

- **Match mid-request suspension refresh to `kSecondRollTimeout`** (`RollServer.cpp`):
  - Changed hardcoded `reconnectSuspender_(std::chrono::seconds(10))` to `reconnectSuspender_(kSecondRollTimeout)`.
  - Ensures the reconnect suspension stays active for the entire second-roll window (15s from first die's roll). Previously expired 5s before the new deadline, allowing watchdog reconnects to block the advert channel in the last 5s.

#### Expected impact after Session 6

- **Advantage/disadvantage timeouts at 12-13s**: should drop to near zero. The advert path was always delivering the correct answer; it just needed 3-5 more seconds.
- **Single-die normal roll timeout (gen2 pattern, 25s)**: unaffected by this fix — those are caused by Windows BLE never showing adverts within 25s for a die with persistent "Connection error: 1" failures. This is a hardware/driver issue. The system correctly recovers the result via `checkMissedRoll` when the die eventually reconnects in a subsequent request.

### 🔵 Future work — activate if instability returns

The following improvements were analysed and deprioritised while the current implementation is stable. Revisit if a real BG3 session surfaces new failure modes.

#### FW-1: Cached GATT service discovery (`Peripheral.cpp:108,160`)

Currently `GetGattServicesAsync(BluetoothCacheMode::Uncached)` and `GetCharacteristicsAsync(BluetoothCacheMode::Uncached)` are called on every reconnect. This can queue behind other Windows BLE operations and add ~7 s per request.

**Proposed change:** try `Cached` first; fall back to `Uncached` only if the cached result is empty or returns `AccessDenied`.

**Risk:** low-moderate — cached results can occasionally be stale after a firmware update. The fallback to uncached handles this.

**When to activate:** if reconnect time after a disconnect is consistently > 5 s even with `ThroughputOptimized` in place, or if multiple "Connection error: 1" lines appear in sequence.

#### FW-2: Global BLE operation gate (`DiceManager`)

Two dice reconnecting simultaneously can saturate the Windows BLE adapter and cause both to fail, triggering a full adapter reset. A global serialisation gate (one connect/discover attempt at a time) prevents the stampede.

The current `checkFullBleReset` staggered resume (`die 1 now, die 2 after 8 s`) is a partial workaround but does not prevent simultaneous initial attempts.

**Risk:** moderate — changes reconnect scheduling; needs careful testing with 2-die advantage rolls under poor radio conditions.

**When to activate:** if adapter resets (`[RESET] BLE adapter appears stuck`) appear frequently in production sessions, especially during advantage/disadvantage rolls.

#### FW-3: Advertisement-as-result (widened advert gate)

Currently `processAdvertisement` is gated to `connectionState_ == Selected || Reconnecting` and only fires when the die was rolling at disconnect. Widening this gate would allow advert data to complete a pending `RollServer` request even when GATT is connected but silent (the "lost notification" case).

This goes beyond what Android does (Android ignores adverts when connected) and is a Windows-specific rescue path.

**Risk:** higher — requires generation tracking plumbed through `DieConnection` → `RollObserver` → `RollServer` to avoid duplicate completions and stale-advert false positives. Also requires the scanner to run during active roll requests, which competes with GATT connections for radio time.

**When to activate:** only if GATT-connected-but-silent failures appear in production logs (roll requests that time out despite both dice showing `Ready` status and no disconnection events).

#### FW-4: `RequestPreferredConnectionParameters` (`Peripheral.cpp`)

Tested `ThroughputOptimized` on 2025-04-25. With a single die it may work, but with **two simultaneous dice** it caused the Windows BLE adapter to become unstable: repeated `Connection error: 1`, reconstructive reconnects, and 60 s timeouts. Reverted.

**If retrying:** test `BluetoothLEPreferredConnectionParameters::Balanced()` instead — it is less aggressive and less likely to cause adapter contention. Always measure with 2 dice under realistic conditions before shipping.

**When to activate:** only after the advertisement-as-result path (FW-3) is in place and providing a latency floor, so any regression from connection-parameter tuning is bounded.

---

## Session-2 Findings (2026-04-25)

Deep analysis by Opus 4 Thinking of the following observed test session:

```
normal  gen1–4:        ~2.5s each ✓
advantage gen1–2:      ~3.4s each ✓
disadvantage gen1–3:  22–29s ❌  (expected 8&6, got 8&19)
```

### Root cause of 20s+ delays

Windows suppresses BLE advertisement packets from a device it is actively reconnecting to via GATT. While `reconnectPixel()` / `reconstructiveReconnect()` is running (typically 15–25s for 3 retries), the advertisement scanner cannot receive packets from that die. The `kAdvertSettledThreshold=3` debounce counter only increments during the brief 2-second gaps between retry attempts, so building up 3 consecutive counts requires 3 full reconnect cycles ≈ 20s.

Advantage rolls worked because both dice were still `Ready`; the die degraded between the advantage and disadvantage phases.

### Root cause of wrong faces (die2=10, 10, 19 instead of the actual landed face)

The stale-`currentFace` priority-3 fallback returns whatever face the die last reported via GATT — which was from a previous roll, not the current one. Priority-2 (`recentRollFaces` post-request-start) cannot fire because the die never delivered a roll within the 20s window.

### Five bugs identified

| # | Location | Bug | Impact |
|---|---|---|---|
| **B1** | `DieConnection::checkMissedRoll` | Clears `rollStateBeforeDisconnect_` to `Unknown` before calling `markRollResult`. If RollServer snapshots the die during this window, `wasRollingAtDisconnect=false` and priority-3 fallback incorrectly allows stale `currentFace`. | Wrong face from fallback |
| **B2** | `RollServer` priority-1 fallback | Uses `advertSettledCount >= 1` instead of `>= kAdvertSettledThreshold`. A single advert is accepted as ground truth, bypassing the debounce. | Premature single-advert result |
| **B3** | `DieConnection::processAdvertisement` | Advert debounce (`advertSettledFace_/Count_`) is not reset on all transitions into `Selected` state (only reset in `onPixelDisconnected`). If a die cycles Selected→Connecting→Selected without going through disconnect, stale count carries forward. | Cross-roll advert contamination |
| **B4** | `RollServer` priority-3 fallback | Returns stale `currentFace` as a silent valid result. Wrong value is worse than a timeout — BG3 gets a plausible-looking incorrect face. | Wrong result (confirmed in test) |
| **B5** | `RollServer` / `DieConnection` | GATT `onRolled` buffers can flush a stale roll from a previous generation after reconnect, since there is no per-request generation guard on `markRollResult`. | Potential stale-result poisoning |

### Fix implementation order (highest impact first)

1. **Suspend reconnects when waiting for second roll** (`DiceManager` + `RollServer`) — call `suspendReconnectUntil(now + 10s)` on all non-Ready dice as soon as a multi-die request begins. Eliminates scanner contention so advert path fires in ~1s. This is the single biggest win.
2. **Remove priority-3 fallback** (`RollServer`) — return `{"error":"timeout"}` instead of stale `currentFace`. Correct timeout > wrong face.
3. **Fix B1** (`DieConnection::checkMissedRoll`) — move `faceBeforeDisconnect_=0` / `rollStateBeforeDisconnect_=Unknown` reset to AFTER `markRollResult` so snapshot stays accurate until the result is delivered.
4. **Fix B2** (`RollServer` priority-1) — change `advertSettledCount >= 1` to `>= kAdvertSettledThreshold`.
5. **Lower `kAdvertSettledThreshold`** from 3 → 2 (`DieConnection.h`) — with reconnects suspended, adverts flow cleanly; 2 consecutive is sufficient and halves advert-path latency to ~400–600ms.
6. **Lower `kSecondRollTimeout`** from 20s → 10s (`RollServer`) — with fixes 1–5, the fast path completes in 1–3s; 10s is still a generous safety margin.

---

---

## 1. Executive Summary

The current implementation has a good fast path when GATT notifications are healthy, but it still has a long-tail failure mode where a roll result can be delayed by many seconds, timeout entirely, or fall back to a stale face.

The strongest conclusion from today:

> **Do not make roll-result delivery depend on Windows GATT reconnect/discovery finishing.**

Instead, use a **two-source race**:

```text
GATT RollState notification  vs.  BLE advertisement roll state/current face
```

Whichever source produces a credible settled face first should complete the pending roll. GATT remains the best path when connected. Advertisements become the low-latency rescue path when GATT disconnects, reconnect stalls, or Windows queues BLE operations.

This should mainly improve the **bad rolls**, not necessarily the already-good rolls.

Estimated effect from the uploaded log:

| Metric | Current estimate | After advertisement-as-result estimate |
|---|---:|---:|
| Good/clean roll | ~4-5 s request-to-response, often probably lower after physical roll | ~same, possibly slightly better |
| Bad roll that eventually succeeds | ~22 s avg | ~3-6 s |
| Bad roll including timeouts | ~32 s avg | ~3-6 s, or bounded uncertain result |
| Timeout tail | ~60 s | ~4-6 s cap |
| All roll requests | ~11.5 s avg | ~4-5 s estimated |

These numbers are approximate because the uploaded log does not include absolute timestamps. The estimate uses the repeated poll cadence as a timing proxy. See Section 4.

---

## 2. What We Found Today

### 2.1 Hardware may contribute, but the log shows a software/architecture issue too

Earlier discussion considered whether the Windows Bluetooth chip/driver could be responsible. The answer is still: **yes, it may contribute**, especially because Windows BLE behavior depends heavily on radio, driver, OS stack, antenna placement, USB interference, and concurrent BLE operations.

But the log shows that the software can do better even on the same chip:

- The app receives useful advertisement data while GATT is disconnected or reconnecting.
- The app sometimes knows a die landed on a face from advertisements/reconnect evidence, but RollServer still times out or falls back to a stale `currentFace`.
- Reconnect and adapter-reset paths are currently allowed to affect user-visible roll latency.

So the first improvement should be software architecture: **decouple roll result reporting from BLE recovery**.

### 2.2 Android is likely more stable partly because of connection-parameter behavior

Android exposes `BluetoothGatt.requestConnectionPriority(...)`, including `CONNECTION_PRIORITY_HIGH`, which requests high-priority/low-latency connection parameters.

Windows has some related APIs on Windows 11, but the Windows GATT stack still has documented queueing behavior and non-cancelable connection behavior. The Windows documentation says GATT service discovery/read/write can wait a variable time, from instantaneous to minutes, and queued requests can add about seven seconds per queued request.

Important external references:

- Microsoft GATT client docs: <https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/gatt-client>
- Android `BluetoothGatt.requestConnectionPriority`: <https://developer.android.com/reference/android/bluetooth/BluetoothGatt#requestConnectionPriority(int)>
- Android `CONNECTION_PRIORITY_HIGH`: <https://developer.android.com/reference/android/bluetooth/BluetoothGatt#CONNECTION_PRIORITY_HIGH>

### 2.3 Nordic nRF52840/nRF52832 is not a plug-and-play Android BLE stack

A Nordic nRF52840 can be a custom BLE gateway, but it does not give Windows the Android BLE stack. It would require a separate firmware/backend over USB serial/HID/custom protocol.

For this project, that is likely a later option, not the first fix.

---

## 3. Evidence From Uploaded Files

### 3.1 Uploaded files analyzed

- `pixels_log-big-session.txt.txt`
  - 45,308 log lines
  - Long live session with connection, roll, reconnect, advertisement, watchdog, timeout, and server events.
- `diff.txt`
  - Git diff from the `server-mode` branch.
  - Mainly modifies BG3 input behavior in `RollServer.cpp`, not core BLE.

### 3.2 Counts from the uploaded session log

From `pixels_log-big-session.txt.txt`:

| Observation | Count |
|---|---:|
| RollServer requests, excluding `ready` commands | 62 |
| Normal roll requests | 59 |
| Advantage roll requests | 3 |
| Ready commands | 3 |
| Roll received events | 60 |
| Timeout responses | 4 |
| `Connection error: 1` | 281 |
| `First connection attempt failed` | 315 |
| `BLE adapter appears stuck` resets | 7 |
| `advert-debug` lines | 156 |
| `missed-roll` lines | 13 |

Interpretation:

- There are many more BLE connection failure lines than roll requests.
- The app has repeated evidence of adapter-level or stack-level trouble.
- Advertisement debug data is present often enough to be useful as a roll source.
- `missed-roll` detections prove that rolls can happen while GATT is disconnected or recovering.

### 3.3 Good GATT path is already fast enough

Example from the log:

```text
pixels_log-big-session.txt.txt lines 437-494

[RollServer] Request: {"mode": "normal", "generation": 1}
[RollServer] Waiting for 1 roll(s), mode=normal, gen=1, configuredDice=2, connectedDice=2
...
[die 1] Roll state changed to 3 with face 3 up
...
[die 1] Roll state changed to 1 with face 3 up
[RollServer] Roll received: die 1 -> 3 (1/1)
...
[RollServer] Response: {"mode": "normal", "generation": 1, "die1": 3}
```

Conclusion:

- When GATT notifications are flowing, the architecture works.
- We should not replace GATT.
- We should race GATT against advertisements and let GATT win when it is healthy.

### 3.4 Reconnect path can consume many seconds

Example from startup:

```text
pixels_log-big-session.txt.txt lines 21-73

[die 2] Status changed to disconnected
[die 2] First connection attempt failed, retrying...
...
[die 2] Connection error: 1
...
[die 2] [watchdog] Disconnected, elapsed=8s since last attempt (backoff=4s, failures=1)
[die 2] [watchdog] Prior failure, trying reconstructive reconnect...
[die 2] [reconstructive] Waiting 3s for BLE cleanup + advertisement scan window...
```

Conclusion:

- The code has explicit multi-second reconnect/backoff/cleanup windows.
- These are reasonable for recovery, but they should not block roll-result delivery.

### 3.5 Adapter-level reset confirms Windows BLE stack/driver contention

Example:

```text
pixels_log-big-session.txt.txt lines 9098-9122

[RESET] BLE adapter appears stuck — disconnecting ALL dice and restarting connections
[die 1] [adapter] Releasing connection to free BLE adapter (face=10)
...
[RESET] Waiting 3s for BLE adapter cleanup.
...
[RESET] BLE cleanup done — resuming dice with staggered timing
[RESET] die 1 will resume in 0s
[RESET] die 2 will resume in 8s
```

Conclusion:

- The app is detecting a stuck adapter/stack condition.
- The reset strategy may be necessary, but it introduces a long user-visible delay if RollServer waits for recovery.
- Roll result handling should continue using advertisements while BLE recovery runs in the background.

### 3.6 Timeout while dice still appear Ready

Example:

```text
pixels_log-big-session.txt.txt lines 30060-30199

[RollServer] Request: {"mode": "normal", "generation": 4}
[RollServer] Waiting for 1 roll(s), mode=normal, gen=4, configuredDice=2, connectedDice=2
...
[die 1] [poll] state=Ready, status=ready, face=1, rollState=1, ...
[die 2] [poll] state=Ready, status=ready, face=7, rollState=5, ...
...
[RollServer] Timeout waiting for first roll
[RollServer] Response: {"error": "timeout"}
```

Conclusion:

- RollServer can time out even when dice are still being polled as `Ready`.
- This means “connected/ready” is not enough; RollServer must receive a qualifying roll event.
- The fallback should use the freshest valid roll observations, including advertisements.

### 3.7 Advertisement saw the likely landed face, but fallback used stale state

This is the clearest issue found today.

```text
pixels_log-big-session.txt.txt lines 42148-42218

[die 2] Roll state changed to 1 with face 8 up
[RollServer] Roll received: die 2 -> 8 (1/2)
...
[die 1] [poll] state=Ready, status=ready, face=14, rollState=3, ...
[die 1] Status changed to disconnected
[die 1] [event] Disconnect detected — requesting immediate reconnect (face=14, rollState=3)
...
[die 1] [advert-debug] face=9, rollState=1, settledFace=0, settledCount=0
[die 1] [advert-debug] Face changed to 9, reset count to 1
[die 1] [advert-debug] Waiting for 3 consecutive, have 1
...
[RollServer] Second roll timed out, attempting face-snapshot fallback...
[RollServer] Fallback: using die 1 currentFace=14
[RollServer] Response: {"mode": "advantage", "generation": 3, "die1": 8, "die2": 14}
...
[die 1] [missed-roll] Die was rolling at disconnect (face=14, rollState=3) -> landed on 9 (rollState=5)
```

Conclusion:

- Advertisement saw `face=9, rollState=1` before RollServer fallback.
- RollServer returned stale `currentFace=14`.
- Reconnect later confirmed the die landed on `9`.
- This proves the fallback hierarchy is wrong: advertisement-settled face should outrank stale current face.

### 3.8 Diff adds BG3 ready-command delay, but it is separate from BLE roll latency

From `diff.txt`:

```text
diff.txt lines 47-51

SetForegroundWindow(bg3Window);
Sleep(100);
MoveMouseAbsolute(pt.x, pt.y);
Sleep(1000);
LeftClickScreenPoint(pt);
```

Conclusion:

- The `server-mode` diff adds about 1.1 seconds to the BG3 `ready` click path.
- This may be necessary for BG3 controller-to-mouse transition.
- It should be measured separately from “user physically rolls die -> result returned.”

---

## 4. Latency Estimate From the Log

### 4.1 Limitation

The uploaded log does **not** include absolute timestamps. Therefore, exact latency cannot be computed.

The estimate below uses the repeated `[poll]` cadence as a proxy:

- Timeout cases show about 24 poll lines between request and timeout.
- Those timeout cases appear to correspond to a ~60 second timeout.
- Therefore: **1 poll line ≈ 2.5 seconds**, or roughly one two-dice poll cycle ≈ 5 seconds.

This is coarse. It is good enough to estimate long-tail behavior, but not precise enough for sub-second GATT-vs-advertisement latency.

### 4.2 Current estimated performance

Excluding `ready` commands, the log contains **62 roll requests**.

| Category | Count | Current estimated avg | Current estimated median | Interpretation |
|---|---:|---:|---:|---|
| Clean/good rolls | 46 | ~4.5 s | ~5.0 s | GATT path mostly works |
| Bad rolls, eventually succeed | 12 | ~22.1 s | ~17.5 s | Reconnect/recovery affects latency |
| Timeout rolls | 4 | ~60 s | ~60 s | Current worst tail |
| Bad rolls including timeouts | 16 | ~31.6 s | ~32.5 s | User-visible bad experience |
| All roll requests | 62 | ~11.5 s | ~5.0 s | Good median, bad tail |

### 4.3 Estimated after proposed change

Assuming advertisements are accepted as a valid result source during an active roll request:

| Category | Current | Estimated after change |
|---|---:|---:|
| Good/clean roll | ~4-5 s request-to-response proxy | ~same, likely unchanged |
| Good roll after physical motion starts | likely ~1-3 s | ~1-3 s |
| Bad roll that currently succeeds late | ~22 s avg | ~3-6 s |
| Timeout roll | ~60 s | ~4-6 s bounded fallback/uncertain result |
| Bad roll including timeouts | ~32 s avg | ~3-6 s |
| Overall average | ~11.5 s | ~4-5 s |

The main improvement is not the median good roll. The main improvement is **eliminating catastrophic tails**.

---

## 5. Proposed Architecture

### 5.1 Current simplified flow

```text
RollServer request
  ↓
Wait for GATT RollState / onRolled callback
  ↓
If GATT fails/disconnects:
  reconnect / reconstruct / adapter cleanup / watchdog / timeout
  ↓
maybe fallback to currentFace
```

Problem:

- Reconnect is slow and sometimes stuck.
- GATT operations can queue on Windows.
- `currentFace` can be stale.
- Advertisement data is available but not promoted to a first-class result source.

### 5.2 Proposed flow

```text
RollServer request
  ↓
Create roll generation / request context
  ↓
Race two result sources:
  1. GATT RollState == OnFace
  2. Advertisement rollState == OnFace
  ↓
First credible settled face wins
  ↓
Reconnect/recovery continues in background if needed
```

Core principle:

> GATT is the preferred live channel. Advertisements are the bounded-latency rescue channel.

---

## 6. Implementation Plan

### Phase 0 — Add proper timestamps and structured metrics

Before changing behavior, improve measurement.

Add monotonic timestamps to every log line:

```cpp
auto nowMs = duration_cast<milliseconds>(
    steady_clock::now() - appStartTime
).count();
```

Log these events with the same `generation`:

```text
request_received
user_roll_detected_gatt
user_roll_detected_advert
onface_gatt
onface_advert
roll_completed
response_sent
disconnect_detected
reconnect_started
reconnect_finished
fallback_used
```

This will allow exact P50/P90/P99 latency later.

Success criteria:

- Every RollServer response includes:
  - generation
  - die id
  - result source: `gatt`, `advertisement`, `missed-roll`, `fallback`, `timeout`
  - latency from request
  - latency from first roll evidence
  - confidence

### Phase 1 — Create a central RollObservationAggregator

Add a central class that receives observations from both GATT and advertisement paths.

Suggested structure:

```cpp
enum class RollSource {
    Gatt,
    Advertisement,
    MissedRollReconnect,
    FallbackCurrentFace
};

struct RollObservation {
    int dieIndex;
    int face;
    int rollState;
    RollSource source;
    uint64_t generation;
    std::chrono::steady_clock::time_point observedAt;
    int rssi;
    bool valid;
};
```

Responsibilities:

- Maintain per-die latest state from GATT.
- Maintain per-die latest state from advertisements.
- Maintain whether this die was observed rolling in the current generation.
- Decide whether an observation is enough to complete a pending RollServer request.

### Phase 2 — Promote advertisements into the result path

Current advertisement handling appears to be mostly recovery/debug/missed-roll logic.

Change it so that advertisement state can complete a pending roll.

Pseudo-code:

```cpp
void onAdvertisementRollState(int dieIndex, int face, int rollState, int rssi) {
    auto now = steady_clock::now();

    updateLatestAdvertisementState(dieIndex, face, rollState, rssi, now);

    if (!rollServer.hasPendingRoll()) {
        return;
    }

    if (rollState != OnFace) {
        rollServer.markSawRolling(dieIndex, RollSource::Advertisement);
        return;
    }

    if (rollServer.sawRollingThisGeneration(dieIndex)) {
        rollServer.completeAfterTinyDebounce(
            dieIndex,
            face,
            RollSource::Advertisement,
            100ms,
            250ms
        );
        return;
    }

    if (sameFaceSeenTwiceRecently(dieIndex, face, 500ms)) {
        rollServer.completeRoll(dieIndex, face, RollSource::Advertisement);
    }
}
```

Recommended debounce rule:

| Case | Rule |
|---|---|
| GATT says `OnFace` | accept immediately |
| Advertisement says `OnFace` after this die was seen rolling in current generation | accept after 100-250 ms stability |
| Advertisement says `OnFace` but no rolling was observed | require 2 matching advertisements, or face differs from pre-roll face |
| Disconnect while rolling | prefer advertisement settled face over stale `currentFace` |

### Phase 3 — Fix fallback priority

Current bug shown in the log:

```text
advertisement saw face=9
fallback returned currentFace=14
reconnect later confirmed landed on 9
```

New fallback order:

```text
1. GATT RollState OnFace for current generation
2. Advertisement OnFace for current generation
3. Missed-roll reconnect result
4. Recent advertisement settled face, even if not fully debounced, marked lower confidence
5. Stale currentFace only as last resort, marked uncertain
6. Timeout/uncertain error
```

Never silently treat stale `currentFace` as equivalent to a new roll result.

Suggested response extension:

```json
{
  "mode": "normal",
  "generation": 12,
  "die1": 9,
  "source": "advertisement",
  "confidence": "high",
  "latencyMs": 2150
}
```

For compatibility, keep the existing fields and add optional metadata.

### Phase 4 — Make reconnect non-blocking relative to roll result

Reconnect should keep running, but it should not gate RollServer response.

Rules:

```text
If GATT is healthy:
  use it.

If GATT disconnects while a roll request is active:
  start/continue advertisement scan immediately.
  complete roll from advertisement if possible.
  reconnect in background.

If reconnect completes after result already sent:
  update internal state only.
  do not emit duplicate result.
```

Use generation IDs to avoid stale reconnect completions affecting new requests.

### Phase 5 — Reduce Windows GATT pressure

Even after advertisement-as-result, improve GATT health.

Recommended changes:

1. **Targeted service discovery**
   - Use the known Pixels service UUID.
   - Prefer cached lookup first.
   - Use uncached only as fallback.

2. **Avoid broad uncached discovery on every reconnect**
   - Broad discovery is expensive.
   - On Windows, service discovery/read/write can queue and stall.

3. **Global GATT operation scheduler**
   - A per-die queue is not enough if the adapter/Windows stack is the bottleneck.
   - Add one global Windows BLE operation gate for connect/discover/subscribe/write operations.

4. **Stagger reconnects**
   - Already partly present.
   - Keep it, but do not make user-visible roll result wait for it.

5. **Optional Windows 11 connection parameter request**
   - Try `RequestPreferredConnectionParameters`.
   - Test both throughput-optimized and balanced/power-optimized modes, especially with multiple dice.
   - Measure; do not assume throughput-optimized is always better.

### Phase 6 — Treat adapter reset as background recovery

Current reset behavior disconnects all dice and waits:

```text
disconnect all dice
wait 3s
resume die 1 now
resume die 2 after 8s
```

Keep this as a recovery mechanism, but during an active roll request:

- Keep advertisement scanning active.
- Complete any pending roll from advertisements.
- Mark reconnect as degraded/background.
- Do not wait for full recovery before responding.

### Phase 7 — Validation plan

Create an A/B test:

```text
A = current server-mode behavior
B = advertisement-as-result behavior
```

Run at least:

- 100 normal rolls with 1 die.
- 100 normal rolls with 2 dice connected.
- 50 advantage rolls.
- 20 induced-disconnect tests:
  - move die farther away
  - temporarily block signal
  - restart scanner
  - stress Windows Bluetooth with another BLE device

Metrics:

| Metric | Target |
|---|---:|
| P50 roll latency | <= 2-3 s |
| P90 roll latency | <= 4 s |
| P99 roll latency | <= 6 s |
| Timeout rate | near 0 |
| Wrong-face fallback rate | 0 |
| Stale `currentFace` fallback | only with `confidence=low` |
| Adapter resets during active roll | should not cause timeout if advertisement is visible |

---

## 7. Concrete Code-Level TODO List

### RollServer

- [ ] Add `RollSource` and `confidence` to internal roll result.
- [ ] Add optional response metadata, while preserving existing JSON shape.
- [ ] Complete pending rolls from advertisement observations.
- [ ] Replace `currentFace` fallback with ordered fallback policy.
- [ ] Add generation guard to reject stale reconnect/missed-roll results.
- [ ] Add explicit bounded deadline:
  - normal: 4-6 seconds
  - advantage: per-die deadline, not all-or-nothing
- [ ] Log source and latency for every response.

### Pixel / Peripheral layer

- [ ] Expose advertisement roll observations to RollServer/aggregator.
- [ ] Keep GATT notifications as the primary source when healthy.
- [ ] Rebuild GATT objects after disconnect.
- [ ] Use targeted cached discovery first.
- [ ] Avoid reconnect storms.

### Advertisement scanner

- [ ] Keep scanner running during active gameplay or at least during active RollServer requests.
- [ ] Decode:
  - Pixel ID
  - face
  - rollState
  - battery
  - RSSI
  - firmware timestamp if useful
- [ ] Maintain recent observation window per die.
- [ ] Provide debounced `OnFace` observation.
- [ ] Do not require 3 consecutive advertisements in every case; use adaptive confidence.

### Logging

- [ ] Add monotonic timestamp to each log line.
- [ ] Add request/generation id to advertisement logs.
- [ ] Add per-roll timeline summary after each response:

```text
[RollServer] Timeline gen=12:
  request=0ms
  advertRolling=820ms
  advertOnFace=2310ms
  response=2500ms
  source=advertisement
  confidence=high
```

---

## 8. Suggested Roll Completion Rules

### Normal mode

Complete as soon as any configured die produces a credible result.

```text
GATT OnFace:
  complete immediately.

Advertisement OnFace:
  if die was rolling in current generation:
    complete after 100-250 ms stability.
  else:
    require 2 matching advertisements or face changed from pre-roll snapshot.
```

### Advantage mode

Need two rolls. Each die result should be independent.

```text
If die A completed from GATT and die B disconnects:
  do not wait for die B reconnect.
  use die B advertisement result if available.

If only one die is connected:
  allow disconnected die advertisement to complete second roll.

If second die times out:
  prefer last advertisement-settled face over stale currentFace.
```

### Edge cases

| Case | Behavior |
|---|---|
| GATT and advertisement agree | high confidence |
| GATT wins first | return GATT result |
| Advertisement wins first, GATT later agrees | ignore duplicate |
| Advertisement wins first, GATT later disagrees | log correction, investigate; do not silently change already-returned result |
| No useful GATT or advert | return uncertainty within bounded deadline |
| Stale `currentFace` only | return low confidence or error, not silent success |

---

## 9. Expected User-Visible Improvement

### What will improve

- Fewer 20-60 second waits.
- Fewer RollServer timeouts.
- Fewer stale fallback results.
- Better advantage-mode behavior when one die disconnects mid-roll.
- More predictable user experience.

### What may not improve much

- Already-good GATT rolls.
- Physical dice settling time.
- BG3 ready-click delay from `Sleep(1000)`.
- Cases where neither GATT nor advertisements are visible.

---

## 10. Risks and Mitigations

### Risk: Advertisement data may be stale

Mitigation:

- Only use advertisement result during active roll generation.
- Prefer observations after seeing `rolling`/non-`OnFace` state.
- Require face change or 2 matching advertisements if no rolling state was observed.
- Include confidence metadata.

### Risk: Duplicate results

Mitigation:

- Use generation IDs.
- Mark each die result completed once.
- Ignore late duplicate events from reconnect/GATT.

### Risk: Advertisement rate is too slow on Windows

Mitigation:

- Keep scanner active before the roll request, not only after disconnect.
- Measure advertisement intervals with timestamps.
- Tune debounce from real measured intervals.

### Risk: BG3 integration hides BLE improvements

Mitigation:

- Measure separately:
  - `ready` command latency
  - physical roll-to-result latency
  - RollServer request-to-response latency
  - BG3 click/input latency

---

## 11. Conversation Recap From Today

### Question 1: Could the instability be my Windows chip/driver?

Answer:

- Yes, the Windows Bluetooth chip/driver can contribute.
- A normal Windows-supported Bluetooth adapter could improve RF/driver behavior.
- A Nordic nRF52840/nRF52832 is not a plug-and-play Windows Bluetooth adapter for your current WinRT code.
- A better normal USB adapter is a cheap diagnostic step, but the software architecture should still handle Windows BLE stalls.

### Question 2: Could I buy a chip that runs Android BLE internally?

Answer:

- Not really as a drop-in chip.
- A cheap Android phone/tablet could act as a BLE bridge.
- That would use Android’s BLE stack and forward results to Windows over USB/Wi-Fi/local network.
- Your Windows WinRT BLE code would not work unchanged; you would replace the BLE backend with a bridge protocol.
- This is viable, but it adds another device and more integration work.

### Question 3: Is there a better Windows-only API or approach?

Answer:

- WinRT is still the right primary Windows BLE API.
- A different wrapper probably will not avoid Windows BLE stack behavior.
- The better approach is architectural:
  - GATT for live notifications when healthy.
  - Advertisements for bounded-latency roll result fallback.
  - Reconnect as background recovery.
  - Targeted/cached GATT discovery.
  - Global BLE operation scheduling.

### Question 4: Would advertisement-as-result reduce time to roll result?

Answer:

- Yes, mainly in bad cases.
- Good rolls are already fast when GATT works.
- Bad rolls should stop waiting for reconnect/discovery.
- Advertisement settled face should complete the request if credible.

### Question 5: Estimated speedup from logs

Answer:

- Current all-roll average estimated around 11.5 seconds.
- Current clean/good rolls around 4-5 seconds request-to-response by coarse poll proxy.
- Current bad rolls around 22-32 seconds depending on whether timeouts are included.
- Timeout tail around 60 seconds.
- After advertisement-as-result, bad rolls should be around 3-6 seconds if advertisements are visible.
- Overall average could drop toward 4-5 seconds.
- The biggest gain is eliminating the long tail.

---

## 12. Recommended Next Commit Plan

### Commit 1: Instrumentation

```text
Add monotonic timestamps and roll timeline logging
```

Files likely touched:

- `RollServer.cpp`
- logger/util files
- any advertisement scanner file
- Pixel/Peripheral event callback path

### Commit 2: Observation aggregator

```text
Add RollObservationAggregator and source-aware roll completion
```

Files likely touched:

- RollServer
- Pixel event handling
- advertisement scanner

### Commit 3: Advertisement completion path

```text
Allow advertisement OnFace observations to complete pending RollServer requests
```

Key behavior:

- GATT remains primary.
- Advertisement can complete only active generation.
- Add adaptive debounce.
- Add result source/confidence.

### Commit 4: Fix fallback priority

```text
Prefer advertisement/missed-roll result over stale currentFace fallback
```

This directly addresses the line 42181 -> 42195 issue.

### Commit 5: GATT pressure reduction

```text
Use targeted cached Pixels service discovery and global BLE operation gate
```

### Commit 6: Validation report

```text
Add latency summary logging and compare before/after
```

Include:

- P50/P90/P99
- timeout count
- source distribution
- wrong/stale fallback count

---

## 13. Bottom Line

The log strongly supports the previous assumption:

> The performance problem is not only radio quality. It stems from coupling roll-result delivery to a fragile Windows GATT/reconnect path, while advertisement data that already contains useful landed-face information is not yet allowed to complete the RollServer request.

The fastest practical Windows-only improvement is:

```text
Keep GATT as the fast path.
Promote advertisements to a first-class roll result source.
Make reconnect background-only.
Fix fallback priority so stale currentFace is last resort.
Add timestamps to prove the improvement.
```

Expected result:

```text
Good rolls: roughly unchanged.
Bad rolls: likely 5-10x faster.
Timeout tail: should move from ~60s to a bounded ~4-6s uncertain/result path.
```

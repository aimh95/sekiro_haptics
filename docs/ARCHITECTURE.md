# Architecture

## Goals for this stage

Establish a clean separation between three concerns that will otherwise get
tangled together the moment a real controller and real game hooks show up:

1. **What a haptic effect *is*** (data)
2. **How an effect gets delivered to a device** (backend)
3. **When an effect gets delivered** (scheduling, cancellation, reset)

Nothing in this stage talks to real hardware or the game. That boundary is
enforced by construction: the only `IHapticBackend` implementation that
exists is `MockHapticBackend`, and nothing in the codebase reads from or
injects into a running process.

## Components

### `HapticEffect` (`include/sekiro_haptics/HapticEffect.hpp`)

Plain data. A `HapticEffect` has:

- `HapticEffectType type` — a small, closed enum (`Generic`, `PerfectDeflect`,
  `PostureBreak`, `TakeDamage`, `Deathblow`) identifying *why* the effect is
  playing, not just its raw motor values. Only `PerfectDeflect` has an
  actual preset and consumer right now; the others exist to show how the
  vocabulary is meant to grow.
- `MotorIntensity intensity` — normalized `[0.0, 1.0]` left/right values.
  No adaptive-trigger modeling yet.
- `std::chrono::milliseconds duration`
- `std::string debugLabel` — free-form, logging only.

`HapticEffect` has no behavior and no backend awareness. It is intentionally
boring so it's cheap to construct, copy, and test in isolation.

`include/sekiro_haptics/Presets.hpp` holds named factory functions
(currently just `presets::PerfectDeflect()`) that build well-known effects.
This keeps "what does a Perfect Deflect feel like" as a single, greppable
definition instead of being duplicated wherever it's triggered.

### `IHapticBackend` (`include/sekiro_haptics/IHapticBackend.hpp`)

Three-method abstract interface:

- `SendEffect(const HapticEffect&)` — play one effect now
- `Reset()` — stop and return to neutral
- `IsConnected() const`

This is the seam where a real DualSense/HID backend would plug in later,
without any caller-facing code changing. `SendEffect` is documented to
return quickly; a backend that needs to sustain an effect over its
`duration` should do that internally (e.g. on its own thread), not by
blocking the caller.

### `MockHapticBackend` (`include/sekiro_haptics/MockHapticBackend.hpp`)

The only backend implementation in this stage. It:

- Logs every effect to an injectable `std::ostream` (defaults to
  `std::cout`)
- Records every effect in an in-memory `History()`
- Counts `Reset()` calls via `ResetCount()`
- Is thread-safe, because `HapticScheduler` dispatches from a background
  thread

It also exposes `WaitForEffectCount(count, timeout)`, a condition-variable
wait used only by tests and the console app to observe scheduler output
deterministically instead of sleeping and hoping.

### `HapticScheduler` (`include/sekiro_haptics/HapticScheduler.hpp`)

Owns a single background thread and a list of pending effects, each with a
target dispatch time (`steady_clock::time_point`). Public API:

- `Schedule(effect, delay = 0ms) -> HapticEffectId`
- `Cancel(id) -> bool` — removes a still-pending effect; false if it already
  fired or the id is unknown
- `Reset()` — clears all pending effects and calls `backend.Reset()`

Design notes:

- The scheduler does **not** own the backend (`IHapticBackend&`); the caller
  controls the backend's lifetime, which must outlive the scheduler.
- The worker loop repeatedly finds the earliest pending effect and either
  dispatches it (if due) or sleeps until it's due. `Schedule`/`Cancel`/
  `Reset` all notify the worker's condition variable, so a newly scheduled
  effect with an earlier deadline preempts an existing sleep instead of
  waiting for a stale one.
- Cancellation is by id, stored in a small vector — fine at the scale this
  project operates at (a handful of pending effects), and avoids the
  complexity of a priority queue that supports arbitrary removal.
- The destructor stops the worker and joins it, so a `HapticScheduler`
  never outlives its own thread.

### Console test app (`apps/console_test/main.cpp`)

Wires a `MockHapticBackend` to a `HapticScheduler` and schedules
`presets::PerfectDeflect()`, then waits on `WaitForEffectCount` before
printing a summary. This is the shape a future "Sekiro event source" would
follow: something decides an event happened, builds/looks up a
`HapticEffect`, and calls `scheduler.Schedule(...)`.

### Tests (`tests/`)

No third-party test framework — `tests/testing.hpp` is a ~40-line
registration/assertion header (`SH_TEST`, `SH_CHECK`) sufficient for this
project's needs without adding a dependency. Each `SH_TEST` self-registers
via a static initializer; `test_main.cpp` runs the registry and reports
pass/fail counts, returning non-zero on any failure so `ctest` picks it up.

Scheduler tests rely on `WaitForEffectCount` with generous timeouts rather
than fixed sleeps, to avoid flakiness from thread scheduling jitter.

## Explicitly out of scope (this stage)

- **Real DualSense access.** No HID/USB/Bluetooth code exists. A real
  backend would implement `IHapticBackend` and live alongside
  `MockHapticBackend`; nothing else would need to change.
- **HID packet construction.** Follows from the above — there is no wire
  format anywhere in this codebase.
- **Sekiro hooking/injection.** There is no process attachment, memory
  reading, or event capture. `HapticEffectType` anticipates a few Sekiro
  events by name, but nothing produces them from the game.
- **Networking / DSX integration.** No sockets, no external protocol
  support.

## Likely next steps (not implemented)

- A real event source (log-file tailing, memory reading, or a companion
  tool) that calls into `HapticScheduler` — this is the piece that would
  need to hook into Sekiro, out of scope here.
- A real hardware backend implementing `IHapticBackend`.
- Effect queuing/priority policy in `HapticScheduler` if simultaneous
  effects need blending rather than independent dispatch.

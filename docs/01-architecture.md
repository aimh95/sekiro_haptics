# Architecture

*Doc 1 of 6 — start here. Next: [02-dualsense-output.md](02-dualsense-output.md).*

## Goals for this stage

Establish a clean separation between three concerns that will otherwise get
tangled together the moment a real controller and real game hooks show up:

1. **What a haptic effect *is*** (data)
2. **How an effect gets delivered to a device** (backend)
3. **When an effect gets delivered** (scheduling, cancellation, reset)

Nothing in this stage talks to the game, and nothing in the codebase reads
from or injects into a running process. The only `IHapticBackend`
implementation is still `MockHapticBackend` — no code sends a haptic effect
to real hardware.

A separate, narrower piece *does* now talk to real hardware:
`HidApiDualSenseTransport` (see `IDualSenseTransport` below) is a USB HID
transport spike that can enumerate, open, write to, and close a real
DualSense controller. It is deliberately kept behind its own interface,
separate from `IHapticBackend`, and does not know what a `HapticEffect` is
or what bytes a DualSense output report should contain — that keeps "can
we talk to the device at all" verifiable independently of, and before,
"do we send it the right bytes."

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

- `SendEffect(const HapticEffect&) -> HapticBackendResult` — play one
  effect now
- `Reset() -> HapticBackendResult` — stop and return to neutral
- `IsConnected() const -> bool`

`SendEffect` and `Reset` return a `HapticBackendResult`
(`Success` / `NotConnected` / `DeviceError`) rather than `void`, so that a
backend talking to real hardware — which can be unplugged, busy, or
otherwise unable to service a request — can tell callers explicitly when a
request didn't land, instead of failing silently. `MockHapticBackend`
always returns `Success`; a real backend is expected to use the other
values once one exists.

This is the seam where a real DualSense/HID backend would plug in later,
without any caller-facing code changing. `SendEffect` is documented to
return quickly; a backend that needs to sustain an effect over its
`duration` should do that internally (e.g. on its own thread), not by
blocking the caller.

### `MockHapticBackend` (`include/sekiro_haptics/MockHapticBackend.hpp`)

The only `IHapticBackend` implementation in this stage. It:

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

### `IDualSenseTransport` (`include/sekiro_haptics/IDualSenseTransport.hpp`)

A separate, narrower interface from `IHapticBackend`. Where
`IHapticBackend` is "play this `HapticEffect`", `IDualSenseTransport` is
"talk to this USB HID device": it owns HID device discovery and device
lifetime only, and knows nothing about `HapticEffect`, motor intensities,
or what a DualSense output report should contain.

- `EnumerateCandidates() -> vector<HidDeviceInfo>` — enumerate connected
  HID devices and return the ones that look like a Sony DualSense (matched
  by vendor/product id)
- `Open(path) -> TransportResult` — open one device by its OS path
- `Close()` — close the open device, if any; safe to call when nothing is
  open or more than once
- `IsOpen() const -> bool`
- `WriteOutputReport(report, length) -> TransportResult` — write raw,
  caller-constructed bytes; the transport does not interpret them

This split exists so that **HID access and device lifetime belong to the
transport**, while **DualSense packet construction stays outside it** — a
future `DualSenseBackend` (implementing `IHapticBackend`) would build
report bytes and hand them to an `IDualSenseTransport` to write, but the
transport itself has no wire-format knowledge to get wrong.
`TransportResult` (`Success` / `NotFound` / `OpenFailed` / `WriteFailed` /
`NotOpen`) mirrors `HapticBackendResult`'s explicit-failure approach.

### `HidApiDualSenseTransport` (`include/sekiro_haptics/HidApiDualSenseTransport.hpp`)

The only `IDualSenseTransport` implementation, and the only code in this
repo that touches real hardware. Built on
[HIDAPI](https://github.com/libusb/hidapi) (see "New dependency" below).
USB only — Bluetooth is not implemented.

- `EnumerateCandidates()` calls `hid_enumerate()` filtered to Sony's vendor
  id (`0x054C`) and the DualSense's USB product id (`0x0CE6`) — public
  USB-IF identifiers (the same ones the Linux kernel's `hid-playstation`
  driver matches on), not part of any DualSense wire protocol.
- `Open`/`Close` wrap `hid_open_path`/`hid_close`. `Close()` is a no-op
  when nothing is open, and idempotent.
- `WriteOutputReport` wraps `hid_write`, translating hidapi's `-1`-on-error
  convention into `TransportResult::WriteFailed` instead of throwing or
  crashing — this covers a device being unplugged mid-session.
- Every operation logs vendor id, product id, path, connection result, and
  write result to an injectable `std::ostream` (default `std::cout`), the
  same pattern `MockHapticBackend` uses for its log.
- `hid_init()`/`hid_exit()` are process-global HIDAPI calls; this class
  reference-counts them across all live instances (a static mutex-guarded
  counter) so one transport's destructor can't tear down HIDAPI while
  another instance, or the same instance mid-shutdown, is still using it.
  Combined with the destructor always calling `Close()` first, this is what
  keeps device disconnection and program termination from crashing.

`DualSenseBackend` (a full `IHapticBackend` implementation that maps
`HapticEffect`/`MotorIntensity` to output reports and calls through this
transport) is intentionally **not** implemented yet — see "Explicitly out
of scope" below.

### `dualsense_protocol::BuildRumbleReport` (`include/sekiro_haptics/DualSenseUsbReport.hpp`)

Once the transport above was verified against a real controller, a single
manual question remained: does writing an actual output report make it
rumble? `BuildRumbleReport(leftMotor, rightMotor)` builds the minimal
64-byte DualSense USB output report needed to answer that — report id
`0x02`, the "set main motors" flag bits, and the two raw 0-255 motor
bytes. It touches no LED, audio, or adaptive-trigger fields (left zeroed),
and it does no HID/USB I/O itself; it is pure data construction, called by
`apps/dualsense_rumble_test/main.cpp` and handed to
`IDualSenseTransport::WriteOutputReport`. This keeps "DualSense packet
construction outside the transport" intact — the transport still has no
idea what the bytes it writes mean.

The byte layout is not invented: it follows the DualSense USB output
report as implemented in
[flok/pydualsense](https://github.com/flok/pydualsense) (MIT licensed),
specifically `pydualsense.py`'s `prepareReport()` — report id `0x02`
(`OUTPUT_REPORT_USB`), flag byte `outReport[1]` bits `0x01|0x02` ("set the
main motors"), and motor bytes at offsets 3 (right) and 4 (left) of a
64-byte (`output_report_length`) report. No pydualsense code was copied;
`BuildRumbleReport` is fresh code that reproduces only the byte
offsets/values needed for rumble.

`test_dualsense_usb_report.cpp` covers this with hardware-independent
tests (report length/id, motor byte offsets, flag bits, and that
unrelated bytes stay zero). `apps/dualsense_rumble_test/main.cpp` is a
manual hardware test — it opens the first enumerated DualSense, resends a
rumble report for ~1.5s, then sends an all-zero report and closes. It is
not part of `ctest` because "did the controller actually buzz" isn't
something an automated assertion can check.

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
`test_dualsense_transport.cpp` exercises `HidApiDualSenseTransport`'s error
paths and lifetime safety (invalid path, write-when-not-open, repeated
close, construct/destruct ordering) — none of its checks require a real
DualSense to be attached, so the suite stays green on CI or any dev
machine. It is built and linked only when
`SEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT` is on (the default).

### The replay pipeline: `GameSignal` → `GameEvent` → preset → `HapticScheduler`

Everything above this point answers "how does an already-decided
`HapticEffect` reach a backend." Nothing above it answers "how does the
system decide a `HapticEffect` should happen at all." The replay pipeline
is the piece that fills that gap — entirely against recorded traces and
`MockHapticBackend`, since neither Sekiro nor a real DualSense are
available in this environment:

```
Recorded raw game signals
  → ReplaySignalSource
  → GameEventDetector
  → GameEvent
  → EventMapping
  → HapticPreset
  → HapticScheduler
  → MockHapticBackend
```

Each arrow is a real interface boundary, not just a pipeline stage drawn
for documentation purposes:

- **`GameSignal`** (`include/sekiro_haptics/signals/GameSignal.hpp`) is a
  single raw, timestamped observation — a stable string `signal` id (e.g.
  `"manual.perfect_deflect"`), a stringified `value`, and an `extra` map for
  anything else a trace line carried. It knows nothing about what any of
  that *means*. See docs/03-trace-format.md for the full on-disk schema and,
  importantly, what these signal names are and are not claiming about real
  Sekiro internals (nothing — no addresses, offsets, or engine IDs).
- **`IGameSignalSource`** (`include/sekiro_haptics/signals/IGameSignalSource.hpp`)
  abstracts "a stream of `GameSignal`s in timestamp order." Two
  implementations exist: `ReplaySignalSource` (backed by a JSONL trace file
  via `trace::TraceReader`) and `VectorSignalSource` (in-memory, for
  tests). Both support `Reset()` to replay from the start. A future
  `LiveSekiroSignalSource` would implement the same interface on top of
  real process observation and would return `false` from `Reset()` (a live
  stream can't rewind) — that class does not exist; this is a documented
  extension point only.
- **`IGameEventDetector`** (`include/sekiro_haptics/events/IGameEventDetector.hpp`)
  turns `GameSignal`s into `GameEvent`s: `OnSignal()` is called once per
  signal and may emit zero or more events; a default-no-op `Flush()` exists
  as a seam for a future detector that needs to correlate several signals
  across a time window and flush pending state once the stream ends. The
  only implementation is `ManualLabelEventDetector`
  (`include/sekiro_haptics/events/ManualLabelEventDetector.hpp`), which is
  explicitly **not a real Sekiro detector** — it only recognizes
  hand-authored `manual.perfect_deflect`/`manual.take_damage` trace labels,
  purely to exercise the rest of the pipeline deterministically. See
  docs/04-testing.md.
- **`GameEvent`** (`include/sekiro_haptics/events/GameEvent.hpp`) describes
  *what happened* (`gameId`, `eventId`, `timestamp`, `metadata`) and
  nothing about what should be felt.
- **`EventMapping`**/**`HapticPreset`**
  (`include/sekiro_haptics/presets/EventMapping.hpp`,
  `include/sekiro_haptics/presets/HapticPreset.hpp`) split "which preset
  does this event use" from "what does that preset actually feel like" —
  `EventMapping{gameId, eventId, presetId}` and
  `HapticPreset{presetId, displayName, HapticEffect effect}`. Both are
  normally loaded from JSON via `PresetRepository`/`MappingRepository`
  (`include/sekiro_haptics/presets/{Preset,Mapping}Repository.hpp`) —
  see docs/03-trace-format.md for the schema and validation policy. The two
  repositories are loaded independently and do not cross-validate each
  other; that's `ReplayPipeline`'s job, since it's the one component that
  actually needs both at once.
- **`ReplayPipeline`** (`include/sekiro_haptics/pipeline/ReplayPipeline.hpp`)
  wires a detector, both repositories, a `HapticScheduler`, and an
  `IHapticBackend` together. `ProcessSignal()` runs one signal through
  detection → mapping lookup → preset lookup → a connectivity check →
  `scheduler.Schedule()`, returning a `PipelineStepOutcome` per event (or a
  single `NoEventDetected` outcome, which is a normal result, not an
  error). Every failure mode is a typed `PipelineError`
  (`NoMappingForEvent`, `MappingReferencesMissingPreset`,
  `BackendNotConnected`) on that outcome, matching the
  `HapticBackendResult`/`TransportResult` convention elsewhere in this repo
  — never exceptions, never silent failure. `RunReplayLoop()` drains an
  `IGameSignalSource` fully, recording (not aborting on) any malformed
  trace line. `ReplayPipeline` itself never checks whether its signals came
  from a real game or a replay file — nothing in its code path is
  replay-specific.
- **`apps/replay_cli`** is the developer-facing entry point:
  `sekiro_haptics_replay --trace <path> --presets <path> --mappings <path>
  [--fast]` loads both repositories, replays a trace through the pipeline
  against a `MockHapticBackend`, and prints detected events, resolved
  presets, dispatched effects, and a final summary. `--fast` uses
  `RunReplayLoop` directly (sleep-free, deterministic — the same helper the
  test suite uses); without it, the CLI sleeps between signals based on
  their timestamp deltas to approximate real-time playback.

### Migration: from `HapticEffectType` to `presetId`

`HapticEffectType`, `presets::PerfectDeflect()`, and every existing call
site (`apps/console_test/main.cpp`, `test_haptic_effect.cpp`,
`test_scheduler.cpp`) are unchanged and remain fully supported — this is
not a deprecation with a planned removal date, just a statement about
where new growth goes.

New game events should **not** get a new `HapticEffectType` enumerator.
They should get a new `presetId` entry in a presets JSON file (mapped to a
`gameId`/`eventId` in a mappings JSON file) — see docs/03-trace-format.md.
This is why `HapticPreset::effect.type` is always `HapticEffectType::Generic`
for JSON-loaded presets: identity now lives in the string `presetId`, which
scales to arbitrarily many future events without an enum (and a recompile)
per event, the way `HapticEffectType` would have required.

## New dependency: HIDAPI

[HIDAPI](https://github.com/libusb/hidapi) (`libusb/hidapi`) is fetched at
configure time via CMake `FetchContent`, pinned to release tag
`hidapi-0.15.0`, and built as a static library — nothing is vendored or
copied into this repository. On Windows it compiles to the `winapi`
backend, which talks to `hid.dll`/`cfgmgr32.dll` via `LoadLibrary` at
runtime, so no extra system import libraries need to be linked.

License: HIDAPI is offered under the user's choice of the GNU GPL v3, a
BSD-style license, or the original "HIDAPI license" (a short, permissive,
attribution-preserving license — see `LICENSE-orig.txt` in the HIDAPI
repository). This project relies on the permissive option; it links
against HIDAPI rather than copying any of its source, so its license terms
apply only to the fetched HIDAPI build, not to this repository.

## Explicitly out of scope (this stage)

- **Adaptive-trigger / LED / audio packet fields.** `BuildRumbleReport`
  covers rumble only; every other DualSense output-report field (adaptive
  triggers, LEDs, mic mute, speaker) is left zeroed/untouched. No packet
  bytes beyond the minimal rumble set have been invented.
- **`DualSenseBackend`.** No `IHapticBackend` implementation talks to real
  hardware yet. `HidApiDualSenseTransport` (device I/O) and
  `BuildRumbleReport` (packet bytes) both exist and are wired together
  only by the manual `dualsense_rumble_test` app, not by anything
  implementing `IHapticBackend` — mapping `HapticEffect`/`MotorIntensity`
  to motor bytes on a background thread, the way `MockHapticBackend` does
  synchronously today, is still not done.
- **Bluetooth.** `HidApiDualSenseTransport` only opens devices HIDAPI
  reports as USB; there is no Bluetooth HID or DualSense-over-Bluetooth
  handling.
- **Sekiro hooking/injection.** There is no process attachment, memory
  reading, or event capture. `IGameSignalSource`/`GameSignal` anticipate a
  future live source by name and interface shape only; `ManualLabelEventDetector`
  is explicitly test-only (see docs/04-testing.md). No speculative Sekiro
  memory addresses, offsets, or internal event IDs appear anywhere in this
  repo.
- **Real multi-signal correlation.** `IGameEventDetector::Flush()` exists
  as an interface seam; no detector in this repo buffers signals or
  correlates across a time window.
- **PCM haptics / adaptive-trigger commands from the pipeline.**
  `HapticPreset` deliberately holds a full `HapticEffect` (not bare
  left/right floats) so a future preset kind could be added without
  changing `GameEvent` or `EventMapping`, but no such kind exists yet —
  today's presets are legacy rumble only, same as everywhere else in this
  repo.
- **Networking / DSX integration.** No sockets, no external protocol
  support.
- **GUI.** `apps/replay_cli` is a console tool.

## Likely next steps (not implemented)

- A real, live event source: a `LiveSekiroSignalSource` implementing
  `IGameSignalSource` on top of actual process observation — this is the
  piece that would need to hook into Sekiro, out of scope here. The replay
  pipeline (this document, "The replay pipeline" section above) exists
  specifically so this is the *only* new piece a live source would need;
  `IGameEventDetector`, `EventMapping`, `PresetRepository`,
  `HapticScheduler`, and `ReplayPipeline` itself would not change.
- A real, correlating `IGameEventDetector` that looks at more than one
  signal (`ManualLabelEventDetector` is test-only and single-signal).
- `DualSenseBackend`: an `IHapticBackend` implementation that maps
  `HapticEffect`/`MotorIntensity` to `BuildRumbleReport` calls and writes
  them through an `IDualSenseTransport`, so real hardware can plug into
  `HapticScheduler` the same way `MockHapticBackend` does today. This is
  also what the replay pipeline would dispatch through instead of
  `MockHapticBackend`, once it exists — no pipeline code would need to
  change, since `ReplayPipeline` only depends on `IHapticBackend`.
- Adaptive-trigger and LED output-report fields, once rumble-only usage is
  proven out.
- A PCM-capable or adaptive-trigger-capable `HapticPreset` representation,
  once `DualSenseBackend` supports more than legacy rumble.
- Effect queuing/priority policy in `HapticScheduler` if simultaneous
  effects need blending rather than independent dispatch.

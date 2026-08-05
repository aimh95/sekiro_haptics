# SekiroHaptics

A Windows C++20 scaffold for driving controller haptics off Sekiro-flavored
game events (Perfect Deflect, Posture Break, ...), built around a
backend-agnostic effect model so real hardware support can be added later
without touching scheduling or game-event logic.

## Current scope

This is an early architecture skeleton. It deliberately does **not**:

- Talk to a real DualSense controller
- Implement any HID packets or device I/O
- Hook into or inject anything into Sekiro
- Do any networking or DSX integration

Everything runs against `MockHapticBackend`, which only logs effects and
records them in memory.

## Layout

```
include/sekiro_haptics/   Public headers (the library's API)
  signals/, events/, presets/, replay/, pipeline/
                           Game-signal -> event -> preset -> scheduler
                           pipeline (replay-only; see docs/01-architecture.md)
src/                       Library implementation (mirrors include/)
apps/console_test/         Console app: triggers a PerfectDeflect effect
apps/replay_cli/           Replays a JSONL trace through the full pipeline
config/                    Example presets.json / mappings.json for the CLI
tests/                     Unit tests (no external test framework)
tests/fixtures/            JSONL traces and preset/mapping JSON used by tests
docs/01-architecture.md       Design notes and rationale
docs/03-trace-format.md       JSONL trace / presets / mappings JSON schemas
docs/04-testing.md            Test conventions and levels
```

See [docs/01-architecture.md](docs/01-architecture.md) for how the pieces fit
together.

## Requirements

- CMake 3.20+
- A C++20 compiler (MSVC on Windows; GCC/Clang elsewhere for local dev)

No third-party dependencies are used. Threading uses the standard library
(`find_package(Threads)` only links the platform's thread support).

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run the console demo

```sh
./build/apps/console_test/sekiro_haptics_console
```

This triggers a single `PerfectDeflect` effect through `HapticScheduler`
into a `MockHapticBackend` and prints what was received.

## Run the replay CLI

```sh
./build/apps/replay_cli/sekiro_haptics_replay \
  --trace tests/fixtures/perfect_deflect.jsonl \
  --presets config/presets.json \
  --mappings config/mappings.json \
  --fast
```

Replays a recorded JSONL signal trace through the full
signal → event detector → mapping → preset → `HapticScheduler` →
`MockHapticBackend` pipeline and prints detected events, resolved presets,
dispatched effects, and a summary. `--fast` skips the delays between
signal timestamps; omit it for an approximate real-time replay. See
[docs/03-trace-format.md](docs/03-trace-format.md) for the trace/config JSON
schemas and [docs/01-architecture.md](docs/01-architecture.md) for how this
pipeline fits together (it is replay-only -- no real Sekiro event
detection).

## Run the tests

```sh
cd build
ctest --output-on-failure
```

or run the test binary directly for per-test output:

```sh
./build/tests/sekiro_haptics_tests
```

## Build options

| Option | Default | Purpose |
|---|---|---|
| `SEKIRO_HAPTICS_BUILD_APPS` | `ON` | Build the console test app |
| `SEKIRO_HAPTICS_BUILD_TESTS` | `ON` | Build and register the unit test suite |

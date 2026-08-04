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
src/                       Library implementation
apps/console_test/         Console app: triggers a PerfectDeflect effect
tests/                     Unit tests (no external test framework)
docs/ARCHITECTURE.md       Design notes and rationale
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit
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

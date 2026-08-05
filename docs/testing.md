# Testing conventions

This document covers how tests in this repository are named, organized,
and leveled, and what to add when a new game event is introduced. It
complements [docs/ARCHITECTURE.md](ARCHITECTURE.md) (what the pieces are)
and [docs/trace-format.md](trace-format.md) (the JSON/JSONL schemas the
tests below load).

## Test framework

All C++ tests use the in-house header-only framework in
`tests/testing.hpp` (`SH_TEST(Name) { ... }` / `SH_CHECK(condition)`) --
see `docs/ARCHITECTURE.md`'s "Tests" section for why this exists instead of
a third-party framework. There is one test binary
(`sekiro_haptics_tests`) and one `ctest` entry
(`sekiro_haptics_unit_tests`); every `SH_TEST` in every linked `.cpp` file
self-registers and runs as part of that single binary.

## Naming conventions

- **Test files**: `test_<subject>.cpp` under `tests/`, one file per
  component or pipeline stage (e.g. `test_trace_parser.cpp`,
  `test_pipeline.cpp`). A new pipeline stage gets its own file rather than
  being folded into an existing one.
- **Test names**: `SH_TEST(Subject_Action_ExpectedOutcome)`, e.g.
  `ManualLabelEventDetector_PerfectDeflectSignal_EmitsExactlyOneEvent`.
  Subject is the class/free-function under test; Action is the specific
  call or scenario; ExpectedOutcome is what's being asserted -- a test
  name alone should describe its assertion without opening the file.
- **Fixtures**: `tests/fixtures/<scenario>.jsonl` for traces,
  `tests/fixtures/<scenario>.json` for presets/mappings. Name fixtures by
  scenario, not by the test that happens to use them first (e.g.
  `perfect_deflect.jsonl`, not `test1.jsonl`) -- multiple tests reuse the
  same fixture where the scenario matches (see `test_pipeline.cpp` and
  `test_replay_signal_source.cpp` both reading `perfect_deflect.jsonl`).

## Test levels

| Level | What it exercises | Requires |
|---|---|---|
| **Unit** | A single class/function in isolation (`JsonValue`, `TraceReader`, `ManualLabelEventDetector`, `PresetRepository`, `HapticScheduler`, ...) | Nothing external |
| **Replay** | A full signal → event → preset → dispatch run against a recorded JSONL trace and `MockHapticBackend` (`test_pipeline.cpp`, `apps/replay_cli`) | A trace fixture file; nothing external |
| **Integration** | Multiple components wired together beyond a single replay run (none exist yet beyond the replay-level tests above) | Nothing external |
| **Hardware-smoke** | Real USB HID I/O against a real DualSense (`apps/dualsense_rumble_test`, `test_dualsense_transport.cpp`'s error-path checks) | A real DualSense for the manual smoke app; the automated `test_dualsense_transport.cpp` checks themselves do not require one (see `docs/ARCHITECTURE.md`) |
| **Game-acceptance** | Real Sekiro, a real live event source, real hardware, a human judging feel/latency | Sekiro + real hardware + a human -- does not exist yet; see the template below for when it does |

Everything in **Unit** and **Replay** runs in `sekiro_haptics_tests` today
and requires neither Sekiro nor real hardware -- this is the whole point of
the replay pipeline (`docs/ARCHITECTURE.md`, "The replay pipeline"
section). **Hardware-smoke**'s automated half (`test_dualsense_transport.cpp`)
also runs in CI; its manual half (`apps/dualsense_rumble_test`) and all of
**Game-acceptance** require a human and/or hardware that CI doesn't have.

## What every new game event should add

When a new `gameId`/`eventId` is introduced (whether by extending
`ManualLabelEventDetector` for testing, or eventually by a real detector):

1. **A positive fixture** -- a `tests/fixtures/<event>.jsonl` trace where
   the event's triggering signal(s) are present, following the pattern of
   `perfect_deflect.jsonl`.
2. **At least one negative fixture** -- a trace where similar-but-not-quite
   signals appear and the event must **not** fire (`normal_block.jsonl` is
   the existing example for `combat.perfect_deflect`). This is what catches
   an overly broad detector condition.
3. **Detector tests** -- in the relevant `test_*event_detector*.cpp`,
   assert the positive fixture's signal produces exactly the expected
   event and the negative fixture's signals produce none.
4. **Mapping tests** -- in `test_profile_resolution.cpp`, add a mapping
   fixture entry and assert `MappingRepository::Find()` resolves it to the
   expected `presetId`.
5. **Full-pipeline tests** -- in `test_pipeline.cpp`, replay the positive
   fixture through `ReplayPipeline`/`RunReplayLoop` and assert exactly one
   `PipelineStage::Dispatched` outcome with the expected `presetId`, plus
   the negative fixture producing zero dispatches.

## Future test-case ID conventions

Once tests grow beyond what a file name comfortably describes, use these
prefixes for a tracked TC ID (in a test-management tool, PR description,
or a comment referencing this document):

- `EVT-###` -- event-detection test cases (a signal/trace pattern should
  or should not produce a given `GameEvent`)
- `MAP-###` -- mapping/preset-resolution test cases
- `HAP-###` -- full pipeline / haptic-dispatch test cases (what actually
  reaches `HapticScheduler`/a backend)
- `HW-###` -- hardware-smoke or game-acceptance test cases that need real
  hardware and/or a real game

## CI-runnable vs. hardware/game-required

| Runs in CI without Sekiro or hardware | Requires real controller and/or real game |
|---|---|
| All of `test_trace_parser.cpp`, `test_replay_signal_source.cpp`, `test_event_detector.cpp`, `test_profile_resolution.cpp`, `test_pipeline.cpp`, `test_scheduler.cpp`, `test_haptic_effect.cpp`, `test_mock_backend.cpp`, `test_dualsense_usb_report.cpp` | `apps/dualsense_rumble_test` (manual rumble smoke test) |
| `test_dualsense_transport.cpp`'s error-path checks (invalid path, not-open writes, construct/destruct safety -- see `docs/ARCHITECTURE.md`) | Any future `HW-###` game-acceptance case (real Sekiro + real DualSense + a human) |
| `apps/replay_cli` against any trace fixture | |

## Future acceptance-test template

For use once a live signal source and/or real hardware backend exist and a
human can validate an actual in-game event against actual controller
feedback:

```text
TC ID:
Purpose:
Preconditions:
Input trace or live scenario:
Expected game events:
Expected haptic commands:
Allowed latency:
Failure conditions:
Evidence to capture:
```

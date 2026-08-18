# Live signal-discovery probe (`SEK-PROBE-001A`)

*Doc 6 of 6 — previous: [05-process-access.md](05-process-access.md).*

This document covers a **developer-only** tool, not part of the production
haptics runtime: `apps/sekiro_signal_probe`, plus the two read-only
building blocks it's built from --
`IProcessMemoryMap`/`Win32ProcessReader::EnumerateReadableRegions()`
(`include/sekiro_haptics/process/IProcessMemoryMap.hpp`) and the generic
unknown-value candidate scanner
(`include/sekiro_haptics/process/CandidateScanner.hpp`) -- and the session
metadata/watch-capture format
(`include/sekiro_haptics/process/DiscoverySession.hpp`) it writes.

The goal of this stage is narrow, and deliberately **not** "detect Perfect
Deflect": record a real Sekiro build's identity, and safely find and
observe raw memory-value candidates that change alongside real user
actions. Nothing here judges a game event, and nothing here writes to the
target process.

## Safety boundary (this tool specifically)

Everything in [05-process-access.md](05-process-access.md)'s access-rights
boundary applies unchanged: `PROCESS_QUERY_LIMITED_INFORMATION` +
`PROCESS_VM_READ` only, `ReadProcessMemory` only, never
`PROCESS_VM_WRITE`/`PROCESS_VM_OPERATION`/`PROCESS_ALL_ACCESS`, never
`WriteProcessMemory`/`VirtualAllocEx`/`VirtualProtectEx`/
`CreateRemoteThread`, never DLL injection, never a hook, never a game-code
patch. `VirtualQueryEx` (the new capability this stage adds) is used
*only* to query a memory region's own metadata (base, size, state,
protection, type) -- it never changes a region's protection or contents,
and nothing in this codebase ever calls `VirtualProtectEx`. This tool also
never automates game input and never touches save data, achievements, or
Steam state -- it only reads process memory and writes its own local
capture files.

## Readable-region enumeration (`IProcessMemoryMap`)

`Win32ProcessReader` (already the shared implementation of
`IProcessReader`/`IProcessInspector`) also implements `IProcessMemoryMap`,
reusing the same attached handle. `EnumerateReadableRegions()` walks the
*entire* queryable address space via repeated `VirtualQueryEx` calls and
appends only regions that are, all at once: `MEM_COMMIT`; have a
read-granting protection (`PAGE_READONLY`, `PAGE_READWRITE`,
`PAGE_WRITECOPY`, `PAGE_EXECUTE_READ`, `PAGE_EXECUTE_READWRITE`,
`PAGE_EXECUTE_WRITECOPY` -- `PAGE_EXECUTE` alone, with no explicit read
bit, does not count); not `PAGE_GUARD`; not `PAGE_NOACCESS`; non-zero size;
and `base + size` not overflowing the address space. This classification
policy lives in testable, seam-independent code (not hidden inside the
real Win32 call), driven by `RawMemoryRegionInfo` (unclassified OS facts:
committed/guarded/no-access/readable-protection/kind) through
`IWin32Api::QueryNextMemoryRegion()` -- so a Fake can drive every
combination (a guarded region, an uncommitted region, a non-readable
protection, an overflowing region) deterministically, without needing real
OS memory to provoke them. See `test_process_memory_map.cpp`.

**A successful enumeration is a snapshot, not a promise.** The target
process's memory map can change (pages freed, protection changed, the
process exiting) at any moment after `EnumerateReadableRegions()` returns
-- every later read still has to handle failure on its own terms; nothing
here assumes a region stays readable just because it was readable a moment
ago.

## Generic candidate scanning (`CandidateScanner`)

Deliberately **not** built on `AobScanner` -- that scanner searches for a
known byte *pattern*; this one has no pattern, only a value type
(`u8`/`u16`/`u32`/`i32`/`f32`) and a change policy, so it's its own small,
independent implementation.

**Alignment policy**: a candidate is only ever created at an address the
chosen type's own size naturally aligns to (e.g. a `u32` candidate only at
a 4-byte-aligned address) -- raw bytes are never treated as a meaningful
value at an arbitrary offset.

**`BeginCandidateScan(scope, type)`** reads every readable region in
`scope` (`main-module` clips `IProcessMemoryMap`'s `Image`-kind regions to
the attached process's own main module range via
`IProcessInspector::GetMainModule()`; `private-readable` keeps only
`Private`-kind regions; `all-readable` keeps everything) in fixed
`kCandidateScanChunkBytes` chunks, each overlapping the next by
`valueSize - 1` bytes (the same technique as `AobScanner`'s chunk overlap)
so a candidate straddling a chunk boundary is still found exactly once.
Any single chunk read failing aborts the *whole* scan -- a partial
candidate set is never returned as if it were complete. Two public,
named caps bound the work: `kDefaultMaxCandidates` (2,000,000) and
`kDefaultMaxScanBytes` (2 GiB) -- exceeding either fails the call closed
(`CandidateLimitExceeded`), never silently truncating the result.

**`FilterCandidates(kind)`** re-reads every candidate's current value and
applies `kind` (`Changed`/`Unchanged`/`Increased`/`Decreased`/`ExactValue`)
in place: a candidate that no longer passes is removed; a survivor's
stored value becomes its just-read current value (the baseline for the
*next* filter call -- chaining `Increased` directly after `Changed`
without an intervening real change will compare a value against itself
and show nothing as increased; see `test_signal_probe_integration.cpp`'s
integration test for the corrected pattern of branching filters from a
shared earlier baseline). A candidate whose *individual* re-read fails
(hard failure or a partial read, process still attached and alive) is
simply dropped -- it does not fail the whole command, since that's a
per-address condition. If the process itself is not attached or has
exited, the *entire* filter command fails and the candidate list is left
completely unchanged, since those are process-wide conditions no
per-candidate handling can paper over.

**Float comparison policy**: an absolute epsilon
(`kCandidateFloatEpsilon`, `0.0001f`). A `NaN` is never equal to anything,
including another `NaN` (so `NaN` always counts as `Changed`, never
`Unchanged`, and never passes `Increased`/`Decreased` against anything);
`+Infinity`/`-Infinity` compare exactly equal to themselves (checked before
the epsilon subtraction, which would otherwise compute `Infinity -
Infinity = NaN` and wrongly report "not equal").

## Session metadata and watch capture (`DiscoverySession`)

Every live session writes `session.json` once at attach (fields: schema
version, process image path, executable file size, SHA-256, main module
name/base/size, PID, session start wall-clock, selected scope/value type,
sampling interval, tool version, and how the session ended) and again at
exit with the final `endedNormally`/`endReason`. **Only
`executableFileSizeBytes` + `sha256` are a stable build identity** -- PID
and the module base/image-size fields are valid for *that one run only*
(ASLR changes the base every attach), and this file is documented as such
in the struct itself. Nothing here is, or is automatically turned into, a
production `SignatureProfile`; promoting a discovered candidate address
into one is a manual, human decision made later, never automatic.

Watch records (`watch.jsonl`, newline-delimited JSON) are one of two
kinds:
- `{"schemaVersion":1,"timestampUs":..,"recordKind":"sample","signalName":..,"runtimeAddress":"0x..","valueType":..,"value":..}`
- `{"schemaVersion":1,"timestampUs":..,"recordKind":"marker","label":..}`

`timestampUs` is a **session-local monotonic** microsecond counter
(`std::chrono::steady_clock`, never the wall clock) -- it preserves
ordering within one session and is never meant to be compared across
sessions or interpreted as a real timestamp. A `mark <label>` is a
user-entered observation only -- `mark perfect_deflect` records that the
user believes they just saw a perfect deflect; it is not, and never
becomes, an automatic event judgment.

## Disk-backed scanning (`DiskCandidateScanner` / `SignalProbeScanController`)

`CandidateScanner`'s in-memory path holds every candidate as a
`std::vector<Candidate>` -- fine for `main-module` scope, but Sekiro's real
`private-readable` scope is roughly 10-11 GiB across ~5,300 regions. As
`u32` candidates (`sizeof(Candidate)` bytes each) that does not fit in RAM
on a 16 GB development machine, and `BeginCandidateScan` fails closed with
`CandidateLimitExceeded` rather than silently truncating. `SEK-PROBE-001C`
added a second, RAM-bounded path for exactly this case:

- **`CheckInMemoryScanBudget`** computes `comparableValueCount *
  sizeof(Candidate)` from region metadata alone (no process read) and
  compares it against a configured memory budget *before* any candidate
  vector is allocated -- the in-memory path is refused closed, not attempted
  and then killed.
- **`BeginDiskCandidateScan`** reads the full scope once and writes it to
  `<session>/baseline.bin`, using a fixed-size I/O buffer independent of
  scope size -- RAM usage stays bounded by the configured budget (default
  512 MiB) regardless of whether the scope is 10 MiB or 10 GiB.
- **`FilterDiskCandidates`** applies one filter pass, reading from the
  current generation file and writing survivors to the next
  `<session>/candidates-NNNN.bin`, coalescing process reads per I/O chunk
  (dense first filter) or per nearby-address window (sparse later filters)
  rather than one `ReadBytes()` call per candidate.
- Every write follows the project's existing tmp-then-verify-then-rename
  crash-safe publish protocol (see `CandidateStorage.hpp`): a file is only
  ever treated as complete after a full magic/size/count cross-check
  passes, and an interrupted attempt never overwrites the last known-good
  complete generation.

`SignalProbeScanController` (`include/sekiro_haptics/process/
SignalProbeScanController.hpp`) is the OS-independent command/state layer
that wires both paths (`Begin`/`BeginDisk`) plus `Plan`/`Filter`/`Resume`/
`Status`/`List` into the CLI, and `SignalProbeCommandProcessor`
(`SignalProbeCommandProcessor.hpp`) parses one command line and formats a
result -- neither depends on `<windows.h>` or any Win32 handle, so both are
built and tested on Linux with Fakes (`test_signal_probe_controller.cpp`).
`apps/sekiro_signal_probe/main.cpp` stays a thin adapter: real Win32
attach, real identity lookup, and stdin/stdout wiring only.

## CLI usage

```
sekiro_haptics_signal_probe (--pid <pid> | --process-name <name>) --output <dir>
                             [--max-candidates <N>] [--max-scan-bytes <N>]
```

Then, interactively:

| Command | Purpose |
|---|---|
| `identity` | Print the attached process's build identity (file size, SHA-256, main module base/size). |
| `regions` | Summarize readable region counts/bytes by kind. |
| `plan <type> <scope>` | Compute scan estimates only -- **never** touches process memory content and **never** creates a file or session directory. |
| `begin <type> <scope>` | The legacy in-memory scan. Refused closed (`InMemoryBudgetExceeded`) before any allocation if the estimate exceeds the memory budget. |
| `begin-disk <type> <scope>` | The RAM-bounded disk-backed scan. Re-runs the same preflight as `plan`; only creates files if the preflight and the scan both fully succeed. |
| `filter <changed\|unchanged\|increased\|decreased>` | Narrow the active scan (whichever mode is active) by how each candidate's value changed since the last filter. |
| `filter exact <value>` | Narrow to candidates currently holding exactly `<value>`. |
| `resume` | Reattach to the on-disk session in `--output <dir>` after a CLI restart, after validating process/build identity and the data file. |
| `status` | Mode, manifest state, type/scope, generation, candidate count, processed/total bytes, coverage %, memory budget, peak buffered bytes, dropped-unreadable count, current file name, active watch count. |
| `list <count>` | Print up to `<count>` candidates. In disk mode this streams directly from the current baseline/generation file -- it never loads the whole file into memory, so `list 50` is cheap even against a multi-gigabyte baseline. |
| `watch <address> <type> <name>` | Start sampling one address into `watch.jsonl`. Takes a literal address -- copy one straight out of `list`'s output, in either scan mode, with no extra step. |
| `mark <label>` | Write a manual observation marker into `watch.jsonl`. |
| `stop` | Stop all active watches. |
| `quit` | End the session cleanly and write final `session.json`. |

`<type>` is one of `u8`/`u16`/`u32`/`i32`/`f32`; `<scope>` is one of
`main-module`/`private-readable`/`all-readable`.

### `begin` vs `begin-disk`

Always run `plan <type> <scope>` first and read `inMemoryFeasible=`. If it
says `no`, do not raise `--max-candidates`/`--max-scan-bytes` and retry
`begin` -- those two flags are a secondary cap *inside* the in-memory path,
downstream of the memory-budget check, and cannot make an infeasible
in-memory scan feasible; they can only make a *feasible* one hit its old
truncation limit sooner. Use `begin-disk <type> <scope>` instead, which is
designed for exactly this case and keeps RAM bounded regardless of scope
size. On a 16 GB development machine, `private-readable`/`all-readable`
scope over a large game process should be assumed to need `begin-disk`,
not a larger cap.

### Filter order: never `unchanged` first on a disk scan

A fresh disk baseline's first filter must not be `unchanged`: with no
narrowing yet applied, `unchanged` removes almost nothing (a game process
at rest has enormous numbers of values that simply haven't moved),
defeating the whole point of filtering down from a multi-gigabyte
baseline. `begin-disk` correctly rejects an initial
`unchanged` filter (`InitialFilterTooBroad`) -- start with `decreased`,
`increased`, `changed`, or `exact <value>` instead, right after a real,
known value change (e.g. taking damage, for an HP search). `unchanged` is
useful (and accepted) from the *second* filter onward, once the candidate
set is already small.

### Snapshot size and local storage

`begin-disk`'s baseline file is roughly the same size as the scanned
scope itself (header/footer overhead is small and fixed) -- a
`private-readable` scan of a multi-gigabyte process produces a
multi-gigabyte `baseline.bin`. Check `plan`'s `estimatedBaselineFileBytes`
and `outputDriveFreeBytes` before running `begin-disk`, and point
`--output` at a drive with real headroom; `begin-disk` itself re-checks
free space and fails closed (`InsufficientDiskSpace`) before writing
anything if there isn't enough. **`captures/local/` (and any other
directory under it you pass as `--output`) is never committed or uploaded**
-- it is `.gitignore`d by policy, exactly like every other real capture
this tool produces (see "Why runtime addresses are never reused across
attaches" below); treat a real baseline/generation file with the same care
as a real address or hash.

### Resuming after a restart

If the CLI is closed (or crashes) mid-session, relaunch it against the
**same** `--output <dir>` and the **same** target process/build, then run
`resume`. `resume` re-validates the on-disk manifest's identity (file
size, SHA-256, PID, main module base and image size, value type, scope)
against the freshly-attached process, confirms the process is still alive,
and validates the current data file's own header/footer/size before
restoring the session -- any mismatch is refused (`SessionMismatch`), and
an incomplete `.tmp` left behind by an interrupted filter is never treated
as a resume target. On success, `filter`/`status`/`list` continue exactly
where the previous run left off.

### Recommended HP-search order

1. `plan u32 private-readable` (or the narrowest scope you have a real
   reason to expect the value in) -- confirm feasibility and disk headroom.
2. `begin-disk u32 private-readable`.
3. Take a hit in-game (a real, known HP decrease).
4. `filter decreased`.
5. Take another hit (or let HP regenerate/heal, if applicable) and continue
   narrowing with `decreased`/`increased`/`changed`/`unchanged`/`exact
   <value>` until `list` shows a small, plausible candidate set.
6. `watch <address> u32 hp` on the remaining candidate(s) to confirm the
   value tracks HP specifically (not some other counter that happened to
   move the same way once) before treating it as a real discovery.

## Linux development vs. Windows verification

This repository's core scan/controller logic (`CandidateScanner`,
`CandidateScanShared`, `ScanManifest`, `CandidateStorage`,
`DiskCandidateScanner`, `SignalProbeScanController`,
`SignalProbeCommandProcessor`) is OS-independent and is developed, built,
and tested on native Linux using `IProcessReader`/`IProcessInspector`/
`IProcessMemoryMap` Fakes and real temporary files -- **no real Sekiro
process, no Win32 API, no hardware**. This Linux environment is
**core-development-only**: it can never attach to a real Windows process,
so it can never run `apps/sekiro_signal_probe` itself, the Win32-only
integration tests (`test_win32_process_reader_integration.cpp`,
`test_aob_scanner_integration.cpp`, `test_signal_probe_integration.cpp`,
`test_disk_candidate_scanner_integration.cpp`), or any real Sekiro live
acceptance run.

**WSL is not a substitute for real Windows here either.** Even under WSL,
a Linux kernel cannot open a handle to a native Windows process
(`OpenProcess`/`ReadProcessMemory` have no meaning across that boundary),
so a Sekiro process running on the Windows side of a WSL machine is just
as unreachable from WSL as from any other Linux box. Real building,
testing, and live acceptance against Sekiro all happen in a genuine
**Windows PowerShell** session on Windows-native tooling.

**Never reuse a Linux or WSL build directory on Windows.** CMake build
directories are toolchain- and platform-specific (compiler, ABI, generator,
cached paths) -- always run a fresh `cmake -S . -B build` (a new directory,
e.g. `build-win`) from a Windows shell rather than pointing Windows tooling
at a `build/` directory a Linux `cmake` run already configured.

**Before rebuilding on Windows, `quit` any already-running probe session.**
The running `sekiro_haptics_signal_probe.exe` (and the DLLs next to it)
stay locked by the OS while the process is alive -- a rebuild while it's
still running will fail to overwrite the `.exe`/`.dll` files. Type `quit`
in the probe's own prompt (or close its window) before re-running the
build.

**MinGW DLL/PATH troubleshooting.** If building with MinGW-w64 g++ rather
than MSVC, the produced `.exe` dynamically links the MinGW runtime
(`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) unless the
build passes static-linking flags. If launching the probe fails with a
missing-DLL error, either add MinGW's `bin` directory to `PATH` for that
shell, copy the three DLLs next to the `.exe`, or reconfigure with static
linking (e.g. `-static-libgcc -static-libstdc++ -static`) so the `.exe` is
self-contained.

## Why runtime addresses are never reused across attaches

ASLR relocates a module (and, transitively, anything reachable only via a
fixed offset from it) to a new base essentially every process launch. A
runtime address recorded by one live session is only meaningful for *that*
attach -- it is never written into source, a committed fixture, or a
production `SignatureProfile`. Live captures (including any real
build hash discovered this way) are written only to a local, gitignored
capture directory (see `.gitignore`'s `captures/local/` entry), and this
document contains no real Sekiro pattern, address, offset, or hash, by
policy.

## What this tool does not do

- No `GameSignal`, no event type (`PerfectDeflect`, `NormalBlock`,
  `TakeDamage`, `PostureBreak`, `Deathblow`), and no rule mapping a
  candidate's change to any of them -- this stage's output is raw value
  samples, manual markers, candidate addresses, and build identity only.
- No pointer-chain resolution -- a discovered candidate address is exactly
  what `watch` reports, nothing chased through it.
- No automated input, no save/achievement/Steam interaction, no game-code
  modification of any kind (see the safety boundary above).

## CI-runnable tests

Fully portable (build and run on Linux, no `<windows.h>`, Fakes + real
temporary files only): `test_process_memory_map.cpp` (fake `IWin32Api`),
`test_candidate_scanner.cpp`, `test_candidate_storage.cpp`,
`test_disk_scan_plan.cpp`, `test_disk_baseline_scan.cpp`,
`test_disk_first_filter.cpp`, `test_disk_generation_chain.cpp`,
`test_disk_scan_resume.cpp`, and `test_signal_probe_controller.cpp` (fake
`IProcessReader`/`IProcessInspector`/`IProcessMemoryMap`, covering
`SignalProbeScanController`'s and `SignalProbeCommandProcessor`'s full
`plan`/`begin`/`begin-disk`/`filter`/`resume`/`status`/`list` behavior).

Win32-only (spawn and attach to a real process; registered in CMake only
under `if(TARGET sekiro_haptics_win32_process)`, never executed in this
Linux sandbox): `test_win32_process_reader_integration.cpp`,
`test_aob_scanner_integration.cpp`, `test_signal_probe_integration.cpp`
(the in-memory scan/filter/watch path), and
`test_disk_candidate_scanner_integration.cpp` (the disk-backed
plan/begin-disk/filter-chain/resume path, reusing the same helper process
as the others, extended with a simulated CLI-restart via a second
independent `SignalProbeScanController` instance). All of these use a
real, self-contained, non-game helper process
(`process_reader_helper_main.cpp`) with independent
sentinel/increment/decrement/toggle/noise watch values and a
command/acknowledgement pipe protocol -- no fixed `sleep_for` anywhere.
**These have only been written and CMake-registered from this Linux
sandbox; they have not been run here and must be executed on a real
Windows machine before being reported as verified.**

Real Sekiro live acceptance (the `sekiro_haptics_signal_probe` CLI
attached to an actual running game) is a manual procedure a human runs and
judges -- see the ticket report for the exact steps; it is not automated
and not part of the test suite.

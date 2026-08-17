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

`test_process_memory_map.cpp` (fake `IWin32Api`),
`test_candidate_scanner.cpp` (fake `IProcessReader`/`IProcessInspector`/
`IProcessMemoryMap`), and `test_signal_probe_integration.cpp` (a real,
self-contained, non-game helper process extended with independent
sentinel/increment/decrement/toggle/noise watch values and a
command/acknowledgement pipe protocol, no fixed `sleep_for` anywhere) all
run in CI without Sekiro or hardware. Real Sekiro live acceptance (the
`sekiro_haptics_signal_probe` CLI attached to an actual running game) is a
manual procedure a human runs and judges -- see the ticket report for the
exact steps; it is not automated and not part of the test suite.

# Read-only process access, executable identity, and AOB scanning (`SEK-READ-001A`–`001C`)

*Doc 5 of 6 — previous: [04-testing.md](04-testing.md), next: [06-signal-discovery-probe.md](06-signal-discovery-probe.md).*

This document covers three closely related, deliberately separate
responsibilities living under `include/sekiro_haptics/process/`:

- **`IProcessReader`/`Win32ProcessReader`** (`SEK-READ-001A`): finding,
  attaching to, and reading raw bytes from an external Windows process.
- **`IProcessInspector`/`ExecutableIdentity`** (`SEK-READ-001B`):
  identifying *what* an already-attached process's executable is --
  its full path, its main module's runtime placement, and a build
  identity stable enough to recognize "the same executable" across runs.
- **`AobPattern`/`AobScanner`/`RipRelative`** (`SEK-READ-001C`): parsing an
  array-of-bytes pattern, searching a validated memory range for it, and
  resolving an x64 RIP-relative (`rel32`) address from a unique match.

All three are deliberately **not** Sekiro-specific -- see "What this stage
does not support yet" below for the exact boundary.

## Why two interfaces instead of one bigger `IProcessReader`

`IProcessReader` stayed exactly as narrow as `SEK-READ-001A` left it:
attach, detach, liveness, PID, raw byte reads. Path/module/identity
queries are a **different kind of responsibility** -- reading a live
process's memory says nothing about needing to know what executable it
*is*, and vice versa -- so they live on a separate interface,
`IProcessInspector`, rather than growing `IProcessReader`'s contract (and
every existing consumer's mental model of it).

`Win32ProcessReader` implements *both* interfaces on the same object,
reusing the same attached OS handle, instead of standing up a second class
that would need its own (redundant) `OpenProcess` call to the same PID.
`ComputeExecutableIdentity()`/`BuildExecutableIdentity()`
(`ExecutableIdentity.hpp`) are free functions, not interface methods, for
a third reason: computing a SHA-256 digest is pure file I/O against a
path already on disk -- it needs no live process, no `IProcessReader`, and
no `IProcessInspector` at all, so it's directly testable against plain
temp files (see `test_executable_identity.cpp`) rather than needing a
Fake process to exist.

## What this layer is responsible for

- Finding a running process, by PID or by exact executable filename.
- Opening it with the minimum access rights needed to read its memory.
- Reporting whether it's still attached, and whether the attached process
  is still alive.
- Reading raw bytes at a caller-supplied address, with an explicit,
  typed result for every way that can fail or partially succeed.
- Querying an attached process's own full executable path.
- Enumerating and identifying its main module's runtime base
  address/image size.
- Computing a stable build identity (file size + SHA-256) for an
  executable on disk.

That's the whole scope. Nothing here has any idea what the bytes read
through `ReadBytes()` *mean* -- it doesn't know about Sekiro, HP, posture,
or any other game concept, and it never will. Interpreting raw bytes
(offsets, pointer chains, an AOB scanner, matching a build via a
signature/version *profile* so the right offsets get selected) is
explicitly `SEK-READ-001C — AOB Scanner and Address Calculations`'s job,
kept behind these same interfaces so nothing above them needs to change
when that arrives.

## Access rights requested

Exactly `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ`
(`sekiro_haptics::process::kProcessAccessMask`,
`include/sekiro_haptics/process/IProcessReader.hpp`). Nothing else is ever
requested from `OpenProcess`. `src/process/Win32ProcessReader.cpp`
`static_assert`s this constant against the real Win32 macros, and a
Fake-API unit test (`test_win32_process_reader.cpp`) asserts the exact
value Win32ProcessReader actually passes to `OpenProcess` on every attach
-- the permission boundary is enforced at both compile time and test time,
not just documented.

## Explicitly never used

None of the following API or access rights appear anywhere in this
module, and none should ever be added to it:

- `PROCESS_ALL_ACCESS`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION`
- `WriteProcessMemory`
- `VirtualAllocEx`, `VirtualProtectEx`
- `CreateRemoteThread`, `SetThreadContext`
- Any DLL-injection API

This is not about protecting achievements specifically (that constraint
was dropped for this project) -- it's about game stability and save-data
safety. A tool that only ever reads never has a way to corrupt the target
process's state, no matter how buggy it is.

`IProcessInspector`'s path/module queries need no additional access
rights: `QueryFullProcessImageNameW` works with the
`PROCESS_QUERY_LIMITED_INFORMATION` already in `kProcessAccessMask`, and
module enumeration uses a `CreateToolhelp32Snapshot` snapshot (a separate
OS mechanism, not an extra access right on the target process's handle).
`ExecutableIdentity`'s SHA-256 hashing uses Windows' own CNG API
(`<bcrypt.h>`, linked via `bcrypt` -- no third-party crypto library added)
against the executable file on disk, not the live process at all.

## Attach / read / detach lifecycle

- **`AttachByPid(pid)`** opens that PID directly.
- **`AttachByName(exeFileName)`** enumerates running processes and matches
  the executable filename exactly, case-insensitively (no substring
  matching). Zero matches is `ProcessNotFound`; more than one is
  `MultipleMatches` -- this reader never guesses which process was meant.
  A caller facing `MultipleMatches` must resolve the ambiguity itself and
  retry via `AttachByPid`.
- **Reattach contract**: either attach method only replaces the current
  attachment *after* the new handle is confirmed open. A failed attach
  attempt (not found, ambiguous, or `OpenProcess` itself failing) leaves
  any existing attachment completely untouched -- a good connection is
  never lost because a *later* attach attempt failed. See
  `Win32ProcessReader_FailedReattachByPid_PreservesExistingAttachment` and
  its by-name counterpart.
- **`Detach()`** closes the handle if one is open. Safe to call when not
  attached, and safe to call repeatedly -- only the first call in a row
  actually closes anything.
- **`IsAlive()`** reflects whether the attached process is still running;
  `false` if nothing is attached at all.
- The destructor calls `Detach()`, so a handle is never leaked by going
  out of scope. Copying is disabled (a copy would double-close the
  handle); move is not supported either -- there is always exactly one
  owner.

## Typed result meanings (`ProcessReaderResult`)

| Result | Meaning |
|---|---|
| `Success` | The operation completed exactly as requested. |
| `NotAttached` | `ReadBytes()` called with nothing attached. |
| `ProcessNotFound` | `AttachByName()` found no matching process. |
| `MultipleMatches` | `AttachByName()` found more than one; caller must use `AttachByPid()`. |
| `OpenFailed` | `OpenProcess` failed for a specific PID (access denied, already exited, ...). |
| `ProcessExited` | `ReadBytes()` detected the attached process is no longer running, before attempting the read. |
| `InvalidArgument` | A null destination buffer, or an `address + size` that would overflow the address space, was passed to `ReadBytes()`. |
| `ReadFailed` | The underlying read call itself failed for a still-running process (e.g. unmapped/protected address). |
| `PartialRead` | The read call reported success but returned fewer bytes than requested. **Never** treated as `Success` -- `destination`'s contents must not be used. |

`ReadBytes()`'s full precedence, in order: not attached → null destination
→ zero-size (trivially `Success`, no OS call made, regardless of process
state) → address+size overflow → process no longer alive → the actual
read (failed / partial / success). A caller only ever trusts
`destination`'s bytes on `Success`.

## Executable path and main module (`IProcessInspector`)

- **`GetImagePath()`** returns the attached process's own full executable
  path as a `std::filesystem::path` -- backed by the OS's native wide
  string representation on Windows, so a non-ASCII path component (a
  non-English username, for example) round-trips losslessly. The real
  implementation uses a 32K-character buffer with
  `QueryFullProcessImageNameW`, which *fails* (rather than silently
  truncating) if a path somehow still didn't fit -- there is no path-length
  edge case that reports success with a cut-off path.
- **`GetMainModule()`** does not assume the main module is module
  enumeration's first entry. It compares the process's own image path
  (from `GetImagePath()`) against every enumerated module's path,
  case-insensitively after lexical normalization, and only succeeds if
  that comparison resolves to **exactly one** module. Zero or multiple
  matches is `MainModuleNotFound` either way -- this deliberately does not
  attempt to resolve symlinks/junctions/path aliases to disambiguate a
  multi-match case; that's out of this module's supported range, and is
  reported as a failure rather than a guess.
- **`FindModuleExact(name)`** matches a loaded module by exact,
  case-insensitive basename -- no substring matching, same convention as
  `AttachByName`. Zero matches is `ModuleNotFound`; more than one
  identically-named module is `MultipleModules`.

### Unicode module-name matching

`ModuleEnumEntry::name` (`IWin32Api.hpp`) is kept as `std::wstring`, exactly
as reported by `Module32FirstW`/`NextW` -- never narrowed. An earlier
version of this module narrowed it through `NarrowAsciiApprox()`, replacing
every character above `0x7F` with `'?'`; that let two genuinely different
non-ASCII module names collide into the same approximated string (e.g.
`café.dll` and `cafè.dll` both became `caf?.dll`), which could make
`FindModuleExact` match the wrong module. `NarrowAsciiApprox()` has been
removed.

Matching now happens on the wide strings directly, via
`CompareStringOrdinal(..., TRUE)` (`EqualsIgnoreCaseWide()` in
`Win32ProcessReader.cpp`) -- Windows' own ordinal, case-insensitive,
Unicode-correct comparison, used instead of a hand-rolled `towlower()` loop
(locale-dependent, not guaranteed correct across the full Unicode range).
The same helper now also backs `GetMainModule`'s path comparison
(`PathsEqualIgnoreCase`), for the same reason. `FindModuleExact`'s public
parameter stays `const std::string&` (UTF-8, this project's convention for
narrow strings that may carry non-ASCII content) -- it's converted to wide
via `MultiByteToWideChar(CP_UTF8, ...)` once per call and compared against
each module's already-wide name; nothing is narrowed before comparing.
`ModuleInfo::name` (the public, returned struct) is populated via
`WideCharToMultiByte(CP_UTF8, ...)` -- still a plain `std::string`, so
`IProcessInspector`'s public shape is unchanged, but now a lossless UTF-8
encoding of the real name instead of a lossy ASCII approximation. Two
identical Unicode names always match; two different Unicode names never
match, including pairs that would have collided under the old
approximation; a case-only difference (including a case difference on a
non-ASCII letter, e.g. `É`/`é`) matches, while a difference in the actual
base character (e.g. `é` vs `e`) does not -- see
`Win32ProcessReader_FindModuleExact_UnicodeName_*` in
`test_win32_process_reader.cpp`.
- Every returned `ModuleInfo` has been validated: `baseAddress != 0`,
  `imageSize != 0`, and `baseAddress + imageSize` does not overflow the
  address space. Any of those failing is `InvalidModuleRange`, and
  `outModule` is left untouched -- a caller never receives a
  partially-filled `ModuleInfo`.

## Executable identity: build identity vs. runtime placement

`ExecutableIdentity` (`ExecutableIdentity.hpp`) deliberately keeps two
different kinds of information side by side without conflating them:

| | Fields | Stable across... |
|---|---|---|
| **Build identity** | `fileSizeBytes`, `sha256` | Every run of the same executable file, regardless of load address, path used to launch it, or command line. |
| **Runtime placement** | `mainModuleBaseAddress`, `mainModuleImageSize` | Nothing -- changes every run under ASLR, even for the exact same build. |

**Why SHA-256, not just file size or path**: file size alone collides
easily (two different builds can happen to be the same number of bytes);
a path only says where a file *was*, not what bytes it contains, and says
nothing if the same build is copied or renamed. A cryptographic hash of
the full file content is the only one of the three that's actually tied to
*what the executable contains* -- see
`ComputeExecutableIdentity_DifferentPathsSameContent_SameBuildIdentity`
and `..._DifferentContent_DifferentBuildIdentity` in
`test_executable_identity.cpp`.

**Why not the module base address**: ASLR relocates a module to a
different base address essentially every process launch. Two attaches to
the exact same build, seconds apart, will very likely report two
different `mainModuleBaseAddress` values -- using it (or the launch path)
as part of "is this the same build" would make identical builds look
different from each other. `mainModuleBaseAddress`/`mainModuleImageSize`
exist in `ExecutableIdentity` purely as *this run's* placement info (useful
to a future address-resolution step), never as an input to a build-equality
check -- see `ComputeExecutableIdentity_ASLRLikeDifferentBase_SameFile_SameBuildDigest`.

**Hashing contract**: `ComputeExecutableIdentity()` reads the entire file
and compares the number of bytes actually read against the file's own
reported size; a mismatch -- including a file modified/truncated during the
read -- is `FileReadFailed`, never `Success`. A file that can't be opened at
all (missing, a directory, locked) is `FileOpenFailed`. A hashing-API
failure is `HashFailed`. Every one of these leaves `outIdentity` untouched
-- see `ComputeExecutableIdentity_FailedCall_NeverLeavesPartiallyFilledIdentity`.

**Single-handle file snapshot and sharing-mode contract**: earlier, the
size query (`std::filesystem::file_size(path)`) and the read
(`std::ifstream` opened separately by path) were two independent
filesystem operations -- a same-size content change landing in the window
between them couldn't be detected, since the only check was "bytes read ==
size queried earlier," and a same-size overwrite satisfies that trivially.
`ComputeExecutableIdentity()` now opens the file exactly **once**, via
`CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, ..., OPEN_EXISTING, ...)`,
and performs both the size query (`GetFileSizeEx`) and the full read
(`ReadFile`, in a loop) against that **same** `HANDLE` (`Win32FileReader`,
`src/process/ExecutableIdentity.cpp`). `FILE_SHARE_READ` only (no
`FILE_SHARE_WRITE`, no `FILE_SHARE_DELETE`) means: for as long as this
handle stays open, any other handle opened against the same file that
requests write or delete access is denied by the OS with a sharing
violation -- so an ordinary overwrite, truncate, or delete-and-replace
cannot succeed while a hash is in progress. This is the standard Windows
mandatory-locking guarantee for *cooperating* opens; it is **not** an
absolute guarantee against every conceivable modification path (e.g. a
raw-volume write that bypasses the filesystem's own sharing checks), and
this module does not claim otherwise -- if the read loop or the final
byte-count check ever can't confirm the file was read in full and
unchanged, the result is `FileReadFailed`, never a guessed `Success`. A
short read, a mid-read failure, or a size query that doesn't match what
was actually read are all treated identically: fail closed, never partial.

**File-read and hash seam**: to make every one of `FileReadFailed`'s and
`HashFailed`'s distinct causes deterministically testable (not dependent on
winning a race against a background thread), `ComputeExecutableIdentity()`'s
actual logic lives in `ComputeExecutableIdentityWithSeams()`
(`include/sekiro_haptics/process/FileHashSeam.hpp`,
`src/process/ExecutableIdentity.cpp`), parameterized over two small
interfaces:

- **`IFileReader`**: `Open`/`GetSize`/`Read`/`Close`, matching
  `Win32FileReader`'s single-handle shape above.
- **`ICryptoApi`**: the five distinct steps a BCrypt SHA-256 computation
  involves -- `OpenAlgorithmProvider`, `GetHashObjectLength`
  (`BCryptGetProperty(BCRYPT_OBJECT_LENGTH)`), `CreateHash`, `HashData`,
  `FinishHash` -- plus `DestroyHash`/`CloseAlgorithmProvider` for cleanup.

`ComputeExecutableIdentity()` (the public function callers use) is a thin
wrapper that calls `ComputeExecutableIdentityWithSeams()` with the real
`Win32FileReader`/`Win32CryptoApi`. This mirrors `IWin32Api`'s role for
`Win32ProcessReader`, but is deliberately **not** folded into
`IWin32Api`/`IProcessReader`/`IProcessInspector` -- file hashing is a
different responsibility (pure file I/O plus CNG, no live process
involved), so it gets its own minimal, separately-scoped seam instead of
bloating those interfaces. `tests/FakeFileHashSeam.hpp` provides
`FakeFileReader`/`FakeCryptoApi`, used by `test_executable_identity.cpp` to
deterministically inject: a size-query failure, an explicit read failure at
a specific call (including one before what would have been the last
chunk), a short read (reported size larger than what the fake ever
actually delivers), and each of the five CNG steps failing independently
-- every one of these re-runs the same way every time, and each also
asserts the corresponding cleanup happened exactly once (no leaked
algorithm-provider/hash-object handle, no double-close) and that a failure
never corrupts a subsequent, separate successful call. The previous
race-based test
(`ComputeExecutableIdentity_ConcurrencySmokeTest_FileTruncatedDuringRead_HandlesRaceEitherWaySafely`)
is kept only as a best-effort concurrency smoke test, explicitly not
counted as verifying any specific branch.

`BuildExecutableIdentity(inspector, outIdentity)` is the convenience path
for the common case: `GetImagePath()` + `GetMainModule()` +
`ComputeExecutableIdentity()` against an already-attached process, failing
closed at the first failing step.

## AOB pattern scanning and RIP-relative resolution (`SEK-READ-001C`)

Three small, independent building blocks live alongside the two above,
under the same `include/sekiro_haptics/process/` directory:

- **`AobPattern.hpp`/`AobPattern.cpp`**: a strict text-to-byte/mask parser
  (`ParseAobPattern`) plus the shared `AobScanResult` enum every function
  below returns.
- **`AobScanner.hpp`/`AobScanner.cpp`**: `ScanBuffer()` (a pure in-memory
  scan) and `ScanProcessRange()` (a chunked, `IProcessReader`-bounded scan).
- **`RipRelative.hpp`/`RipRelative.cpp`**: `ResolveRipRelativeAddress()`
  (pure arithmetic/validation) and its two displacement-reading wrappers,
  `ResolveRipRelativeFromBuffer()`/`ResolveRipRelativeFromProcess()`.

None of these know anything about Sekiro or any other specific game --
same boundary as `IProcessReader`/`IProcessInspector` above. No real
signature, address, or offset is used anywhere in this repository; every
example in this doc and in the test suite is synthetic (made-up bytes and
made-up target addresses for the purpose of proving the arithmetic and
scanning logic, nothing more).

### AOB pattern grammar

Whitespace-separated tokens, each either:
- An **exact byte**: exactly two hex digits, case-insensitive (`"48"`,
  `"8b"`).
- A **wildcard**: `"?"` or `"??"`.

Rejected as `InvalidPattern` (never partially parsed): an empty pattern; a
single hex digit alone (only wildcards may be one character); a partial
wildcard mixing a hex digit and `'?'` (`"4?"`, `"?F"`); any token three or
more characters long; malformed hex; a pattern made entirely of
wildcards (no anchor byte at all); a pattern longer than
`kMaxAobPatternBytes` (256) bytes. A parsed `AobPattern` keeps byte values
and their exact/wildcard mask as two parallel arrays -- never a single
lossy representation.

### Unique-match principle

Both `ScanBuffer()` and `ScanProcessRange()` scan the **entire** given
buffer/range regardless of where the first match lands -- a scan never
stops early and never returns the first match found. Zero matches is
`NoMatch`; more than one is `MultipleMatches`; only exactly one match is
`Success`. This applies uniformly everywhere a byte pattern is searched
for in this module, with no exception -- an address behind an ambiguous
(zero or multiple match) scan is never used, by design, the same
fail-closed convention `IProcessInspector::FindModuleExact` already
established for module names.

### Process range scanning: chunking and overlap

`ScanProcessRange()` never assumes an entire module is safely readable in
one unbounded allocation. It reads the caller-supplied `[rangeBase,
rangeBase + rangeSize)` in fixed `kAobScanChunkBytes` (64 KiB, exposed as a
named constant so tests can deliberately target a boundary) pieces. Each
chunk's read is extended `pattern.size() - 1` bytes past its own "owned"
portion (bounded by the end of the requested range) so a pattern starting
near the end of one chunk but finishing in the next is still found -- but
only start offsets within a chunk's own owned portion are ever counted as
candidate matches there, so a boundary-straddling match is attributed to
exactly one chunk and never counted twice.

Any single chunk read failing -- `NotAttached`/`ProcessExited`/
`ReadFailed`/`PartialRead`, mapped 1:1 from the underlying
`ProcessReaderResult` -- aborts the whole scan immediately; any matches
already found in earlier, successfully-read chunks are discarded, never
partially reported. Liveness is re-checked once more after the full range
has been read and before declaring success, closing the window where the
process exits in the instant after the last chunk read. The single
match's address (plus its full pattern length) is re-verified to fall
completely inside the requested range before being returned.

### RIP-relative (`rel32`) calculation

```
instructionAddress = matchAddress + instructionOffset
target = instructionAddress + instructionLength + sign_extend(disp32)
```
where `disp32` is the 4 little-endian bytes at `matchAddress +
displacementOffset`. A signature match's start and the actual instruction
it identifies are not always the same address, so `RipRelativeSpec`
expresses both independently (`instructionOffset`, `displacementOffset`),
rather than assuming the displacement sits at a fixed offset from the
match.

Every one of these is validated, in this order, before a target is ever
returned: the source/target ranges themselves (non-zero base and size, no
`base + size` overflow); the 4 displacement bytes lying fully within
`[instructionOffset, instructionOffset + instructionLength)`
(`InvalidDisplacementLayout` otherwise); `matchAddress +
instructionOffset`, the displacement's own end address, and
`instructionAddress + instructionLength` each not overflowing the address
space (`AddressOverflow`); the instruction and its displacement bytes
falling within the allowed **source** range (`InvalidRange`); the final
`instructionAddress + instructionLength + disp32` not underflowing below
address 0 (`AddressOverflow`); and the computed target being non-zero and
falling within the allowed **target** range (`TargetOutOfRange` otherwise
-- including for a target that computes to exactly 0, which is never
treated as valid regardless of what the target range happens to be).
Nothing here ever widens or guesses at an out-of-range target -- see
`test_rip_relative.cpp`'s `TargetOutOfRange`/`TargetIsExactlyZero` cases.

The 4-byte displacement can be supplied from an in-memory buffer
(`ResolveRipRelativeFromBuffer`, used by unit/fixture tests against a fake
module image) or read live via `IProcessReader`
(`ResolveRipRelativeFromProcess`, used by the real Win32 integration
test) -- both delegate every validation to the same pure
`ResolveRipRelativeAddress()`, so the arithmetic/range logic is tested
exactly once regardless of where the bytes came from.

### AOB is not a substitute for version identity

A unique pattern match today says nothing about tomorrow -- a different
build of the same executable can shift, remove, or duplicate the exact
bytes a pattern was written against, silently turning a `Success` into a
`NoMatch`, a `MultipleMatches`, or (worse) a *different* unique match at
the wrong address. Nothing in `AobScanner`/`RipRelative` verifies "this is
the build I think it is" -- that's `ExecutableIdentity`'s job
(`fileSizeBytes` + `sha256`, see above). Any future caller of this module
is expected to gate pattern-based address resolution behind a confirmed
build identity match first; scanning is not, on its own, a safety net
against running against the wrong build.

## Signature profiles and single-step address resolution (`SEK-READ-001D`)

Built on top of everything above: `SignatureProfile`/`SignatureProfileRepository`
(`include/sekiro_haptics/process/SignatureProfile.hpp`,
`SignatureProfileRepository.hpp`) and `AddressResolver`
(`AddressResolver.hpp`).

A `SignatureProfile` ties one exact build (`fileSizeBytes` + `sha256`,
compared exactly against `ExecutableIdentity` -- never by path, filename,
module base, image size, or a partial/nearest match) to a list of named
`AddressSpec`s. Each `AddressSpec` is entirely **module-relative**: a scan
offset/size, an AOB pattern (parsed via the same `ParseAobPattern()` from
above), a resolution kind (`match_address` with a signed offset, or
`rip_relative_32` reusing `RipRelative.hpp` unchanged), and an allowed
target range -- never a stored runtime absolute address.
`SignatureProfileRepository::LoadFromFile()` is strict and whole-file
fail-closed: any single invalid profile rejects the entire file, and a
failed load never partially overwrites profiles from an earlier successful
load. `SignatureProfileRepository::SelectFor()` only ever returns a
profile whose build identity matches exactly; zero matches is
`UnsupportedBuild`, more than one is `AmbiguousProfile` -- never a
fallback to "closest."

`AddressResolver::ResolveAddressesForIdentity()` orchestrates: select a
profile (or fail the whole attempt closed, without touching the process at
all, if no profile matches) -> for each `AddressSpec` independently, exact
module lookup -> module-relative scan/target range validated against the
module's actual runtime bounds -> unique AOB scan (`ScanProcessRange()`,
unchanged) -> resolution-kind-specific calculation -> final target-range
check -> `Resolved` or `Disabled` (with a specific reason). One address's
failure never affects another's, and this layer still never dereferences a
pointer -- it resolves exactly one address per `AddressSpec` and stops.

## What this stage does not support yet

- No pointer-chain dereferencing -- `RipRelative`/`AddressResolver` each
  resolve exactly one address per spec; chasing a multi-level pointer
  chain from a resolved address remains a separate, not-yet-built layer
  (paused for `SEK-PROBE-001A`, see below; still open for a future
  `SEK-READ-001E`).
- No real Sekiro (or any other game's) memory addresses, offsets, profile
  entries, or signature bytes anywhere in this repository.
- No automatic promotion of anything discovered by the signal-discovery
  probe (`06-signal-discovery-probe.md`) into a `SignatureProfile` -- a
  live capture's addresses are only ever valid for the run that produced
  them (ASLR), and only a human deciding a candidate is real and stable
  moves it into a profile, by hand, later.
- No `GameSignal` production. This module produces raw bytes, identity
  facts, and resolved addresses only; turning them into a `GameSignal` (or
  deciding a signal is `unavailable` because a read/scan failed) is
  `LiveSekiroSignalSource`'s job, which does not exist yet.
- No screen capture, function hooks, or DLL injection -- none of those
  belong in a read-only process-memory module regardless.

## Next: signal discovery (`SEK-PROBE-001A`) and, later, pointer chains

`SEK-READ-001E` (a production pointer-chain resolver) is paused. Before
building that on invented offsets, `SEK-PROBE-001A` adds a **developer-only**
live discovery probe -- attach read-only to a real `sekiro.exe`, enumerate
its readable memory, and search for raw value *candidates* that change
alongside real user actions, without ever hardcoding or guessing an
address. See [06-signal-discovery-probe.md](06-signal-discovery-probe.md)
for that tool -- it is deliberately a separate document and a separate
safety boundary from everything above it, since it is not part of the
production haptics runtime.

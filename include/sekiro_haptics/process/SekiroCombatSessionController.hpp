#pragma once

// Thin OS-independent orchestrator wiring SekiroRawCombatReader (Stage B)
// and SekiroCombatCaptureSession (Stage C) into the probe CLI's
// combat-plan/combat-resolve/combat-status/combat-capture/combat-mark/
// combat-stop/combat-analyze commands -- same layering convention as
// SignalProbeScanController: this class owns state and process-facing
// operations, SekiroCombatCommandProcessor owns text parsing/formatting,
// apps/sekiro_signal_probe/main.cpp stays a thin Win32 adapter. See
// docs/07-combat-signal-reader.md.

#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"
#include "sekiro_haptics/process/SekiroCombatCaptureSession.hpp"
#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace sekiro_haptics::process {

/// Metadata-only preflight report -- touches no process memory *content*
/// beyond GetMainModule()'s own OS module-enumeration query. `fullScanUsed`
/// is always false: this whole path never performs a private-readable
/// memory scan (see DiskCandidateScanner.hpp for that separate, explicit
/// fallback path).
struct CombatPlanReport {
    bool moduleFound = false;
    /// Bytes an AOB (re-)scan covers -- the main module's own image size.
    /// Only meaningful when `moduleFound`.
    std::size_t aobScanRangeBytes = 0;
    /// Bytes one ReadSnapshot() call reads in steady state.
    std::size_t expectedBytesPerSampleBytes = kCombatFieldBlockSizeBytes;
    bool fullScanUsed = false;
};

class SekiroCombatSessionController {
public:
    /// `combatReader`/`inspector`/`processReader` must outlive this
    /// controller. `processReader` is used only for the capture session's
    /// own raw region reads -- kept separate from whatever IProcessReader
    /// `combatReader` was itself constructed with, since this controller
    /// has no way to reach back into that already-constructed object.
    SekiroCombatSessionController(SekiroRawCombatReader& combatReader, IProcessInspector& inspector,
                                   IProcessReader& processReader);

    CombatPlanReport Plan() const;

    /// Resolves (or re-resolves) the pointer chain; result also becomes
    /// LastResolve(). Never called automatically by CaptureTick() -- a
    /// capture keeps using whatever address the most recent Resolve()
    /// produced until the caller resolves again.
    CombatResolveResult Resolve();

    /// Takes one snapshot against the currently resolved address (does not
    /// re-resolve); result also becomes LastSnapshot().
    CombatSnapshot Snapshot();

    CombatResolveResult LastResolve() const { return lastResolve_; }
    CombatSnapshot LastSnapshot() const { return lastSnapshot_; }

    /// Starts a bounded, delta-only capture at the *currently* resolved
    /// PlayerGameData address (LastResolve()) -- InvalidConfig if nothing
    /// is resolved yet (address 0).
    CombatCaptureStartResult StartCapture(const CombatCaptureConfig& config, const std::string& outputPath,
                                           std::int64_t nowMonotonicUs);

    /// One capture read+diff+write cycle, using whatever address/generation
    /// LastResolve() currently holds (never re-scans). False if no capture
    /// is running.
    bool CaptureTick(std::int64_t nowMonotonicUs);

    bool CaptureMark(const std::string& label, std::int64_t nowMonotonicUs);

    void StopCapture();

    bool IsCapturing() const;
    CombatCaptureStats CaptureStats() const;
    std::chrono::milliseconds CaptureSamplingInterval() const;

private:
    SekiroRawCombatReader& combatReader_;
    IProcessInspector& inspector_;
    IProcessReader& processReader_;

    CombatResolveResult lastResolve_;
    CombatSnapshot lastSnapshot_;

    std::unique_ptr<SekiroCombatCaptureSession> captureSession_;
};

} // namespace sekiro_haptics::process

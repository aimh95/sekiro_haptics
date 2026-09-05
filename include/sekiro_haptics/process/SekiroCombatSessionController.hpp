#pragma once

// Thin OS-independent orchestrator wiring SekiroRawCombatReader (Stage B)
// into the probe CLI's combat-plan/combat-resolve/combat-status commands --
// same layering convention as SignalProbeScanController: this class owns
// state and process-facing operations, SekiroCombatCommandProcessor owns
// text parsing/formatting, apps/sekiro_signal_probe/main.cpp stays a thin
// Win32 adapter. See docs/07-combat-signal-reader.md.

#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"

#include <cstddef>

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
    /// `reader`/`inspector` must outlive this controller.
    SekiroCombatSessionController(SekiroRawCombatReader& reader, IProcessInspector& inspector);

    CombatPlanReport Plan() const;

    /// Resolves (or re-resolves) the pointer chain; result also becomes
    /// LastResolve().
    CombatResolveResult Resolve();

    /// Takes one snapshot against the currently resolved address (does not
    /// re-resolve); result also becomes LastSnapshot().
    CombatSnapshot Snapshot();

    CombatResolveResult LastResolve() const { return lastResolve_; }
    CombatSnapshot LastSnapshot() const { return lastSnapshot_; }

private:
    SekiroRawCombatReader& reader_;
    IProcessInspector& inspector_;

    CombatResolveResult lastResolve_;
    CombatSnapshot lastSnapshot_;
};

} // namespace sekiro_haptics::process

// trigger_lab -- adaptive triggers on a real DualSense, with no game running.
//
// This is the bench for step 1 of the output-runtime work: apply an effect to
// L2 or R2, hold it or give it a lifetime, replace it, cancel it, reset both,
// and watch what the lifetime rules actually do. It also has a --dry-run mode
// that prints the exact report bytes without opening any device, so the
// encoding can be inspected on a machine with no controller attached.
//
// It deliberately does NOT open the USB audio endpoint. Simultaneous
// trigger + PCM output is sekiro_guard_feedback's job (--trigger-demo), which
// is the binary that already owns the mixer.

#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

namespace {

std::int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Help() {
    std::cout <<
R"(trigger_lab -- DualSense adaptive triggers, no game required

DEVICE
  --list                     list DualSense USB HID interfaces and exit
  --device <n>               which one to open (default 0)
  --dry-run                  print the report bytes, open nothing

EFFECT
  --side l2|r2|both          which trigger (default r2)
  --effect <mode>            off | feedback | weapon | vibration | simple
  --duration-ms <n>          release after n ms; 0 or omitted = hold (default hold)

  Each mode reads ONLY its own parameters. Passing a parameter a mode does
  not have is an error, not a silently ignored flag.

    feedback   --position 0..9      zone where resistance starts
               --strength 0..8      0 releases the trigger
    weapon     --start 2..7         zone where resistance starts
               --end   start+1..8   zone where it releases
               --strength 0..8      0 releases the trigger
    vibration  --position 0..9      zone where cycling starts
               --amplitude 0..8     0 releases the trigger
               --frequency <hz>     1..255, one byte; 0 releases the trigger
    simple     --start-byte 0..255  raw start position (legacy mode 0x01)
               --force-byte 0..255  raw force

  position/start/end are ZONE INDICES (0 = untouched, 9 = fully pressed),
  strength/amplitude is a 1..8 scale. Neither is a percentage and neither is
  a force in newtons.

LIFETIME DEMOS (each prints what it expects before doing it)
  --demo replace             show that an old effect's expiry does not kill
                             the effect that replaced it
  --demo expire              apply with a lifetime and watch it release
  --demo independence        drive L2 and R2 with different effects
  --demo reconnect           reset, then re-open the device and show that the
                             previous effect does NOT come back

OTHER
  --off l2|r2|both           release the trigger(s) and exit
  --reset                    both triggers to neutral and exit
  --hold-seconds <n>         keep the process alive n seconds after applying,
                             ticking the runtime, so an expiry can be felt
                             (default 0 for --duration-ms 0, else duration+1s)
  --show-bytes               also print the 48 bytes actually sent

WHAT THIS TOOL CANNOT TELL YOU
  Whether the resistance feels right. It reports what was encoded and what
  the HID write returned. How hard the trigger actually pushes back is a
  hardware observation, and this tool never claims to have made one.
)";
}

struct Options {
    bool list = false;
    bool dryRun = false;
    bool reset = false;
    bool showBytes = false;
    int device = 0;
    std::string side = "r2";
    std::string effect;
    std::string off;
    std::string demo;
    std::int64_t durationMs = 0;
    double holdSeconds = -1.0;

    // Per-mode parameters. Each is optional so "you passed a parameter this
    // mode does not have" is detectable rather than silently defaulted.
    std::optional<int> position, strength, start, end, amplitude, frequency;
    std::optional<int> startByte, forceByte;
};

std::string Hex(const std::uint8_t* data, std::size_t length) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) {
        if (i && i % 16 == 0) out << "\n           ";
        out << std::setw(2) << static_cast<int>(data[i]) << ' ';
    }
    return out.str();
}

bool InRange(const std::optional<int>& value, int low, int high) {
    return value && *value >= low && *value <= high;
}

/// Builds the spec from the flags, and refuses parameters that do not belong
/// to the chosen mode. `error` is filled on failure.
bool BuildSpec(const Options& o, TriggerEffectSpec& spec, std::string& error) {
    const auto mode = ParseTriggerEffectMode(o.effect);
    if (!mode) { error = "unknown --effect '" + o.effect + "'"; return false; }

    auto reject = [&](const char* name, const std::optional<int>& value) {
        if (!value) return false;
        error = std::string("--") + name + " is not a parameter of " + o.effect;
        return true;
    };

    switch (*mode) {
        case TriggerEffectMode::Off:
            if (reject("position", o.position) || reject("strength", o.strength) ||
                reject("start", o.start) || reject("end", o.end) ||
                reject("amplitude", o.amplitude) || reject("frequency", o.frequency))
                return false;
            spec = TriggerEffectSpec::MakeOff();
            return true;

        case TriggerEffectMode::Feedback:
            if (reject("start", o.start) || reject("end", o.end) ||
                reject("amplitude", o.amplitude) || reject("frequency", o.frequency))
                return false;
            if (!InRange(o.position, 0, 9)) { error = "feedback needs --position 0..9"; return false; }
            if (!InRange(o.strength, 0, 8)) { error = "feedback needs --strength 0..8"; return false; }
            spec = TriggerEffectSpec::MakeFeedback(static_cast<std::uint8_t>(*o.position),
                                                   static_cast<std::uint8_t>(*o.strength));
            return true;

        case TriggerEffectMode::Weapon:
            if (reject("position", o.position) || reject("amplitude", o.amplitude) ||
                reject("frequency", o.frequency))
                return false;
            if (!InRange(o.start, 2, 7)) { error = "weapon needs --start 2..7"; return false; }
            if (!InRange(o.end, 3, 8)) { error = "weapon needs --end 3..8"; return false; }
            if (!InRange(o.strength, 0, 8)) { error = "weapon needs --strength 0..8"; return false; }
            spec = TriggerEffectSpec::MakeWeapon(static_cast<std::uint8_t>(*o.start),
                                                 static_cast<std::uint8_t>(*o.end),
                                                 static_cast<std::uint8_t>(*o.strength));
            return true;

        case TriggerEffectMode::Vibration:
            if (reject("start", o.start) || reject("end", o.end) || reject("strength", o.strength))
                return false;
            if (!InRange(o.position, 0, 9)) { error = "vibration needs --position 0..9"; return false; }
            if (!InRange(o.amplitude, 0, 8)) { error = "vibration needs --amplitude 0..8"; return false; }
            if (!InRange(o.frequency, 0, 255)) { error = "vibration needs --frequency 0..255"; return false; }
            spec = TriggerEffectSpec::MakeVibration(static_cast<std::uint8_t>(*o.position),
                                                    static_cast<std::uint8_t>(*o.amplitude),
                                                    static_cast<std::uint8_t>(*o.frequency));
            return true;

        case TriggerEffectMode::SimpleFeedback:
            if (!InRange(o.startByte, 0, 255) || !InRange(o.forceByte, 0, 255)) {
                error = "simple needs --start-byte 0..255 and --force-byte 0..255";
                return false;
            }
            spec = TriggerEffectSpec::MakeSimpleFeedback(static_cast<std::uint8_t>(*o.startByte),
                                                         static_cast<std::uint8_t>(*o.forceByte));
            return true;
    }
    error = "unhandled mode";
    return false;
}

std::vector<TriggerSide> SidesFrom(const std::string& text, std::string& error) {
    if (text == "both") return {TriggerSide::Left, TriggerSide::Right};
    if (const auto side = ParseTriggerSide(text)) return {*side};
    error = "unknown side '" + text + "' (use l2, r2 or both)";
    return {};
}

int ListDevices() {
    HidApiDualSenseTransport transport;
    const auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) {
        std::cout << "no DualSense USB gamepad interface found\n";
        return 1;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        std::cout << "[" << i << "] vid=0x" << std::hex << candidates[i].vendorId
                  << " pid=0x" << candidates[i].productId << std::dec
                  << " path=" << candidates[i].path << "\n";
    }
    return 0;
}

/// Opens the requested device. Returns false with a message on failure.
bool OpenDevice(HidApiDualSenseTransport& transport, int index) {
    const auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) {
        std::cout << "no DualSense USB gamepad interface found -- is it plugged in over USB?\n";
        return false;
    }
    if (index < 0 || index >= static_cast<int>(candidates.size())) {
        std::cout << "--device " << index << " out of range; run --list\n";
        return false;
    }
    if (transport.Open(candidates[static_cast<std::size_t>(index)].path) != TransportResult::Success) {
        std::cout << "could not open the device (another process may own it)\n";
        return false;
    }
    std::cout << "opened device " << index << ", HID output report length "
              << transport.DeclaredOutputReportLength() << " bytes\n";
    return true;
}

void PrintReport(const DualSenseOutputState& state, std::size_t length) {
    const auto report = state.BuildReport();
    const auto shown = length > 0 && length <= report.size() ? length : report.size();
    std::cout << "  bytes:   " << Hex(report.data(), shown) << "\n";
}

/// Ticks the runtime on a 5 ms grid for `seconds`, so expiries land close to
/// their nominal time. 5 ms matches the live detection loop's grid, and
/// Win32 Sleep is used for the same reason the live loop uses it -- MinGW's
/// sleep_for does not honour timeBeginPeriod (docs/11-guard-feedback.md 12.3).
void TickFor(AdaptiveTriggerRuntime& runtime, double seconds) {
    if (seconds <= 0.0) return;
    timeBeginPeriod(1);
    const auto started = NowUs();
    const auto endUs = started + static_cast<std::int64_t>(seconds * 1'000'000.0);
    while (NowUs() < endUs) {
        runtime.Tick(NowUs());
        ::Sleep(1);
    }
    runtime.Tick(NowUs());
    timeEndPeriod(1);
}

void ReportSides(const AdaptiveTriggerRuntime& runtime) {
    for (const auto side : {TriggerSide::Left, TriggerSide::Right}) {
        const auto active = runtime.Active(side);
        std::cout << "  " << ToString(side) << ": ";
        if (!active.active) { std::cout << "released\n"; continue; }
        std::cout << DescribeTriggerEffect(active.spec) << "  id=" << active.effectId;
        if (active.expiresAtUs == 0) std::cout << "  holds until replaced";
        else std::cout << "  expires in " << (active.expiresAtUs - NowUs()) / 1000 << " ms";
        std::cout << "\n";
    }
}

int RunDemo(const Options& o) {
    HidApiDualSenseTransport transport(std::cerr);   // keep stdout readable
    if (!OpenDevice(transport, o.device)) return 1;
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);
    runtime.OnReconnect(NowUs());   // always start from a known neutral state

    if (o.demo == "expire") {
        std::cout << "\nExpecting: R2 resists for 800 ms, then releases on its own.\n";
        const auto applied = runtime.Apply(TriggerSide::Right,
                                           TriggerEffectSpec::MakeFeedback(3, 7), 800'000, NowUs());
        std::cout << "applied id=" << applied.effectId << "; hold R2 down now\n";
        TickFor(runtime, 1.5);
        std::cout << "after 1.5 s:\n";
        ReportSides(runtime);
        std::cout << "expired=" << runtime.Stats().expired << " (expected 1)\n";
    } else if (o.demo == "replace") {
        std::cout << "\nExpecting: a 300 ms effect is replaced at 100 ms by a 1200 ms one.\n"
                     "The first effect's expiry at 300 ms must NOT release the trigger --\n"
                     "R2 should stay resistant until about 1300 ms.\n";
        runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 4), 300'000, NowUs());
        TickFor(runtime, 0.1);
        const auto second = runtime.Apply(TriggerSide::Right,
                                          TriggerEffectSpec::MakeWeapon(3, 7, 8), 1'200'000, NowUs());
        std::cout << "replaced with id=" << second.effectId << "\n";
        TickFor(runtime, 0.5);
        std::cout << "at ~600 ms (the first effect would have expired at 300 ms):\n";
        ReportSides(runtime);
        std::cout << "  expired so far = " << runtime.Stats().expired << " (expected 0)\n";
        TickFor(runtime, 1.0);
        std::cout << "at ~1600 ms:\n";
        ReportSides(runtime);
        std::cout << "  expired = " << runtime.Stats().expired << " (expected 1)"
                  << "  replaced = " << runtime.Stats().replaced << " (expected 1)\n";
    } else if (o.demo == "independence") {
        std::cout << "\nExpecting: L2 gets a weapon effect, R2 a constant resistance.\n"
                     "They should feel different, and releasing L2 must not change R2.\n";
        runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeWeapon(2, 6, 8), kHoldUntilReplaced, NowUs());
        runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(1, 5), kHoldUntilReplaced, NowUs());
        ReportSides(runtime);
        std::cout << "press both triggers, then wait -- L2 releases in 3 s, R2 holds\n";
        TickFor(runtime, 3.0);
        runtime.CancelSide(TriggerSide::Left, NowUs());
        std::cout << "after releasing L2:\n";
        ReportSides(runtime);
        TickFor(runtime, 3.0);
        runtime.ResetToNeutral(NowUs());
    } else if (o.demo == "reconnect") {
        std::cout << "\nExpecting: an effect is applied, then the device is closed and\n"
                     "re-opened. After the re-open both triggers are neutral and the old\n"
                     "effect is NOT replayed.\n";
        runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 8), kHoldUntilReplaced, NowUs());
        ReportSides(runtime);
        TickFor(runtime, 2.0);
        std::cout << "closing the device (this is a software close, not an unplug)\n";
        runtime.OnDeviceLost();
        transport.Close();
        if (!OpenDevice(transport, o.device)) return 1;
        runtime.OnReconnect(NowUs());
        std::cout << "after re-open:\n";
        ReportSides(runtime);
        std::cout << "  reconnects=" << runtime.Stats().reconnects
                  << "  applied=" << runtime.Stats().applied << " (no new apply expected)\n";
        std::cout << "\nNOTE: a real unplug cannot be reset by software. This demo closes\n"
                     "the handle; it does not prove anything about a yanked cable.\n";
    } else {
        std::cout << "unknown --demo '" << o.demo << "'\n";
        return 2;
    }

    runtime.ResetToNeutral(NowUs());
    std::cout << "\nboth triggers returned to neutral. writes=" << state.Stats().submits
              << " failures=" << state.Stats().writeFailures << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        auto nextInt = [&]() { return std::atoi(next().c_str()); };
        if (a == "--help" || a == "-h") { Help(); return 0; }
        else if (a == "--list") o.list = true;
        else if (a == "--device") o.device = nextInt();
        else if (a == "--dry-run") o.dryRun = true;
        else if (a == "--side") o.side = next();
        else if (a == "--effect") o.effect = next();
        else if (a == "--duration-ms") o.durationMs = nextInt();
        else if (a == "--position") o.position = nextInt();
        else if (a == "--strength") o.strength = nextInt();
        else if (a == "--start") o.start = nextInt();
        else if (a == "--end") o.end = nextInt();
        else if (a == "--amplitude") o.amplitude = nextInt();
        else if (a == "--frequency") o.frequency = nextInt();
        else if (a == "--start-byte") o.startByte = nextInt();
        else if (a == "--force-byte") o.forceByte = nextInt();
        else if (a == "--off") o.off = next();
        else if (a == "--reset") o.reset = true;
        else if (a == "--demo") o.demo = next();
        else if (a == "--hold-seconds") o.holdSeconds = std::atof(next().c_str());
        else if (a == "--show-bytes") o.showBytes = true;
        else { std::cout << "unknown option " << a << " (try --help)\n"; return 2; }
    }

    if (argc == 1) { Help(); return 0; }
    if (o.list) return ListDevices();
    if (!o.demo.empty()) return RunDemo(o);

    // --- what are we applying? -------------------------------------------
    std::string error;
    std::vector<TriggerSide> sides;
    TriggerEffectSpec spec;

    if (o.reset || !o.off.empty()) {
        sides = SidesFrom(o.reset ? "both" : o.off, error);
        spec = TriggerEffectSpec::MakeOff();
    } else {
        if (o.effect.empty()) { std::cout << "--effect is required (try --help)\n"; return 2; }
        sides = SidesFrom(o.side, error);
        if (!error.empty() || !BuildSpec(o, spec, error)) {
            std::cout << error << "\n";
            return 2;
        }
    }
    if (!error.empty()) { std::cout << error << "\n"; return 2; }

    std::cout << "effect:  " << DescribeTriggerEffect(spec) << "\n";
    std::cout << "sides:   ";
    for (const auto side : sides) std::cout << ToString(side) << ' ';
    std::cout << "\nlifetime: "
              << (o.durationMs > 0 ? std::to_string(o.durationMs) + " ms"
                                   : std::string("holds until replaced or released"))
              << "\n";

    // --- dry run: encode and print, open nothing --------------------------
    if (o.dryRun) {
        DualSenseOutputState state;
        for (const auto side : sides) {
            const auto encoded = state.SetTrigger(side, spec);
            if (!encoded.ok) { std::cout << "rejected: " << encoded.error << "\n"; return 2; }
            if (encoded.reducedToOff)
                std::cout << "note: those parameters describe no effect, so "
                          << ToString(side) << " was encoded as Off\n";
        }
        PrintReport(state, 48);
        std::cout << "\ndry run: nothing was sent to any device.\n";
        return 0;
    }

    // --- real device ------------------------------------------------------
    HidApiDualSenseTransport transport(std::cerr);
    if (!OpenDevice(transport, o.device)) return 1;

    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);
    // Always begin from neutral: the controller may still be holding whatever
    // a previous run left on it, and starting from an unknown state would
    // make everything after this ambiguous.
    runtime.OnReconnect(NowUs());

    const auto durationUs = o.durationMs > 0 ? o.durationMs * 1000 : kHoldUntilReplaced;
    const bool releasing = o.reset || !o.off.empty();
    bool anyFailed = false;
    for (const auto side : sides) {
        // A release goes through CancelSide rather than "apply an Off effect",
        // so the stats call it a cancellation and no effect id is minted for
        // what is the absence of an effect.
        if (releasing) {
            runtime.CancelSide(side, NowUs());
            std::cout << "  " << ToString(side) << ": released\n";
            continue;
        }
        const auto result = runtime.Apply(side, spec, durationUs, NowUs());
        std::cout << "  " << ToString(side) << ": "
                  << (result.accepted ? "applied" : "FAILED");
        if (result.accepted) std::cout << " id=" << result.effectId;
        if (result.reducedToOff)
            std::cout << " (those parameters describe no effect, so it was released)";
        if (!result.error.empty()) std::cout << "  " << result.error;
        std::cout << "\n";
        anyFailed = anyFailed || !result.accepted;
    }
    if (o.showBytes) PrintReport(state, transport.DeclaredOutputReportLength());

    // How long to stay alive. An effect with a lifetime needs the process to
    // outlive it, or nothing would ever release it.
    double hold = o.holdSeconds;
    if (hold < 0.0) hold = o.durationMs > 0 ? (o.durationMs / 1000.0) + 1.0 : 0.0;
    if (hold > 0.0) {
        std::cout << "holding for " << hold << " s (ticking at 5 ms)...\n";
        TickFor(runtime, hold);
        ReportSides(runtime);
    }

    const auto stats = runtime.Stats();
    std::cout << "\napplied=" << stats.applied << " expired=" << stats.expired
              << " cancelled=" << stats.cancelled << " rejected=" << stats.rejectedSpecs
              << " writeFailures=" << stats.writeFailures << "\n";
    std::cout << "HID writes=" << state.Stats().submits
              << " skipped(unchanged)=" << state.Stats().skippedUnchanged << "\n";

    if (releasing || o.durationMs > 0) {
        // Leave the device neutral when this run was about releasing, or when
        // the effect had a lifetime that has now elapsed.
        runtime.ResetToNeutral(NowUs());
        std::cout << "triggers left neutral.\n";
    } else {
        std::cout << "NOTE: this effect holds. It stays on the controller after this\n"
                     "      process exits -- run --reset to release it.\n";
    }
    return anyFailed ? 1 : 0;
}

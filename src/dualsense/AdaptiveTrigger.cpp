#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"

#include <sstream>

namespace sekiro_haptics::dualsense {

namespace {

/// Zone packing shared by Feedback (0x21) and Vibration (0x26), transcribed
/// from [1]: one bit per zone in a 16-bit active mask, and a 3-bit level per
/// zone in a 32-bit value, both filled from `fromZone` to zone 9. Only the
/// low 32 bits of the level field fit in the four bytes the block reserves
/// for it, which is what [1] does too -- zones 0..9 need 30 bits, so nothing
/// is lost.
struct ZonePacking {
    std::uint16_t activeZones = 0;
    std::uint32_t levelZones = 0;
};

ZonePacking PackZones(std::uint8_t fromZone, std::uint8_t level) {
    ZonePacking packed;
    const auto levelValue = static_cast<std::uint32_t>((level - 1) & 0x07);
    for (unsigned zone = fromZone; zone <= kMaxZoneIndex; ++zone) {
        packed.levelZones |= levelValue << (3 * zone);
        packed.activeZones |= static_cast<std::uint16_t>(1u << zone);
    }
    return packed;
}

void WriteZoneBlock(TriggerBlock& out, TriggerEffectMode mode, const ZonePacking& packed) {
    out.fill(0);
    out[0] = static_cast<std::uint8_t>(mode);
    out[1] = static_cast<std::uint8_t>(packed.activeZones & 0xFF);
    out[2] = static_cast<std::uint8_t>((packed.activeZones >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(packed.levelZones & 0xFF);
    out[4] = static_cast<std::uint8_t>((packed.levelZones >> 8) & 0xFF);
    out[5] = static_cast<std::uint8_t>((packed.levelZones >> 16) & 0xFF);
    out[6] = static_cast<std::uint8_t>((packed.levelZones >> 24) & 0xFF);
    // Bytes 7..10 stay zero. [1] notes 7 and 8 would hold the high bits of a
    // 64-bit level field that zones 0..9 never reach, and Vibration puts its
    // frequency in byte 9 (written by the caller).
}

TriggerEncodeResult Ok(bool reducedToOff = false) {
    TriggerEncodeResult result;
    result.ok = true;
    result.reducedToOff = reducedToOff;
    return result;
}

TriggerEncodeResult Rejected(std::string message) {
    TriggerEncodeResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

} // namespace

const char* ToString(TriggerSide side) {
    return side == TriggerSide::Left ? "L2" : "R2";
}

std::optional<TriggerSide> ParseTriggerSide(std::string_view text) {
    if (text == "l2" || text == "L2" || text == "left" || text == "l") return TriggerSide::Left;
    if (text == "r2" || text == "R2" || text == "right" || text == "r") return TriggerSide::Right;
    return std::nullopt;
}

const char* ToString(TriggerEffectMode mode) {
    switch (mode) {
        case TriggerEffectMode::Off: return "off";
        case TriggerEffectMode::Feedback: return "feedback";
        case TriggerEffectMode::Weapon: return "weapon";
        case TriggerEffectMode::Vibration: return "vibration";
        case TriggerEffectMode::SimpleFeedback: return "simple-feedback";
    }
    return "unknown";
}

std::optional<TriggerEffectMode> ParseTriggerEffectMode(std::string_view text) {
    if (text == "off" || text == "none") return TriggerEffectMode::Off;
    if (text == "feedback" || text == "resistance") return TriggerEffectMode::Feedback;
    if (text == "weapon") return TriggerEffectMode::Weapon;
    if (text == "vibration") return TriggerEffectMode::Vibration;
    if (text == "simple-feedback" || text == "simple") return TriggerEffectMode::SimpleFeedback;
    return std::nullopt;
}

TriggerEffectSpec TriggerEffectSpec::MakeOff() {
    return TriggerEffectSpec{};
}

TriggerEffectSpec TriggerEffectSpec::MakeFeedback(std::uint8_t position, std::uint8_t strength) {
    TriggerEffectSpec spec;
    spec.mode = TriggerEffectMode::Feedback;
    spec.feedback = FeedbackParams{position, strength};
    return spec;
}

TriggerEffectSpec TriggerEffectSpec::MakeWeapon(std::uint8_t startPosition, std::uint8_t endPosition,
                                                std::uint8_t strength) {
    TriggerEffectSpec spec;
    spec.mode = TriggerEffectMode::Weapon;
    spec.weapon = WeaponParams{startPosition, endPosition, strength};
    return spec;
}

TriggerEffectSpec TriggerEffectSpec::MakeVibration(std::uint8_t position, std::uint8_t amplitude,
                                                   std::uint8_t frequencyHz) {
    TriggerEffectSpec spec;
    spec.mode = TriggerEffectMode::Vibration;
    spec.vibration = VibrationParams{position, amplitude, frequencyHz};
    return spec;
}

TriggerEffectSpec TriggerEffectSpec::MakeSimpleFeedback(std::uint8_t startPosition, std::uint8_t force) {
    TriggerEffectSpec spec;
    spec.mode = TriggerEffectMode::SimpleFeedback;
    spec.simple = SimpleFeedbackParams{startPosition, force};
    return spec;
}

TriggerEncodeResult ValidateTriggerEffect(const TriggerEffectSpec& spec) {
    switch (spec.mode) {
        case TriggerEffectMode::Off:
            return Ok();

        case TriggerEffectMode::Feedback: {
            const auto& p = spec.feedback;
            if (p.position > kMaxZoneIndex)
                return Rejected("feedback.position must be a zone index 0..9");
            if (p.strength > kMaxStrength)
                return Rejected("feedback.strength must be 0..8");
            return Ok(p.strength == 0);
        }

        case TriggerEffectMode::Weapon: {
            const auto& p = spec.weapon;
            if (p.startPosition < 2 || p.startPosition > 7)
                return Rejected("weapon.startPosition must be a zone index 2..7");
            if (p.endPosition > 8)
                return Rejected("weapon.endPosition must be a zone index at most 8");
            if (p.endPosition <= p.startPosition)
                return Rejected("weapon.endPosition must be greater than weapon.startPosition");
            if (p.strength > kMaxStrength)
                return Rejected("weapon.strength must be 0..8");
            return Ok(p.strength == 0);
        }

        case TriggerEffectMode::Vibration: {
            const auto& p = spec.vibration;
            if (p.position > kMaxZoneIndex)
                return Rejected("vibration.position must be a zone index 0..9");
            if (p.amplitude > kMaxStrength)
                return Rejected("vibration.amplitude must be 0..8");
            // frequencyHz is a full byte, so every value 0..255 is in range;
            // 0 is "no cycling", which is no effect.
            return Ok(p.amplitude == 0 || p.frequencyHz == 0);
        }

        case TriggerEffectMode::SimpleFeedback: {
            // Both parameters are raw bytes with no documented sub-range, so
            // nothing here can be out of range. Zero force is a release.
            return Ok(spec.simple.force == 0);
        }
    }
    return Rejected("unknown trigger effect mode");
}

TriggerBlock OffTriggerBlock() {
    TriggerBlock block{};
    block[0] = static_cast<std::uint8_t>(TriggerEffectMode::Off);
    return block;
}

TriggerEncodeResult EncodeTriggerEffect(const TriggerEffectSpec& spec, TriggerBlock& out) {
    const auto validation = ValidateTriggerEffect(spec);
    if (!validation.ok) return validation;

    // [1]: a valid parameter set that describes no effect is sent as Off, not
    // as a zero-strength effect. Doing it here means one code path releases
    // the trigger, so "strength 0" and "off" cannot diverge.
    if (validation.reducedToOff) {
        out = OffTriggerBlock();
        return validation;
    }

    switch (spec.mode) {
        case TriggerEffectMode::Off:
            out = OffTriggerBlock();
            return validation;

        case TriggerEffectMode::Feedback:
            WriteZoneBlock(out, TriggerEffectMode::Feedback,
                           PackZones(spec.feedback.position, spec.feedback.strength));
            return validation;

        case TriggerEffectMode::Weapon: {
            out.fill(0);
            const auto startAndStopZones = static_cast<std::uint16_t>(
                (1u << spec.weapon.startPosition) | (1u << spec.weapon.endPosition));
            out[0] = static_cast<std::uint8_t>(TriggerEffectMode::Weapon);
            out[1] = static_cast<std::uint8_t>(startAndStopZones & 0xFF);
            out[2] = static_cast<std::uint8_t>((startAndStopZones >> 8) & 0xFF);
            out[3] = static_cast<std::uint8_t>(spec.weapon.strength - 1);
            return validation;
        }

        case TriggerEffectMode::Vibration:
            WriteZoneBlock(out, TriggerEffectMode::Vibration,
                           PackZones(spec.vibration.position, spec.vibration.amplitude));
            out[9] = spec.vibration.frequencyHz;
            return validation;

        case TriggerEffectMode::SimpleFeedback:
            out.fill(0);
            out[0] = static_cast<std::uint8_t>(TriggerEffectMode::SimpleFeedback);
            out[1] = spec.simple.startPosition;
            out[2] = spec.simple.force;
            return validation;
    }
    return Rejected("unknown trigger effect mode");
}

std::string DescribeTriggerEffect(const TriggerEffectSpec& spec) {
    std::ostringstream out;
    out << ToString(spec.mode);
    switch (spec.mode) {
        case TriggerEffectMode::Off:
            break;
        case TriggerEffectMode::Feedback:
            out << " position=" << static_cast<int>(spec.feedback.position)
                << " strength=" << static_cast<int>(spec.feedback.strength);
            break;
        case TriggerEffectMode::Weapon:
            out << " start=" << static_cast<int>(spec.weapon.startPosition)
                << " end=" << static_cast<int>(spec.weapon.endPosition)
                << " strength=" << static_cast<int>(spec.weapon.strength);
            break;
        case TriggerEffectMode::Vibration:
            out << " position=" << static_cast<int>(spec.vibration.position)
                << " amplitude=" << static_cast<int>(spec.vibration.amplitude)
                << " frequency=" << static_cast<int>(spec.vibration.frequencyHz) << "Hz";
            break;
        case TriggerEffectMode::SimpleFeedback:
            out << " startByte=" << static_cast<int>(spec.simple.startPosition)
                << " forceByte=" << static_cast<int>(spec.simple.force);
            break;
    }
    return out.str();
}

} // namespace sekiro_haptics::dualsense

// Standalone C++20 reference, NOT a Sekiro memory reader or confirmed detector.
// No real Sekiro address, animation ID, or activation-counter layout is known here.
// All IDs in main() are synthetic test fixtures. Production candidates default empty.
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

struct ReactionSample {
    bool supported_build = false;
    bool valid = false;       // Exact reads + adapter's consistency checks passed.
    bool player_owned = false;
    std::uint64_t session = 0;
    std::uint64_t epoch = 0;  // Changes on object/lifecycle discontinuity, not just PID.
    std::uint64_t sample_seq = 0;
    std::uint64_t observed_us = 0; // Monotonic host observation time, not impact time.
    std::int32_t animation_id = 0;

    // REQUIRED CONTRACT, NOT an assertion that Sekiro exposes this field:
    // Game-derived activation serial for the selected reaction lane, monotonic
    // within an epoch; unchanged while an activation persists, changes for a
    // new activation even if the same animation is replayed. Null if unresolved.
    // NEVER substitute polling sample_seq, wall-clock time, or a persistent actor
    // pointer. Animation-time rollback alone is not proof of a new deflect.
    std::optional<std::uint64_t> activation;
};

struct CandidateDeflect {
    std::uint64_t session;
    std::uint64_t epoch;
    std::uint64_t activation;
    std::uint64_t observed_us;
    std::int32_t animation_id;
};

class CandidateDetector {
public:
    explicit CandidateDetector(std::unordered_set<std::int32_t> candidates = {},
                               std::uint64_t max_gap_us = 50'000)
        : candidates_(std::move(candidates)), max_gap_us_(max_gap_us) {}

    std::optional<CandidateDeflect> Update(const ReactionSample& s) {
        if (!s.supported_build || !s.valid || !s.player_owned || !s.activation) {
            previous_.reset(); // Failed first read cannot retain an old baseline.
            return std::nullopt;
        }
        if (!previous_) {
            previous_ = s;
            return std::nullopt; // Attaching during a reaction does not invent an event.
        }
        const auto p = *previous_;
        previous_ = s;
        if (s.session != p.session || s.epoch != p.epoch ||
            s.sample_seq <= p.sample_seq || s.sample_seq - p.sample_seq != 1 ||
            s.observed_us <= p.observed_us ||
            s.observed_us - p.observed_us > max_gap_us_ ||
            *s.activation < *p.activation) {
            return std::nullopt; // Conservative rebaseline; a real event may be missed.
        }
        if (*s.activation == *p.activation ||
            !candidates_.contains(s.animation_id)) {
            return std::nullopt;
        }
        return CandidateDeflect{s.session, s.epoch, *s.activation,
                                s.observed_us, s.animation_id};
    }

private:
    std::unordered_set<std::int32_t> candidates_;
    std::uint64_t max_gap_us_;
    std::optional<ReactionSample> previous_;
};

// Synthetic, offline contract tests. They do not prove any game field exists,
// that an animation means deflect, or that polling captures every activation.
int main() {
    constexpr std::int32_t candidate = 900'000'001; // NOT a Sekiro ID.
    constexpr std::int32_t neutral = 900'000'002;   // NOT a Sekiro ID.
    const auto sample = [](std::uint64_t seq, std::int32_t id,
                           std::uint64_t activation) {
        return ReactionSample{true, true, true, 1, 1, seq, seq * 5'000,
                              id, activation};
    };
    unsigned passed = 0;
    const auto check = [&passed](bool ok, const char* name) {
        if (!ok) throw std::runtime_error(name);
        ++passed;
        std::cout << "PASS " << name << '\n';
    };

    CandidateDetector d({candidate});
    check(!d.Update(sample(1, neutral, 10)), "baseline_emits_nothing");
    check(d.Update(sample(2, candidate, 11)).has_value(), "new_candidate_activation");
    check(!d.Update(sample(3, candidate, 11)), "same_activation_no_duplicate");
    check(d.Update(sample(4, candidate, 12)).has_value(), "same_id_new_activation");
    check(!d.Update(sample(5, neutral, 13)), "noncandidate_no_event");

    auto enemy = sample(6, candidate, 14);
    enemy.player_owned = false;
    check(!d.Update(enemy), "other_actor_no_event");
    check(!d.Update(sample(7, candidate, 15)), "after_invalid_requires_baseline");

    auto first_new_epoch = sample(8, candidate, 16);
    first_new_epoch.epoch = 2;
    first_new_epoch.valid = false;
    check(!d.Update(first_new_epoch), "new_epoch_first_read_fails");
    auto next_new_epoch = sample(9, candidate, 17);
    next_new_epoch.epoch = 2;
    check(!d.Update(next_new_epoch), "new_epoch_success_only_baselines");

    CandidateDetector startup({candidate});
    check(!startup.Update(sample(1, candidate, 1)), "attach_midreaction_no_event");

    CandidateDetector no_profile;
    no_profile.Update(sample(1, neutral, 1));
    check(!no_profile.Update(sample(2, candidate, 2)), "empty_profile_fail_closed");

    CandidateDetector gaps({candidate});
    gaps.Update(sample(1, neutral, 1));
    check(!gaps.Update(sample(3, candidate, 2)), "missing_sample_rebaseline");

    CandidateDetector time_gap({candidate});
    time_gap.Update(sample(1, neutral, 1));
    auto late = sample(2, candidate, 2);
    late.observed_us = 500'000;
    check(!time_gap.Update(late), "long_time_gap_rebaseline");

    CandidateDetector unsupported({candidate});
    unsupported.Update(sample(1, neutral, 1));
    auto wrong_build = sample(2, candidate, 2);
    wrong_build.supported_build = false;
    check(!unsupported.Update(wrong_build), "unsupported_build_no_event");

    CandidateDetector missing_serial({candidate});
    missing_serial.Update(sample(1, neutral, 1));
    auto unknown = sample(2, candidate, 2);
    unknown.activation.reset();
    check(!missing_serial.Update(unknown), "unresolved_activation_no_event");

    CandidateDetector rollback({candidate});
    rollback.Update(sample(1, neutral, 10));
    check(!rollback.Update(sample(2, candidate, 9)), "activation_rollback_rebaseline");

    CandidateDetector session_change({candidate});
    session_change.Update(sample(1, neutral, 1));
    auto restarted = sample(2, candidate, 2);
    restarted.session = 2;
    check(!session_change.Update(restarted), "new_session_rebaseline");

    CandidateDetector epoch_change({candidate});
    epoch_change.Update(sample(1, neutral, 1));
    auto replaced = sample(2, candidate, 2);
    replaced.epoch = 2;
    check(!epoch_change.Update(replaced), "new_epoch_rebaseline");

    std::cout << "Synthetic contract tests: " << passed << '/' << passed << " passed\n";
}

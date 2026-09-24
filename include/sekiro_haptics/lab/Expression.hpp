#pragma once

// A small, deliberately limited arithmetic evaluator for waveform formulas.
//
// WHY THIS IS NOT A SCRIPT ENGINE
// -------------------------------
// The point of letting a formula into this tool is to type
// `cos(2*t)*sin(460*t)` and hear it. It is NOT to run code. So this grammar
// has no assignment, no variables the caller did not provide, no loops, no
// recursion, and a fixed list of functions -- there is nothing to escape from
// because there is nothing else in here. An unknown name is a parse error
// rather than something to look up somewhere.
//
// Cost is bounded by construction: an expression parses to a finite tree with
// a node cap, and one evaluation walks it once. There is no way to write
// something here that takes unbounded time.
//
// RADIANS, NOT HERTZ
// ------------------
// `sin(460*t)` is 460 RADIANS per second, which is 460/2pi ~= 73.2 Hz. This
// file does no conversion and makes no assumption either way: it evaluates
// what was written. Anything presenting a formula's numbers to a person has
// to say which of the two it is showing -- calling 460 "Hz" here would be
// wrong by a factor of 2pi, and the ordinary frequency inputs elsewhere in
// the lab are the ones that are in Hz.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sekiro_haptics::lab {

/// A name the expression may use, supplied by the caller.
struct ExpressionBinding {
    std::string name;
    double value = 0.0;
};

struct ExpressionDiagnostics {
    /// Samples where a division had a zero denominator. The result is defined
    /// as 0 for those, and counted here rather than allowed to become an
    /// infinity that quietly poisons the rest of the sum.
    std::uint64_t guardedDivisions = 0;
    /// Samples where the result was NaN or infinite for any other reason --
    /// sqrt of a negative, log of zero, an overflow. Also replaced by 0.
    std::uint64_t nonFiniteResults = 0;
};

/// A parsed formula. Parse once, evaluate per sample.
class Expression {
public:
    /// The parsed tree. Public only so the evaluator in the .cpp can name it;
    /// nothing outside that file has a reason to touch it.
    struct Node;

    Expression();
    ~Expression();
    Expression(Expression&&) noexcept;
    Expression& operator=(Expression&&) noexcept;
    Expression(const Expression&) = delete;
    Expression& operator=(const Expression&) = delete;

    /// Empty on success, otherwise why it was refused. A refused expression
    /// evaluates to 0 rather than to something arbitrary.
    ///
    /// `allowedNames` is the complete set of identifiers the formula may
    /// reference, beyond the built-in constants `pi` and `e`. Anything else
    /// is a parse error -- naming a variable that does not exist is a typo,
    /// and silently treating it as zero would produce a waveform that looks
    /// like it worked.
    std::string Parse(const std::string& text, const std::vector<std::string>& allowedNames);

    bool Ok() const;

    /// One sample. `bindings` must cover every name `Parse` was told about;
    /// a missing one evaluates as 0 and is counted in `nonFiniteResults` only
    /// if that makes the result non-finite.
    ///
    /// `diagnostics` accumulates across calls, so a whole render reports how
    /// many samples needed guarding rather than only the last one.
    double Evaluate(const std::vector<ExpressionBinding>& bindings,
                    ExpressionDiagnostics& diagnostics) const;

    /// Every function this grammar knows, for the UI to list. There is no
    /// other way in.
    static std::vector<std::string> SupportedFunctions();

private:
    std::unique_ptr<Node> root_;
};

} // namespace sekiro_haptics::lab

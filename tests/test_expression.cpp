// The formula evaluator behind the waveform workbench.
//
// Two things are being defended here. First, that the arithmetic is right --
// including the two formulas the tool ships as worked examples, whose whole
// point is a branch taken BEFORE a division. Second, that the grammar is a
// closed room: an unknown name or function is refused, not looked up.

#include "sekiro_haptics/lab/Expression.hpp"
#include "testing.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace sekiro_haptics::lab;

namespace {

double Eval(const std::string& text, double t, double t0 = 0.0) {
    Expression expression;
    if (!expression.Parse(text, {"t", "t0"}).empty()) return std::nan("");
    ExpressionDiagnostics diagnostics;
    return expression.Evaluate({{"t", t}, {"t0", t0}}, diagnostics);
}

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

} // namespace

SH_TEST(Expression_ArithmeticAndPrecedence) {
    SH_CHECK(Near(Eval("1+2*3", 0.0), 7.0));
    SH_CHECK(Near(Eval("(1+2)*3", 0.0), 9.0));
    SH_CHECK(Near(Eval("2^3^2", 0.0), 512.0));      // right associative
    SH_CHECK(Near(Eval("-2^2", 0.0), -4.0));        // unary minus binds loosest
    SH_CHECK(Near(Eval("10-3-2", 0.0), 5.0));       // left associative
}

SH_TEST(Expression_ExampleA_IsTheProductOfTwoCosinesInRadians) {
    // A(t) = cos(2t) * sin(460t). RADIANS -- 460 rad/s is about 73 Hz, and
    // this test exists partly so that stays written down next to the number.
    const double t = 0.013;
    SH_CHECK(Near(Eval("cos(2*t)*sin(460*t)", t), std::cos(2 * t) * std::sin(460 * t), 1e-12));
}

SH_TEST(Expression_ExampleB_TakesTheBranchBeforeDividing) {
    // B(t) = 0 for t < t0, else cos(60(t - t0)) / t.
    //
    // THE RULE. `if` short-circuits, so the division is never evaluated on
    // the branch that was not taken. At t = 0 the denominator is zero, and a
    // strict evaluator would divide anyway and produce an infinity that
    // poisons the sum.
    const std::string formula = "if(t < t0, 0, cos(60*(t - t0))/t)";

    Expression expression;
    SH_CHECK(expression.Parse(formula, {"t", "t0"}).empty());

    ExpressionDiagnostics diagnostics;
    // Before t0, including exactly t = 0.
    SH_CHECK(expression.Evaluate({{"t", 0.0}, {"t0", 0.05}}, diagnostics) == 0.0);
    SH_CHECK(expression.Evaluate({{"t", 0.04}, {"t0", 0.05}}, diagnostics) == 0.0);
    // And no division was attempted on that branch.
    SH_CHECK(diagnostics.guardedDivisions == 0);

    const double t = 0.2, t0 = 0.05;
    SH_CHECK(Near(expression.Evaluate({{"t", t}, {"t0", t0}}, diagnostics),
                  std::cos(60 * (t - t0)) / t, 1e-12));
}

SH_TEST(Expression_ADivisionByZeroIsCountedRatherThanReturningInfinity) {
    Expression expression;
    SH_CHECK(expression.Parse("1/t", {"t"}).empty());
    ExpressionDiagnostics diagnostics;
    const double value = expression.Evaluate({{"t", 0.0}}, diagnostics);
    SH_CHECK(value == 0.0);
    SH_CHECK(diagnostics.guardedDivisions == 1);
    // An infinity would propagate into the summed waveform and make every
    // later measurement meaningless, so it never leaves this function.
    SH_CHECK(std::isfinite(value));
}

SH_TEST(Expression_UnknownNamesAndFunctionsAreRefused) {
    Expression expression;
    SH_CHECK(!expression.Parse("system(1)", {"t"}).empty());
    SH_CHECK(!expression.Parse("x*2", {"t"}).empty());        // x was not offered
    SH_CHECK(!expression.Parse("sin(t", {"t"}).empty());      // unbalanced
    SH_CHECK(!expression.Parse("sin(t) t", {"t"}).empty());   // trailing junk
    SH_CHECK(!expression.Parse("", {"t"}).empty());
    SH_CHECK(!expression.Parse("sin(t, 2)", {"t"}).empty());  // wrong arity
    SH_CHECK(!expression.Parse("t @ 2", {"t"}).empty());      // stray character
    SH_CHECK(!expression.Ok());

    // A name IS usable once it is offered.
    SH_CHECK(expression.Parse("x*2", {"t", "x"}).empty());
}

SH_TEST(Expression_ARefusedFormulaEvaluatesToZeroRatherThanGarbage) {
    Expression expression;
    SH_CHECK(!expression.Parse("nope(t)", {"t"}).empty());
    ExpressionDiagnostics diagnostics;
    SH_CHECK(expression.Evaluate({{"t", 1.0}}, diagnostics) == 0.0);
}

SH_TEST(Expression_BuiltInConstantsAndGuardedFunctions) {
    SH_CHECK(Near(Eval("pi", 0.0), 3.14159265358979323846, 1e-12));
    SH_CHECK(Near(Eval("sin(pi/2)", 0.0), 1.0, 1e-12));

    // sqrt and log of an out-of-domain value give 0 and are counted, rather
    // than returning a NaN that silently spreads through the sum.
    Expression expression;
    SH_CHECK(expression.Parse("sqrt(t)", {"t"}).empty());
    ExpressionDiagnostics diagnostics;
    SH_CHECK(expression.Evaluate({{"t", -1.0}}, diagnostics) == 0.0);
    SH_CHECK(diagnostics.nonFiniteResults == 1);
}

SH_TEST(Expression_ComparisonsAndClamp) {
    SH_CHECK(Eval("t < 1", 0.5) == 1.0);
    SH_CHECK(Eval("t < 1", 1.5) == 0.0);
    SH_CHECK(Eval("t >= 1", 1.0) == 1.0);
    SH_CHECK(Near(Eval("clamp(t, 0, 1)", 5.0), 1.0));
    SH_CHECK(Near(Eval("clamp(t, 0, 1)", -5.0), 0.0));
    SH_CHECK(Near(Eval("min(t, 3)", 9.0), 3.0));
}

SH_TEST(Expression_LongFormulasAreRefusedRatherThanRunningUnbounded) {
    // The node cap is what makes the cost of one evaluation provably bounded.
    std::string deep = "t";
    for (int i = 0; i < 600; ++i) deep += "+t";
    Expression expression;
    SH_CHECK(!expression.Parse(deep, {"t"}).empty());
}

SH_TEST(Expression_SupportedFunctionsIsTheWholeList) {
    const auto names = Expression::SupportedFunctions();
    SH_CHECK(!names.empty());
    Expression expression;
    // Every name it advertises parses, which is what makes the list usable as
    // help text rather than a hopeful description.
    for (const auto& name : names) {
        const std::string one = name + "(t)";
        const std::string two = name + "(t,t)";
        const std::string three = name + "(t,t,t)";
        const bool any = expression.Parse(one, {"t"}).empty() ||
                         expression.Parse(two, {"t"}).empty() ||
                         expression.Parse(three, {"t"}).empty();
        SH_CHECK(any);
    }
}

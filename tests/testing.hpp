#pragma once

// Minimal, dependency-free test registration/assertion framework. This
// project intentionally avoids pulling in a third-party test framework
// (Catch2/GoogleTest) since the test suite here is small and simple.

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> fn) {
        Registry().push_back(TestCase{name, std::move(fn)});
    }
};

/// Thrown by SH_CHECK on failure; caught and reported by the test runner.
struct AssertionFailure {
    std::string message;
};

} // namespace sekiro_haptics::testing

#define SH_CONCAT_INNER(a, b) a##b
#define SH_CONCAT(a, b) SH_CONCAT_INNER(a, b)

#define SH_TEST(testName)                                                              \
    static void testName();                                                            \
    static ::sekiro_haptics::testing::TestRegistrar SH_CONCAT(sh_registrar_, testName)( \
        #testName, testName);                                                           \
    static void testName()

#define SH_CHECK(condition)                                                     \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::ostringstream sh_oss;                                         \
            sh_oss << "CHECK failed: " #condition " at " << __FILE__ << ":"    \
                   << __LINE__;                                                 \
            throw ::sekiro_haptics::testing::AssertionFailure{sh_oss.str()};    \
        }                                                                       \
    } while (0)

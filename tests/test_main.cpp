#include "testing.hpp"

#include <iostream>

int main() {
    using namespace sekiro_haptics::testing;

    int failed = 0;
    for (const auto& test : Registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const AssertionFailure& failure) {
            std::cout << "[FAIL] " << test.name << ": " << failure.message << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << test.name << ": unexpected exception: " << ex.what() << '\n';
            ++failed;
        }
    }

    const int total = static_cast<int>(Registry().size());
    std::cout << '\n' << (total - failed) << '/' << total << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

// =========================================================================
// test_framework.hpp - Minimal Test Framework
// =========================================================================
// TEACHING NOTE: Real test frameworks like Google Test and Catch2 are
// large, complex pieces of software. But at their core, they do three
// simple things:
//   1. Run test functions
//   2. Check assertions (if a check fails, report it)
//   3. Summarize results (X passed, Y failed)
//
// We implement these three things with simple macros. No external
// dependency. No build system integration. Just #include this file and
// write tests. This is how all test frameworks start before they grow
// complex features like fixtures, parameterized tests, death tests, etc.
//
// Usage:
//   TEST(MyTestName) {
//       ASSERT_EQ(2 + 2, 4);
//       ASSERT_TRUE(1 == 1);
//   }
//   int main() { RUN_TESTS(); }
//
// The RUN_TESTS macro expands to a main() that runs all registered tests.
// =========================================================================

#ifndef CHINSTRAP_TEST_FRAMEWORK_HPP
#define CHINSTRAP_TEST_FRAMEWORK_HPP

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstdlib>

// -------------------------------------------------------------------------
// Global test registry
// -------------------------------------------------------------------------
// TEACHING NOTE: We use a static vector of test functions. Each TEST()
// macro registers its function into this vector at static initialization
// time (before main() runs). RUN_TESTS() then iterates and calls them.
// This pattern is used by Catch2 and many other frameworks. The static
// initialization order fiasco is a real C++ problem, but function-local
// statics avoid it (Meyers Singleton pattern).

namespace chinstrap_test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

// Track current test for better error messages
inline std::string& current_test_name() {
    static std::string name;
    return name;
}

inline int& test_count() {
    static int count = 0;
    return count;
}

inline int& fail_count() {
    static int count = 0;
    return count;
}

} // namespace chinstrap_test

// -------------------------------------------------------------------------
// Assertion macros
// -------------------------------------------------------------------------
// TEACHING NOTE: These macros use a do-while(0) idiom so they behave like
// statements (require a semicolon, work in if/else without braces).
// The __FILE__ and __LINE__ macros give us the source location of the
// assertion, which is essential for debugging test failures.

#define ASSERT_TRUE(expr) \
    do { \
        chinstrap_test::test_count()++; \
        if (!(expr)) { \
            chinstrap_test::fail_count()++; \
            std::cerr << "FAIL: " << chinstrap_test::current_test_name() \
                      << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << " ASSERT_TRUE(" #expr << ")" << std::endl; \
            return; \
        } \
    } while (0)

#define ASSERT_FALSE(expr) \
    do { \
        chinstrap_test::test_count()++; \
        if ((expr)) { \
            chinstrap_test::fail_count()++; \
            std::cerr << "FAIL: " << chinstrap_test::current_test_name() \
                      << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << " ASSERT_FALSE(" #expr << ")" << std::endl; \
            return; \
        } \
    } while (0)

#define ASSERT_EQ(actual, expected) \
    do { \
        chinstrap_test::test_count()++; \
        auto _a = (actual); \
        auto _e = (expected); \
        if (!(_a == _e)) { \
            chinstrap_test::fail_count()++; \
            std::cerr << "FAIL: " << chinstrap_test::current_test_name() \
                      << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << " ASSERT_EQ(" #actual ", " #expected ")" << std::endl \
                      << "  expected: " << _e << std::endl \
                      << "  actual:   " << _a << std::endl; \
            return; \
        } \
    } while (0)

#define ASSERT_NE(actual, expected) \
    do { \
        chinstrap_test::test_count()++; \
        auto _a = (actual); \
        auto _e = (expected); \
        if (_a == _e) { \
            chinstrap_test::fail_count()++; \
            std::cerr << "FAIL: " << chinstrap_test::current_test_name() \
                      << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << " ASSERT_NE(" #actual ", " #expected ")" << std::endl \
                      << "  both equal: " << _a << std::endl; \
            return; \
        } \
    } while (0)

#define ASSERT_STREQ(actual, expected) \
    do { \
        chinstrap_test::test_count()++; \
        std::string _a(actual); \
        std::string _e(expected); \
        if (_a != _e) { \
            chinstrap_test::fail_count()++; \
            std::cerr << "FAIL: " << chinstrap_test::current_test_name() \
                      << " [" << __FILE__ << ":" << __LINE__ << "]" \
                      << " ASSERT_STREQ" << std::endl \
                      << "  expected: \"" << _e << "\"" << std::endl \
                      << "  actual:   \"" << _a << "\"" << std::endl; \
            return; \
        } \
    } while (0)

// -------------------------------------------------------------------------
// TEST macro - define and register a test case
// -------------------------------------------------------------------------
// TEACHING NOTE: The ## operator in the macro concatenates tokens to
// create unique function names. We register the function in the global
// registry, and RUN_TESTS will call it later.

#define TEST(name) \
    static void test_##name(); \
    static int reg_##name = [] { \
        chinstrap_test::test_registry().push_back({#name, test_##name}); \
        return 0; \
    }(); \
    static void test_##name()

// -------------------------------------------------------------------------
// RUN_TESTS macro - main() that runs all registered tests
// -------------------------------------------------------------------------
// TEACHING NOTE: This expands to a main() function. We iterate over all
// registered tests, run each one, and report pass/fail counts. If any
// test failed, we exit with status 1 so CI systems know tests failed.

#define RUN_TESTS() \
    int main() { \
        std::cout << "Running " << chinstrap_test::test_registry().size() \
                  << " test(s)..." << std::endl; \
        for (const auto& tc : chinstrap_test::test_registry()) { \
            chinstrap_test::current_test_name() = tc.name; \
            std::cout << "[ RUN      ] " << tc.name << std::endl; \
            tc.func(); \
            std::cout << "[       OK ] " << tc.name << std::endl; \
        } \
        std::cout << "========================================" << std::endl; \
        std::cout << "Tests run: " << chinstrap_test::test_count() << std::endl; \
        std::cout << "Failures:  " << chinstrap_test::fail_count() << std::endl; \
        if (chinstrap_test::fail_count() > 0) { \
            std::cerr << "TEST SUITE FAILED" << std::endl; \
            return 1; \
        } \
        std::cout << "ALL TESTS PASSED" << std::endl; \
        return 0; \
    }

#endif // CHINSTRAP_TEST_FRAMEWORK_HPP
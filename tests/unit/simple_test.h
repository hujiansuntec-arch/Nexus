#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <sstream>
#include <cmath>
#include <cstring>

namespace nexus_test {

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry instance;
        return instance;
    }

    void registerTest(const std::string& suite, const std::string& name, std::function<void()> body) {
        tests_.push_back({suite, name, body});
    }

    int runAll() {
        int passed = 0;
        int failed = 0;

        std::cout << "[==========] Running " << tests_.size() << " tests." << std::endl;

        for (const auto& test : tests_) {
            std::cout << "[ RUN      ] " << test.suite << "." << test.name << std::endl;
            try {
                test.body();
                std::cout << "[       OK ] " << test.suite << "." << test.name << std::endl;
                passed++;
            } catch (const std::exception& e) {
                std::cout << "[  FAILED  ] " << test.suite << "." << test.name << std::endl;
                std::cout << "[   INFO   ] " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "[  FAILED  ] " << test.suite << "." << test.name << std::endl;
                std::cout << "[   INFO   ] Unknown exception" << std::endl;
                failed++;
            }
        }

        std::cout << "[==========] " << tests_.size() << " tests ran." << std::endl;
        std::cout << "[  PASSED  ] " << passed << " tests." << std::endl;
        if (failed > 0) {
            std::cout << "[  FAILED  ] " << failed << " tests." << std::endl;
        }

        return (failed == 0) ? 0 : 1;
    }

private:
    struct TestInfo {
        std::string suite;
        std::string name;
        std::function<void()> body;
    };

    std::vector<TestInfo> tests_;
};

class TestRegistrar {
public:
    TestRegistrar(const std::string& suite, const std::string& name, std::function<void()> body) {
        TestRegistry::instance().registerTest(suite, name, body);
    }
};

class AssertionFailure : public std::exception {
public:
    AssertionFailure(const std::string& msg) : msg_(msg) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

} // namespace nexus_test

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    static nexus_test::TestRegistrar registrar_##suite##_##name(#suite, #name, test_##suite##_##name); \
    void test_##suite##_##name()

#define ASSERT_TRUE(condition) \
    if (!(condition)) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #condition << " at " << __FILE__ << ":" << __LINE__; \
        throw nexus_test::AssertionFailure(ss.str()); \
    }

#define ASSERT_FALSE(condition) \
    if (condition) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #condition << " is true at " << __FILE__ << ":" << __LINE__; \
        throw nexus_test::AssertionFailure(ss.str()); \
    }

#define ASSERT_EQ(expected, actual) \
    if ((expected) != (actual)) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #expected << " == " << #actual << " at " << __FILE__ << ":" << __LINE__ \
           << "\n  Expected: " << (expected) << "\n  Actual:   " << (actual); \
        throw nexus_test::AssertionFailure(ss.str()); \
    }

#define ASSERT_NE(val1, val2) \
    if ((val1) == (val2)) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #val1 << " != " << #val2 << " at " << __FILE__ << ":" << __LINE__; \
        throw nexus_test::AssertionFailure(ss.str()); \
    }

#define ASSERT_GT(val1, val2) \
    if ((val1) <= (val2)) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #val1 << " > " << #val2 << " at " << __FILE__ << ":" << __LINE__; \
        throw nexus_test::AssertionFailure(ss.str()); \
    }

#define ASSERT_LT(val1, val2) \
    if ((val1) >= (val2)) { \
        std::stringstream ss; \
        ss << "Assertion failed: " << #val1 << " < " << #val2 << " at " << __FILE__ << ":" << __LINE__; \
        throw nexus_test::AssertionFailure(ss.str()); \
    }


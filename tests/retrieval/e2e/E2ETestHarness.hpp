#pragma once

// =============================================================================
// Roclint Retrieval Subsystem — Unified E2E Test Harness
// Header-only test runner and non-fatal assertion engine for MSVC C++20.
// Conforms strictly to /W4 /w14061 /WX /utf-8 /permissive-.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::e2e {

// -----------------------------------------------------------------------------
// Enums
// -----------------------------------------------------------------------------

enum class TestTier : int {
    Tier1 = 1,
    Tier2 = 2,
    Tier3 = 3,
    Tier4 = 4
};

[[nodiscard]] inline constexpr const char* toString(TestTier tier) noexcept {
    switch (tier) {
        case TestTier::Tier1: return "Tier1";
        case TestTier::Tier2: return "Tier2";
        case TestTier::Tier3: return "Tier3";
        case TestTier::Tier4: return "Tier4";
    }
    return "UnknownTier";
}

[[nodiscard]] inline constexpr int toInt(TestTier tier) noexcept {
    switch (tier) {
        case TestTier::Tier1: return 1;
        case TestTier::Tier2: return 2;
        case TestTier::Tier3: return 3;
        case TestTier::Tier4: return 4;
    }
    return 1;
}

[[nodiscard]] inline std::optional<TestTier> parseTier(std::string_view sv) noexcept {
    if (sv == "1" || sv == "Tier1" || sv == "tier1") return TestTier::Tier1;
    if (sv == "2" || sv == "Tier2" || sv == "tier2") return TestTier::Tier2;
    if (sv == "3" || sv == "Tier3" || sv == "tier3") return TestTier::Tier3;
    if (sv == "4" || sv == "Tier4" || sv == "tier4") return TestTier::Tier4;
    return std::nullopt;
}

enum class TestStatus : int {
    Passed = 0,
    Failed = 1,
    NotRun = 2
};

[[nodiscard]] inline constexpr const char* toString(TestStatus status) noexcept {
    switch (status) {
        case TestStatus::Passed: return "PASSED";
        case TestStatus::Failed: return "FAILED";
        case TestStatus::NotRun: return "NOT_RUN";
    }
    return "UNKNOWN";
}

enum class OutputFormat : int {
    Console = 0,
    Tap = 1,
    JUnit = 2,
    Json = 3
};

[[nodiscard]] inline std::optional<OutputFormat> parseFormat(std::string_view sv) noexcept {
    if (sv == "console" || sv == "CONSOLE") return OutputFormat::Console;
    if (sv == "tap" || sv == "TAP") return OutputFormat::Tap;
    if (sv == "junit" || sv == "JUNIT" || sv == "xml" || sv == "XML") return OutputFormat::JUnit;
    if (sv == "json" || sv == "JSON") return OutputFormat::Json;
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Failure Diagnostics & Exceptions
// -----------------------------------------------------------------------------

struct FailureInfo {
    std::string file;
    int line{0};
    std::string expression;
    std::string details;
};

class RequireFailedException final : public std::exception {
public:
    RequireFailedException(const char* file, int line, const char* expr)
        : message_(std::string(file ? file : "") + ":" + std::to_string(line) + ": REQUIRE failed: " + (expr ? expr : ""))
    {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

class NotRunException final : public std::exception {
public:
    explicit NotRunException(std::string reason)
        : reason_(std::move(reason))
    {}
    [[nodiscard]] const char* what() const noexcept override { return reason_.c_str(); }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
private:
    std::string reason_;
};

// -----------------------------------------------------------------------------
// Test Context (Per-Test Execution Scope)
// -----------------------------------------------------------------------------

class TestContext {
public:
    TestContext() = default;

    void addFailure(const char* file, int line, const char* expr, std::string details = "") {
        failures_.push_back(FailureInfo{
            file ? file : "",
            line,
            expr ? expr : "",
            std::move(details)
        });
    }

    [[nodiscard]] bool hasFailures() const noexcept { return !failures_.empty(); }
    [[nodiscard]] std::size_t failureCount() const noexcept { return failures_.size(); }
    [[nodiscard]] const std::vector<FailureInfo>& failures() const noexcept { return failures_; }

    static void setCurrent(TestContext* ctx) noexcept { currentContext_ = ctx; }
    [[nodiscard]] static TestContext* current() noexcept { return currentContext_; }

private:
    std::vector<FailureInfo> failures_;
    static inline thread_local TestContext* currentContext_{nullptr};
};

// -----------------------------------------------------------------------------
// Test Definition & Results
// -----------------------------------------------------------------------------

struct TestDefinition {
    std::string suite;
    std::string name;
    std::string fullName; // "suite.name"
    TestTier tier{TestTier::Tier1};
    std::string feature;  // "F1" .. "F19"
    std::function<void(TestContext&)> func;
};

struct TestResult {
    TestDefinition test;
    TestStatus status{TestStatus::Passed};
    std::vector<FailureInfo> failures;
    std::string skipReason;
    double durationMs{0.0};
};

// -----------------------------------------------------------------------------
// Test Registry & Registrar
// -----------------------------------------------------------------------------

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void registerTest(TestDefinition def) {
        tests_.push_back(std::move(def));
    }

    [[nodiscard]] const std::vector<TestDefinition>& tests() const noexcept {
        return tests_;
    }

private:
    TestRegistry() = default;
    std::vector<TestDefinition> tests_;
};

class TestRegistrar {
public:
    TestRegistrar(
        const char* suite,
        const char* name,
        TestTier tier,
        const char* feature,
        void (*func)(TestContext&))
    {
        registerInternal(suite, name, tier, feature, func);
    }

    TestRegistrar(
        const char* suite,
        const char* name,
        int tierInt,
        const char* feature,
        void (*func)(TestContext&))
    {
        TestTier tier = TestTier::Tier1;
        switch (tierInt) {
            case 1: tier = TestTier::Tier1; break;
            case 2: tier = TestTier::Tier2; break;
            case 3: tier = TestTier::Tier3; break;
            case 4: tier = TestTier::Tier4; break;
            default: tier = TestTier::Tier1; break;
        }
        registerInternal(suite, name, tier, feature, func);
    }

private:
    static void registerInternal(
        const char* suite,
        const char* name,
        TestTier tier,
        const char* feature,
        void (*func)(TestContext&))
    {
        TestDefinition def;
        def.suite = suite ? suite : "DefaultSuite";
        def.name = name ? name : "UnnamedTest";
        def.fullName = def.suite + "." + def.name;
        def.tier = tier;
        def.feature = feature ? feature : "F0";
        def.func = func;
        TestRegistry::instance().registerTest(std::move(def));
    }
};

// -----------------------------------------------------------------------------
// String Utilities
// -----------------------------------------------------------------------------

namespace detail {

template <typename T>
[[nodiscard]] inline std::string formatValue(const T& val) {
    if constexpr (requires(std::ostream& os) { os << val; }) {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<std::underlying_type_t<T>>(val));
    } else {
        return "<unprintable>";
    }
}

[[nodiscard]] inline std::string toLower(std::string_view sv) {
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

[[nodiscard]] inline bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto h = toLower(haystack);
    auto n = toLower(needle);
    return h.find(n) != std::string::npos;
}

[[nodiscard]] inline bool equalsCaseInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    auto la = toLower(a);
    auto lb = toLower(b);
    return la == lb;
}

[[nodiscard]] inline std::string xmlEscape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 16);
    for (char c : sv) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

[[nodiscard]] inline std::string jsonEscape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 16);
    for (char c : sv) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    out += oss.str();
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

} // namespace detail

// -----------------------------------------------------------------------------
// Test Runner Configuration & Execution
// -----------------------------------------------------------------------------

struct RunnerOptions {
    std::vector<TestTier> tiers;
    std::string featureFilter;
    std::string nameFilter;
    OutputFormat format{OutputFormat::Console};
    std::string outputFile;
    std::string modelDir;
    bool requireRealModel{false};
    bool listOnly{false};
    bool verbose{false};
};

class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner r;
        return r;
    }

    [[nodiscard]] const RunnerOptions& options() const noexcept { return options_; }

    int run(int argc, char* argv[]) {
        if (!parseArguments(argc, argv)) {
            printUsage();
            return 2;
        }

        const auto& allTests = TestRegistry::instance().tests();

        if (options_.listOnly) {
            std::cout << "Registered E2E Tests (" << allTests.size() << " total):\n";
            for (const auto& t : allTests) {
                if (isTestSelected(t)) {
                    std::cout << "  [" << toString(t.tier) << "] ["
                              << t.feature << "] " << t.fullName << "\n";
                }
            }
            return 0;
        }

        std::vector<TestDefinition> selected;
        for (const auto& t : allTests) {
            if (isTestSelected(t)) {
                selected.push_back(t);
            }
        }

        if (options_.format == OutputFormat::Console) {
            std::cout << "======================================================================\n";
            std::cout << "Roclint Retrieval E2E Test Suite\n";
            std::cout << "Selected Tests: " << selected.size() << " of " << allTests.size() << "\n";
            std::cout << "======================================================================\n";
        }

        std::vector<TestResult> results;
        results.reserve(selected.size());

        std::size_t passedCount = 0;
        std::size_t failedCount = 0;
        std::size_t notRunCount = 0;

        const auto suiteStart = std::chrono::steady_clock::now();

        for (const auto& test : selected) {
            TestResult res;
            res.test = test;

            TestContext ctx;
            TestContext::setCurrent(&ctx);

            const auto testStart = std::chrono::steady_clock::now();

            try {
                test.func(ctx);
                if (ctx.hasFailures()) {
                    res.status = TestStatus::Failed;
                    res.failures = ctx.failures();
                    ++failedCount;
                } else {
                    res.status = TestStatus::Passed;
                    ++passedCount;
                }
            } catch (const NotRunException& e) {
                res.status = TestStatus::NotRun;
                res.skipReason = e.reason();
                if (options_.requireRealModel) {
                    res.status = TestStatus::Failed;
                    res.failures.push_back(FailureInfo{"", 0, "Missing required model assets", e.reason()});
                    ++failedCount;
                } else {
                    ++notRunCount;
                }
            } catch (const RequireFailedException& e) {
                res.status = TestStatus::Failed;
                res.failures = ctx.failures();
                if (res.failures.empty()) {
                    res.failures.push_back(FailureInfo{"", 0, e.what(), ""});
                }
                ++failedCount;
            } catch (const std::exception& e) {
                res.status = TestStatus::Failed;
                res.failures = ctx.failures();
                res.failures.push_back(FailureInfo{"", 0, "Unhandled std::exception", e.what()});
                ++failedCount;
            } catch (...) {
                res.status = TestStatus::Failed;
                res.failures = ctx.failures();
                res.failures.push_back(FailureInfo{"", 0, "Unknown unhandled exception", ""});
                ++failedCount;
            }

            const auto testEnd = std::chrono::steady_clock::now();
            res.durationMs = std::chrono::duration<double, std::milli>(testEnd - testStart).count();

            TestContext::setCurrent(nullptr);
            results.push_back(res);

            if (options_.format == OutputFormat::Console) {
                printTestResultConsole(res);
            }
        }

        const auto suiteEnd = std::chrono::steady_clock::now();
        const double totalDurationMs = std::chrono::duration<double, std::milli>(suiteEnd - suiteStart).count();

        std::string formattedOutput;
        switch (options_.format) {
            case OutputFormat::Console:
                formattedOutput = formatConsoleSummary(results, passedCount, failedCount, notRunCount, totalDurationMs);
                break;
            case OutputFormat::Tap:
                formattedOutput = formatTap(results);
                break;
            case OutputFormat::JUnit:
                formattedOutput = formatJUnit(results, totalDurationMs);
                break;
            case OutputFormat::Json:
                formattedOutput = formatJson(results, totalDurationMs);
                break;
        }

        if (!options_.outputFile.empty()) {
            std::ofstream ofs(options_.outputFile);
            if (ofs.is_open()) {
                ofs << formattedOutput;
                ofs.close();
            } else {
                std::cerr << "[ERROR] Unable to write report to file: " << options_.outputFile << "\n";
            }
        } else if (options_.format != OutputFormat::Console) {
            std::cout << formattedOutput;
        } else {
            std::cout << formattedOutput;
        }

        if (failedCount > 0) {
            return 1;
        }
        return 0;
    }

private:
    TestRunner() = default;
    RunnerOptions options_;

    [[nodiscard]] bool isTestSelected(const TestDefinition& test) const {
        if (!options_.tiers.empty()) {
            bool tierMatch = false;
            for (auto t : options_.tiers) {
                if (test.tier == t) {
                    tierMatch = true;
                    break;
                }
            }
            if (!tierMatch) return false;
        }
        if (!options_.featureFilter.empty()) {
            if (!detail::equalsCaseInsensitive(test.feature, options_.featureFilter)) {
                return false;
            }
        }
        if (!options_.nameFilter.empty()) {
            if (!detail::containsCaseInsensitive(test.fullName, options_.nameFilter)) {
                return false;
            }
        }
        return true;
    }

    bool parseArguments(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);

            if (arg == "--tier") {
                if (++i >= argc) return false;
                std::string s(argv[i]);
                std::stringstream ss(s);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (auto t = parseTier(item)) {
                        options_.tiers.push_back(*t);
                    } else {
                        std::cerr << "Invalid tier value: " << item << "\n";
                        return false;
                    }
                }
            } else if (arg == "--feature") {
                if (++i >= argc) return false;
                options_.featureFilter = argv[i];
            } else if (arg == "--filter") {
                if (++i >= argc) return false;
                options_.nameFilter = argv[i];
            } else if (arg == "--format") {
                if (++i >= argc) return false;
                if (auto f = parseFormat(argv[i])) {
                    options_.format = *f;
                } else {
                    std::cerr << "Unknown format: " << argv[i] << "\n";
                    return false;
                }
            } else if (arg == "--output") {
                if (++i >= argc) return false;
                options_.outputFile = argv[i];
            } else if (arg == "--model-dir") {
                if (++i >= argc) return false;
                options_.modelDir = argv[i];
            } else if (arg == "--require-real-model") {
                options_.requireRealModel = true;
            } else if (arg == "--list") {
                options_.listOnly = true;
            } else if (arg == "--verbose" || arg == "-v") {
                options_.verbose = true;
            } else if (arg == "--help" || arg == "-h") {
                printUsage();
                std::exit(0);
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }
        return true;
    }

    void printUsage() const {
        std::cout << "Usage: roclint_e2e_tests [options]\n\n"
                  << "Options:\n"
                  << "  --tier <1|2|3|4>            Run tests for specified tier(s) (comma-separated, e.g. 1,2)\n"
                  << "  --feature <F1..F19>         Run tests matching feature ID\n"
                  << "  --filter <substr>           Case-insensitive substring filter for test name\n"
                  << "  --format <console|tap|junit|json>  Output format (default: console)\n"
                  << "  --output <file>             Write report output to specified file\n"
                  << "  --model-dir <path>          Path to pinned Granite embedding model assets\n"
                  << "  --require-real-model        Fail tests if required model assets are missing\n"
                  << "  --list                      List matching registered tests and exit\n"
                  << "  --verbose, -v               Enable detailed test output\n"
                  << "  --help, -h                  Display this help message\n";
    }

    void printTestResultConsole(const TestResult& res) const {
        switch (res.status) {
            case TestStatus::Passed:
                std::cout << "[PASSED]  " << res.test.fullName
                          << " (" << std::fixed << std::setprecision(2) << res.durationMs << " ms)\n";
                break;
            case TestStatus::Failed:
                std::cout << "[FAILED]  " << res.test.fullName
                          << " (" << std::fixed << std::setprecision(2) << res.durationMs << " ms)\n";
                for (const auto& fail : res.failures) {
                    std::cout << "    --> " << fail.file << ":" << fail.line
                              << ": " << fail.expression;
                    if (!fail.details.empty()) {
                        std::cout << " (" << fail.details << ")";
                    }
                    std::cout << "\n";
                }
                break;
            case TestStatus::NotRun:
                std::cout << "[NOT RUN] " << res.test.fullName;
                if (!res.skipReason.empty()) {
                    std::cout << " - Reason: " << res.skipReason;
                }
                std::cout << "\n";
                break;
        }
    }

    [[nodiscard]] std::string formatConsoleSummary(
        const std::vector<TestResult>& results,
        std::size_t passed,
        std::size_t failed,
        std::size_t notRun,
        double totalDurationMs) const
    {
        (void)results;
        std::ostringstream oss;
        oss << "\n======================================================================\n";
        oss << "Execution Summary:\n";
        oss << "  Total:    " << (passed + failed + notRun) << "\n";
        oss << "  Passed:   " << passed << "\n";
        oss << "  Failed:   " << failed << "\n";
        oss << "  NOT RUN:  " << notRun << "\n";
        oss << "  Duration: " << std::fixed << std::setprecision(2) << totalDurationMs << " ms\n";
        oss << "======================================================================\n";
        if (failed > 0) {
            oss << "OVERALL STATUS: FAILED\n";
        } else {
            oss << "OVERALL STATUS: PASSED\n";
        }
        return oss.str();
    }

    [[nodiscard]] std::string formatTap(const std::vector<TestResult>& results) const {
        std::ostringstream oss;
        oss << "TAP version 13\n";
        oss << "1.." << results.size() << "\n";

        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& res = results[i];
            const std::size_t idx = i + 1;

            switch (res.status) {
                case TestStatus::Passed:
                    oss << "ok " << idx << " - " << res.test.fullName << "\n";
                    break;
                case TestStatus::Failed:
                    oss << "not ok " << idx << " - " << res.test.fullName << "\n";
                    oss << "  ---\n";
                    for (const auto& f : res.failures) {
                        oss << "  message: \"" << detail::jsonEscape(f.expression) << "\"\n";
                        oss << "  file: \"" << detail::jsonEscape(f.file) << "\"\n";
                        oss << "  line: " << f.line << "\n";
                    }
                    oss << "  ...\n";
                    break;
                case TestStatus::NotRun:
                    oss << "ok " << idx << " - " << res.test.fullName
                        << " # SKIP " << res.skipReason << "\n";
                    break;
            }
        }
        return oss.str();
    }

    [[nodiscard]] std::string formatJUnit(
        const std::vector<TestResult>& results,
        double totalDurationMs) const
    {
        std::size_t failures = 0;
        std::size_t skipped = 0;
        for (const auto& r : results) {
            if (r.status == TestStatus::Failed) ++failures;
            else if (r.status == TestStatus::NotRun) ++skipped;
        }

        std::ostringstream oss;
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        oss << "<testsuites name=\"roclint_e2e\" tests=\"" << results.size()
            << "\" failures=\"" << failures
            << "\" skipped=\"" << skipped
            << "\" time=\"" << std::fixed << std::setprecision(3) << (totalDurationMs / 1000.0)
            << "\">\n";

        oss << "  <testsuite name=\"roclint_e2e_suite\" tests=\"" << results.size()
            << "\" failures=\"" << failures
            << "\" skipped=\"" << skipped
            << "\" time=\"" << std::fixed << std::setprecision(3) << (totalDurationMs / 1000.0)
            << "\">\n";

        for (const auto& res : results) {
            oss << "    <testcase classname=\"" << detail::xmlEscape(res.test.suite)
                << "\" name=\"" << detail::xmlEscape(res.test.name)
                << "\" time=\"" << std::fixed << std::setprecision(3) << (res.durationMs / 1000.0)
                << "\">\n";

            switch (res.status) {
                case TestStatus::Passed:
                    break;
                case TestStatus::Failed:
                    oss << "      <failure message=\"Assertion failure\" type=\"AssertionError\">\n";
                    for (const auto& f : res.failures) {
                        oss << detail::xmlEscape(f.file) << ":" << f.line << ": "
                            << detail::xmlEscape(f.expression) << "\n";
                    }
                    oss << "      </failure>\n";
                    break;
                case TestStatus::NotRun:
                    oss << "      <skipped message=\""
                        << detail::xmlEscape(res.skipReason.empty() ? "Precondition not met" : res.skipReason)
                        << "\"/>\n";
                    break;
            }

            oss << "    </testcase>\n";
        }

        oss << "  </testsuite>\n";
        oss << "</testsuites>\n";
        return oss.str();
    }

    [[nodiscard]] std::string formatJson(
        const std::vector<TestResult>& results,
        double totalDurationMs) const
    {
        std::size_t passed = 0;
        std::size_t failed = 0;
        std::size_t notRun = 0;
        for (const auto& r : results) {
            if (r.status == TestStatus::Passed) ++passed;
            else if (r.status == TestStatus::Failed) ++failed;
            else if (r.status == TestStatus::NotRun) ++notRun;
        }

        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"summary\": {\n";
        oss << "    \"total\": " << results.size() << ",\n";
        oss << "    \"passed\": " << passed << ",\n";
        oss << "    \"failed\": " << failed << ",\n";
        oss << "    \"not_run\": " << notRun << ",\n";
        oss << "    \"duration_ms\": " << std::fixed << std::setprecision(2) << totalDurationMs << "\n";
        oss << "  },\n";
        oss << "  \"tests\": [\n";

        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            oss << "    {\n";
            oss << "      \"suite\": \"" << detail::jsonEscape(r.test.suite) << "\",\n";
            oss << "      \"name\": \"" << detail::jsonEscape(r.test.name) << "\",\n";
            oss << "      \"fullName\": \"" << detail::jsonEscape(r.test.fullName) << "\",\n";
            oss << "      \"tier\": " << toInt(r.test.tier) << ",\n";
            oss << "      \"feature\": \"" << detail::jsonEscape(r.test.feature) << "\",\n";
            oss << "      \"status\": \"" << toString(r.status) << "\",\n";
            oss << "      \"duration_ms\": " << std::fixed << std::setprecision(2) << r.durationMs << ",\n";
            oss << "      \"skip_reason\": \"" << detail::jsonEscape(r.skipReason) << "\",\n";
            oss << "      \"failures\": [";
            for (std::size_t j = 0; j < r.failures.size(); ++j) {
                const auto& f = r.failures[j];
                oss << "{\"file\":\"" << detail::jsonEscape(f.file) << "\",\"line\":" << f.line
                    << ",\"expression\":\"" << detail::jsonEscape(f.expression) << "\"}";
                if (j + 1 < r.failures.size()) oss << ",";
            }
            oss << "]\n";
            oss << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
        }

        oss << "  ]\n";
        oss << "}\n";
        return oss.str();
    }
};

} // namespace roclint::e2e

// -----------------------------------------------------------------------------
// Public E2E Macros
// -----------------------------------------------------------------------------

#define E2E_TEST(suite, name, tier, feature) \
    static void e2e_test_func_##suite##_##name(roclint::e2e::TestContext& ctx); \
    static const roclint::e2e::TestRegistrar e2e_test_reg_##suite##_##name( \
        #suite, #name, (tier), (feature), &e2e_test_func_##suite##_##name); \
    static void e2e_test_func_##suite##_##name(roclint::e2e::TestContext& ctx)

#define E2E_CHECK(expr) \
    do { \
        if (!(expr)) { \
            if (auto* _e2e_c = roclint::e2e::TestContext::current()) { \
                _e2e_c->addFailure(__FILE__, __LINE__, #expr); \
            } else { \
                ctx.addFailure(__FILE__, __LINE__, #expr); \
            } \
        } \
    } while (false)

#define E2E_CHECK_EQ(lhs, rhs) \
    do { \
        if (!((lhs) == (rhs))) { \
            std::ostringstream _e2e_oss; \
            _e2e_oss << #lhs << " == " << #rhs << " (lhs: " << roclint::e2e::detail::formatValue(lhs) << ", rhs: " << roclint::e2e::detail::formatValue(rhs) << ")"; \
            if (auto* _e2e_c = roclint::e2e::TestContext::current()) { \
                _e2e_c->addFailure(__FILE__, __LINE__, #lhs " == " #rhs, _e2e_oss.str()); \
            } else { \
                ctx.addFailure(__FILE__, __LINE__, #lhs " == " #rhs, _e2e_oss.str()); \
            } \
        } \
    } while (false)

#define E2E_CHECK_NE(lhs, rhs) \
    do { \
        if ((lhs) == (rhs)) { \
            std::ostringstream _e2e_oss; \
            _e2e_oss << #lhs << " != " << #rhs << " (both equal: " << roclint::e2e::detail::formatValue(lhs) << ")"; \
            if (auto* _e2e_c = roclint::e2e::TestContext::current()) { \
                _e2e_c->addFailure(__FILE__, __LINE__, #lhs " != " #rhs, _e2e_oss.str()); \
            } else { \
                ctx.addFailure(__FILE__, __LINE__, #lhs " != " #rhs, _e2e_oss.str()); \
            } \
        } \
    } while (false)

#define E2E_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            if (auto* _e2e_c = roclint::e2e::TestContext::current()) { \
                _e2e_c->addFailure(__FILE__, __LINE__, #expr); \
            } else { \
                ctx.addFailure(__FILE__, __LINE__, #expr); \
            } \
            throw roclint::e2e::RequireFailedException(__FILE__, __LINE__, #expr); \
        } \
    } while (false)

#define E2E_REQUIRE_EQ(lhs, rhs) \
    do { \
        if (!((lhs) == (rhs))) { \
            std::ostringstream _e2e_oss; \
            _e2e_oss << #lhs << " == " << #rhs << " (lhs: " << roclint::e2e::detail::formatValue(lhs) << ", rhs: " << roclint::e2e::detail::formatValue(rhs) << ")"; \
            if (auto* _e2e_c = roclint::e2e::TestContext::current()) { \
                _e2e_c->addFailure(__FILE__, __LINE__, #lhs " == " #rhs, _e2e_oss.str()); \
            } else { \
                ctx.addFailure(__FILE__, __LINE__, #lhs " == " #rhs, _e2e_oss.str()); \
            } \
            throw roclint::e2e::RequireFailedException(__FILE__, __LINE__, #lhs " == " #rhs); \
        } \
    } while (false)

#define E2E_NOT_RUN(reason) \
    do { \
        throw roclint::e2e::NotRunException(reason); \
    } while (false)

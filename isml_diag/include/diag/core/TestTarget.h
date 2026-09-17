#pragma once

#include "diag/core/Platform.h"
#include "diag/core/TestInfo.h"

#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// A string keeps module onboarding local: <target_type>.yaml defines the type.
using TargetType = std::string;

struct TestArgumentPolicy {
    std::vector<std::string> range;
};

struct TestcasePolicy {
    std::unordered_map<std::string, TestArgumentPolicy> arguments;
};

using TestcasePolicies = std::unordered_map<std::string, TestcasePolicy>;
using TestcasePolicyCatalog = std::unordered_map<TargetType, TestcasePolicies>;

// Each YAML file contributes one target type named by its file stem.
TestcasePolicyCatalog load_testcase_policies(const std::string& directory);

using TestcaseFunc = std::function<TestStatus(TestInfo& ti)>;

struct TestArgumentDefinition {
    std::string name;
    std::string default_value;
    std::string format;
};

struct TestcaseDefinition {
    std::vector<TestArgumentDefinition> arguments;
    TestcaseFunc execute;
};

class TestTarget {
private:
    std::mutex testcase_mutex_;

protected:
    std::string name_;
    TargetType target_type_;
    DeviceContext ctx_;
    std::unordered_map<std::string, TestcaseDefinition> registered_tests_;

    // Modules declare testcase defaults, formats, and callbacks here.
    void _add_test(const std::string& test_name,
                   std::initializer_list<TestArgumentDefinition> arguments,
                   TestcaseFunc func);

private:
    static bool parse_u64(const std::string& value, uint64_t& parsed);
    static bool is_numeric_format(const std::string& format);
    static bool validate_format(const TestArgumentDefinition& definition,
                                const std::string& value,
                                std::string& error);
    static bool validate_range(const TestArgumentDefinition& definition,
                               const std::string& value,
                               const TestArgumentPolicy& policy,
                               std::string& error);

public:
    TestTarget(const std::string& name,
               TargetType target_type,
               const DeviceContext& ctx);
    virtual ~TestTarget() = default;

    virtual TestStatus run_testcase(const std::string& test_name,
                                    const TestArgs& args = {},
                                    Logger* logger = nullptr,
                                    HalContext* hal = nullptr,
                                    const TestcasePolicy* policy = nullptr);

    const std::string& get_name() const { return name_; }
    const TargetType& get_target_type() const { return target_type_; }
    void* get_bar_base_addr() const { return ctx_.mapped_bar_base; }
    DeviceContext& get_context() { return ctx_; }
    const DeviceContext& get_context() const { return ctx_; }
    std::vector<std::string> get_registered_test_names() const;

    virtual std::vector<TestTarget*> child_targets() const { return {}; }
};

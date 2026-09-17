#include "diag/core/TestTarget.h"

#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

TestTarget::TestTarget(const std::string& name,
                       TargetType target_type,
                       const DeviceContext& ctx)
    : name_(name), target_type_(std::move(target_type)), ctx_(ctx)
{
}

void TestTarget::_add_test(const std::string& test_name,
                           std::initializer_list<TestArgumentDefinition> arguments,
                           TestcaseFunc func)
{
    registered_tests_[test_name] = {
        std::vector<TestArgumentDefinition>(arguments), std::move(func)};
}

bool TestTarget::parse_u64(const std::string& value, uint64_t& parsed)
{
    if (value.empty() || value.front() == '-') return false;
    try {
        size_t consumed = 0;
        parsed = std::stoull(value, &consumed, 0);
        return consumed == value.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool TestTarget::is_numeric_format(const std::string& format)
{
    return format == "u64" || format == "bytes" ||
           format == "ms" || format == "offset";
}

bool TestTarget::validate_format(const TestArgumentDefinition& definition,
                                 const std::string& value,
                                 std::string& error)
{
    if (definition.format == "string") return true;
    if (is_numeric_format(definition.format)) {
        uint64_t parsed = 0;
        if (parse_u64(value, parsed)) return true;
        error = "argument " + definition.name + " must use format " +
                definition.format + ": " + value;
        return false;
    }
    error = "argument " + definition.name + " has unsupported format: " +
            definition.format;
    return false;
}

bool TestTarget::validate_range(const TestArgumentDefinition& definition,
                                const std::string& value,
                                const TestArgumentPolicy& policy,
                                std::string& error)
{
    if (policy.range.empty()) return true;

    if (is_numeric_format(definition.format) && policy.range.size() == 2) {
        uint64_t parsed = 0;
        uint64_t minimum = 0;
        uint64_t maximum = 0;
        if (!parse_u64(value, parsed) || !parse_u64(policy.range[0], minimum) ||
            !parse_u64(policy.range[1], maximum) || minimum > maximum) {
            error = "invalid numeric policy range for argument: " + definition.name;
            return false;
        }
        if (parsed >= minimum && parsed <= maximum) return true;
    } else {
        for (const auto& allowed : policy.range) {
            if (value == allowed) return true;
        }
    }

    std::ostringstream message;
    message << "argument " << definition.name << "=" << value
            << " is outside policy range [";
    for (size_t i = 0; i < policy.range.size(); ++i) {
        if (i != 0) message << ", ";
        message << policy.range[i];
    }
    message << "]";
    error = message.str();
    return false;
}

TestStatus TestTarget::run_testcase(const std::string& test_name,
                                    const TestArgs& args,
                                    Logger* logger,
                                    HalContext* hal,
                                    const TestcasePolicy* policy)
{
    std::lock_guard<std::mutex> lock(testcase_mutex_);
    Logger default_logger;
    TestInfo ti;
    ti.target_name = name_;
    ti.test_name = test_name;
    ti.logger = logger == nullptr ? &default_logger : logger;
    ti.hal = hal;

    const auto test = registered_tests_.find(test_name);
    if (test == registered_tests_.end()) {
        ti.logger->error("testcase is not registered: target=" + name_ +
                         " test=" + test_name);
        return TestStatus::UNIMPLEMENTED;
    }

    TestArgs resolved_args;
    std::unordered_map<std::string, const TestArgumentDefinition*> definitions;
    for (const auto& definition : test->second.arguments) {
        resolved_args[definition.name] = definition.default_value;
        definitions[definition.name] = &definition;
    }
    for (const auto& argument : args) {
        if (definitions.find(argument.first) == definitions.end()) {
            ti.logger->error("unknown argument: test=" + test_name +
                             " argument=" + argument.first);
            return TestStatus::INVALID;
        }
        resolved_args[argument.first] = argument.second;
    }
    for (const auto& definition : test->second.arguments) {
        const auto& value = resolved_args.at(definition.name);
        std::string error;
        if (!validate_format(definition, value, error)) {
            ti.logger->error(error);
            return TestStatus::INVALID;
        }
        if (policy != nullptr) {
            const auto argument = policy->arguments.find(definition.name);
            if (argument != policy->arguments.end() &&
                !validate_range(definition, value, argument->second, error)) {
                ti.logger->error(error);
                return TestStatus::INVALID;
            }
        }
    }
    ti.args = std::move(resolved_args);

    try {
        return test->second.execute(ti);
    } catch (const std::invalid_argument& e) {
        ti.logger->error(std::string("invalid test argument: ") + e.what());
        return TestStatus::INVALID;
    } catch (const std::out_of_range& e) {
        ti.logger->error(std::string("test argument is out of range: ") + e.what());
        return TestStatus::INVALID;
    } catch (const std::exception& e) {
        ti.logger->error(std::string("testcase exception: ") + e.what());
        return TestStatus::ERROR;
    }
}

std::vector<std::string> TestTarget::get_registered_test_names() const
{
    std::vector<std::string> names;
    for (const auto& item : registered_tests_) names.push_back(item.first);
    return names;
}

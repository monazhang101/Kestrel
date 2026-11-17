#include "diag/core/TestTarget.h"

extern "C" {
#include <phal/phal.h>
}

#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

const char* to_string(TargetType type)
{
    switch (type) {
    case TargetType::Tpu: return "tpu";
    case TargetType::Ddp: return "ddp";
    case TargetType::Isi: return "isi";
    case TargetType::Pcie: return "pcie";
    case TargetType::Pmu: return "pmu";
    case TargetType::Soc: return "soc";
    }
    return "unknown";
}

TargetType target_type_from_string(const std::string& value)
{
    if (value == "tpu") return TargetType::Tpu;
    if (value == "ddp") return TargetType::Ddp;
    if (value == "isi") return TargetType::Isi;
    if (value == "pcie") return TargetType::Pcie;
    if (value == "pmu") return TargetType::Pmu;
    if (value == "soc") return TargetType::Soc;
    throw std::invalid_argument("unknown target type: " + value);
}

TestTarget::TestTarget(const std::string& name,
                       TargetType target_type,
                       const DeviceContext& ctx)
    : name_(name), target_type_(target_type), ctx_(ctx)
{
    ctx_.target_name = name;
}

void TestTarget::_add_test(const std::string& test_name,
                           std::initializer_list<TestArgumentDefinition> arguments,
                           TestcaseFunc func,
                           CtxResources ctx_resources)
{
    registered_tests_[std::string(to_string(target_type_)) + "_" + test_name] = {
        std::vector<TestArgumentDefinition>(arguments),
        std::move(func),
        std::move(ctx_resources)};
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
    return format == "u64" || format == "u32" || format == "bytes" ||
           format == "ms" || format == "us" || format == "offset" || format == "bool";
}

bool TestTarget::validate_format(const TestArgumentDefinition& definition,
                                 const std::string& value,
                                 std::string& error)
{
    if (definition.format == "string") return true;
    if (is_numeric_format(definition.format)) {
        uint64_t parsed = 0;
        if (parse_u64(value, parsed) &&
            ((definition.format != "u32" && definition.format != "us") ||
             parsed <= std::numeric_limits<uint32_t>::max()) &&
            (definition.format != "bool" || parsed <= 1)) return true;
        error = "argument " + definition.name + " must use format " +
                definition.format + ": " + value;
        return false;
    }
    error = "argument " + definition.name + " has unsupported format: " +
            definition.format;
    return false;
}

TestStatus TestTarget::run_testcase_with(const std::string& test_name,
                                         const TestArgs& args, Logger* logger,
                                         const Executor& execute)
{
    std::lock_guard<std::mutex> lock(*ctx_.testcase_mutex);
    Logger default_logger;
    TestInfo ti;
    ti.target_name = owner_name_.empty() ? name_ : owner_name_;
    ti.test_name = test_name;
    ti.logger = logger == nullptr ? &default_logger : logger;

    const auto test = registered_tests_.find(test_name);
    if (test == registered_tests_.end()) {
        ti.logger->error("testcase is not registered: target=" + name_ +
                         " test=" + test_name);
        return PHAL_STATUS_UNIMPLEMENTED;
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
            return PHAL_STATUS_INVALID;
        }
        resolved_args[argument.first] = argument.second;
    }
    for (const auto& definition : test->second.arguments) {
        const auto& value = resolved_args.at(definition.name);
        std::string error;
        if (!validate_format(definition, value, error)) {
            ti.logger->error(error);
            return PHAL_STATUS_INVALID;
        }
    }
    ti.args = std::move(resolved_args);

    try {
        return execute(ti, test->second);
    } catch (const std::invalid_argument& e) {
        ti.logger->error(std::string("invalid test argument: ") + e.what());
        return PHAL_STATUS_INVALID;
    } catch (const std::out_of_range& e) {
        ti.logger->error(std::string("test argument is out of range: ") + e.what());
        return PHAL_STATUS_INVALID;
    } catch (const std::exception& e) {
        ti.logger->error(std::string("testcase exception: ") + e.what());
        return PHAL_STATUS_ERROR;
    }
}

std::vector<std::string> TestTarget::get_registered_test_names() const
{
    std::vector<std::string> names;
    for (const auto& item : registered_tests_) names.push_back(item.first);
    return names;
}

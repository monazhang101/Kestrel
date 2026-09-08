#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <phal/components/pcie/pcie.h>
}

namespace {

constexpr uint8_t IDENTITY = 0;
constexpr uint64_t DMEM_BASE = 0x10000000;
constexpr uint64_t APERTURE_SIZE = 0x100000;
constexpr uint64_t BAR23_FIRST_CONFIGURABLE_OFFSET = 0x10000000;
constexpr size_t VERIFY_BYTES = 1024;

struct Edge {
    const char* name;
    uint64_t offset;
};

constexpr std::array<Edge, 2> EDGES = {{
    {"start", 0},
    {"end", APERTURE_SIZE - VERIFY_BYTES},
}};

struct ApertureCase {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint64_t bar_offset = 0;
    uint64_t target_addr = 0;
    uint8_t pattern_byte = 0;
    std::array<std::vector<uint8_t>, EDGES.size()> originals;
    std::array<bool, EDGES.size()> written = {};
};

std::vector<ApertureCase> make_aperture_cases()
{
    std::vector<ApertureCase> cases;
    cases.reserve(15);

    for (uint8_t aperture = 1; aperture <= 7; ++aperture) {
        const auto global_index = static_cast<uint64_t>(cases.size());
        cases.push_back({
            2,
            aperture,
            BAR23_FIRST_CONFIGURABLE_OFFSET +
                static_cast<uint64_t>(aperture - 1) * APERTURE_SIZE,
            DMEM_BASE + global_index * APERTURE_SIZE,
            static_cast<uint8_t>(0x20 | aperture),
        });
    }

    for (uint8_t aperture = 0; aperture <= 7; ++aperture) {
        const auto global_index = static_cast<uint64_t>(cases.size());
        cases.push_back({
            4,
            aperture,
            static_cast<uint64_t>(aperture) * APERTURE_SIZE,
            DMEM_BASE + global_index * APERTURE_SIZE,
            static_cast<uint8_t>(0x40 | aperture),
        });
    }

    return cases;
}

class SequentialApertureMappingRun {
public:
    SequentialApertureMappingRun(TestInfo& ti,
                                 const DeviceContext& ctx,
                                 uint32_t control_bar_index,
                                 uint64_t pcie_base)
        : ti_(ti),
          ctx_(ctx),
          control_bar_index_(control_bar_index),
          pcie_base_(pcie_base),
          cases_(make_aperture_cases())
    {
        metrics_["aperture_count"] = std::to_string(cases_.size());
        metrics_["aperture_size"] = hex_u64(APERTURE_SIZE);
        metrics_["verify_bytes_per_edge"] = std::to_string(VERIFY_BYTES);
    }

    TestResult run();

private:
    bool configure_all();
    bool save_originals_all();
    void write_patterns_all();
    void readback_compare_all();
    void restore_all();

    bool configure_aperture(const ApertureCase& aperture_case,
                            std::string& error);
    bool verify_aperture(const ApertureCase& aperture_case,
                         std::string& error);
    bool read_edge(const ApertureCase& aperture_case,
                   const Edge& edge,
                   std::vector<uint8_t>& data) const;
    bool write_edge(const ApertureCase& aperture_case,
                    const Edge& edge,
                    const std::vector<uint8_t>& data) const;
    bool compare_edge(const ApertureCase& aperture_case,
                      const Edge& edge,
                      const std::vector<uint8_t>& expected,
                      const std::vector<uint8_t>& actual,
                      std::string& failure_details) const;

    TestResult make_result(bool passed,
                           const std::string& description = {},
                           const std::string& details = {}) const;
    void record_failure(const std::string& description,
                        const std::string& details);
    void log_context() const;
    void log_phase(const char* phase, const char* state) const;
    void log_aperture(const char* phase,
                      const ApertureCase& aperture_case,
                      const std::string& details = {}) const;
    void log_edge(const char* phase,
                  const ApertureCase& aperture_case,
                  const Edge& edge,
                  const std::vector<uint8_t>& data,
                  bool ok) const;

    static std::string aperture_name(const ApertureCase& aperture_case);
    static std::string data_sample(const std::vector<uint8_t>& data);
    static std::string hex_u64(uint64_t value);
    static std::string pattern_name(uint8_t pattern_byte);
    static std::string phal_status_string(const char* api,
                                          phal_status_t status);

    TestInfo& ti_;
    const DeviceContext& ctx_;
    uint32_t control_bar_index_ = 0;
    uint64_t pcie_base_ = 0;
    phal_ctx_t* phal_ = nullptr;
    std::vector<ApertureCase> cases_;
    TestMetrics metrics_;
    std::string failure_description_;
    std::string failure_details_;
};

TestResult SequentialApertureMappingRun::run()
{
    if (ti_.hal == nullptr) {
        return make_result(false, "HAL context is not available");
    }

    std::string phal_error;
    phal_ = static_cast<phal_ctx_t*>(ti_.hal->phal().get_context(
        ctx_,
        control_bar_index_,
        pcie_base_,
        phal_project_from_tpu_type(ctx_.tpu_type),
        &phal_error));
    if (phal_ == nullptr) {
        return make_result(false, "PHAL context init failed", phal_error);
    }

    log_context();
    if (!configure_all()) {
        return make_result(false);
    }
    if (!save_originals_all()) {
        metrics_["restore_status"] = "not_needed";
        return make_result(false);
    }

    write_patterns_all();
    readback_compare_all();
    restore_all();
    return make_result(failure_description_.empty());
}

// Add future MultiApertureAccessRun and ApertureStressRun classes beside this
// runner. They may share the case/edge model, but should own their sequencing.

}

// Configure every aperture, save all original edges, write all patterns,
// read and compare all patterns, then restore every modified edge.
TestResult PCIeModule::sequential_aperture_mapping(TestInfo& ti)
{
    return SequentialApertureMappingRun{
        ti,
        ctx_,
        bar_index_,
        reg_base_offset_,
    }.run();
}

namespace {

bool SequentialApertureMappingRun::configure_all()
{
    log_phase("configure_all", "begin");
    size_t configured = 0;

    for (const auto& aperture_case : cases_) {
        std::string error;
        if (!configure_aperture(aperture_case, error)) {
            metrics_["configured"] = std::to_string(configured);
            record_failure("aperture configure failed", error);
            log_phase("configure_all", "failed");
            return false;
        }
        ++configured;
    }

    metrics_["configured"] = std::to_string(configured);
    log_phase("configure_all", "complete");
    return true;
}

bool SequentialApertureMappingRun::save_originals_all()
{
    log_phase("save_originals_all", "begin");
    size_t originals_saved = 0;

    for (auto& aperture_case : cases_) {
        bool aperture_saved = true;
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            auto& original = aperture_case.originals[edge_index];
            const auto ok = read_edge(aperture_case, edge, original);
            log_edge("read_original", aperture_case, edge, original, ok);
            aperture_saved = ok && aperture_saved;
        }

        if (aperture_saved) {
            ++originals_saved;
        } else {
            record_failure("aperture original read failed",
                           aperture_name(aperture_case));
        }
    }

    metrics_["originals_saved"] = std::to_string(originals_saved);
    const auto all_saved = originals_saved == cases_.size();
    log_phase("save_originals_all", all_saved ? "complete" : "failed");
    return all_saved;
}

void SequentialApertureMappingRun::write_patterns_all()
{
    log_phase("write_patterns_all", "begin");
    size_t written = 0;

    for (auto& aperture_case : cases_) {
        const std::vector<uint8_t> expected(VERIFY_BYTES,
                                            aperture_case.pattern_byte);
        bool aperture_written = true;

        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            const auto ok = write_edge(aperture_case, edge, expected);
            aperture_case.written[edge_index] = ok;
            log_edge("write_pattern", aperture_case, edge, expected, ok);
            aperture_written = ok && aperture_written;
        }

        if (aperture_written) {
            ++written;
        } else {
            record_failure("aperture pattern write failed",
                           aperture_name(aperture_case));
        }
    }

    metrics_["written"] = std::to_string(written);
    log_phase("write_patterns_all",
              written == cases_.size() ? "complete" : "failed");
}

void SequentialApertureMappingRun::readback_compare_all()
{
    log_phase("readback_compare_all", "begin");
    size_t verified = 0;
    size_t aperture_verify_failures = 0;
    size_t read_failures = 0;
    size_t compare_failures = 0;

    for (const auto& aperture_case : cases_) {
        std::string aperture_error;
        if (!verify_aperture(aperture_case, aperture_error)) {
            ++aperture_verify_failures;
            record_failure("aperture configuration changed before readback",
                           aperture_error);
            continue;
        }

        std::array<std::vector<uint8_t>, EDGES.size()> actual;
        bool read_ok = true;
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            const auto ok = read_edge(aperture_case, edge, actual[edge_index]);
            log_edge("readback", aperture_case, edge, actual[edge_index], ok);
            read_ok = ok && read_ok;
        }

        if (!read_ok) {
            ++read_failures;
            record_failure("aperture pattern read failed",
                           aperture_name(aperture_case));
            continue;
        }

        const std::vector<uint8_t> expected(VERIFY_BYTES,
                                            aperture_case.pattern_byte);
        bool aperture_matches = true;
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            std::string mismatch;
            const auto matches = compare_edge(aperture_case,
                                              EDGES[edge_index],
                                              expected,
                                              actual[edge_index],
                                              mismatch);
            if (!matches) {
                ++compare_failures;
                record_failure("aperture pattern compare failed", mismatch);
            }
            aperture_matches = matches && aperture_matches;
        }

        if (aperture_matches) {
            ++verified;
        }
    }

    metrics_["verified"] = std::to_string(verified);
    metrics_["aperture_verify_failures"] =
        std::to_string(aperture_verify_failures);
    metrics_["read_failures"] = std::to_string(read_failures);
    metrics_["compare_failures"] = std::to_string(compare_failures);
    log_phase("readback_compare_all",
              verified == cases_.size() ? "complete" : "failed");
}

void SequentialApertureMappingRun::restore_all()
{
    log_phase("restore_all", "begin");
    bool restore_ok = true;

    for (const auto& aperture_case : cases_) {
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            if (!aperture_case.written[edge_index]) {
                continue;
            }

            const auto& edge = EDGES[edge_index];
            const auto& original = aperture_case.originals[edge_index];
            const auto restored = write_edge(aperture_case, edge, original);
            log_edge("restore", aperture_case, edge, original, restored);
            restore_ok = restored && restore_ok;
            if (!restored) {
                record_failure("aperture restore failed",
                               aperture_name(aperture_case) +
                                   " edge=" + edge.name);
            }
        }
    }

    metrics_["restore_status"] = restore_ok ? "ok" : "failed";
    log_phase("restore_all", restore_ok ? "complete" : "failed");
}

bool SequentialApertureMappingRun::configure_aperture(
    const ApertureCase& aperture_case,
    std::string& error)
{
    phal_pcie_aperture_t aperture = {};
    aperture.identity = IDENTITY;
    aperture.target_addr = aperture_case.target_addr;
    aperture.size = APERTURE_SIZE;

    log_aperture("aperture_set",
                 aperture_case,
                 "identity=" + std::to_string(IDENTITY) +
                     " size=" + hex_u64(APERTURE_SIZE));

    const auto status = phal_pcie_aperture_set(phal_,
                                               aperture_case.bar_index,
                                               aperture_case.aperture_index,
                                               &aperture);
    log_aperture("aperture_set_result",
                 aperture_case,
                 "status=" + std::to_string(static_cast<int>(status)));
    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) + ": " +
                phal_status_string("phal_pcie_aperture_set", status);
        return false;
    }

    return verify_aperture(aperture_case, error);
}

bool SequentialApertureMappingRun::verify_aperture(
    const ApertureCase& aperture_case,
    std::string& error)
{
    phal_pcie_aperture_t actual = {};
    const auto status = phal_pcie_aperture_get(phal_,
                                               aperture_case.bar_index,
                                               aperture_case.aperture_index,
                                               &actual);

    std::ostringstream details;
    details << "status=" << static_cast<int>(status)
            << " actual_identity=" << static_cast<uint32_t>(actual.identity)
            << " actual_target=" << hex_u64(actual.target_addr)
            << " actual_size=" << hex_u64(actual.size);
    log_aperture("aperture_get", aperture_case, details.str());

    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) + ": " +
                phal_status_string("phal_pcie_aperture_get", status);
        return false;
    }
    if (actual.identity == IDENTITY &&
        actual.target_addr == aperture_case.target_addr &&
        actual.size == APERTURE_SIZE) {
        return true;
    }

    std::ostringstream mismatch;
    mismatch << aperture_name(aperture_case)
             << ": aperture get mismatch expected(identity="
             << static_cast<uint32_t>(IDENTITY)
             << ", target_addr=" << hex_u64(aperture_case.target_addr)
             << ", size=" << hex_u64(APERTURE_SIZE)
             << ") actual(identity=" << static_cast<uint32_t>(actual.identity)
             << ", target_addr=" << hex_u64(actual.target_addr)
             << ", size=" << hex_u64(actual.size) << ")";
    error = mismatch.str();
    return false;
}

bool SequentialApertureMappingRun::read_edge(
    const ApertureCase& aperture_case,
    const Edge& edge,
    std::vector<uint8_t>& data) const
{
    data.assign(VERIFY_BYTES, 0);
    return common::bar::read(ctx_,
                             aperture_case.bar_index,
                             aperture_case.bar_offset + edge.offset,
                             data.data(),
                             data.size());
}

bool SequentialApertureMappingRun::write_edge(
    const ApertureCase& aperture_case,
    const Edge& edge,
    const std::vector<uint8_t>& data) const
{
    return common::bar::write(ctx_,
                              aperture_case.bar_index,
                              aperture_case.bar_offset + edge.offset,
                              data.data(),
                              data.size());
}

bool SequentialApertureMappingRun::compare_edge(
    const ApertureCase& aperture_case,
    const Edge& edge,
    const std::vector<uint8_t>& expected,
    const std::vector<uint8_t>& actual,
    std::string& failure_details) const
{
    const auto mismatch = std::mismatch(expected.begin(),
                                        expected.end(),
                                        actual.begin());
    if (mismatch.first == expected.end()) {
        log_edge("compared", aperture_case, edge, actual, true);
        return true;
    }

    const auto mismatch_offset =
        static_cast<size_t>(mismatch.first - expected.begin());
    std::ostringstream details;
    details << aperture_name(aperture_case)
            << " edge=" << edge.name
            << " absolute_bar_offset="
            << hex_u64(aperture_case.bar_offset + edge.offset)
            << " mismatch_offset=" << mismatch_offset
            << " expected=0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<uint32_t>(*mismatch.first)
            << " actual=0x" << std::setw(2)
            << static_cast<uint32_t>(*mismatch.second)
            << " expected_sample=" << data_sample(expected)
            << " actual_sample=" << data_sample(actual);
    failure_details = details.str();
    log_aperture("compare_failed", aperture_case, failure_details);
    return false;
}

TestResult SequentialApertureMappingRun::make_result(
    bool passed,
    const std::string& description,
    const std::string& details) const
{
    return {
        ti_.test_name,
        ti_.target_name,
        passed,
        metrics_,
        description.empty() ? failure_description_ : description,
        details.empty() ? failure_details_ : details,
    };
}

void SequentialApertureMappingRun::record_failure(
    const std::string& description,
    const std::string& details)
{
    if (failure_description_.empty()) {
        failure_description_ = description;
    }
    if (!failure_details_.empty()) {
        failure_details_ += "; ";
    }
    failure_details_ += details;
}

void SequentialApertureMappingRun::log_context() const
{
    if (ti_.logger == nullptr) {
        return;
    }

    std::ostringstream stream;
    stream << "sequential_aperture_mapping context"
           << " control_bar=" << control_bar_index_
           << " pcie_base=" << hex_u64(pcie_base_)
           << " phal_base=" << hex_u64(pcie_base_)
           << " aperture_count=" << cases_.size();
    ti_.logger->debug(stream.str());
}

void SequentialApertureMappingRun::log_phase(const char* phase,
                                              const char* state) const
{
    if (ti_.logger != nullptr) {
        ti_.logger->info(std::string("sequential_aperture_mapping phase=") +
                         phase + " state=" + state);
    }
}

void SequentialApertureMappingRun::log_aperture(
    const char* phase,
    const ApertureCase& aperture_case,
    const std::string& details) const
{
    if (ti_.logger == nullptr) {
        return;
    }

    std::ostringstream stream;
    stream << "sequential_aperture_mapping " << phase
           << " aperture bar_id=" << aperture_case.bar_index
           << " apt_id="
           << static_cast<uint32_t>(aperture_case.aperture_index)
           << " target_addr=" << hex_u64(aperture_case.target_addr)
           << " bar_offset=" << hex_u64(aperture_case.bar_offset)
           << " pattern=" << pattern_name(aperture_case.pattern_byte);
    if (!details.empty()) {
        stream << " " << details;
    }
    ti_.logger->debug(stream.str());
}

void SequentialApertureMappingRun::log_edge(
    const char* phase,
    const ApertureCase& aperture_case,
    const Edge& edge,
    const std::vector<uint8_t>& data,
    bool ok) const
{
    std::ostringstream details;
    details << "edge=" << edge.name
            << " edge_offset=" << hex_u64(edge.offset)
            << " absolute_bar_offset="
            << hex_u64(aperture_case.bar_offset + edge.offset)
            << " len=" << data.size()
            << " sample=" << data_sample(data)
            << " status=" << (ok ? "ok" : "failed");
    log_aperture(phase, aperture_case, details.str());
}

std::string SequentialApertureMappingRun::aperture_name(
    const ApertureCase& aperture_case)
{
    std::ostringstream stream;
    stream << "bar" << aperture_case.bar_index
           << (aperture_case.bar_index + 1)
           << "_aperture"
           << static_cast<uint32_t>(aperture_case.aperture_index);
    return stream.str();
}

std::string SequentialApertureMappingRun::data_sample(
    const std::vector<uint8_t>& data)
{
    std::ostringstream stream;
    stream << "0x";
    const auto count = std::min<size_t>(data.size(), 8);
    for (size_t i = 0; i < count; ++i) {
        stream << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<uint32_t>(data[i]);
    }
    return stream.str();
}

std::string SequentialApertureMappingRun::hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string SequentialApertureMappingRun::pattern_name(uint8_t pattern_byte)
{
    std::ostringstream stream;
    stream << "byte=0x" << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<uint32_t>(pattern_byte);
    return stream.str();
}

std::string SequentialApertureMappingRun::phal_status_string(
    const char* api,
    phal_status_t status)
{
    std::ostringstream stream;
    stream << api << " status=" << static_cast<int>(status);
    return stream.str();
}

}

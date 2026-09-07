#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

#include <algorithm>
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
constexpr uint64_t DMEM_BASE = 0x10000000; //DF space 256M can't be configured thru aperture
constexpr uint64_t APERTURE_SIZE = 0x100000;
constexpr uint64_t BAR23_FIRST_CONFIGURABLE_OFFSET = 0x10000000; //First 256M can't be configured thru aperture
constexpr size_t VERIFY_BYTES = 1024; //Verify first and last 1kb

struct ApertureCase {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint32_t global_index = 0;
    uint64_t bar_offset = 0;
    uint64_t target_addr = 0;
    uint8_t pattern_byte = 0;
    std::vector<uint8_t> original_start;
    std::vector<uint8_t> original_end;
    bool start_written = false;
    bool end_written = false;
};

std::string aperture_name(const ApertureCase& aperture_case);

std::string hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::vector<ApertureCase> make_aperture_cases()
{
    std::vector<ApertureCase> cases;
    cases.reserve(15);

    uint32_t global_index = 0;
    for (uint8_t aperture = 1; aperture <= 7; ++aperture) {
        cases.push_back({
            2,
            aperture,
            global_index,
            BAR23_FIRST_CONFIGURABLE_OFFSET +
                static_cast<uint64_t>(aperture - 1) * APERTURE_SIZE,
            DMEM_BASE + static_cast<uint64_t>(global_index) * APERTURE_SIZE,
            static_cast<uint8_t>(0x20 | aperture),
            {},
            {},
            false
        });
        ++global_index;
    }

    for (uint8_t aperture = 0; aperture <= 7; ++aperture) {
        cases.push_back({
            4,
            aperture,
            global_index,
            static_cast<uint64_t>(aperture) * APERTURE_SIZE,
            DMEM_BASE + static_cast<uint64_t>(global_index) * APERTURE_SIZE,
            static_cast<uint8_t>(0x40 | aperture),
            {},
            {},
            false
        });
        ++global_index;
    }

    return cases;
}

std::string pattern_name(uint8_t pattern_byte)
{
    std::ostringstream stream;
    stream << "byte=0x" << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<uint32_t>(pattern_byte);
    return stream.str();
}

std::string phal_status_string(const char* api, phal_status_t status)
{
    std::ostringstream stream;
    stream << api << " status=" << static_cast<int>(status);
    return stream.str();
}

void log_aperture_step(Logger* logger,
                       const char* phase,
                       const ApertureCase& aperture_case,
                       const std::string& details = {})
{
    if (logger == nullptr) {
        return;
    }

    std::ostringstream stream;
    stream << "sequential_aperture_mapping " << phase
           << " aperture bar_id=" << aperture_case.bar_index
           << " apt_id=" << static_cast<uint32_t>(aperture_case.aperture_index)
           << " target_addr=" << hex_u64(aperture_case.target_addr)
           << " bar_offset=" << hex_u64(aperture_case.bar_offset)
           << " pattern=" << pattern_name(aperture_case.pattern_byte);
    if (!details.empty()) {
        stream << " " << details;
    }
    logger->debug(stream.str());
}

void log_phase(Logger* logger, const char* phase, const char* state)
{
    if (logger != nullptr) {
        logger->info(std::string("sequential_aperture_mapping phase=") + phase +
                     " state=" + state);
    }
}

uint64_t absolute_bar_offset(const ApertureCase& aperture_case,
                             uint64_t edge_offset)
{
    return aperture_case.bar_offset + edge_offset;
}

std::string data_sample(const std::vector<uint8_t>& data)
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

void append_failure(std::string& failure_description,
                    std::string& failure_details,
                    const std::string& description,
                    const std::string& details)
{
    if (failure_description.empty()) {
        failure_description = description;
    }
    if (!failure_details.empty()) {
        failure_details += "; ";
    }
    failure_details += details;
}

bool verify_aperture(phal_ctx_t* phal,
                     const ApertureCase& aperture_case,
                     Logger* logger,
                     std::string& error)
{
    phal_pcie_aperture_t actual = {};
    const auto status = phal_pcie_aperture_get(phal,
                                               aperture_case.bar_index,
                                               aperture_case.aperture_index,
                                               &actual);

    std::ostringstream details;
    details << "status=" << static_cast<int>(status)
            << " actual_identity=" << static_cast<uint32_t>(actual.identity)
            << " actual_target=" << hex_u64(actual.target_addr)
            << " actual_size=" << hex_u64(actual.size);
    log_aperture_step(logger, "aperture_get", aperture_case, details.str());

    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) + ": " +
                phal_status_string("phal_pcie_aperture_get", status);
        return false;
    }
    if (actual.identity != IDENTITY ||
        actual.target_addr != aperture_case.target_addr ||
        actual.size != APERTURE_SIZE) {
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
    return true;
}

bool configure_aperture(phal_ctx_t* phal,
                        const ApertureCase& aperture_case,
                        Logger* logger,
                        std::string& error)
{
    phal_pcie_aperture_t apt = {};
    apt.identity = IDENTITY;
    apt.target_addr = aperture_case.target_addr;
    apt.size = APERTURE_SIZE;

    log_aperture_step(logger,
                      "aperture_set",
                      aperture_case,
                      "identity=" + std::to_string(IDENTITY) +
                          " size=" + hex_u64(APERTURE_SIZE));

    auto status = phal_pcie_aperture_set(phal,
                                         aperture_case.bar_index,
                                         aperture_case.aperture_index,
                                         &apt);
    log_aperture_step(logger,
                      "aperture_set_result",
                      aperture_case,
                      "status=" + std::to_string(static_cast<int>(status)));
    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) + ": " +
                phal_status_string("phal_pcie_aperture_set", status);
        return false;
    }

    return verify_aperture(phal, aperture_case, logger, error);
}

bool read_window_edge(const DeviceContext& ctx,
                      const ApertureCase& aperture_case,
                      uint64_t edge_offset,
                      std::vector<uint8_t>& data)
{
    data.assign(VERIFY_BYTES, 0);
    return common::bar::read(ctx,
                             aperture_case.bar_index,
                             aperture_case.bar_offset + edge_offset,
                             data.data(),
                             data.size());
}

bool write_window_edge(const DeviceContext& ctx,
                       const ApertureCase& aperture_case,
                       uint64_t edge_offset,
                       const std::vector<uint8_t>& data)
{
    return common::bar::write(ctx,
                              aperture_case.bar_index,
                              aperture_case.bar_offset + edge_offset,
                              data.data(),
                              data.size());
}

void log_edge_io(Logger* logger,
                 const char* phase,
                 const ApertureCase& aperture_case,
                 const char* edge,
                 uint64_t edge_offset,
                 const std::vector<uint8_t>& data,
                 bool ok)
{
    std::ostringstream details;
    details << "edge=" << edge
            << " edge_offset=" << hex_u64(edge_offset)
            << " absolute_bar_offset="
            << hex_u64(absolute_bar_offset(aperture_case, edge_offset))
            << " len=" << data.size()
            << " sample=" << data_sample(data)
            << " status=" << (ok ? "ok" : "failed");
    log_aperture_step(logger, phase, aperture_case, details.str());
}

bool compare_edge(Logger* logger,
                  const ApertureCase& aperture_case,
                  const char* edge,
                  uint64_t edge_offset,
                  const std::vector<uint8_t>& expected,
                  const std::vector<uint8_t>& actual,
                  std::string& failure_details)
{
    const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
    if (mismatch.first == expected.end()) {
        log_edge_io(logger,
                    "compared",
                    aperture_case,
                    edge,
                    edge_offset,
                    actual,
                    true);
        return true;
    }

    const auto mismatch_offset = static_cast<size_t>(mismatch.first - expected.begin());
    std::ostringstream details;
    details << aperture_name(aperture_case)
            << " edge=" << edge
            << " absolute_bar_offset="
            << hex_u64(absolute_bar_offset(aperture_case, edge_offset))
            << " mismatch_offset=" << mismatch_offset
            << " expected=0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<uint32_t>(*mismatch.first)
            << " actual=0x" << std::setw(2)
            << static_cast<uint32_t>(*mismatch.second)
            << " expected_sample=" << data_sample(expected)
            << " actual_sample=" << data_sample(actual);
    failure_details = details.str();
    log_aperture_step(logger, "compare_failed", aperture_case, failure_details);
    return false;
}

std::string aperture_name(const ApertureCase& aperture_case)
{
    std::ostringstream stream;
    stream << "bar" << aperture_case.bar_index << (aperture_case.bar_index + 1)
           << "_aperture" << static_cast<uint32_t>(aperture_case.aperture_index);
    return stream.str();
}

}

// pcie_bar_read32 : To read one 32-bit word from this PCIe module BAR window.
// @input: args["offset"] offset relative to the PCIe module register window.
// @output: TestResult metrics include offset, absolute_bar_offset, and value.
TestResult PCIeModule::pcie_bar_read32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_read32(ti);
}

// pcie_bar_scan32 : To scan a small range of 32-bit words from this PCIe module BAR window.
// @input: args["offset"] start offset, args["words"] number of 32-bit words.
// @output: TestResult metrics include offset, words, and word_N values.
TestResult PCIeModule::pcie_bar_scan32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_scan32(ti);
}

// sequential_aperture_mapping : Configure all configurable BAR23/BAR45 apertures,
// save every original edge, write every pattern, read/compare every pattern,
// then restore every modified edge.
TestResult PCIeModule::sequential_aperture_mapping(TestInfo& ti)
{
    TestMetrics metrics = {
        {"aperture_count", "15"},
        {"aperture_size", hex_u64(APERTURE_SIZE)},
        {"verify_bytes_per_edge", std::to_string(VERIFY_BYTES)},
    };

    auto make_result = [&](bool passed,
                           const std::string& error_description = {},
                           const std::string& error_details = {}) {
        return TestResult{
            ti.test_name,
            ti.target_name,
            passed,
            metrics,
            error_description,
            error_details
        };
    };

    if (ti.hal == nullptr) {
        return make_result(false, "HAL context is not available");
    }

    auto cases = make_aperture_cases();
    std::string phal_error;
    auto* phal = static_cast<phal_ctx_t*>(ti.hal->phal().get_context(
        ctx_,
        bar_index_,
        reg_base_offset_,
        phal_project_from_tpu_type(ctx_.tpu_type),
        &phal_error));
    if (phal == nullptr) {
        return make_result(false, "PHAL context init failed", phal_error);
    }
    if (ti.logger != nullptr) {
        std::ostringstream context_log;
        context_log << "sequential_aperture_mapping context"
                    << " control_bar=" << bar_index_
                    << " pcie_base=" << hex_u64(reg_base_offset_)
                    << " phal_base="
                    << hex_u64(reg_base_offset_)
                    << " aperture_count=" << cases.size();
        ti.logger->debug(context_log.str());
    }

    log_phase(ti.logger, "configure_all", "begin");
    size_t configured_count = 0;
    for (const auto& aperture_case : cases) {
        std::string configure_error;
        if (!configure_aperture(phal, aperture_case, ti.logger, configure_error)) {
            metrics["configured"] = std::to_string(configured_count);
            log_phase(ti.logger, "configure_all", "failed");
            return make_result(false, "aperture configure failed", configure_error);
        }
        ++configured_count;
    }
    metrics["configured"] = std::to_string(configured_count);
    log_phase(ti.logger, "configure_all", "complete");

    std::string failure_description;
    std::string failure_details;

    log_phase(ti.logger, "save_originals_all", "begin");
    size_t originals_saved = 0;
    for (auto& aperture_case : cases) {
        const auto start_ok = read_window_edge(ctx_,
                                               aperture_case,
                                               0,
                                               aperture_case.original_start);
        log_edge_io(ti.logger,
                    "read_original",
                    aperture_case,
                    "start",
                    0,
                    aperture_case.original_start,
                    start_ok);

        const auto end_offset = APERTURE_SIZE - VERIFY_BYTES;
        const auto end_ok = read_window_edge(ctx_,
                                             aperture_case,
                                             end_offset,
                                             aperture_case.original_end);
        log_edge_io(ti.logger,
                    "read_original",
                    aperture_case,
                    "end",
                    end_offset,
                    aperture_case.original_end,
                    end_ok);

        if (start_ok && end_ok) {
            ++originals_saved;
        } else {
            append_failure(failure_description,
                           failure_details,
                           "aperture original read failed",
                           aperture_name(aperture_case));
        }
    }
    metrics["originals_saved"] = std::to_string(originals_saved);
    log_phase(ti.logger,
              "save_originals_all",
              originals_saved == cases.size() ? "complete" : "failed");

    // Do not modify any device memory unless every original edge was saved.
    if (originals_saved != cases.size()) {
        metrics["restore_status"] = "not_needed";
        return make_result(false, failure_description, failure_details);
    }

    log_phase(ti.logger, "write_patterns_all", "begin");
    size_t written_count = 0;
    for (auto& aperture_case : cases) {
        const std::vector<uint8_t> expected(VERIFY_BYTES, aperture_case.pattern_byte);

        aperture_case.start_written = write_window_edge(ctx_,
                                                        aperture_case,
                                                        0,
                                                        expected);
        log_edge_io(ti.logger,
                    "write_pattern",
                    aperture_case,
                    "start",
                    0,
                    expected,
                    aperture_case.start_written);

        const auto end_offset = APERTURE_SIZE - VERIFY_BYTES;
        aperture_case.end_written = write_window_edge(ctx_,
                                                      aperture_case,
                                                      end_offset,
                                                      expected);
        log_edge_io(ti.logger,
                    "write_pattern",
                    aperture_case,
                    "end",
                    end_offset,
                    expected,
                    aperture_case.end_written);

        if (aperture_case.start_written && aperture_case.end_written) {
            ++written_count;
        } else {
            append_failure(failure_description,
                           failure_details,
                           "aperture pattern write failed",
                           aperture_name(aperture_case));
        }
    }
    metrics["written"] = std::to_string(written_count);
    log_phase(ti.logger,
              "write_patterns_all",
              written_count == cases.size() ? "complete" : "failed");

    log_phase(ti.logger, "readback_compare_all", "begin");
    size_t verified_count = 0;
    size_t aperture_verify_failures = 0;
    size_t read_failures = 0;
    size_t compare_failures = 0;
    for (const auto& aperture_case : cases) {
        std::string aperture_error;
        if (!verify_aperture(phal, aperture_case, ti.logger, aperture_error)) {
            ++aperture_verify_failures;
            append_failure(failure_description,
                           failure_details,
                           "aperture configuration changed before readback",
                           aperture_error);
            continue;
        }

        const std::vector<uint8_t> expected(VERIFY_BYTES, aperture_case.pattern_byte);
        std::vector<uint8_t> actual_start;
        std::vector<uint8_t> actual_end;

        const auto start_ok = read_window_edge(ctx_, aperture_case, 0, actual_start);
        log_edge_io(ti.logger,
                    "readback",
                    aperture_case,
                    "start",
                    0,
                    actual_start,
                    start_ok);

        const auto end_offset = APERTURE_SIZE - VERIFY_BYTES;
        const auto end_ok = read_window_edge(ctx_,
                                             aperture_case,
                                             end_offset,
                                             actual_end);
        log_edge_io(ti.logger,
                    "readback",
                    aperture_case,
                    "end",
                    end_offset,
                    actual_end,
                    end_ok);

        if (!start_ok || !end_ok) {
            ++read_failures;
            append_failure(failure_description,
                           failure_details,
                           "aperture pattern read failed",
                           aperture_name(aperture_case));
            continue;
        }

        std::string start_mismatch;
        const auto start_matches = compare_edge(ti.logger,
                                                aperture_case,
                                                "start",
                                                0,
                                                expected,
                                                actual_start,
                                                start_mismatch);
        if (!start_matches) {
            ++compare_failures;
            append_failure(failure_description,
                           failure_details,
                           "aperture pattern compare failed",
                           start_mismatch);
        }

        std::string end_mismatch;
        const auto end_matches = compare_edge(ti.logger,
                                              aperture_case,
                                              "end",
                                              end_offset,
                                              expected,
                                              actual_end,
                                              end_mismatch);
        if (!end_matches) {
            ++compare_failures;
            append_failure(failure_description,
                           failure_details,
                           "aperture pattern compare failed",
                           end_mismatch);
        }

        if (start_matches && end_matches) {
            ++verified_count;
        }
    }
    metrics["verified"] = std::to_string(verified_count);
    metrics["aperture_verify_failures"] = std::to_string(aperture_verify_failures);
    metrics["read_failures"] = std::to_string(read_failures);
    metrics["compare_failures"] = std::to_string(compare_failures);
    log_phase(ti.logger,
              "readback_compare_all",
              verified_count == cases.size() ? "complete" : "failed");

    log_phase(ti.logger, "restore_all", "begin");
    bool restore_ok = true;
    for (const auto& aperture_case : cases) {
        if (aperture_case.start_written) {
            const auto restored = write_window_edge(ctx_,
                                                    aperture_case,
                                                    0,
                                                    aperture_case.original_start);
            log_edge_io(ti.logger,
                        "restore",
                        aperture_case,
                        "start",
                        0,
                        aperture_case.original_start,
                        restored);
            restore_ok = restored && restore_ok;
            if (!restored) {
                append_failure(failure_description,
                               failure_details,
                               "aperture restore failed",
                               aperture_name(aperture_case) + " edge=start");
            }
        }

        if (aperture_case.end_written) {
            const auto end_offset = APERTURE_SIZE - VERIFY_BYTES;
            const auto restored = write_window_edge(ctx_,
                                                    aperture_case,
                                                    end_offset,
                                                    aperture_case.original_end);
            log_edge_io(ti.logger,
                        "restore",
                        aperture_case,
                        "end",
                        end_offset,
                        aperture_case.original_end,
                        restored);
            restore_ok = restored && restore_ok;
            if (!restored) {
                append_failure(failure_description,
                               failure_details,
                               "aperture restore failed",
                               aperture_name(aperture_case) + " edge=end");
            }
        }
    }
    metrics["restore_status"] = restore_ok ? "ok" : "failed";
    log_phase(ti.logger, "restore_all", restore_ok ? "complete" : "failed");

    if (!failure_description.empty()) {
        return make_result(false, failure_description, failure_details);
    }

    return make_result(true);
}

#include "diag/modules/PCIeModule.h"

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
            {},
            {},
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
            {},
            {},
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
    }

    TestStatus run();

private:
    bool configure_all();
    bool save_originals_all();
    bool write_patterns_all();
    bool readback_compare_all();
    bool restore_all();

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

    static std::string aperture_name(const ApertureCase& aperture_case);
    static std::string data_sample(const std::vector<uint8_t>& data);
    static std::string hex_u64(uint64_t value);

    TestInfo& ti_;
    const DeviceContext& ctx_;
    uint32_t control_bar_index_ = 0;
    uint64_t pcie_base_ = 0;
    phal_ctx_t* phal_ = nullptr;
    std::vector<ApertureCase> cases_;
};

TestStatus SequentialApertureMappingRun::run()
{
    if (ti_.hal == nullptr) {
        if (ti_.logger != nullptr) {
            ti_.logger->error("HAL context is not available");
        }
        return TestStatus::ERROR;
    }

    std::string phal_error;
    phal_ = static_cast<phal_ctx_t*>(ti_.hal->phal().get_context(
        ctx_,
        control_bar_index_,
        pcie_base_,
        phal_project_from_tpu_type(ctx_.tpu_type),
        &phal_error));
    if (phal_ == nullptr) {
        if (ti_.logger != nullptr) {
            ti_.logger->error("PHAL context init failed: " + phal_error);
        }
        return TestStatus::ERROR;
    }

    if (!configure_all()) {
        return TestStatus::ERROR;
    }
    if (!save_originals_all()) {
        return TestStatus::ERROR;
    }

    const bool write_ok = write_patterns_all();
    const bool compare_ok = write_ok && readback_compare_all();
    const bool restore_ok = restore_all();
    return write_ok && compare_ok && restore_ok
               ? TestStatus::OK
               : TestStatus::ERROR;
}

// Add future MultiApertureAccessRun and ApertureStressRun classes beside this
// runner. They may share the case/edge model, but should own their sequencing.

}

// Configure every aperture, save all original edges, write all patterns,
// read and compare all patterns, then restore every modified edge.
TestStatus PCIeModule::sequential_aperture_mapping(TestInfo& ti)
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
    for (const auto& aperture_case : cases_) {
        std::string error;
        if (!configure_aperture(aperture_case, error)) {
            if (ti_.logger != nullptr) {
                ti_.logger->error("aperture configure failed: " + error);
            }
            return false;
        }
    }

    if (ti_.logger != nullptr) {
        ti_.logger->info("configured apertures=" +
                         std::to_string(cases_.size()));
    }
    return true;
}

bool SequentialApertureMappingRun::save_originals_all()
{
    for (auto& aperture_case : cases_) {
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            auto& original = aperture_case.originals[edge_index];
            if (!read_edge(aperture_case, edge, original)) {
                if (ti_.logger != nullptr) {
                    ti_.logger->error(
                        "aperture original read failed: " +
                        aperture_name(aperture_case) +
                        " edge=" + edge.name +
                        " bar_offset=" +
                        hex_u64(aperture_case.bar_offset + edge.offset));
                }
                return false;
            }
        }
    }

    if (ti_.logger != nullptr) {
        ti_.logger->info("saved aperture originals=" +
                         std::to_string(cases_.size()));
    }
    return true;
}

bool SequentialApertureMappingRun::write_patterns_all()
{
    for (auto& aperture_case : cases_) {
        const std::vector<uint8_t> expected(VERIFY_BYTES,
                                            aperture_case.pattern_byte);

        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            const auto ok = write_edge(aperture_case, edge, expected);
            aperture_case.written[edge_index] = ok;
            if (!ok) {
                if (ti_.logger != nullptr) {
                    ti_.logger->error(
                        "aperture pattern write failed: " +
                        aperture_name(aperture_case) +
                        " edge=" + edge.name +
                        " bar_offset=" +
                        hex_u64(aperture_case.bar_offset + edge.offset));
                }
                return false;
            }
        }
    }

    if (ti_.logger != nullptr) {
        ti_.logger->info("wrote aperture patterns=" +
                         std::to_string(cases_.size()));
    }
    return true;
}

bool SequentialApertureMappingRun::readback_compare_all()
{
    for (const auto& aperture_case : cases_) {
        std::string aperture_error;
        if (!verify_aperture(aperture_case, aperture_error)) {
            if (ti_.logger != nullptr) {
                ti_.logger->error(
                    "aperture configuration changed before readback: " +
                    aperture_error);
            }
            return false;
        }

        std::array<std::vector<uint8_t>, EDGES.size()> actual;
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            const auto& edge = EDGES[edge_index];
            if (!read_edge(aperture_case, edge, actual[edge_index])) {
                if (ti_.logger != nullptr) {
                    ti_.logger->error(
                        "aperture pattern read failed: " +
                        aperture_name(aperture_case) +
                        " edge=" + edge.name +
                        " bar_offset=" +
                        hex_u64(aperture_case.bar_offset + edge.offset));
                }
                return false;
            }
        }

        const std::vector<uint8_t> expected(VERIFY_BYTES,
                                            aperture_case.pattern_byte);
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            std::string mismatch;
            if (!compare_edge(aperture_case,
                              EDGES[edge_index],
                              expected,
                              actual[edge_index],
                              mismatch)) {
                if (ti_.logger != nullptr) {
                    ti_.logger->error(
                        "aperture pattern compare failed: " + mismatch);
                }
                return false;
            }
        }
    }

    if (ti_.logger != nullptr) {
        ti_.logger->info("verified aperture patterns=" +
                         std::to_string(cases_.size()));
    }
    return true;
}

bool SequentialApertureMappingRun::restore_all()
{
    bool restore_ok = true;
    size_t restored_edges = 0;

    for (const auto& aperture_case : cases_) {
        for (size_t edge_index = 0; edge_index < EDGES.size(); ++edge_index) {
            if (!aperture_case.written[edge_index]) {
                continue;
            }

            const auto& edge = EDGES[edge_index];
            const auto& original = aperture_case.originals[edge_index];
            const auto restored = write_edge(aperture_case, edge, original);
            restore_ok = restored && restore_ok;
            if (!restored) {
                if (ti_.logger != nullptr) {
                    ti_.logger->error(
                        "aperture restore failed: " +
                        aperture_name(aperture_case) +
                        " edge=" + edge.name +
                        " bar_offset=" +
                        hex_u64(aperture_case.bar_offset + edge.offset));
                }
            } else {
                ++restored_edges;
            }
        }
    }

    if (ti_.logger != nullptr) {
        ti_.logger->info("restored aperture edges=" +
                         std::to_string(restored_edges));
    }
    return restore_ok;
}

bool SequentialApertureMappingRun::configure_aperture(
    const ApertureCase& aperture_case,
    std::string& error)
{
    phal_pcie_aperture_t aperture = {};
    aperture.identity = IDENTITY;
    aperture.target_addr = aperture_case.target_addr;
    aperture.size = APERTURE_SIZE;

    const auto status = phal_pcie_aperture_set(phal_,
                                               aperture_case.bar_index,
                                               aperture_case.aperture_index,
                                               &aperture);
    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) +
                " phal_pcie_aperture_set status=" +
                std::to_string(static_cast<int>(status));
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

    if (status != PHAL_STATUS_OK) {
        error = aperture_name(aperture_case) +
                " phal_pcie_aperture_get status=" +
                std::to_string(static_cast<int>(status));
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
    return false;
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


}

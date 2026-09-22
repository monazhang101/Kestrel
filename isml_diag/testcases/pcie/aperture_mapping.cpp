#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

#include <algorithm>
#include <array>
#include <vector>

using common::format::hex;

extern "C" {
#include <phal/components/pcie/pcie.h>
}

namespace {
constexpr uint8_t IDENTITY = 0;
constexpr uint64_t DMEM_BASE = 0x10000000;
constexpr uint64_t APERTURE_SIZE = 0x100000;
constexpr uint64_t BAR23_FIRST_CONFIGURABLE_OFFSET = 0x10000000;
constexpr size_t VERIFY_BYTES = 1024;
constexpr std::array<uint64_t, 2> EDGES = {0, APERTURE_SIZE - VERIFY_BYTES};
using Buffer = std::array<uint8_t, VERIFY_BYTES>;

struct ApertureCase {
    uint32_t bar;
    uint8_t aperture;
    uint64_t bar_offset;
    uint64_t target_addr;
    uint8_t pattern;
    std::array<Buffer, EDGES.size()> originals{};
    std::array<bool, EDGES.size()> written{};
};

std::string location(const ApertureCase& c)
{
    return "bar=" + std::to_string(c.bar) + " aperture=" + std::to_string(c.aperture) +
           " bar_offset=" + hex(c.bar_offset);
}

// Used after set and again before payload readback; never reprogram the mapping.
TestStatus verify_aperture(phal_ctx_t* phal, const ApertureCase& c, Logger& logger)
{
    phal_pcie_aperture_t actual = {};
    const auto native = phal_pcie_aperture_get(phal, c.bar, c.aperture, &actual);
    if (native != PHAL_STATUS_OK) {
        logger.error("aperture get failed: " + location(c) +
                     " status=" + std::to_string(static_cast<int>(native)));
        return native;
    }
    if (actual.identity != IDENTITY || actual.target_addr != c.target_addr ||
        actual.size != APERTURE_SIZE) {
        logger.error("aperture get mismatch: " + location(c) +
                     " expected(identity=0, target=" + hex(c.target_addr) +
                     ", size=" + hex(APERTURE_SIZE) +
                     ") actual(identity=" + std::to_string(actual.identity) +
                     ", target=" + hex(actual.target_addr) +
                     ", size=" + hex(actual.size) + ")");
        return PHAL_STATUS_ERROR;
    }
    return PHAL_STATUS_OK;
}
}

TestStatus PCIeModule::sequential_aperture_mapping(TestInfo& ti)
{
    using common::bar::read;
    using common::bar::write;
    auto& ctx = ctx_;
    auto* phal = ctx.pcie_phal;
    auto& logger = *ti.logger;
    std::vector<ApertureCase> cases;
    cases.reserve(15);

    // BAR2 apertures 1..7, then BAR4 apertures 0..7; distinct 1 MiB DMEM ranges.
    for (uint8_t id = 1; id <= 7; ++id)
        cases.push_back({2, id, BAR23_FIRST_CONFIGURABLE_OFFSET + (id - 1) * APERTURE_SIZE,
                         DMEM_BASE + cases.size() * APERTURE_SIZE, static_cast<uint8_t>(0x20 | id)});
    for (uint8_t id = 0; id <= 7; ++id)
        cases.push_back({4, id, id * APERTURE_SIZE,
                         DMEM_BASE + cases.size() * APERTURE_SIZE, static_cast<uint8_t>(0x40 | id)});

    // 1. Configure and verify all windows before accessing any payload.
    for (const auto& c : cases) {
        phal_pcie_aperture_t aperture = {};
        aperture.identity = IDENTITY;
        aperture.target_addr = c.target_addr;
        aperture.size = APERTURE_SIZE;
        const auto native = phal_pcie_aperture_set(phal, c.bar, c.aperture, &aperture);
        if (native != PHAL_STATUS_OK) {
            logger.error("aperture set failed: " + location(c) +
                         " status=" + std::to_string(static_cast<int>(native)));
            return native;
        }
        const auto status = verify_aperture(phal, c, logger);
        if (status != PHAL_STATUS_OK) return status;
    }
    logger.info("configured apertures=15");

    // 2. Save both 1 KiB edges of every window before writing any pattern.
    for (auto& c : cases) {
        for (size_t edge = 0; edge < EDGES.size(); ++edge) {
            if (!read(ctx, c.bar, c.bar_offset + EDGES[edge], c.originals[edge].data(), VERIFY_BYTES)) {
                logger.error("original read failed: " + location(c) +
                             " edge_offset=" + hex(EDGES[edge]));
                return PHAL_STATUS_ERROR;
            }
        }
    }
    logger.info("saved aperture originals=15");

    // 3. Write all patterns before readback, so cross-window interference is visible.
    auto status = PHAL_STATUS_OK;
    Buffer expected{}, actual{};
    for (auto& c : cases) {
        expected.fill(c.pattern);
        for (size_t edge = 0; edge < EDGES.size(); ++edge) {
            c.written[edge] = write(ctx, c.bar, c.bar_offset + EDGES[edge], expected.data(), VERIFY_BYTES);
            if (!c.written[edge]) {
                logger.error("pattern write failed: " + location(c) +
                             " edge_offset=" + hex(EDGES[edge]));
                status |= PHAL_STATUS_ERROR;
                break;
            }
        }
        if (status != PHAL_STATUS_OK) break;
    }
    if (status == PHAL_STATUS_OK) logger.info("wrote aperture patterns=15");

    // 4. Check mappings and data without setting any aperture again.
    for (const auto& c : cases) {
        if (status != PHAL_STATUS_OK) break;
        status |= verify_aperture(phal, c, logger);
        if (status != PHAL_STATUS_OK) break;
        expected.fill(c.pattern);
        for (const auto edge : EDGES) {
            if (!read(ctx, c.bar, c.bar_offset + edge, actual.data(), VERIFY_BYTES)) {
                logger.error("pattern read failed: " + location(c) +
                             " edge_offset=" + hex(edge));
                status |= PHAL_STATUS_ERROR;
                break;
            }
            const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
            if (mismatch.first != expected.end()) {
                const auto offset = static_cast<size_t>(mismatch.first - expected.begin());
                logger.error("pattern mismatch: " + location(c) +
                             " absolute_bar_offset=" + hex(c.bar_offset + edge + offset) +
                             " expected=" + hex(*mismatch.first, 2) +
                             " actual=" + hex(*mismatch.second, 2));
                status |= PHAL_STATUS_ERROR;
                break;
            }
        }
    }
    if (status == PHAL_STATUS_OK) logger.info("verified aperture patterns=15");

    // 5. Restore every written edge, including after write/read/compare failure.
    size_t restored = 0;
    for (const auto& c : cases) {
        for (size_t edge = 0; edge < EDGES.size(); ++edge) {
            if (!c.written[edge]) continue;
            if (!write(ctx, c.bar, c.bar_offset + EDGES[edge], c.originals[edge].data(), VERIFY_BYTES)) {
                logger.error("restore failed: " + location(c) +
                             " edge_offset=" + hex(EDGES[edge]));
                status |= PHAL_STATUS_ERROR;
            } else {
                ++restored;
            }
        }
    }
    logger.info("restored aperture edges=" + std::to_string(restored));
    return status;
}

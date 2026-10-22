#include "diag/implementer/HqcDma.h"

#include "diag/core/Format.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/PCIE_HQC_REG/PCIE_HQC_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_NDM_REG/DDP_NDM_REG.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {
constexpr uint64_t ALIGNMENT = 32;
constexpr uint32_t POLL_US = 2;
constexpr uint32_t CTRL_TIMEOUT = 8000000;
constexpr auto POLL_LOG_INTERVAL = std::chrono::seconds(1);
constexpr uint32_t DDP_COUNT = 4;
constexpr uint32_t DDP_Q_PHASE[DDP_COUNT] = {0, 1, 2, 3};
constexpr uint32_t DDP_Q_STATION_ID[DDP_COUNT] = {0xc, 0xd, 0xe, 0xf};
constexpr uintptr_t DDP_OFFSETS[DDP_COUNT] = {
    TOP_U_DDP_0_OFFSET, TOP_U_DDP_1_OFFSET,
    TOP_U_DDP_2_OFFSET, TOP_U_DDP_3_OFFSET};

Logger* dma_logger(HalContext& hal)
{
    return hal.device_context().logger;
}

void log_flow(HalContext& hal, const std::string& message)
{
    if (auto* logger = dma_logger(hal); logger != nullptr) logger->flow(message);
}

void log_regio(HalContext& hal, const std::string& message)
{
    if (auto* logger = dma_logger(hal); logger != nullptr) logger->regio(message);
}

void log_error(HalContext& hal, const std::string& message)
{
    if (auto* logger = dma_logger(hal); logger != nullptr) logger->error(message);
}

int64_t elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

void log_poll_sample(HalContext& hal, const std::string& name,
                     phal_ctx_t* ctx, uintptr_t offset, uint64_t attempts,
                     std::chrono::steady_clock::time_point start,
                     uint32_t value)
{
    log_regio(hal, name + " polling sample: bar0_offset=" +
                       common::format::hex(ctx->env.base + offset) +
                       " attempts=" + std::to_string(attempts) +
                       " elapsed_ms=" + std::to_string(elapsed_ms(start)) +
                       " value=" + common::format::hex(value, 8));
}

uint64_t endpoint_address(const DmaEndpoint& endpoint)
{
    if (endpoint.host != nullptr) return endpoint.host->device_addr() + endpoint.address;
    if (endpoint.device != nullptr) return endpoint.device->offset() + endpoint.address;
    return endpoint.address;
}

bool is_host(const DmaEndpoint& endpoint) { return endpoint.memory == DmaMemory::Host; }

void map_region(MemoryRegion region, DmaMemory& memory, uint32_t& core, uint32_t& mem)
{
    memory = DmaMemory::Dmem;
    core = 1;
    mem = 0;
    switch (region) {
    case AIC_IMEM: memory = DmaMemory::Imem; core = 2; mem = 2; break;
    case AIC_SMEM: memory = DmaMemory::Smem; core = 2; mem = 1; break;
    case AIC_VMEM: memory = DmaMemory::Vmem; core = 2; mem = 0; break;
    default: break;
    }
}
}

TestStatus DmaDescriptor::bind(DmaEndpoint& endpoint, DmaMemory memory,
                               uint32_t core, uint32_t mem,
                               const DmaBuffer* host, const common::MemBuffer* device,
                               uint64_t offset)
{
    const auto capacity = host != nullptr ? host->size() : (device != nullptr ? device->size() : UINT64_MAX);
    if (offset > capacity) return PHAL_STATUS_INVALID;
    endpoint = {memory, core, mem, offset, host, device, true};
    finalized_ = false;
    return PHAL_STATUS_OK;
}

DmaDescriptor::DmaDescriptor(HqcDmaEngine& engine, DmaProfile profile)
    : engine_(engine), profile_(profile)
{
    init_defaults();
    init_status_ = engine_.init_desc_ctrl();
}

void DmaDescriptor::set_field(uint32_t bit, uint32_t width, uint64_t value)
{
    for (uint32_t i = 0; i < width; ++i) {
        const uint32_t index = bit + i;
        const uint32_t word = index / 32;
        const uint32_t shift = index % 32;
        words_[word] = (words_[word] & ~(1u << shift)) |
                       (((value >> i) & 1u) << shift);
    }
}

void DmaDescriptor::init_defaults()
{
    words_.fill(0);
    set_field(25, 1, 0);  // execution order
    set_field(26, 1, 1);  // count-done sync mode
    set_field(27, 2, 0);  // thread
    set_field(29, 2, static_cast<uint32_t>(DmaType::LocalHost));
    set_field(31, 1, 0);  // trace
    set_field(224, 32, 16); // src stride level 0
    set_field(256, 32, 16); // dst stride level 0
    set_field(288, 32, 1);  // steps level 1
    set_field(320, 32, 1);  // src stride level 1
    set_field(352, 32, 1);  // dst stride level 1
    set_field(384, 32, 1);  // steps level 2
    set_field(416, 32, 1);  // src stride level 2
    set_field(448, 32, 1);  // dst stride level 2
    set_field(480, 32, 1);  // steps level 3
    set_field(512, 32, 1);  // src stride level 3
    set_field(544, 32, 1);  // dst stride level 3
    set_field(603, 5, 16);  // steps level 4
    dma_type_ = DmaType::LocalHost;
    switch (profile_) {
    case DmaProfile::H2D: sync_flag_ = 0; break;
    case DmaProfile::D2H: sync_flag_ = 1; break;
    case DmaProfile::D2I: sync_flag_ = 2; break;
    case DmaProfile::I2D: sync_flag_ = 3; break;
    case DmaProfile::D2S: sync_flag_ = 4; break;
    case DmaProfile::S2D: sync_flag_ = 5; break;
    case DmaProfile::D2V: sync_flag_ = 6; break;
    case DmaProfile::V2D: sync_flag_ = 7; break;
    case DmaProfile::H2I: sync_flag_ = 8; break;
    case DmaProfile::I2H: sync_flag_ = 9; break;
    case DmaProfile::IsiRrd:
        dma_type_ = DmaType::IsiRrd;
        sync_flag_ = 11; break;
    case DmaProfile::IsiLb:
        dma_type_ = DmaType::IsiLb;
        sync_flag_ = 10; break;
    case DmaProfile::H2S: sync_flag_ = 12; break;
    case DmaProfile::S2H: sync_flag_ = 13; break;
    default: break;
    }
    set_field(29, 2, static_cast<uint32_t>(dma_type_));
    set_field(80, 14, sync_flag_); // destination sync flag 0
    set_field(94, 2, 1);           // destination sync flag 0 core = HQC
}

TestStatus DmaDescriptor::set_src(const DmaBuffer& b, uint64_t o) { return bind(src_, DmaMemory::Host, 1, 1, &b, nullptr, o); }
TestStatus DmaDescriptor::set_dst(const DmaBuffer& b, uint64_t o) { return bind(dst_, DmaMemory::Host, 1, 1, &b, nullptr, o); }
TestStatus DmaDescriptor::set_src(const common::MemBuffer& b, uint64_t o)
{
    DmaMemory memory; uint32_t core; uint32_t mem;
    map_region(b.region(), memory, core, mem);
    return bind(src_, memory, core, mem, nullptr, &b, o);
}
TestStatus DmaDescriptor::set_dst(const common::MemBuffer& b, uint64_t o)
{
    DmaMemory memory; uint32_t core; uint32_t mem;
    map_region(b.region(), memory, core, mem);
    return bind(dst_, memory, core, mem, nullptr, &b, o);
}
TestStatus DmaDescriptor::set_src(DmaMemory m, uint32_t c, uint32_t id, uint64_t o) { return bind(src_, m, c, id, nullptr, nullptr, o); }
TestStatus DmaDescriptor::set_dst(DmaMemory m, uint32_t c, uint32_t id, uint64_t o) { return bind(dst_, m, c, id, nullptr, nullptr, o); }
TestStatus DmaDescriptor::set_size(uint64_t bytes) { size_bytes_ = bytes; finalized_ = false; return PHAL_STATUS_OK; }
TestStatus DmaDescriptor::set_dma_type(DmaType type) { dma_type_ = type; set_field(29, 2, static_cast<uint32_t>(type)); finalized_ = false; return PHAL_STATUS_OK; }
TestStatus DmaDescriptor::set_dst_chip_id(uint32_t id) { if (id >= (1u << 20)) return PHAL_STATUS_INVALID; dst_chip_id_ = id; set_field(0, 20, id); finalized_ = false; return PHAL_STATUS_OK; }

TestStatus DmaDescriptor::encode_endpoint(const DmaEndpoint& endpoint, bool source)
{
    const uint64_t address = endpoint_address(endpoint);
    if (is_host(endpoint)) {
        if ((address % ALIGNMENT) != 0) return PHAL_STATUS_INVALID;
        set_field(608, 32, address >> 32);
        set_field(source ? 116 : 104, 12, 0);
        set_field(source ? 160 : 192, 32, static_cast<uint32_t>(address));
    } else {
        if ((address % 4) != 0) return PHAL_STATUS_INVALID;
        const uint64_t word = address / 4;
        set_field(source ? 116 : 104, 12, word >> 32);
        set_field(source ? 160 : 192, 32, static_cast<uint32_t>(word));
    }
    set_field(source ? 58 : 50, 2, endpoint.mem_id);
    set_field(source ? 60 : 52, 2, endpoint.core_id);
    return PHAL_STATUS_OK;
}

TestStatus DmaDescriptor::finalize()
{
    if (init_status_ != PHAL_STATUS_OK) return init_status_;
    if (!src_.bound || !dst_.bound) return PHAL_STATUS_INVALID;
    if (size_bytes_ == 0 || size_bytes_ % ALIGNMENT != 0 || size_bytes_ / ALIGNMENT > UINT32_MAX) return PHAL_STATUS_INVALID;
    if (src_.host && src_.address + size_bytes_ > src_.host->size()) return PHAL_STATUS_INVALID;
    if (dst_.host && dst_.address + size_bytes_ > dst_.host->size()) return PHAL_STATUS_INVALID;
    set_field(128, 32, size_bytes_ / ALIGNMENT);
    set_field(29, 2, static_cast<uint32_t>(dma_type_));
    auto status = encode_endpoint(src_, true);
    if (status != PHAL_STATUS_OK) return status;
    status = encode_endpoint(dst_, false);
    if (status == PHAL_STATUS_OK &&
        (src_.memory == DmaMemory::Dmem || dst_.memory == DmaMemory::Dmem))
        status = engine_.init_ddp_ctrl();
    finalized_ = status == PHAL_STATUS_OK;
    return status;
}

TestStatus DmaDescriptor::submit()
{
    if (!finalized_) return PHAL_STATUS_INVALID;
    return engine_.copy_descriptor(*this);
}
TestStatus DmaDescriptor::wait(uint64_t timeout) { return engine_.wait_sync_flag(sync_flag_, timeout); }
TestStatus DmaDescriptor::clear_completion() { return engine_.clear_sync_flag_done(sync_flag_); }
TestStatus DmaDescriptor::execute(uint64_t timeout)
{
    auto status = finalize();
    if (status != PHAL_STATUS_OK) return status;
    // Clear previous sync flag
    if (auto s = engine_.clear_sync_flag_done(sync_flag_); s != PHAL_STATUS_OK) return s;
    status = submit();
    if (status != PHAL_STATUS_OK) return status;
    status = wait(timeout);
    if (status != PHAL_STATUS_OK) {
        // Do not release the SRAM range while hardware may still read it.
        descriptor_sram_.keep_allocated();
        return status;
    }
    const auto clear = clear_completion();
    if (clear == PHAL_STATUS_OK) descriptor_sram_.release();
    return clear;
}

TestStatus HqcDmaEngine::init_desc_ctrl()
{
    if (initialized_) return PHAL_STATUS_OK;
    log_flow(hal_, "DMA configuration: HQC descriptor control");
    const auto status = hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        phal_block_ctx_enter(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);
        phal_status_t ret = PHAL_STATUS_OK;
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_TIMEOUT_THRESH_OFFSET,
                          CTRL_TIMEOUT);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_LOW_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_HI_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_TIMEOUT_THRESH_OFFSET,
                          CTRL_TIMEOUT);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_LOW_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_HI_OFFSET, 0);

        // RANG0: ACC_MS
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_STAR_ADDR_LOW_OFFSET, 0x880);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_STAR_ADDR_HIGH_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_END_ADDR_LOW_OFFSET, 0x880);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_END_ADDR_HIGH_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_CTRL_OFFSET,
            PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG0_CTRL_HQC_DESC_CTRL_DCF_RANG_CRD_ENABLE_bm);

        // RANG1: QMSM
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG1_STAR_ADDR_LOW_OFFSET, 0x400);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG1_STAR_ADDR_HIGH_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG1_END_ADDR_LOW_OFFSET, 0x400);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG1_END_ADDR_HIGH_OFFSET, 0);

        // RANG2: PCIE_BS
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG2_STAR_ADDR_LOW_OFFSET, 0x80);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG2_STAR_ADDR_HIGH_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG2_END_ADDR_LOW_OFFSET, 0x80);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG2_END_ADDR_HIGH_OFFSET, 0);

        // RANG3: ISI_BS
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG3_STAR_ADDR_LOW_OFFSET, 0x1000);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG3_STAR_ADDR_HIGH_OFFSET, 0);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG3_END_ADDR_LOW_OFFSET, 0x1000);
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DCF_RANG3_END_ADDR_HIGH_OFFSET, 0);

        // 0x2 = little-endian descriptor format
        ret |= phal_write(ctx, PCIE_HQC_REG_HQC_DESC_CTRL_DD_BIGLITTLE_END_MODE_OFFSET, 0x2);

        phal_block_ctx_exit(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
        return ret;
    });
    if (status == PHAL_STATUS_OK) initialized_ = true;
    return status;
}

TestStatus HqcDmaEngine::init_ddp_ctrl()
{
    if (ddp_initialized_) return PHAL_STATUS_OK;
    auto* ctx = hal_.phal_context();
    if (ctx == nullptr) return PHAL_STATUS_ERROR;
    log_flow(hal_, "DMA configuration: DDP routing");
    phal_status_t status = PHAL_STATUS_OK;
    for (uint32_t index = 0; index < DDP_COUNT; ++index) {
        phal_block_ctx_enter(ctx, DDP_OFFSETS[index]);
        phal_block_ctx_enter(ctx, DDP_U_DDP_NDM_REG_0_OFFSET);
        status |= phal_field_write(
            ctx, DDP_NDM_REG_NDM_CTRL_OFFSET,
            DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bm,
            DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bp,
            DDP_Q_PHASE[index]);
        status |= phal_field_write(
            ctx, DDP_NDM_REG_NDM_CTRL_OFFSET,
            DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bm,
            DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bp,
            DDP_Q_STATION_ID[index]);
        phal_block_ctx_exit(ctx, DDP_U_DDP_NDM_REG_0_OFFSET);
        phal_block_ctx_exit(ctx, DDP_OFFSETS[index]);
    }
    if (status == PHAL_STATUS_OK) {
        ddp_initialized_ = true;
    }
    return status;
}

TestStatus HqcDmaEngine::copy_descriptor(DmaDescriptor& desc)
{
    if (!desc.finalized_) return PHAL_STATUS_INVALID;
    auto status = init_desc_ctrl();
    if (status != PHAL_STATUS_OK) return status;
    desc.descriptor_sram_ = hal_.device_mem_alloc(HQC_SRAM_0, DmaDescriptor::BYTE_SIZE);
    if (!desc.descriptor_sram_.valid()) return PHAL_STATUS_ERROR;
    std::vector<uint8_t> bytes(DmaDescriptor::BYTE_SIZE);
    std::memcpy(bytes.data(), desc.words(), bytes.size());
    log_flow(hal_, "HQC descriptor write: sram_offset=" +
                       common::format::hex(desc.descriptor_sram_.offset()) +
                       " size_bytes=" + std::to_string(bytes.size()));
    status = mem_write(desc.descriptor_sram_, 0, bytes);
    if (status != PHAL_STATUS_OK) return status;
    return send_descriptor(desc, 0); // Use default core id 0
}

TestStatus HqcDmaEngine::send_descriptor(DmaDescriptor& desc, uint32_t core_id)
{
    if (core_id > 1) return PHAL_STATUS_INVALID;
    log_flow(hal_, "HQC descriptor submission: core_id=" + std::to_string(core_id) +
                       " sram_offset=" +
                       common::format::hex(desc.descriptor_sram_.offset()) +
                       " size_bytes=" + std::to_string(DmaDescriptor::BYTE_SIZE));
    return hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        phal_block_ctx_enter(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);

        uint32_t idle_offset;
        uint32_t addr_offset;
        uint32_t size_offset;
        uint32_t trig_offset;
        uint32_t idle_bm;
        uint32_t trig_bm;

        if (core_id == 0){
            idle_offset = PCIE_HQC_REG_CORE0_DESC_CTRL_IDLE_OFFSET;
            addr_offset = PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_LOW_OFFSET;
            size_offset = PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET;
            trig_offset = PCIE_HQC_REG_CORE0_DESC_CTRL_TRIG_OFFSET;
            idle_bm = PCIE_HQC_REG_CORE0_DESC_CTRL_IDLE_CORE0_DESC_CTRL_IDLE_bm;
            trig_bm = PCIE_HQC_REG_CORE0_DESC_CTRL_TRIG_CORE0_DESC_CTRL_TRIG_bm;
        } else {
            idle_offset = PCIE_HQC_REG_CORE1_DESC_CTRL_IDLE_OFFSET;
            addr_offset = PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_LOW_OFFSET;
            size_offset = PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET;
            trig_offset = PCIE_HQC_REG_CORE1_DESC_CTRL_TRIG_OFFSET;
            idle_bm = PCIE_HQC_REG_CORE1_DESC_CTRL_IDLE_CORE1_DESC_CTRL_IDLE_bm;
            trig_bm = PCIE_HQC_REG_CORE1_DESC_CTRL_TRIG_CORE1_DESC_CTRL_TRIG_bm;
        }

        uint32_t idle = 0;
        phal_status_t status = PHAL_STATUS_OK;
        bool idle_seen = false;
        uint64_t attempts = 0;
        const auto poll_start = std::chrono::steady_clock::now();
        auto next_sample = poll_start + POLL_LOG_INTERVAL;
        log_flow(hal_, "HQC idle polling started: core_id=" +
                           std::to_string(core_id) +
                           " max_attempts=" + std::to_string(CTRL_TIMEOUT));
        {
            ScopedPhalRegIoSuppression suppress_regio(ctx);
            for (uint32_t i = 0; i < CTRL_TIMEOUT; ++i) {
                status = phal_read(ctx, idle_offset, &idle);
                attempts = static_cast<uint64_t>(i) + 1;
                const auto now = std::chrono::steady_clock::now();
                if (attempts == 1 || now >= next_sample) {
                    log_poll_sample(hal_, "HQC idle", ctx, idle_offset,
                                    attempts, poll_start, idle);
                    if (now >= next_sample) next_sample = now + POLL_LOG_INTERVAL;
                }
                if (status != PHAL_STATUS_OK) break;
                if (idle & idle_bm) {
                    idle_seen = true;
                    break;
                }
                phal_delay_us(ctx, POLL_US);
                if (i + 1 == CTRL_TIMEOUT) status = PHAL_STATUS_TIMEOUT;
            }
        }
        if (idle_seen) {
            log_flow(hal_, "HQC idle polling complete: core_id=" +
                               std::to_string(core_id) +
                               " attempts=" + std::to_string(attempts) +
                               " elapsed_ms=" + std::to_string(elapsed_ms(poll_start)));
        } else if (status == PHAL_STATUS_TIMEOUT) {
            log_error(hal_, "HQC idle polling timed out: core_id=" +
                                std::to_string(core_id) +
                                " attempts=" + std::to_string(attempts) +
                                " elapsed_ms=" + std::to_string(elapsed_ms(poll_start)) +
                                " last_value=" + common::format::hex(idle, 8));
        } else {
            log_error(hal_, "HQC idle polling failed: core_id=" +
                                std::to_string(core_id) +
                                " attempts=" + std::to_string(attempts) +
                                " status=" + std::to_string(static_cast<int>(status)));
        }
        if (status == PHAL_STATUS_OK && idle_seen) {
            const uint64_t fetch_address = ATL_HQC_SRAM_MCU_LOCAL_BASE +
                                           desc.descriptor_sram_.offset();
            if (fetch_address > UINT32_MAX) {
                status = PHAL_STATUS_INVALID;
            } else {
                status |= phal_write(ctx, trig_offset, 0);
                status |= phal_write(ctx, addr_offset, static_cast<uint32_t>(fetch_address));
                status |= phal_write(ctx, size_offset, DmaDescriptor::BYTE_SIZE);
                status |= phal_write(ctx, trig_offset, trig_bm);
            }
        }
        phal_block_ctx_exit(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
        desc.submitted_ = status == PHAL_STATUS_OK;
        return status;
    });
}

TestStatus HqcDmaEngine::check_sync_flag_done(uint32_t flag) const
{
    if (flag > SOC_HQC_SYNCFLAG_MAX_ID) return PHAL_STATUS_INVALID;
    TestStatus result = PHAL_STATUS_ERROR;
    hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        uint32_t value = 0;
        result = phal_read(ctx, SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(flag), &value);
        if (result == PHAL_STATUS_OK && !(value & 0x1u)) {
            result = PHAL_STATUS_INVALID;
        }
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
        return result;
    });
    return result;
}

TestStatus HqcDmaEngine::clear_sync_flag_done(uint32_t flag) const
{
    if (flag > SOC_HQC_SYNCFLAG_MAX_ID) return PHAL_STATUS_INVALID;
    log_flow(hal_, "Syncflag clear: flag=" + std::to_string(flag));
    TestStatus result = PHAL_STATUS_ERROR;
    hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        result = phal_write(ctx, SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(flag), 0);
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
        return result;
    });
    return result;
}

TestStatus HqcDmaEngine::wait_sync_flag(uint32_t flag, uint64_t timeout_ms) const
{
    if (flag > SOC_HQC_SYNCFLAG_MAX_ID) return PHAL_STATUS_INVALID;
    log_flow(hal_, "Syncflag polling started: flag=" + std::to_string(flag) +
                       " timeout_ms=" + std::to_string(timeout_ms));
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    uint64_t attempts = 0;
    uint32_t value = 0;
    const auto status = hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        phal_status_t poll_status = PHAL_STATUS_OK;
        auto next_sample = start + POLL_LOG_INTERVAL;
        {
            ScopedPhalRegIoSuppression suppress_regio(ctx);
            while (std::chrono::steady_clock::now() < deadline) {
                poll_status = phal_read(
                    ctx, SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(flag), &value);
                ++attempts;
                const auto now = std::chrono::steady_clock::now();
                if (attempts == 1 || now >= next_sample) {
                    log_poll_sample(hal_, "Syncflag", ctx,
                                    SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(flag),
                                    attempts, start, value);
                    if (now >= next_sample) next_sample = now + POLL_LOG_INTERVAL;
                }
                if (poll_status != PHAL_STATUS_OK || (value & 0x1u)) break;
                phal_delay_us(ctx, POLL_US);
            }
        }
        if (poll_status == PHAL_STATUS_OK && !(value & 0x1u)) {
            poll_status = PHAL_STATUS_TIMEOUT;
        }
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
        return poll_status;
    });
    if (status == PHAL_STATUS_OK) {
        log_flow(hal_, "Syncflag polling complete: flag=" + std::to_string(flag) +
                           " attempts=" + std::to_string(attempts) +
                           " elapsed_ms=" + std::to_string(elapsed_ms(start)));
    } else if (status == PHAL_STATUS_TIMEOUT) {
        log_error(hal_, "Syncflag polling timed out: flag=" + std::to_string(flag) +
                            " attempts=" + std::to_string(attempts) +
                            " elapsed_ms=" + std::to_string(elapsed_ms(start)) +
                            " last_value=" + common::format::hex(value, 8));
    } else {
        log_error(hal_, "Syncflag polling failed: flag=" + std::to_string(flag) +
                            " attempts=" + std::to_string(attempts) +
                            " status=" + std::to_string(static_cast<int>(status)));
    }
    return status;
}

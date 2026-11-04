#include "diag/implementer/HqcDma.h"

#include "diag/core/Format.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/HQC.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/PCIE_HQC_REG/PCIE_HQC_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_NDM_REG/DDP_NDM_REG.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

namespace {
constexpr uint64_t DMA_WORD_BYTES = 32;
constexpr uint64_t IMEM_TRANSACTION_BYTES = 256;
constexpr uint64_t INTERNAL_WORD_BYTES = 4;
constexpr uint64_t HQC_SRAM_BANK_BYTES = 0x80000;
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

bool endpoint_address(const DmaEndpoint& endpoint, uint64_t& address)
{
    uint64_t base = 0;
    if (endpoint.host != nullptr) base = endpoint.host->device_addr();
    else if (endpoint.device != nullptr) base = endpoint.device->offset();
    if (endpoint.address > std::numeric_limits<uint64_t>::max() - base)
        return false;
    address = base + endpoint.address;
    return true;
}

uint64_t address_alignment(DmaMemory memory)
{
    switch (memory) {
    case DmaMemory::Host:
    case DmaMemory::Dmem:
    case DmaMemory::Vmem:
        return DMA_WORD_BYTES;
    case DmaMemory::Imem:
        return IMEM_TRANSACTION_BYTES;
    case DmaMemory::Smem:
        return INTERNAL_WORD_BYTES;
    }
    return 0;
}

bool range_valid(const DmaEndpoint& endpoint, uint64_t bytes)
{
    const auto capacity = endpoint.host != nullptr
        ? endpoint.host->size()
        : (endpoint.device != nullptr ? endpoint.device->size()
                                      : std::numeric_limits<uint64_t>::max());
    return endpoint.address <= capacity && bytes <= capacity - endpoint.address;
}

bool map_region(MemoryRegion region, DmaMemory& memory,
                uint32_t& core, uint32_t& mem)
{
    switch (region) {
    case DMEM: memory = DmaMemory::Dmem; core = 1; mem = 0; return true;
    case AIC_IMEM: memory = DmaMemory::Imem; core = 2; mem = 2; return true;
    case AIC_SMEM: memory = DmaMemory::Smem; core = 2; mem = 1; return true;
    case AIC_VMEM: memory = DmaMemory::Vmem; core = 2; mem = 0; return true;
    default: return false;
    }
}
}

TestStatus DmaDescriptor::bind(DmaEndpoint& endpoint, DmaMemory memory,
                               uint32_t core, uint32_t mem,
                               const DmaBuffer* host, const common::MemBuffer* device,
                               uint64_t offset)
{
    const auto capacity = host != nullptr ? host->size() : (device != nullptr ? device->size() : UINT64_MAX);
    if ((state_ != DmaJobState::Draft && state_ != DmaJobState::Completed) ||
        core >= 4 || mem >= 4 || offset > capacity ||
        (host != nullptr && !host->belongs_to(engine_.hal_)) ||
        (device != nullptr && !device->belongs_to(engine_.hal_))) {
        return PHAL_STATUS_INVALID;
    }
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

DmaDescriptor::~DmaDescriptor()
{
    engine_.abandon_descriptor(*this);
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
    // Required by the Atlas DMA descriptor contract.
    set_field(25, 1, 1);  // execution_order_required
    set_field(26, 1, 1);  // count-done sync mode
    set_field(27, 2, 0);  // execution thread, assigned by fill_descriptor()
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
    case DmaProfile::IsiRrd:
        dma_type_ = DmaType::IsiRrd; break;
    case DmaProfile::IsiLb:
        dma_type_ = DmaType::IsiLb; break;
    default: break;
    }
    set_field(29, 2, static_cast<uint32_t>(dma_type_));
    set_field(80, 14, 0);          // assigned with the descriptor slot
    set_field(94, 2, 1);           // destination sync flag 0 core = HQC
}

TestStatus DmaDescriptor::set_src(const DmaBuffer& b, uint64_t o) { return bind(src_, DmaMemory::Host, 1, 1, &b, nullptr, o); }
TestStatus DmaDescriptor::set_dst(const DmaBuffer& b, uint64_t o) { return bind(dst_, DmaMemory::Host, 1, 1, &b, nullptr, o); }
TestStatus DmaDescriptor::set_src(const common::MemBuffer& b, uint64_t o)
{
    DmaMemory memory; uint32_t core; uint32_t mem;
    if (!map_region(b.region(), memory, core, mem)) return PHAL_STATUS_INVALID;
    return bind(src_, memory, core, mem, nullptr, &b, o);
}
TestStatus DmaDescriptor::set_dst(const common::MemBuffer& b, uint64_t o)
{
    DmaMemory memory; uint32_t core; uint32_t mem;
    if (!map_region(b.region(), memory, core, mem)) return PHAL_STATUS_INVALID;
    return bind(dst_, memory, core, mem, nullptr, &b, o);
}
TestStatus DmaDescriptor::set_src(DmaMemory m, uint32_t c, uint32_t id, uint64_t o) { return bind(src_, m, c, id, nullptr, nullptr, o); }
TestStatus DmaDescriptor::set_dst(DmaMemory m, uint32_t c, uint32_t id, uint64_t o) { return bind(dst_, m, c, id, nullptr, nullptr, o); }
TestStatus DmaDescriptor::set_size(uint64_t bytes)
{
    if (state_ != DmaJobState::Draft && state_ != DmaJobState::Completed)
        return PHAL_STATUS_INVALID;
    size_bytes_ = bytes;
    finalized_ = false;
    return PHAL_STATUS_OK;
}

TestStatus DmaDescriptor::set_dma_type(DmaType type)
{
    if ((state_ != DmaJobState::Draft && state_ != DmaJobState::Completed) ||
        static_cast<uint32_t>(type) > 3) {
        return PHAL_STATUS_INVALID;
    }
    dma_type_ = type;
    set_field(29, 2, static_cast<uint32_t>(type));
    finalized_ = false;
    return PHAL_STATUS_OK;
}

TestStatus DmaDescriptor::set_dst_chip_id(uint32_t id)
{
    if ((state_ != DmaJobState::Draft && state_ != DmaJobState::Completed) ||
        id >= (1u << 20)) {
        return PHAL_STATUS_INVALID;
    }
    dst_chip_id_ = id;
    set_field(0, 20, id);
    finalized_ = false;
    return PHAL_STATUS_OK;
}

TestStatus DmaDescriptor::encode_endpoint(const DmaEndpoint& endpoint, bool source)
{
    uint64_t address = 0;
    const auto alignment = address_alignment(endpoint.memory);
    if (!endpoint_address(endpoint, address) || alignment == 0 ||
        address % alignment != 0) {
        return PHAL_STATUS_INVALID;
    }
    if (endpoint.memory == DmaMemory::Host) {
        set_field(608, 32, address >> 32);
        set_field(source ? 116 : 104, 12, 0);
        set_field(source ? 160 : 192, 32, static_cast<uint32_t>(address));
    } else {
        const uint64_t word = address / INTERNAL_WORD_BYTES;
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
    if ((state_ != DmaJobState::Draft && state_ != DmaJobState::Completed) ||
        !src_.bound || !dst_.bound) {
        return PHAL_STATUS_INVALID;
    }
    const uint64_t length_alignment =
        src_.memory == DmaMemory::Imem || dst_.memory == DmaMemory::Imem
            ? IMEM_TRANSACTION_BYTES : DMA_WORD_BYTES;
    // The special 4-byte SMEM write opcode is intentionally not exposed yet.
    if (size_bytes_ == 0 || size_bytes_ % length_alignment != 0 ||
        size_bytes_ / DMA_WORD_BYTES > UINT32_MAX ||
        !range_valid(src_, size_bytes_) || !range_valid(dst_, size_bytes_)) {
        return PHAL_STATUS_INVALID;
    }
    set_field(128, 32, size_bytes_ / DMA_WORD_BYTES);
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
    return engine_.submit(*this);
}
TestStatus DmaDescriptor::fill_descriptor() { return engine_.fill_descriptor(*this); }
TestStatus DmaDescriptor::poll_completion() { return engine_.poll_completion(*this); }
TestStatus DmaDescriptor::wait(uint64_t timeout) { return engine_.wait_completion(*this, timeout); }
TestStatus DmaDescriptor::clear_completion()
{
    return sync_flag_ <= SOC_HQC_SYNCFLAG_MAX_ID
        ? engine_.clear_sync_flag_done(sync_flag_) : PHAL_STATUS_INVALID;
}
TestStatus DmaDescriptor::execute(uint64_t timeout)
{
    auto status = fill_descriptor();
    if (status != PHAL_STATUS_OK) return status;
    status = submit();
    return status == PHAL_STATUS_OK ? wait(timeout) : status;
}

HqcDmaEngine::~HqcDmaEngine()
{
    bool hardware_may_own_descriptor = faulted_;
    for (const auto state : slot_states_) {
        hardware_may_own_descriptor |=
            state == SlotState::Submitted || state == SlotState::Faulted;
    }
    if (hardware_may_own_descriptor && descriptor_pool_.valid()) {
        descriptor_pool_.keep_allocated();
    }
}

TestStatus HqcDmaEngine::ensure_descriptor_pool()
{
    if (descriptor_pool_.valid()) return PHAL_STATUS_OK;
    if (faulted_) return PHAL_STATUS_ERROR;
    descriptor_pool_ = hal_.device_mem_alloc(
        HQC_SRAM_1, DESCRIPTOR_POOL_SIZE, 0);
    if (!descriptor_pool_.valid()) {
        log_error(hal_, "DMA descriptor pool reservation failed: SRAM1 [0, " +
                            common::format::hex(DESCRIPTOR_POOL_SIZE) + ")");
        return PHAL_STATUS_ERROR;
    }
    log_flow(hal_, "DMA descriptor pool reserved: bank=1 slots=" +
                       std::to_string(DESCRIPTOR_SLOT_COUNT) +
                       " stride=" + std::to_string(DESCRIPTOR_SLOT_STRIDE));
    return PHAL_STATUS_OK;
}

void HqcDmaEngine::release_slot(DmaDescriptor& desc)
{
    if (desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT) return;
    slots_in_use_.reset(desc.slot_id_);
    slot_states_[desc.slot_id_] = SlotState::Free;
    desc.slot_id_ = DmaDescriptor::INVALID_SLOT;
}

void HqcDmaEngine::abandon_descriptor(DmaDescriptor& desc)
{
    if (desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT) return;
    if (desc.state_ == DmaJobState::Prepared) {
        release_slot(desc);
        return;
    }
    if (desc.state_ == DmaJobState::Submitted ||
        desc.state_ == DmaJobState::Faulted) {
        faulted_ = true;
        slot_states_[desc.slot_id_] = SlotState::Faulted;
        desc.state_ = DmaJobState::Faulted;
    }
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
    log_flow(hal_, "DMA configuration: DDP routing");
    const auto status = hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_status_t ret = PHAL_STATUS_OK;
        for (uint32_t index = 0; index < DDP_COUNT; ++index) {
            phal_block_ctx_enter(ctx, DDP_OFFSETS[index]);
            phal_block_ctx_enter(ctx, DDP_U_DDP_NDM_REG_0_OFFSET);
            ret |= phal_field_write(
                ctx, DDP_NDM_REG_NDM_CTRL_OFFSET,
                DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bm,
                DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bp,
                DDP_Q_PHASE[index]);
            ret |= phal_field_write(
                ctx, DDP_NDM_REG_NDM_CTRL_OFFSET,
                DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bm,
                DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bp,
                DDP_Q_STATION_ID[index]);
            phal_block_ctx_exit(ctx, DDP_U_DDP_NDM_REG_0_OFFSET);
            phal_block_ctx_exit(ctx, DDP_OFFSETS[index]);
        }
        return ret;
    });
    if (status == PHAL_STATUS_OK) {
        ddp_initialized_ = true;
    }
    return status;
}

TestStatus HqcDmaEngine::fill_descriptor(DmaDescriptor& desc)
{
    if (faulted_ || (desc.state_ != DmaJobState::Draft &&
                     desc.state_ != DmaJobState::Completed)) {
        return PHAL_STATUS_INVALID;
    }
    auto status = ensure_descriptor_pool();
    if (status != PHAL_STATUS_OK) return status;
    status = desc.finalize();
    if (status != PHAL_STATUS_OK) return status;

    uint32_t slot = DESCRIPTOR_SLOT_COUNT;
    for (uint32_t i = 0; i < DESCRIPTOR_SLOT_COUNT; ++i) {
        if (!slots_in_use_.test(i)) { slot = i; break; }
    }
    if (slot == DESCRIPTOR_SLOT_COUNT) {
        log_error(hal_, "DMA descriptor pool is full; poll completions before retrying fill_descriptor");
        return PHAL_STATUS_ERROR;
    }

    slots_in_use_.set(slot);
    slot_states_[slot] = SlotState::Prepared;
    desc.slot_id_ = slot;
    desc.sync_flag_ = slot;
    desc.execution_thread_ = next_thread_++ & 3u;
    desc.set_field(25, 1, 1);
    desc.set_field(26, 1, 1);
    desc.set_field(27, 2, desc.execution_thread_);
    desc.set_field(80, 14, desc.sync_flag_);
    desc.set_field(94, 2, 1);

    status = clear_sync_flag_done(desc.sync_flag_);
    std::vector<uint8_t> bytes(DmaDescriptor::BYTE_SIZE);
    std::vector<uint8_t> readback(DmaDescriptor::BYTE_SIZE);
    std::memcpy(bytes.data(), desc.words(), bytes.size());
    const uint64_t slot_offset = static_cast<uint64_t>(slot) * DESCRIPTOR_SLOT_STRIDE;
    if (status == PHAL_STATUS_OK)
        status = mem_write(descriptor_pool_, slot_offset, bytes);
    // This same-device readback is the conservative bring-up fence until the
    // platform documents ordering between SRAM writes and DESC_CTRL_TRIG.
    if (status == PHAL_STATUS_OK)
        status = mem_read(descriptor_pool_, slot_offset, &readback);
    if (status == PHAL_STATUS_OK && readback != bytes)
        status = PHAL_STATUS_ERROR;
    if (status != PHAL_STATUS_OK) {
        release_slot(desc);
        desc.sync_flag_ = DmaDescriptor::INVALID_SLOT;
        desc.state_ = DmaJobState::Draft;
        return status;
    }

    desc.state_ = DmaJobState::Prepared;
    log_flow(hal_, "HQC descriptor prepared: slot=" + std::to_string(slot) +
                       " sync_flag=" + std::to_string(desc.sync_flag_) +
                       " execution_thread=" + std::to_string(desc.execution_thread_) +
                       " sram1_offset=" + common::format::hex(slot_offset));
    return PHAL_STATUS_OK;
}

TestStatus HqcDmaEngine::submit(DmaDescriptor& desc)
{
    if (faulted_ || desc.state_ != DmaJobState::Prepared ||
        desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT) return PHAL_STATUS_INVALID;
    bool trigger_attempted = false;
    const auto status = send_descriptor(desc, next_core_, trigger_attempted);
    if (status == PHAL_STATUS_OK) {
        desc.state_ = DmaJobState::Submitted;
        slot_states_[desc.slot_id_] = SlotState::Submitted;
    } else if (trigger_attempted) {
        faulted_ = true;
        desc.state_ = DmaJobState::Faulted;
        slot_states_[desc.slot_id_] = SlotState::Faulted;
    }
    return status;
}

TestStatus HqcDmaEngine::submit(
    const std::vector<DmaDescriptor*>& descriptors)
{
    for (const auto* desc : descriptors) {
        if (desc == nullptr || desc->state_ != DmaJobState::Prepared)
            return PHAL_STATUS_INVALID;
    }
    for (auto* desc : descriptors) {
        const auto status = submit(*desc);
        if (status != PHAL_STATUS_OK) return status;
    }
    return PHAL_STATUS_OK;
}

TestStatus HqcDmaEngine::copy_descriptor(DmaDescriptor& desc)
{
    auto status = fill_descriptor(desc);
    return status == PHAL_STATUS_OK ? submit(desc) : status;
}

TestStatus HqcDmaEngine::send_descriptor(
    DmaDescriptor& desc, uint32_t preferred_core, bool& trigger_attempted)
{
    trigger_attempted = false;
    if (preferred_core > 1 || desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT)
        return PHAL_STATUS_INVALID;
    log_flow(hal_, "HQC descriptor submission started: preferred_core=" +
                       std::to_string(preferred_core) +
                       " slot=" + std::to_string(desc.slot_id_));
    return hal_.with_pcie_context([&](phal_ctx_t* ctx) {
        phal_block_ctx_enter(ctx, PCIE_U_HQC_0_OFFSET);
        phal_block_ctx_enter(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);

        struct CoreRegs {
            uint32_t idle;
            uint32_t address_low;
            uint32_t address_high;
            uint32_t size;
            uint32_t trigger;
            uint32_t idle_mask;
            uint32_t trigger_mask;
        };
        const CoreRegs regs[2] = {
            {PCIE_HQC_REG_CORE0_DESC_CTRL_IDLE_OFFSET,
             PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_LOW_OFFSET,
             PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_HI_OFFSET,
             PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET,
             PCIE_HQC_REG_CORE0_DESC_CTRL_TRIG_OFFSET,
             PCIE_HQC_REG_CORE0_DESC_CTRL_IDLE_CORE0_DESC_CTRL_IDLE_bm,
             PCIE_HQC_REG_CORE0_DESC_CTRL_TRIG_CORE0_DESC_CTRL_TRIG_bm},
            {PCIE_HQC_REG_CORE1_DESC_CTRL_IDLE_OFFSET,
             PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_LOW_OFFSET,
             PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_HI_OFFSET,
             PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_SIZE_OFFSET,
             PCIE_HQC_REG_CORE1_DESC_CTRL_TRIG_OFFSET,
             PCIE_HQC_REG_CORE1_DESC_CTRL_IDLE_CORE1_DESC_CTRL_IDLE_bm,
             PCIE_HQC_REG_CORE1_DESC_CTRL_TRIG_CORE1_DESC_CTRL_TRIG_bm}
        };

        uint32_t idle[2] = {};
        uint32_t selected_core = 2;
        phal_status_t status = PHAL_STATUS_OK;
        uint64_t attempts = 0;
        const auto poll_start = std::chrono::steady_clock::now();
        auto next_sample = poll_start + POLL_LOG_INTERVAL;
        {
            ScopedPhalRegIoSuppression suppress_regio(ctx);
            for (uint32_t i = 0; i < CTRL_TIMEOUT; ++i) {
                attempts = static_cast<uint64_t>(i) + 1;
                for (uint32_t pass = 0; pass < 2; ++pass) {
                    const uint32_t core = (preferred_core + pass) & 1u;
                    status = phal_read(ctx, regs[core].idle, &idle[core]);
                    if (status != PHAL_STATUS_OK) break;
                    if (idle[core] & regs[core].idle_mask) {
                        selected_core = core;
                        break;
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (attempts == 1 || now >= next_sample) {
                    log_poll_sample(hal_, "HQC idle", ctx,
                                    regs[preferred_core].idle,
                                    attempts, poll_start, idle[preferred_core]);
                    if (now >= next_sample) next_sample = now + POLL_LOG_INTERVAL;
                }
                if (status != PHAL_STATUS_OK) break;
                if (selected_core < 2) break;
                phal_delay_us(ctx, POLL_US);
                if (i + 1 == CTRL_TIMEOUT) status = PHAL_STATUS_TIMEOUT;
            }
        }
        if (selected_core < 2) {
            log_flow(hal_, "HQC idle polling complete: selected_core=" +
                               std::to_string(selected_core) +
                               " attempts=" + std::to_string(attempts) +
                               " elapsed_ms=" + std::to_string(elapsed_ms(poll_start)));
        } else if (status == PHAL_STATUS_TIMEOUT) {
            log_error(hal_, "HQC idle polling timed out: attempts=" +
                                std::to_string(attempts) +
                                " elapsed_ms=" + std::to_string(elapsed_ms(poll_start)) +
                                " core0=" + common::format::hex(idle[0], 8) +
                                " core1=" + common::format::hex(idle[1], 8));
        } else {
            log_error(hal_, "HQC idle polling failed: attempts=" +
                                std::to_string(attempts) +
                                " status=" + std::to_string(static_cast<int>(status)));
        }
        if (status == PHAL_STATUS_OK && selected_core < 2) {
            const auto& selected = regs[selected_core];
            const uint64_t fetch_address = ATL_HQC_SRAM_MCU_LOCAL_BASE +
                static_cast<uint64_t>(DESCRIPTOR_SRAM_BANK) * HQC_SRAM_BANK_BYTES +
                static_cast<uint64_t>(desc.slot_id_) * DESCRIPTOR_SLOT_STRIDE;
            if (fetch_address > UINT32_MAX) {
                status = PHAL_STATUS_INVALID;
            } else {
                status = phal_write(ctx, selected.trigger, 0);
                if (status == PHAL_STATUS_OK)
                    status = phal_write(ctx, selected.address_low,
                                        static_cast<uint32_t>(fetch_address));
                if (status == PHAL_STATUS_OK)
                    status = phal_write(ctx, selected.address_high,
                                        static_cast<uint32_t>(fetch_address >> 32));
                if (status == PHAL_STATUS_OK)
                    status = phal_write(ctx, selected.size,
                                        DmaDescriptor::BYTE_SIZE);

                uint32_t low = 0, high = 0, size = 0;
                if (status == PHAL_STATUS_OK)
                    status = phal_read(ctx, selected.address_low, &low);
                if (status == PHAL_STATUS_OK)
                    status = phal_read(ctx, selected.address_high, &high);
                if (status == PHAL_STATUS_OK)
                    status = phal_read(ctx, selected.size, &size);
                if (status == PHAL_STATUS_OK &&
                    (low != static_cast<uint32_t>(fetch_address) ||
                     high != static_cast<uint32_t>(fetch_address >> 32) ||
                     size != DmaDescriptor::BYTE_SIZE)) {
                    status = PHAL_STATUS_ERROR;
                }
                if (status == PHAL_STATUS_OK) {
                    trigger_attempted = true;
                    status = phal_write(ctx, selected.trigger,
                                        selected.trigger_mask);
                }
                // Flush the posted trigger write. IDLE may already be either 0
                // or 1, depending on how quickly descriptor routing completes.
                uint32_t post_trigger_idle = 0;
                if (status == PHAL_STATUS_OK)
                    status = phal_read(ctx, selected.idle,
                                       &post_trigger_idle);
                if (status == PHAL_STATUS_OK) {
                    next_core_ = (selected_core + 1) & 1u;
                    log_flow(hal_, "HQC descriptor triggered: core=" +
                                       std::to_string(selected_core) +
                                       " slot=" + std::to_string(desc.slot_id_) +
                                       " fetch_address=" +
                                       common::format::hex(fetch_address));
                }
            }
        }
        phal_block_ctx_exit(ctx, HQC_U_PCIE_HQC_REG_0_OFFSET);
        phal_block_ctx_exit(ctx, PCIE_U_HQC_0_OFFSET);
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

TestStatus HqcDmaEngine::poll_completion(DmaDescriptor& desc)
{
    if ((desc.state_ != DmaJobState::Submitted &&
         desc.state_ != DmaJobState::Faulted) ||
        desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT ||
        desc.sync_flag_ != desc.slot_id_) {
        return PHAL_STATUS_INVALID;
    }
    const auto status = check_sync_flag_done(desc.sync_flag_);
    if (status != PHAL_STATUS_OK) return status;
    const auto clear = clear_sync_flag_done(desc.sync_flag_);
    if (clear != PHAL_STATUS_OK) {
        faulted_ = true;
        desc.state_ = DmaJobState::Faulted;
        slot_states_[desc.slot_id_] = SlotState::Faulted;
        return clear;
    }
    desc.state_ = DmaJobState::Completed;
    release_slot(desc);
    return PHAL_STATUS_OK;
}

TestStatus HqcDmaEngine::wait_completion(
    DmaDescriptor& desc, uint64_t timeout_ms)
{
    if (timeout_ms == 0 ||
        (desc.state_ != DmaJobState::Submitted &&
         desc.state_ != DmaJobState::Faulted) ||
        desc.slot_id_ >= DESCRIPTOR_SLOT_COUNT ||
        desc.sync_flag_ != desc.slot_id_) {
        return PHAL_STATUS_INVALID;
    }
    const auto status = wait_sync_flag(desc.sync_flag_, timeout_ms);
    if (status != PHAL_STATUS_OK) {
        // A software timeout does not prove that hardware discarded the job.
        faulted_ = true;
        desc.state_ = DmaJobState::Faulted;
        slot_states_[desc.slot_id_] = SlotState::Faulted;
        return status;
    }
    const auto clear = clear_sync_flag_done(desc.sync_flag_);
    if (clear != PHAL_STATUS_OK) {
        faulted_ = true;
        desc.state_ = DmaJobState::Faulted;
        slot_states_[desc.slot_id_] = SlotState::Faulted;
        return clear;
    }
    desc.state_ = DmaJobState::Completed;
    release_slot(desc);
    return PHAL_STATUS_OK;
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

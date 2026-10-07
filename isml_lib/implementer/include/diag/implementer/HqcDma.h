#pragma once

#include "diag/core/DevMem.h"
#include "diag/core/HalContext.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#define SOC_HQC_SYNCFLAG_MAX_ID 255U
#define SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(id) (0x400 + (id) * 4)
#define ATL_HQC_SRAM_MCU_LOCAL_BASE 0x30000000u

enum class DmaProfile { Default, H2D, D2H, D2I, I2D, D2S, S2D, D2V, V2D, IsiLb, IsiRrd };
enum class DmaType : uint8_t { LocalHost = 0, IsiLb = 1, Local = 2, IsiRrd = 3 };
enum class DmaMemory : uint8_t { Host, Dmem, Imem, Smem, Vmem };

struct DmaEndpoint {
    DmaMemory memory = DmaMemory::Dmem;
    uint32_t core_id = 0;
    uint32_t mem_id = 0;
    uint64_t address = 0;
    const DmaBuffer* host = nullptr;
    const common::MemBuffer* device = nullptr;
    bool bound = false;
};

class HqcDmaEngine;

class DmaDescriptor {
public:
    static constexpr size_t BYTE_SIZE = 80;
    explicit DmaDescriptor(HqcDmaEngine& engine, DmaProfile profile = DmaProfile::Default);

    TestStatus set_src(const DmaBuffer& buffer, uint64_t offset = 0);
    TestStatus set_dst(const DmaBuffer& buffer, uint64_t offset = 0);
    TestStatus set_src(const common::MemBuffer& buffer, uint64_t offset = 0);
    TestStatus set_dst(const common::MemBuffer& buffer, uint64_t offset = 0);
    TestStatus set_src(DmaMemory memory, uint32_t core, uint32_t mem, uint64_t offset);
    TestStatus set_dst(DmaMemory memory, uint32_t core, uint32_t mem, uint64_t offset);
    TestStatus set_size(uint64_t bytes);
    TestStatus set_dma_type(DmaType type);
    TestStatus set_dst_chip_id(uint32_t chip_id);

    TestStatus finalize();
    TestStatus submit();
    TestStatus wait(uint64_t timeout_ms);
    TestStatus clear_completion();
    TestStatus execute(uint64_t timeout_ms);

    const uint32_t* words() const { return words_.data(); }
    uint32_t sync_flag() const { return sync_flag_; }

private:
    friend class HqcDmaEngine;
    HqcDmaEngine& engine_;
    DmaProfile profile_;
    std::array<uint32_t, 20> words_{};
    DmaEndpoint src_{};
    DmaEndpoint dst_{};
    uint64_t size_bytes_ = 0;
    DmaType dma_type_ = DmaType::LocalHost;
    uint32_t dst_chip_id_ = 0;
    uint32_t sync_flag_ = 10;
    TestStatus init_status_ = PHAL_STATUS_OK;
    bool finalized_ = false;
    bool submitted_ = false;
    common::MemBuffer descriptor_sram_;

    void init_defaults();
    void set_field(uint32_t bit, uint32_t width, uint64_t value);
    TestStatus bind(DmaEndpoint& endpoint, DmaMemory memory, uint32_t core,
                    uint32_t mem, const DmaBuffer* host,
                    const common::MemBuffer* device, uint64_t offset);
    TestStatus encode_endpoint(const DmaEndpoint& endpoint, bool source);
};

class HqcDmaEngine {
public:
    explicit HqcDmaEngine(HalContext& hal) : hal_(hal) {}

    TestStatus init_desc_ctrl();
    TestStatus init_ddp_ctrl();
    TestStatus copy_descriptor(DmaDescriptor& desc);
    TestStatus send_descriptor(DmaDescriptor& desc, uint32_t core_id);
    TestStatus check_sync_flag_done(uint32_t flag) const;
    TestStatus clear_sync_flag_done(uint32_t flag) const;
    TestStatus wait_sync_flag(uint32_t flag, uint64_t timeout_ms) const;

private:
    HalContext& hal_;
    bool initialized_ = false;
    bool ddp_initialized_ = false;
};

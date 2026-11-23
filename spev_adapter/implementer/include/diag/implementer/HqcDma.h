#pragma once

#include "diag/core/DevMem.h"
#include "diag/core/HalContext.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

#define SOC_HQC_SYNCFLAG_MIN_ID 1U
#define SOC_HQC_SYNCFLAG_MAX_ID 255U
#define SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(id) (0x400 + (id) * 4)
#define ATL_HQC_SRAM_MCU_LOCAL_BASE 0x30000000u

enum class DmaProfile {
    Default,
    H2D, D2H,
    D2I, I2D,
    D2S, S2D,
    D2V, V2D,
    H2I, I2H,
    H2S, S2H,
    IsiLb, IsiRrd,
};
enum class DmaType : uint8_t { LocalHost = 0, IsiLb = 1, Local = 2, IsiRrd = 3 };
enum class DmaMemory : uint8_t { Host, Dmem, Imem, Smem, Vmem };
enum class DmaJobState : uint8_t { Draft, Prepared, Submitted, Completed, Faulted };

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
    ~DmaDescriptor();
    DmaDescriptor(const DmaDescriptor&) = delete;
    DmaDescriptor& operator=(const DmaDescriptor&) = delete;
    DmaDescriptor(DmaDescriptor&&) = delete;
    DmaDescriptor& operator=(DmaDescriptor&&) = delete;

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
    TestStatus fill_descriptor();
    TestStatus submit();
    // OK with triggered=false means both descriptor controllers are busy.
    TestStatus try_submit(bool& triggered);
    TestStatus poll_completion();
    TestStatus wait(uint64_t timeout_ms);
    TestStatus clear_completion();
    TestStatus execute(uint64_t timeout_ms);

    const uint32_t* words() const { return words_.data(); }
    uint32_t sync_flag() const { return sync_flag_; }
    uint32_t slot_id() const { return slot_id_; }
    uint32_t execution_thread() const { return execution_thread_; }
    DmaJobState state() const { return state_; }

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
    static constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();
    uint32_t sync_flag_ = INVALID_SLOT;
    uint32_t slot_id_ = INVALID_SLOT;
    uint32_t execution_thread_ = 0;
    TestStatus init_status_ = PHAL_STATUS_OK;
    bool finalized_ = false;
    DmaJobState state_ = DmaJobState::Draft;

    void init_defaults();
    void set_field(uint32_t bit, uint32_t width, uint64_t value);
    TestStatus bind(DmaEndpoint& endpoint, DmaMemory memory, uint32_t core,
                    uint32_t mem, const DmaBuffer* host,
                    const common::MemBuffer* device, uint64_t offset);
    TestStatus encode_endpoint(const DmaEndpoint& endpoint, bool source);
};

class HqcDmaEngine {
public:
    static constexpr uint32_t DESCRIPTOR_SLOT_COUNT = 256;
    static constexpr uint32_t DESCRIPTOR_FIRST_SLOT = SOC_HQC_SYNCFLAG_MIN_ID;
    static constexpr uint32_t DESCRIPTOR_USABLE_SLOT_COUNT =
        DESCRIPTOR_SLOT_COUNT - DESCRIPTOR_FIRST_SLOT;
    static constexpr uint64_t DESCRIPTOR_SLOT_STRIDE = 512;
    static constexpr uint64_t DESCRIPTOR_POOL_SIZE =
        DESCRIPTOR_SLOT_COUNT * DESCRIPTOR_SLOT_STRIDE;
    static constexpr uint32_t DESCRIPTOR_SRAM_BANK = 1;

    explicit HqcDmaEngine(HalContext& hal) : hal_(hal) {}
    ~HqcDmaEngine();
    HqcDmaEngine(const HqcDmaEngine&) = delete;
    HqcDmaEngine& operator=(const HqcDmaEngine&) = delete;

    TestStatus init_desc_ctrl();
    TestStatus init_ddp_ctrl();
    TestStatus fill_descriptor(DmaDescriptor& desc);
    TestStatus submit(DmaDescriptor& desc);
    TestStatus try_submit(DmaDescriptor& desc, bool& triggered);
    TestStatus poll_completion(DmaDescriptor& desc);
    TestStatus wait_completion(DmaDescriptor& desc, uint64_t timeout_ms);
    TestStatus check_sync_flag_done(uint32_t flag) const;
    TestStatus clear_sync_flag_done(uint32_t flag) const;
    TestStatus wait_sync_flag(uint32_t flag, uint64_t timeout_ms) const;
    size_t available_slots() const;

private:
    friend class DmaDescriptor;
    enum class SlotState : uint8_t { Free, Prepared, Submitted, Faulted };

    HalContext& hal_;
    // Initialization is independent from descriptor ownership and may be
    // reached concurrently by the first descriptors created by worker threads.
    std::mutex init_mutex_;
    // Protect descriptor-pool ownership, slot state, and execution-thread IDs.
    mutable std::mutex state_mutex_;
    // Keep idle selection, controller programming, trigger, and next_core_
    // together as one submission transaction.
    std::mutex submission_mutex_;
    bool initialized_ = false;
    bool ddp_initialized_ = false;
    std::atomic<bool> faulted_{false};
    uint32_t next_core_ = 0;
    uint32_t next_thread_ = 0;
    common::MemBuffer descriptor_pool_;
    std::bitset<DESCRIPTOR_SLOT_COUNT> slots_in_use_{};
    std::array<SlotState, DESCRIPTOR_SLOT_COUNT> slot_states_{};

    TestStatus ensure_descriptor_pool();
    TestStatus send_descriptor(DmaDescriptor& desc, uint32_t preferred_core,
                               bool wait_for_idle, bool& triggered,
                               bool& trigger_attempted);
    void release_slot(DmaDescriptor& desc);
    void abandon_descriptor(DmaDescriptor& desc);
};

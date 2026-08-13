#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

enum class CmdQueuePath {
    HQC,
    IPC,
};

struct CmdDescriptor {
    std::string target_name;
    std::string test_name;
    std::string opcode;
    uint64_t payload_addr = 0;
    uint64_t payload_size = 0;
};

struct CmdCompletion {
    uint64_t cmd_id = 0;
    bool completed = false;
    bool passed = false;
    uint32_t fw_status = 0;
    std::string status;
};

class CmdQueueMgm {
private:
    std::string name_;
    CmdQueuePath path_;
    void* doorbell_base_ = nullptr;
    uint64_t next_cmd_id_ = 1;
    uint64_t sq_head_ = 0;
    uint64_t sq_tail_ = 0;
    uint64_t cq_head_ = 0;
    uint64_t cq_tail_ = 0;
    std::unordered_map<uint64_t, CmdDescriptor> pending_cmds_;
    std::unordered_map<uint64_t, CmdCompletion> completions_;
    mutable std::mutex queue_mutex_;

public:
    CmdQueueMgm(const std::string& name,
                CmdQueuePath path,
                void* doorbell_base = nullptr);

    const std::string& name() const { return name_; }
    CmdQueuePath path() const { return path_; }
    void* doorbell_base() const { return doorbell_base_; }

    uint64_t submit(const CmdDescriptor& descriptor);
    bool poll_complete(uint64_t cmd_id, CmdCompletion& completion);
    CmdCompletion wait(uint64_t cmd_id, uint64_t timeout_ms);
    void drain();

    uint64_t sq_head() const;
    uint64_t sq_tail() const;
    uint64_t cq_head() const;
    uint64_t cq_tail() const;
};

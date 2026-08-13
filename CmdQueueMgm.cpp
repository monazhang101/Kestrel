#include "CmdQueueMgm.h"

#include <chrono>
#include <thread>

CmdQueueMgm::CmdQueueMgm(const std::string& name,
                         CmdQueuePath path,
                         void* doorbell_base)
    : name_(name), path_(path), doorbell_base_(doorbell_base)
{
}

uint64_t CmdQueueMgm::submit(const CmdDescriptor& descriptor)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);

    auto cmd_id = next_cmd_id_++;
    pending_cmds_[cmd_id] = descriptor;

    // Pseudocode: write descriptor into SQ[tail], advance SQ tail, then ring doorbell.
    ++sq_tail_;
    (void)doorbell_base_;

    // Pseudocode only: pretend FW consumed SQ and produced CQ immediately.
    ++sq_head_;
    ++cq_tail_;
    completions_[cmd_id] = {
        cmd_id,
        true,
        true,
        0,
        "completed_pseudocode"
    };

    return cmd_id;
}

bool CmdQueueMgm::poll_complete(uint64_t cmd_id, CmdCompletion& completion)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);

    auto it = completions_.find(cmd_id);
    if (it == completions_.end()) {
        return false;
    }

    // Pseudocode: read CQ entry, advance CQ head, and release descriptor ownership.
    completion = it->second;
    ++cq_head_;
    completions_.erase(it);
    pending_cmds_.erase(cmd_id);
    return true;
}

CmdCompletion CmdQueueMgm::wait(uint64_t cmd_id, uint64_t timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    CmdCompletion completion;
    while (std::chrono::steady_clock::now() <= deadline) {
        if (poll_complete(cmd_id, completion)) {
            return completion;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return {
        cmd_id,
        false,
        false,
        0xffffffff,
        "timeout"
    };
}

void CmdQueueMgm::drain()
{
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Pseudocode: wait until SQ/CQ are empty before reset, teardown, or fatal cleanup.
    pending_cmds_.clear();
    completions_.clear();
}

uint64_t CmdQueueMgm::sq_head() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return sq_head_;
}

uint64_t CmdQueueMgm::sq_tail() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return sq_tail_;
}

uint64_t CmdQueueMgm::cq_head() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return cq_head_;
}

uint64_t CmdQueueMgm::cq_tail() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return cq_tail_;
}

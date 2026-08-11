#pragma once
// TODO：错误码、日志、Result 类型、寄存器读写接口、BAR mmap 生命周期管理、设备打开/关闭流程等。

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

// =============================
// Step 1：Device Info & 资源/内存定义
// =============================
struct DeviceContext {
    // TPU整卡级别信息
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;

    // 子模块内存与资源
    void* mapped_bar_base = nullptr; //已经 mmap 到用户态的 BAR 基地址
    uint64_t reg_offset = 0;         //模块寄存器区域起点 - 在BAR空间中的偏移量
    uint64_t reg_size = 0;           // 寄存器空间大小
};

// =======================
// Step 2: 定义 TestResult
// - 不再只返回 true/false，而是返回更完整的测试结果
// =======================
struct TestResult {
    std::string test_name;
    std::string device_name;
    bool passed = false;
    std::unordered_map<std::string, std::string> metrics;
};

// =======================
// Step 3: 定义 TestFunc
// - 输入：DeviceContext* ctx：获取设备信息、BAR 地址、寄存器偏移等
// - 输出：TestResult：返回测试名、设备名、pass/fail、自定义metrics
// =======================
using TestFunc = std::function<TestResult(DeviceContext* ctx)>;

// ==============================
// Step 4: 定义 BaseDevice 基类
// ==============================
class BaseDevice {
private:
    std::mutex device_mutex_;  // 每个设备实例一把锁

protected:
    std::string name_;
    DeviceContext ctx_;

    // 测试注册表：testname - testfunc
    std::unordered_map<std::string, TestFunc> registered_tests_;

public:
    // 构造函数：device name和device info
    BaseDevice(const std::string& name, const DeviceContext& ctx)
        : name_(name), ctx_(ctx)
    {
        // 子类可以在自己的构造函数里注册具体测试。
    }

    // 虚析构函数：真实实现中可以在子类析构中释放 BAR mmap、关闭 fd、释放 HAL session 等
    virtual ~BaseDevice() = default;

    // 注册atomic test
    void register_test(const std::string& test_name, TestFunc func){
        registered_tests_[test_name] = std::move(func);
    }

    // 执行atomic test
    virtual TestResult run_test(const std::string& test_name)
    {
        // 同一子模块（如ISI0）上的测试，只能排队串行执行
        std::lock_guard<std::mutex> lock(device_mutex_);

        TestFunc func;
        auto it = registered_tests_.find(test_name); //查找test有没有被注册
        if (it == registered_tests_.end()) {
            return {test_name, name_, false, {{"error", "Test Not Found"}}};
        }

        func = it->second;
        return func(&ctx_); // run test
    }

    // 获取当前模块实际的寄存器基地址（BAR基址 + 模块offset）
    void* get_module_base_addr() const{
        return static_cast<uint8_t*>(ctx_.mapped_bar_base) + ctx_.reg_offset;
    }

    // Device name
    const std::string& get_name() const { return name_; }

    // Device Info
    DeviceContext& get_context() { return ctx_; }

    const DeviceContext& get_context() const { return ctx_; }

};
 
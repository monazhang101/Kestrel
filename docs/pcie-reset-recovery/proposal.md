# PMU Chip Reset 后的设备恢复方案

## 1. 结论

首版采用手动的两阶段 CLI：

```bash
sudo spev_diag chip-reset-prepare \
  --target TPU0 \
  --output before-reset.json

# 用户通过 PMU 手动执行 chip reset

sudo spev_diag chip-reset-recover \
  --input before-reset.json \
  --timeout-ms 60000
```

命令名不以 `pcie` 开头，因为这是 PMU chip reset 的设备生命周期操作，不是 PCIe module testcase，也不是 Host 发起的 PCIe protocol reset。

首版不执行 Linux PCI `remove` 或 `rescan`。恢复的前提是原 BDF 对应的 Linux device object、canonical sysfs path、inode、BAR resource 和 upstream bridge window 全部保留。任一条件变化时，必须在 config write 前失败。

这里的“重新枚举/重新映射”特指：

```text
DeviceManager::discover()
  -> 重新扫描现存 sysfs PCI device
  -> 重新建立 BAR mmap
  -> 等待 system-ready
  -> 重新建立 TPU/module target tree
```

它不是 Linux PCI bus rescan。

## 2. 模块边界

### `PciHostRecovery`

文件：

```text
spev_diag/include/diag/core/PciHostRecovery.h
spev_diag/src/core/PciHostRecovery.cpp
```

这是一个窄范围、无 HAL session 所有权的 Host helper，负责：

- 读取和校验 256B/4KB PCI config space。
- 采集 boot ID、sysfs inode/path、六项 BAR resource 和 upstream bridge window。
- 解析 Type-0 header、standard/extended capability layout。
- 以不可覆盖方式保存、严格解析 JSON snapshot。
- 获取跨进程的 per-BDF recovery lock。
- driver bind/unbind。
- 建立安全的 unbound 状态。
- 按 `pcie_config_restore.py` 的约束恢复 BAR 和 MPS/MRRS。

公开低层 API 包括 `capture()`、`save_new()`、`load()`、`unbind_driver()`、`bind_driver()`、`make_unbound_device_safe()` 和 `restore()`。后续若 reset 能由 API 触发，可以直接复用这些原语。

### `DeviceManager`

`DeviceManager` 负责组合工作流：

```cpp
TestStatus prepare_before_reset(
    const std::string& target_name,
    const std::string& output_path,
    std::string* error = nullptr);

TestStatus recover_after_reset(
    const std::string& input_path,
    uint32_t timeout_ms = 60000,
    std::string* error = nullptr);
```

它持有 `execution_mutex_`，阻止 testcase/discovery/recovery 并发，并负责销毁旧 target、释放 `HalContext` 映射和重新 discover。

`DeviceContext` 仍然只是运行时数据，不承担 bind/unbind、snapshot 或 recovery 行为。`HalContext` 仍是 PHAL context、DMA engine 和 BAR mapping 的唯一 session owner。

### CLI

CLI 只负责参数校验、调用组合 API 和报告结果。它不会把 lifecycle workflow 注册成 `_add_test()` testcase。

## 3. Snapshot 数据

单个 JSON recovery bundle 保存：

- format version、target、原 BDF 和 UTC 时间。
- reset 前是否绑定 driver，以及原 driver 名称。
- 完整 256B/4KB config image，使用 hex 编码。
- 64B Type-0 header。
- standard/extended capability offset 与 ID。
- PCIe flags、Device Control、Device Capabilities。
- boot ID。
- sysfs inode、canonical path 和 config size。
- 六项 `resource` 行。
- 每级 upstream bridge BDF 与 routing/window registers。

snapshot 在 unbind 或 config write 之前创建，采用 exclusive-create，拒绝覆盖已有文件。这样即使 prepare 后续失败，reset 前基线仍然保留。

JSON 使用专用结构化 parser，不通过正则或字符串切片提取字段。文件大小限制为 1 MiB；字段类型、整数范围、config/header 一致性、capability 和 BAR layout 都会在 load 时重新校验。

## 4. Prepare 阶段

```text
获取 DeviceManager execution lock
  -> Host PCI scan + product policy 定位 target/BDF（不 mmap BAR、不访问 PMU）
  -> 获取跨进程 per-BDF lock
  -> 采集 full config + Host environment + driver
  -> exclusive-create snapshot
  -> 清理 framework target/HAL/BAR mapping
  -> unbind 原 driver（如原来已绑定）
  -> 验证原 sysfs object/resource/bridge 未变化
  -> Command 关闭 I/O、MSE、BME并 mask INTx
  -> 关闭 MSI/MSI-X enable 和 Expansion ROM decode
  -> 验证 PM D0、安全 Command 和 config readback
  -> 返回，等待用户手动 reset
```

prepare 不负责让业务或外部 driver workload quiesce。调用前必须由操作者停止上层业务并确保没有 DMA/queue 活动；框架的 `execution_mutex_` 只能阻止本进程中新启动 testcase。

若 snapshot 已存在，prepare 在任何副作用前失败。若 unbind 或安全化失败，命令返回错误，保留 snapshot，但设备状态必须由操作者检查后再决定是否 reset。

## 5. Recover 阶段

```text
获取 DeviceManager execution lock
  -> load/validate snapshot
  -> 获取原 BDF 的跨进程 lock
  -> 清理可能残留的 framework mapping
  -> 等待原 BDF config identity 恢复
  -> 要求 driver 未绑定
  -> 重新采集 live layout/environment
  -> exact-check boot/inode/path/resource/bridge
  -> check identity/class/subsystem/capabilities/BAR attributes
  -> Command 关闭 decode/BME并 mask INTx
  -> 按 high-then-low 恢复 memory BAR
  -> 只恢复 PCIe MPS/MRRS
  -> 再次验证 BAR、environment 和 Command
  -> 最终 Command: BME=0、MSE=1、I/O=0、INTx masked
  -> 如 reset 前有 driver，绑定原 driver
  -> DeviceManager::discover_locked() 重新 mmap 和建立 target tree
  -> 验证原 target/BDF 已恢复
```

config restore 不回放 MSI/MSI-X address/data、AER status、PM trigger、FLR、BME 或私有寄存器。原 driver 在 bind/probe 时重新取得 DMA 和 interrupt ownership。

如果 reset 前没有 driver，framework rediscover 后会再次强制 `BME=0、MSE=1、I/O=0、INTx masked`，避免 BAR mmap 的准备逻辑意外留下 bus mastering。

restore 一旦开始写配置，任何异常都会尽力回到 decode/BME disabled、INTx masked 状态，并保留原始失败原因。

## 6. Runtime Checks

与参考 Python 的 `check_same_device()` 一样，以下条件必须完全相同：

- boot ID。
- sysfs inode。
- canonical sysfs path。
- config size。
- 六项 BAR resource start/end/flags。
- 所有 upstream bridge window registers。
- vendor/device identity、class 和 subsystem identity。
- standard/extended capability layout。
- PCIe capability type 和 Device Capabilities。
- BAR type、64-bit 和 prefetch attributes。

`Device Capabilities` 中的 Captured Slot Power Limit Value/Scale
（bits 27:18）是由 PCIe `Set_Slot_Power_Limit` message 捕获的动态值。PMU
内部 reset 后 Host 不一定重新发送该 message，因此这两个字段只记录 INFO
诊断，不阻止 recovery；其余 Device Capabilities 位仍要求完全一致。

因此：

- 原 sysfs device 消失：失败，不 rescan。
- Linux 重新创建了同 BDF device：inode/path check 失败。
- Linux 重新分配 BAR：resource check 失败，不写旧 BAR。
- upstream bridge window 变化：失败。
- reset 后 driver 自动绑定：失败，不能在 driver ownership 下 restore。
- multifunction、VF、SR-IOV 或 Resizable BAR：首版不支持。

## 7. Driver 与最终状态

prepare 记录 driver symlink 的最终目录名。unbind 前要求当前 driver 与 snapshot 相同；recover bind 前要求没有其他 driver，bind 后等待 symlink 指向原 driver。

配置恢复结束、driver bind 之前的安全状态是：

```text
Command.BME = 0
Command.MSE = 1
Command.I/O = 0
Command.INTxDisable = 1
```

若原来有 driver，driver probe 可以随后配置 BME、MSI/MSI-X 和其他 operational state。框架不伪造 driver-owned 状态。

## 8. 测试与剩余验证

portable fake-sysfs 单测覆盖：

- snapshot JSON round-trip 和 exclusive-create。
- 256B 与 4KB full config capture。
- 64-bit BAR high-then-low restore。
- MPS/MRRS restore。
- 最终安全 Command。
- resource 变化时在任何 config write 前拒绝。
- malformed snapshot 拒绝。
- CLI 必填参数和 timeout 校验。

仍需在 Linux 真实硬件上验证：

1. SPEV driver unbind 后实际的 MSI/MSI-X、BME、ROM 和 PM 状态。
2. PMU chip reset 期间原 sysfs inode/path/resource 和 bridge window 是否保持不变。
3. config identity 恢复所需的典型/最坏时间。
4. BAR/MPS/MRRS restore 后 driver probe 是否稳定成功。
5. driver bind 后 `DeviceManager::discover()` 的 BAR mmap、system-ready 和 target tree 是否全部恢复。

如果真实硬件会导致 Linux remove/recreate device，本 MVP 会按设计拒绝。那将是独立的第二阶段需求，需要新的 identity matching、PCI rescan 和 Host 资源策略，不能通过放宽当前 exact-check 直接写回旧 BAR。

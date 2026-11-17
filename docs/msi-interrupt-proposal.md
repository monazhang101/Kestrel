# SPEV Framework Kernel Interrupt Capability Proposal

日期：2026-10-13

## 1. 需求重新定义

当前需求应理解为：

> `spev_drv.ko` 提供通用的 PCI MSI/MSI-X 中断接收和用户态事件传递能力，供未来
> PMU、DMA 和其他设备事件使用。

这次不需要实现 PMU testcase、PMU 业务逻辑或具体告警策略，也不应把需求限定为
“把 DMA polling 改成 IRQ”。DMA 可以作为第一个真实 interrupt source，用于证明
kernel interrupt 通道可用。

建议把工作分成以下能力层级：

| 层级 | 能力 | 验收证据 |
| --- | --- | --- |
| L0 | 识别设备支持的 MSI/MSI-X capability | `lspci -vv` 和硬件规格 |
| L1 | Linux 为设备分配 vector，kmod handler 能收到 IRQ | `/proc/interrupts` 和 driver counter |
| L2 | kmod 把事件可靠传给用户态 | blocking read/poll 收到带 sequence 的事件 |
| L3 | 能识别或携带 device source/cause | event 中有 vector 和 raw cause，ack 后无风暴 |
| L4 | 一个真实 source 完成端到端验证 | test interrupt、DMA completion 或 PMU test event |
| L5 | PMU 功能接入 | 后续 PMU HAL/testcase 工作，不属于本任务 |

本任务的推荐 Definition of Done 是 L0-L4。只完成 `request_irq()` 属于 L1，不能称为
框架已经支持中断。

## 2. DMA 能否作为最小验证场景

可以，但需要满足条件。

优先级建议：

1. **硬件 software/test interrupt**：最好。它只验证 interrupt path，不引入 DMA、HQC、
   descriptor 和数据传输问题。
2. **DMA completion interrupt**：可以接受。仓库已有可工作的 DMA submit 和 payload
   验证路径，适合作为真实 source。
3. **PMU test event**：只有硬件团队已经提供安全、可重复的 PMU interrupt trigger 时使用；
   不需要把它包装成 PMU testcase。

DMA 验证成立的前提是：DMA completion 和未来 PMU 事件经过同一个顶层 interrupt
controller/aggregator、相同的 PCI MSI/MSI-X delivery 机制和同一套 mask/cause/ack
规则。如果 DMA 有独立 vector 或绕过 PMU 使用的 interrupt controller，它只能证明
DMA source 可用，不能证明 PMU 路径已经准备好。

因此建议用一个独立的低层 `irq_smoke` 工具或调试命令验证，不需要在本任务中改变
`pcie_dma_data_transfer` 的默认 completion 模式。验证工具的职责是触发一个已知 source、
等待 kmod event、读取 cause/status，并报告结果。

## 3. 先弄清楚“dispatch”指什么

中断 dispatch 有三层，必须确认老板要求到哪一层：

```text
PCI vector dispatch
  Linux 根据 MSI/MSI-X message 把 IRQ 送到 spev_diag handler

device source dispatch
  handler 读取 top-level cause/status，区分 PMU、DMA、error 等 source

framework dispatch
  用户态 HAL 根据 event source/cause 调用对应 subscriber/callback
```

单 vector MSI 只天然解决第一层。多个 PMU/DMA source 共用一个 vector 时，第二层必须
依赖设备 interrupt controller 的 cause/status 寄存器。MSI-X 可以让不同 source/group
映射到不同 vector，但仍需要明确 routing、mask 和 acknowledge 语义。

本任务建议交付前两层，并向用户态传递结构化事件。通用 subscriber/callback 框架可以
等第一个真实 PMU consumer 出现后再增加，避免提前设计错误的 PMU API。

## 4. 开始编码前需要了解什么

### 4.1 向老板或需求方确认

建议直接确认以下问题：

1. “支持 MSI”是指严格支持 PCI MSI，还是把 MSI/MSI-X 统称为消息中断？
2. 目标平台是 FPGA、EMU、silicon，还是都要支持？第一验收平台是哪一个？
3. 交付边界是 kmod 收到 IRQ，还是还要用户态 HAL 能 read/poll 并 dispatch？
4. 第一版需要支持哪些 source：任意一个 smoke source，还是必须包含 PMU source？
5. 需要一个 vector，还是 PMU/DMA/error 要分配不同 vector？
6. 是否要求多个进程同时订阅、是否允许事件合并、事件是否可能丢失？
7. reset、SBR、driver unbind/rebind 后是否要求自动恢复 interrupt routing？

如果对方只能给一句话，建议用本文的 L0-L4 作为默认边界，并让对方确认。

### 4.2 向 PMU、RTL 或硬件团队索取

需要的不是 PMU testcase 设计，而是 interrupt contract：

1. PMU event 到 PCIe endpoint 的完整 routing 图。
2. Endpoint 支持 MSI、MSI-X 或两者；vector 数量和推荐映射。
3. 顶层 interrupt controller/aggregator 的 BAR、base、register offset 和 bit 定义。
4. source enable、mask、pending/cause、clear/ack 寄存器。
5. cause 是 edge、level-latched、read-clear、W1C 还是 auto-clear。
6. 正确顺序：clear-before-unmask、read-before-ack、ack-before-unmask 等。
7. 是否需要 MMIO readback、memory barrier 或 posted-write flush。
8. 是否有 software/test interrupt 寄存器。
9. reset、FLR、SBR、D3/D0 后哪些寄存器会丢失。
10. FPGA/EMU 是否实现 MSI/MSI-X TLP；是否存在已知限制。

当前仓库的 CSR/PHAL 文件没有发现 interrupt controller 的寄存器定义。没有上述资料时，
只能实现 Linux host 侧 vector plumbing，无法正确 mask、dispatch 和 acknowledge device
source。

### 4.3 向平台或现有驱动团队索取

如果 Proton 或其他正式驱动已经支持中断，优先参考其行为：

- 使用 MSI 还是 MSI-X，申请几个 vector。
- vector 与 PMU/DMA/error source 的映射。
- ISR 中读取和清除哪些寄存器。
- interrupt storm、reset 和 remove 的处理方式。
- 用户态使用 eventfd、read/poll、netlink 还是专有 ioctl。

不要直接复制代码，但应复用已经验证过的硬件顺序和寄存器语义。

## 5. 需要从 tester 收集的只读信息

在任何寄存器写入或 driver 修改之前，收集：

```sh
uname -a
lspci -nn -s <BDF>
lspci -nnvv -s <BDF>
readlink /sys/bus/pci/devices/<BDF>/driver
cat /proc/interrupts
grep CONFIG_PCI_MSI /boot/config-$(uname -r)
dmesg | tail -n 200
```

重点检查：

- `Capabilities` 中是 `MSI`、`MSI-X` 还是两者都有。
- MSI-X table/PBA 位于哪个 BAR、offset 和 table size。
- 当前是 `Enable+` 还是 `Enable-`。
- 当前绑定的 driver 和分配的 IRQ。
- 目标 kernel 是否启用 `CONFIG_PCI_MSI`。

仓库的 `HalContext` 把 BAR0 前 2 MiB 描述为 `MSI-X`，但这段描述不能替代真实 PCI
capability 和硬件规格。

## 6. 推荐 MVP 架构

```text
PMU / DMA / error / test interrupt source
              |
              v
device interrupt controller: route + mask + cause + ack
              |
              v
PCI MSI or MSI-X vector
              |
              v
spev_drv.ko ISR
  - read/ack raw cause when required
  - increment per-device sequence/counters
  - wake wait queue
              |
              v
/dev/spev_drvN
  - GET_IRQ_INFO ioctl
  - blocking/nonblocking read
  - poll/epoll
              |
              v
HalContext IRQ channel / irq_smoke tool
```

### 6.1 Kernel 边界

Kernel module 负责：

- PCI vector allocation/free。
- 最小的 device interrupt controller 初始化、mask、cause 和 acknowledge。
- IRQ handler、计数、wait queue 和用户态事件传递。
- probe/remove、open fd、reset/unbind 的安全生命周期。

Kernel module 不负责：

- PMU 功能语义和告警策略。
- DMA descriptor、HQC queue、payload 验证。
- Python testcase 调度。
- 将一次 IRQ 直接判定为某个 operation 成功。

### 6.2 MSI/MSI-X 模式

在需求未确认前，内部设计同时容纳两种模式，但运行时必须报告实际模式：

```c
enum spev_diag_irq_mode {
    SPEV_DIAG_IRQ_NONE = 0,
    SPEV_DIAG_IRQ_MSI = 1,
    SPEV_DIAG_IRQ_MSIX = 2,
};
```

- 如果老板要求“严格 MSI”，使用 `PCI_IRQ_MSI`，不回退到 MSI-X 或 INTx。
- 如果需求是“消息中断均可”，可以用 `PCI_IRQ_MSI | PCI_IRQ_MSIX`，并通过 UAPI
  返回 Linux 最终选择的 mode。
- MVP 不回退到 INTx，否则验收不能证明消息中断能力。

### 6.3 UAPI

保留现有 DMA ioctl 的编号和 struct 大小，新增独立版本化接口：

```c
struct spev_diag_irq_info {
    __u32 size;
    __u32 version;
    __u64 features;
    __u32 mode;
    __u32 vector_count;
    __u64 total_count;
    __u64 supported_source_mask;
    __u64 reserved[5];
};

struct spev_diag_irq_event {
    __u32 size;
    __u32 version;
    __u64 sequence;
    __u64 events_since_last_read;
    __u64 timestamp_ns;
    __u32 vector_index;
    __u32 flags;
    __u64 raw_cause;
    __u64 reserved[4];
};
```

事件语义：

- 每个 open fd 有自己的 `seen_sequence`，不同 fd 不互相偷走事件。
- blocking `read()` 等待新 sequence；`O_NONBLOCK` 无事件时返回 `-EAGAIN`。
- `.poll` 返回 `EPOLLIN | EPOLLRDNORM`。
- 多次 IRQ 可以合并，event 报告累计差值；如果 PMU 要求每个 event 都不可丢，后续必须
  增加 kernel ring buffer 或读取硬件 event FIFO，不能只靠 IRQ count。
- IRQ 是通知，`raw_cause` 才用于 source dispatch；最终状态仍由相应硬件状态确认。

### 6.4 BAR ownership

若 cause/ack 寄存器在 BAR0，kmod 只应访问明确的 interrupt-control offset。现有 HAL
从 sysfs mmap BAR0；kmod 若 claim 整个 BAR0，可能让现有寄存器、PHAL 和 HQC 路径失败。

M0 必须确认目标 kernel 上两种访问能否共存。若不能，需要在以下方案中选择：

- kmod 只映射并拥有 interrupt-control 区域，用户态禁止访问这些 offset。
- 由 kmod 统一拥有 BAR，并为用户态提供受控 MMIO API。
- 使用已有正式 driver/VFIO 的 interrupt 和 BAR 机制。

后两项会明显扩大本任务范围。

### 6.5 生命周期

当前 `spev_file` 持有 `spev_device *`，而 device 使用 devm 分配。driver unbind 时仍有打开
fd 可能造成悬空引用。增加 blocking IRQ wait 后，这个风险更明显，MVP 应一起处理：

1. misc deregister，阻止新 open。
2. 设置 removing，mask device source。
3. 唤醒所有 waiter，使 read/poll 返回 `-ENODEV`/`EPOLLHUP`。
4. `synchronize_irq()`、`free_irq()`、`pci_free_irq_vectors()`。
5. 使用引用计数让 device object 存活到最后一个 fd close。

## 7. 最小验证方案

建议按以下顺序，不先修改 PMU testcase，也不先替换 DMA polling：

### Phase A：Host capability

- driver probe 分配一个 MSI 或 MSI-X vector。
- `/proc/interrupts` 出现该 device/vector。
- source 默认 masked 时 IRQ count 稳定，无中断风暴。

### Phase B：Event transport

- 使用明确的 test source 触发一次中断。
- handler count 增加。
- `irq_smoke` 通过 read/poll 收到 event，sequence、vector、raw cause 正确。
- clear/ack 后 count 不再增长。

### Phase C：Real source

- 优先使用 software/test interrupt。
- 没有 test interrupt 时，使用一次已知 DMA completion。
- 收到 event 后仍检查 DMA sync flag 和 payload，不能仅凭 IRQ 判成功。
- 重复至少 10,000 次，检查丢失、重复、storm 和 timeout。

### Phase D：Robustness

- 没有中断时 read/poll timeout 正确。
- signal interruption、process close、driver unbind 不死锁。
- reset/SBR 后按 contract 恢复，或明确要求 rediscover/rebind。
- 原有 DMA alloc/mmap/free 和 polling 路径不回归。

## 8. 重新评估 Effort

以下估算不包含 PMU testcase 或 PMU 功能实现。

### 方案 A：Host IRQ plumbing demo

范围：分配 vector、handler 计数、`/proc/interrupts` 可见，最多加简单 read/poll。

- **5-8 工程师日**。
- 只能证明 Linux host 部分，不能证明 source dispatch、ack 或 PMU readiness。

### 方案 B：推荐的 framework interrupt MVP

范围：L0-L4，通用 UAPI、raw cause、一个真实 source、生命周期和压力验证。

| 工作项 | Effort（工程师日） |
| --- | ---: |
| 需求、MSI/MSI-X、interrupt contract 和 tester 信息确认 | 2-4 |
| kmod vector/ISR、mask/cause/ack、probe/remove 回滚 | 3-5 |
| UAPI read/poll、per-file cursor、device/file 生命周期 | 3-5 |
| 用户态 IRQ channel 或 `irq_smoke` | 1-3 |
| 一个 test/DMA source 的端到端验证 | 2-4 |
| 压力、timeout、reset/unbind、文档 | 2-4 |
| **合计** | **13-25** |

硬件资料完整并且存在 software test interrupt 时，预期接近 13-17 天；资料缺失或只能用
DMA/PMU 真正路径调试时，接近 20-25 天。一名工程师约 3-5 周，当前估算置信度较低。

### 方案 C：Production PMU-ready event framework

如果还要求多 vector、多个 source subscriber、每个事件不可丢、跨进程订阅、reset 后自动
恢复和 PMU event queue，需在方案 B 上追加 **8-15 工程师日**。这需要先有明确 PMU
consumer 和事件可靠性要求。

## 9. 建议从哪里开始

第一步不是改代码，而是用半天到两天完成一页 interrupt contract：

```text
目标平台/BDF：
严格 MSI 还是 MSI/MSI-X 均可：
vector 数量：
第一验证 source：
source -> interrupt controller -> PCIe routing：
mask/cause/ack 寄存器和语义：
test interrupt 方法：
用户态交付：read/poll/eventfd：
reset/unbind 要求：
Definition of Done：L0-L4 是否认可：
```

拿到 `lspci -vv`、硬件寄存器定义和已有 driver 参考后，再做一个隔离的 M1 spike：只实现
单 vector、counter、read/poll 和 test interrupt。M1 通过后再决定是否用 DMA 做 L4，以及
未来 PMU consumer 需要 kernel raw cause、HAL dispatch 还是更强的 event queue。

向老板同步时可以直接这样描述：

> 我理解这项需求是给 framework kernel module 增加通用 MSI/MSI-X interrupt provider，
> 不包含 PMU testcase。建议 MVP 做到 kernel 能接收 vector、读取并 ack source cause、通过
> read/poll 传给用户态，并用一个 software test interrupt 或 DMA completion 做端到端验证。
> 需要先确认实际是 MSI 还是 MSI-X，以及 interrupt controller 的 routing/mask/cause/ack
> 定义；否则只能完成 host 侧注册，不能证明 PMU 后续可用。

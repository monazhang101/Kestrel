# kmod MSI-X Proposal Review

日期：2026-10-14

## 1. Review 结论

总体技术方向可行，但当前版本还不是可以直接编码的设计。

正确的核心判断包括：

- MSI-X table/PBA 由 Linux PCI/MSI-X core 管理，driver 不手写 table entry。
- 第一阶段只申请一个 vector，暂不展开 24 vector。
- 中断源和 MSI-X 传输是两层：source 负责“为什么触发”，MSI-X 负责“怎么通知 host”。
- ISR 只读/清 cause、记账和唤醒，不放 PMU/DMA 业务逻辑。
- 先用一个可控 source 做端到端验证，再扩展 source/vector。

需要修正的关键问题如下。

### P0：SELFTEST 和 ISR 条件互相矛盾

方案中的 SELFTEST 描述为绕过 PMU 中断源，直接写 `MSI_MESSAGE_* + REQ` 发送 MSI-X。
但 ISR 又先读取 `PMU_INTR_STS`，为 0 就返回 `IRQ_NONE`。

如果 SELFTEST 没有设置 PMU status，MSI-X 即使成功到达，ISR 也会把它当成“不是我的
中断”，不会增加 pending。因此截图中的 P0 验收无法按当前伪代码通过。

修正建议：

- P0 handler 以专用 MSI-X vector 到达为依据记账；SELFTEST cause 单独标记或允许
  `raw_cause=0`。
- P1 接入真实 PMU source 后，再读取顶层 cause/PMU status 并验证 ack。
- 专用、非共享 MSI-X vector 不应仅因 PMU status 为 0 就返回 `IRQ_NONE`；可计为
  spurious/test event 并返回 `IRQ_HANDLED`。legacy shared IRQ 才需要严格判定归属。

### P0：“不被 TBD 阻塞”的结论不成立

软件触发至少依赖以下已确认信息：

- endpoint 实际 MSI-X capability 和可用 vector。
- `MSI_MESSAGE_*`、`REQ` 的 host BAR offset。
- REQ 如何选择 MSI-X table entry/vector。
- 该触发是否使用 Linux 已写入的 table address/data。
- trigger/clear 的顺序以及是否有副作用。

这些信息也是硬件 contract。缺失时可以完成 `pci_alloc_irq_vectors()`，但无法证明设备
真的向 host 发送了一次 MSI-X。P0 可以绕开 PMU source routing，不能绕开 selftest
register contract。

### P0：`pci_request_regions()` 可能破坏当前 HAL BAR mmap

当前 `HalContext` 直接打开并 mmap：

```text
/sys/bus/pci/devices/<BDF>/resource0
/sys/bus/pci/devices/<BDF>/resource2
/sys/bus/pci/devices/<BDF>/resource4
```

方案在 probe 中调用 `pci_request_regions()`，会申请该 PCI function 的全部 BAR 资源。
这与“只 iomap source CSR，不碰 BAR4 table/PBA”的表述不是一回事。在部分 kernel 和
resource exclusivity 配置下，用户态现有 sysfs resource mmap 可能失败。

修正建议：

- P0 若只做 vector registration，避免无必要地 claim/map 全部 BAR。
- P1 明确 interrupt-control CSR 位于哪个 BAR/offset，只让 kernel 拥有该范围。
- 在目标 kernel 上把“加载 kmod 后现有 BAR0/2/4 mmap 和 HQC case 仍正常”列为验收项。
- 如果平台要求 kmod 独占整个 BAR，必须同步重构 HAL BAR access，不能称为 MSI-X 小改动。

### P0/P1：remove 和 open-fd 生命周期不能放到最后

当前 `spev_device` 由 devm 分配，而每个打开的 `spev_file` 保存裸指针。driver unbind 后
仍打开 fd，已经存在悬空引用风险。加入 blocking `IRQ_WAIT` 后，还会出现 waiter 不退出、
remove 后访问已释放 wait queue 等问题。

这必须进入第一次可交付版本：

1. 阻止新 open，设置 `removing`。
2. mask source 并唤醒所有 waiter。
3. `IRQ_WAIT` 返回 `-ENODEV`，poll 返回 HUP/ERR。
4. `synchronize_irq()`、`free_irq()`、`pci_free_irq_vectors()`。
5. device object 通过引用计数存活到最后一个 fd close。

### P1：全局 `atomic_xchg(pending)` 会让多个 fd 互相抢事件

一个 waiter 执行 `atomic_xchg(..., 0)` 后，其他 fd 看不到该事件；copy-to-user 失败时事件
也已经被消费。`irq_status_shadow` 还会覆盖前一次 status，无法把多个 pending event 和
各自 cause 对应起来。

第一版应使用：

- per-device `atomic64_t irq_sequence`。
- 每个 open fd 保存自己的 `seen_sequence`。
- read/wait 返回当前 sequence 和 `current - seen`。

这样多个 fd 可各自观察事件。如果未来 PMU 要求每条 event 都不可丢，需要硬件 FIFO 或
kernel event ring，单一 counter/shadow 不够。

### P1：W1C 和 PMU mask 语义仍是假设

ISR 中 `iowrite32(sts, PMU_INTR_STS)` 假定 status 是 W1C。`IRQ_CTRL` 又假定 32-bit mask
可以直接控制全部 PMU source。这些必须由 CSR spec 或正式 driver 验证。

错误的 clear 顺序可能导致：

- 事件刚到就被清掉。
- level source 未解除，持续产生 interrupt storm。
- 清掉另一个 consumer 尚未处理的 pending bit。

kernel 应拥有 source mask/ack 顺序。普通用户态不应通过未经校验的 raw mask ioctl 任意
改中断控制器。

### P1：当前仓库没有截图中所称的 PMU perf testcase

当前 `PMUModule` 只注册 `example`，内容是 HQC SRAM/DMEM read/write，没有 perf 操作，
也不会触发 PMU interrupt。因此 P1 不能直接“复用现有 PMU testcase”。

这不要求新增 PMU 功能 case，但必须从硬件团队获得一个安全、可重复的真实 source：

- software/test interrupt；或
- 明确可产生 PMU interrupt 的低层操作；或
- 能配置为产生 MSI-X 的 DMA completion。

### P1：不能静默 fallback 到 legacy IRQ

如果需求是 MSI-X，fallback 到 `PCI_IRQ_LEGACY` 后即使测试通过，也不能证明 MSI-X。
兼容旧 kernel/EMU 可以做成显式 module parameter，并在 UAPI/dmesg 返回实际 mode；正式
MSI-X 验收必须禁止 fallback。

### P1：UAPI 需要版本和边界检查

新增 UAPI struct 应包含 `size/version/flags/reserved`。还要明确：

- Phase 1 只允许 `vector=0`，其他值返回 `-EINVAL`。
- timeout、signal、remove、nonblocking 的返回值。
- 实际 mode、vector count、supported source mask 的查询接口。
- 32/64-bit compat layout。
- SELFTEST 若保留，只作为受权限控制的 debug/test 能力，避免永久暴露任意 MSI trigger。

### P2：多 vector 需要 per-vector context

24 vector 扩展不能只把 `request_irq()` 放进循环。每个 handler 需要稳定的 context，至少
包含 device、vector index、sequence/counter；不能假定 Linux IRQ number 连续或等于
MSI-X table index。

## 2. 大白话解释

### 2.1 中断和 polling

把 CPU 想成办公室，把芯片想成楼下设备。

- **Polling**：办公室每隔 2 微秒打电话问一次“事情做完了吗？”当前 DMA completion
  基本是这种方式。
- **Interrupt**：设备完成后主动按门铃，办公室平时可以做别的事。

中断只是“有事了”的通知。收到门铃不代表 DMA 一定成功，也不说明是哪一种 PMU 告警；
软件还要读取状态寄存器确认。

### 2.2 Interrupt source

Source 是“谁按了门铃”，例如：

- PMU 温度/电压/错误事件。
- DMA completion。
- PCIe link error。
- ATU illegal access。
- software test interrupt。

一个 source 通常有三类开关或记录：

- `enable/mask`：允许它按门铃，或把它静音。
- `status/cause`：记录是谁触发。
- `clear/ack`：告诉硬件“我处理完了”。

如果忘记 ack，设备可能一直按门铃，形成 interrupt storm。

### 2.3 MSI-X 是什么

老式 INTx 像一根共享电线。MSI-X 不需要单独的物理中断线；设备执行一次特殊的 PCIe
Memory Write，向 host 写入一组 address+data，host 把这次写识别为中断。

所以截图里说的“发 MSI-X”不是把大块业务数据 DMA 到内存，而是发送一条很小的门铃
消息。

### 2.4 MSI-X table 和 PBA

- **MSI-X table**：设备里的“电话簿”。每个 entry 保存 host 给这个 vector 分配的
  message address、data 和 mask。
- **PBA**：Pending Bit Array。某个 vector 被 mask 时又来了事件，用一个 pending bit
  记录。

Linux 在 `pci_alloc_irq_vectors()` 过程中管理这些内容。driver 应通过 PCI API 申请
vector，不要自己往 table 填 address/data。

### 2.5 Vector 是什么

Vector 可以理解为门铃编号或电话分机号。

假设设备支持 24 个 vector：

```text
vector 0 -> PMU/error 汇总
vector 1 -> DMA queue 0
vector 2 -> DMA queue 1
...
```

这只是可能的路由方式，真实映射必须由硬件定义。支持 24 个不代表第一版必须申请 24 个。
一个 vector 也能承载多个 source，只是 ISR 收到后必须再读 cause 寄存器，确认到底是谁。

需要区分三个数字：

- **source ID/bit**：设备内部哪个原因触发。
- **MSI-X vector index**：使用 table 的第几个 entry。
- **Linux IRQ number**：Linux 为它分配的内部编号。

它们不是同一个数字，也不能假定一一相等。

### 2.6 Routing 和 dispatch

Routing 是硬件里的送信路线：

```text
PMU source -> interrupt hub/ring -> vector 0 -> host
```

`DEST_HOST` 表示送到 host；送到 MCU/HQC 的事件不会自然到 Linux。

Dispatch 是逐层分发：

1. PCI/MSI-X core 根据 message 找到 Linux IRQ。
2. Linux 调用 `spev_diag` 的 ISR。
3. ISR 读取 cause，知道是 PMU、DMA 还是 error。
4. ISR 唤醒等待的用户进程。
5. HAL 根据 cause 交给未来的 PMU/DMA consumer。

“申请到 vector”只完成第 1 步的配置，不能证明后面都完成。

### 2.7 ISR 和 wait queue

- **ISR**：kernel 里的门卫。门铃响时被立即调用，必须很快完成，只读/清状态、计数、
  唤醒等待者。
- **wait queue**：用户进程没有事件时睡在这里。ISR 调用 wake 后，`IRQ_WAIT` 或 poll
  返回。

ISR 中不适合执行 PMU testcase、DMA payload compare、长时间循环或大量日志。

### 2.8 为什么还要读 cause

Vector 是“哪个门铃响了”，cause 是“谁按的”。多个 source 共用 vector 0 时，仅知道
vector 0 不足以 dispatch。未来 PMU 功能是否可用，关键在于是否能正确读取、清除并上报
PMU cause。

## 3. 修订后的可行方案

### Phase 0：确认事实

必须先拿到：

- tester 的 `lspci -nnvv -s <BDF>`。
- MSI-X table/PBA 的实际 BIR、offset、table size。
- SELFTEST register 的 host BAR offset 和 trigger/vector/clear 语义。
- 顶层 source routing、mask/cause/ack 定义。
- 正式 Proton driver 的中断初始化/ISR 顺序（如已有）。

### Phase 1：单 vector host transport

- 申请一个严格 MSI-X vector，不做 legacy fallback。
- 注册专用 ISR，记录 `atomic64` sequence。
- 提供版本化 GET_INFO + read/poll 或 WAIT UAPI。
- SELFTEST 到达时允许 `raw_cause=0/test`，不依赖 PMU status。
- 同时完成 removing/waiter/fd 生命周期。

验收：真实 MSI-X TLP 到达、用户态收到 sequence、无 storm、unbind 不挂。

### Phase 2：一个真实 source

- 接入已确认的 top-level cause/mask/ack。
- 选择一个 PMU test source；没有时选择经过同一 interrupt hub 的 DMA source。
- 用户态收到 vector + raw cause，并读取 source status 验证。

验收：source -> hub -> MSI-X -> ISR -> user 完整闭环。

### Phase 3：扩展和恢复

- 根据真实消费者再扩展多 vector/per-vector context。
- 增加 reset/SBR 后重建 routing。
- PMU 若要求事件不可丢，再增加 event FIFO/ring 和订阅模型。

## 4. 可行性和 Effort

### 可行性

- **架构方向：可行。** Linux PCI API、ISR、wait queue/read/poll 都是标准实现路径。
- **P0 伪代码：当前不可直接实现。** SELFTEST/ISR 矛盾、寄存器地址和语义未确认。
- **PMU-ready 程度：尚未证明。** 当前只有 PMU source 描述，没有可用 trigger 和正式
  source->vector contract。
- **总体判断：有条件可行。** 补齐硬件 contract 并修正 UAPI/lifecycle 后，可以落地。

### Effort

截图用“约 300 行 kmod + 100 行用户态”估算工作量不可靠。风险主要来自硬件语义、BAR
ownership、并发和 remove 生命周期，而不是代码行数。

| 范围 | 估算 |
| --- | ---: |
| 只完成 vector allocation + dmesg/counter 演示 | 3-5 工程师日 |
| 单 vector + verified SELFTEST + 版本化用户态 wait/read + safe remove | 7-12 工程师日 |
| 再接入一个真实 PMU/DMA source、cause/ack 和压力验证 | 累计 13-20 工程师日 |
| 多 vector、可靠事件队列、reset 自动恢复 | 累计 20-30 工程师日 |

前提是硬件资料和 tester 可用。若 SELFTEST 或 source routing 需要 RTL/固件修改，等待和
联调时间另计。

## 5. 推荐决策

保留原 proposal 的整体分层和单 vector MVP，修改以下内容后再进入实现：

1. 把寄存器 contract 收集设为 Phase 0，不宣称 SELFTEST 不依赖 TBD。
2. 分开 SELFTEST arrival 和 PMU cause 判断。
3. 去掉正式验收中的 legacy fallback。
4. 用 global sequence + per-file cursor 代替 global pending `atomic_xchg`。
5. 将 fd/remove 生命周期提前到第一版。
6. 先验证 `pci_request_regions` 对现有 BAR mmap 的影响，再确定 BAR ownership。
7. 删除“复用现有 PMU perf testcase”的假设，换成硬件团队提供的可控 source。

## 6. 两个实验是否足够开工

### 结论

**足够开始写 Phase 1 的 kmod host transport；不足以开始 PMU source 集成。**

两项实验应分别验证“Linux 看到并配置 MSI-X”和“设备真的通过配置好的 MSI-X entry
发出一次消息”。它们通过后，可以进入单 vector、ISR、wait UAPI 和 safe teardown 的实现。
source routing、PMU mask/cause/ack 和多 vector 仍是后续阶段。

### 实验一：PCI capability 和 vector registration

先做只读检查：

```sh
lspci -nnvv -s <BDF>
cat /sys/bus/pci/devices/<BDF>/resource
readlink /sys/bus/pci/devices/<BDF>/driver
```

然后实现一个最小 kmod spike，只做：

```text
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX)
pci_irq_vector(pdev, 0)
request_irq(...)
```

不打开 PMU source，不写 mask/cause，不先 claim 全部 BAR。加载后再次检查：

```sh
lspci -vv -s <BDF>       # MSI-X: Enable+，Count 和 mode 正确
cat /proc/interrupts     # 记录 Linux IRQ number
dmesg | tail -n 100
```

同时运行一次现有的 BAR/discovery smoke，确认加载 kmod 后 `resource0/2/4` mmap 没有
被破坏。这个结果决定后续能否继续保持当前 HAL 的 sysfs BAR ownership。

实验一通过标准：MSI-X capability 可读，vector=1 分配成功，handler 注册成功，实际
mode 没有静默退化为 INTx，existing BAR path 不回归。它证明 host 配置，不证明设备发送
了中断。

### 实验二：SELFTEST 到用户态的单次事件

使用已经从 CSR 确认的 `MSI_MESSAGE_*`/`REQ` 地址和语义，先只选择 vector 0。不要把
“直接写寄存器”做成无权限的永久 public ioctl；bring-up 可用受权限控制的临时 ioctl 或
kernel debug path。

步骤：

1. Linux 已通过 PCI API 配置 MSI-X table。
2. kmod 注册 vector 0 的专用 handler。
3. 触发 SELFTEST 写入一次 MSI-X message。
4. handler 无条件记录 vector arrival；不要求 `PMU_INTR_STS` 非零。
5. 用户态 `IRQ_WAIT`/read/poll 返回一次 event。
6. 读取 `REQ`，确认按定义 self-clear；观察 `/proc/interrupts` 只增加一次。
7. 重复触发，确认每次一条事件；卸载 driver 时 wait 能被唤醒并退出。

实验二通过标准：ISR 计数增加、用户态收到 event、无 PMU status 也能通过、REQ 不持续
触发、remove 不挂。它证明 device-to-host MSI-X transport，不证明 PMU routing。

`VEN_MSI_VECTOR` 的 1/5 等其他值属于多 vector/routing 验证。单 vector MVP 先固定为
0；不要为了验证这些值而提前申请和实现 24 个 vector。

### 两个实验通过后可以做什么

可以开始：

- kmod 的 vector/ISR/wait queue 结构。
- 版本化 `GET_IRQ_INFO` 和单 vector wait UAPI。
- per-device sequence、per-file cursor 或明确的单消费者 MVP 约束。
- probe/remove 错误回滚和 open-fd 生命周期。
- 一个独立的 `irq_smoke` 用户态验证工具。

暂时不要冻结：

- PMU/DMA/error 的 source ID 定义。
- `IRQ_CTRL` 的通用 raw mask API。
- 多 vector 的 vector-to-source 映射。
- PMU 事件不可丢时所需的 ring/FIFO 语义。

### 失败时如何定位

| 现象 | 优先怀疑 |
| --- | --- |
| `Enable+` 但 ISR 没进 | SELFTEST register/REQ 语义、table entry、BAR mapping、设备未真正发 TLP |
| ISR 进了但 wait 不返回 | wait queue、pending/sequence、copy-to-user 或 fd 绑定 |
| 一次触发变成持续中断 | REQ 未清、source 未 mask、ack 地址/语义错误 |
| 只有错误 vector 进来 | `VEN_MSI_VECTOR` 是 table index 还是内部编号的理解错误 |
| 加载 kmod 后 BAR case 失败 | `pci_request_regions` 与现有 sysfs BAR mmap 冲突 |

### 新的开工判断

如果已有 CSR 地址只是静态查表结果，建议先完成实验一，再把实验二作为第一个小型
bring-up spike。实验一通过即可开始写 host-side 代码；实验二通过后才可以说 MSI-X
transport 已经被硬件验证。PMU source 的 Phase 2 仍需硬件/架构团队提供 routing 和
mask/cause/ack contract。

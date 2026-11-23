# DMA Copy

本文以当前 `main` 分支代码和 [PCIE_DMA_HAS](PCIE_DMA_HAS.md) 为准，描述两条不同的
HQC 数据搬移路径：direct-DD DMA descriptor 和 CP queue copy。旧的 HQC FW
admin-command/SQ/CQ 描述已经删除，不适用于当前实现。

## 1. Direct-DD DMA

### 1.1 调用入口

```text
PCIeModule::dma_data_transfer()
  -> AtlasPCIeImpl::dma_copy()
  -> DmaDescriptor::fill_descriptor()
  -> DmaDescriptor::submit()
  -> DmaDescriptor::wait()
  -> DMEM/host payload readback and compare
```

当前 DMA descriptor API 有三种使用模式。所有模式都必须先通过同一个
`HqcDmaEngine` 创建 `DmaDescriptor`，descriptor 的 source、destination、profile
和 size 由调用者独立配置。

#### 同步 one-shot

适合一个 testcase 只搬运一个 descriptor，并且希望 API 返回时搬运已经完成：

```cpp
auto& engine = ti.hal->dma_engine();
DmaDescriptor desc(engine, DmaProfile::H2D);
desc.set_src(host, host_offset);
desc.set_dst(device, device_offset);
desc.set_size(size_bytes);
desc.fill_descriptor();
desc.submit();
desc.wait(timeout_ms);
```

`submit()` 等待 descriptor controller 有空闲 core，然后完成一次 trigger；它不
等待 payload 完成。`wait()` 才负责等待对应 sync flag。`execute(timeout_ms)` 是
上述 `fill_descriptor -> submit -> wait` 的同步快捷路径。

#### 单线程异步 pump

适合一个线程维护多个在途 descriptor。`try_submit()` 只检查一次 controller
idle 状态：返回 `PHAL_STATUS_OK` 且 `triggered == false` 表示当前不能提交，调用者
稍后重试；提交成功后用 `poll_completion()` 收割完成的 descriptor，并复用已经
完成的对象：

```text
fill_descriptor() for N jobs
  -> try_submit() for each Prepared job
  -> poll_completion() for every active job
  -> refill the released slot and submit the next job
```

`try_submit()` 不等待 controller，因此适合 non-blocking pump；它不是硬件 atomic
batch。当前 `submit(vector)` 接口已删除，调用者需要逐个 descriptor 调用
`submit()` 或 `try_submit()`。

#### 多线程 composite testcase

一个 testcase 内的多个 worker 可以共享同一个 `HalContext::dma_engine()`：

```text
worker H2D: DmaDescriptor(engine, H2D) -> fill -> submit/try_submit -> wait/poll
worker D2H: DmaDescriptor(engine, D2H) -> fill -> submit/try_submit -> wait/poll
worker D2I: DmaDescriptor(engine, D2I) -> fill -> submit/try_submit -> wait/poll
```

每个 `DmaDescriptor` 只能由创建它的 worker 使用；engine 内部保护 slot/sync
flag bitmap、controller 选择、寄存器编程和 trigger。不同 profile 可以并发提交，
完成也按 descriptor 独立归属。当前 `pcie_dma_threaded_stress` 示例使用 H2D、
D2H 和 DMEM MMIO 三个 worker；它验证的是共享 engine 和线程安全，不是一个
硬件原子 batch。

当前并发 ownership 只在一个 `HqcDmaEngine` 内成立。slot bitmap 是 engine
成员，而整个 SRAM1 pool 通过 `HalContext` 固定范围 allocation 保护：同一
`HalContext` 的第二个 engine 会因为 pool 重叠而分配失败；不同 process/context
没有共享 bitmap 或跨进程锁，不能安全地同时驱动同一设备。正式 contract 是
一个 testcase/session 对一个 engine，不是多 engine 或多进程并发。

### 1.2 Descriptor pool and slot ownership

- descriptor 大小为 80 bytes；slot stride 为 512 bytes。
- diag 预留 HQC SRAM1 的 `[0, 0x20000)` 作为 descriptor pool。
- slot `n` 的 SRAM offset 是 `n * 0x200`，fetch address 是
  `0x30000000 + 1 * 0x80000 + n * 0x200`。
- slot `0` 保留不用；可用 slot/sync flag 是 `1..255`，共 255 个。
- `slot_id == sync_flag_id`，bitmap 同时表达 descriptor 和 completion flag
  的占用状态。
- `fill_descriptor()` 完成 descriptor 编码、分配 slot、清理旧 done 位并写入
  SRAM；状态变为 `Prepared`。
- `submit()` 只接受 `Prepared` descriptor；提交成功后状态变为 `Submitted`。
- `try_submit()` 在 controller 忙时返回 `OK + triggered=false`，descriptor 保持
  `Prepared`，不会被错误地标记为已提交。
- completion 被观察并清零后，状态变为 `Completed`，slot 可复用。
- timeout 或 trigger 后的错误不会释放在途 descriptor/buffer；当前策略是标记
  engine faulted 并保留资源，等待硬件 reset/quiesce 方案。

### 1.3 Descriptor encoding

`DmaDescriptor::finalize()` 检查 source/destination 归属、范围、地址和长度，
然后编码 80-byte image。当前正式约束为：

| Endpoint | Address | Length |
|---|---:|---:|
| Host/DMEM/VMEM | 32 B | 32 B |
| IMEM involved | 256 B | 256 B |
| SMEM | 4 B address | 32 B length |

allocator 的默认 512-byte placement 是资源策略，不替代 descriptor endpoint
alignment 检查。descriptor slot 的 512-byte stride 也不是 payload alignment。

当前 framework-owned 字段包括：

- bit 25: `1`
- bit 26: count-done sync mode，`1`
- bit 27: software round-robin thread selection
- bits 80..93: dynamically assigned sync flag
- bits 94..95: HQC sync-flag core

profile-specific opcode/stride/step 保留硬件 DD 定义；`D2V/V2D` 的上层 case
当前明确返回 `UNIMPLEMENTED`。

### 1.4 Submit sequence and guide comparison

当前 `submit()` 的实际顺序是：

1. 在 `PCIE_HQC_REG` 下轮询 core0/core1 的 `DESC_CTRL_IDLE`，优先使用
   round-robin core；首选忙时尝试另一 core。
2. 写 `TRIG=0`，写 fetch address low/high，写 descriptor size `80`。
3. 写 `TRIG=1`，即 descriptor-control doorbell/kick。
4. 返回 `Submitted`；不会在这里等待 DMA payload 完成。

这与 guide 的 descriptor 流程在核心顺序上匹配：先等 idle，再写 fetch address，
再 trigger。guide 还展示了 trigger 后再次等待 idle；当前异步实现把这个等待
推迟到下一次 submit 的“提交前 idle polling”，因此不会阻塞 batch 中已提交的
其他 descriptor。若硬件要求每次 trigger 后必须立即等待 idle，必须由硬件团队
确认后再修改，否则会破坏当前双 core 异步窗口模型。

当前没有额外的 descriptor SRAM readback，也没有 fetch-register readback 或
trigger 后的 debug IDLE read。这些曾用于 bring-up 实验，已不属于正式路径。
因此，代码目前只有 CPU `volatile` MMIO store 的程序顺序，没有显式证明最后一个
SRAM store 在 trigger posted write 之前已经对 descriptor controller 可见。guide
规定了先准备 descriptor、再 trigger 的逻辑顺序，但没有给出 host PCIe ordering
primitive；这仍是 FPGA/silicon 上必须验证的硬件可见性问题。

descriptor-controller 初始化并没有完全匹配 guide：

| Item | Guide example | Current code |
|---|---|---|
| DCF range start/end | four ranges, example `0x1000..0x1fff` | `0x880`, `0x400`, `0x80`, `0x1000`; start equals end |
| DCF credit enable/threshold | range controls plus threshold `0x8` | only range0 credit-enable bit; no threshold field |
| fetch timeout threshold | `0x3e8` on core0/core1 | `8,000,000` on core0/core1 |

截图可能是示例值，当前值也可能来自另一个 RTL revision，但仓库中的 compile-only
CSR header 缺少 guide 展示的完整 field 定义，无法证明当前初始化正确。这是
bring-up 前需要和硬件 owner 对齐的高优先级项；EMU payload PASS 只能证明当前
模型接受这组值，不能证明它符合目标 FPGA/silicon contract。

### 1.5 Completion and sync flag

`poll_completion()` 对一个 `Submitted` descriptor：

1. 读取 `0x400 + sync_flag_id * 4` 的 done register；
2. bit 0 为 0 时返回 `PHAL_STATUS_INVALID`，仅表示“尚未完成”，不是错误；
3. bit 0 为 1 时写 0 清除 done；
4. 清除成功后释放 slot，并返回 `PHAL_STATUS_OK`。

`wait()` 使用同一个 done register 轮询，直到 timeout；timeout 不证明 hardware
已经停止，因此不会释放 slot 或 DMA allocation。

guide 的 Appendix 5.10.7--5.10.9 使用了 `HQC_SEND_SMF_*`、`SMF_MEM`、
packed `SYNC_FLAG_DONE_31_0_BIT_STATUS_*` 和 `HQC_SYNC_FLAG_CLR` 等命名。
当前实现没有使用这些正式 field：`HqcDma.h` 自定义了
`SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(id) = 0x400 + id * 4`，然后读取/写入 bit 0。
仓库的 compile-only CSR header 虽然也提供同地址公式的 stand-in macro，但不能
证明目标 RTL 的真实 register layout。当前代码也没有显式执行 `SMF_RDY/VLD`
发送流程或进入独立 `SMF_MEM`/`SYNCFLAG` context。两者是否是同一个硬件
completion 机制必须在真实 CSR header/RTL 上确认：

- `0x400 + id * 4` 是否就是 guide 的 `SYNC_FLAG_DONE` entry；
- done status 是 per-flag word/bit0，还是 guide 展示的 32-bit packed bitmap；
- done 的清除语义是 status word 写 `0`，还是 `HQC_SYNC_FLAG_CLR` 写 `1`；
- `HQC_SEND_SMF_*` 是否只是 standalone software-send path，因而正常 DMA
  completion 只需 descriptor 中 bits 80..95（这是当前实现的预期）；
- sync flag id 的有效范围是否确实为 `1..255`，slot 0 是否由硬件保留。

因此，当前 async submit/poll 逻辑在软件状态机上闭环，但和 guide 的 SMF
寄存器流程只能判定为“部分匹配，completion CSR 需要硬件确认”。

## 2. CP queue copy

CP queue 不是 80-byte descriptor，也不使用 direct-DD sync flag。它直接配置
一条 MEM/COMP queue 的 queue image，通过 pointer 记录 payload 进度。

当前有效流程：

1. 检查 queue、buffer、128-byte address/length alignment 和 ownership。
2. 在 `cp_queue_mutex` 下等待 queue idle：credit 恢复到默认值且 `wptr == rptr`。
3. 读取/初始化 queue remap；检查 4 KiB alias 和其他 queue 冲突。
4. 清零 `wptr/rptr`，写 direction、src low/high/size、dst low/high/size。
5. 读回配置字段和 credit，确认配置已到达硬件。
6. 写 queue doorbell `+0x4 = 1`。
7. 轮询 `wptr - rptr >= size`，然后写 `rptr = wptr`。
8. 再次 drain 并清零 pointer，完成 queue 收尾。

这与 guide Appendix 5.10.0--5.10.4 的 queue fetch/doorbell 顺序基本匹配。
当前代码还做了 guide 未明确要求的配置 readback 和 pointer drain，用来避免
posted write 尚未到达时提前 kick。

CP queue 的“128 alignment”是 payload 两端地址和长度约束，不是 allocator 的
默认 512-byte placement，也不是 direct-DD descriptor slot stride。

## 3. Current command examples

```sh
spev_diag pcie_dma_data_transfer --target TPU0 \
  --direction h2d --size-bytes 4096 --pattern incremental

spev_diag pcie_dma_concurrent_transfer --target TPU0 \
  --job-count 8 --size-bytes 4096 --timeout-ms 30000

spev_diag pcie_dma_concurrent_transfer --target TPU0 \
  --job-count 256 --size-bytes 32 --timeout-ms 30000

spev_diag pcie_dma_threaded_stress --target TPU0 \
  --iterations 100 --size-bytes 4096 --timeout-ms 30000

spev_diag cp_queue_data_transfer --target TPU0 \
  --direction h2sram --queue mem3 --size-bytes 512 --sram-bank 0
```

实际 target 名称以 discovery 输出为准。`--job-count 256` 表示总 job 数，不是
256 个同时在途 job；当前同时在途上限是 255，完成后会滑动复用 slot。
并发 testcase 的 overall deadline 在初始 window 全部 submit 完成后才开始，
不覆盖前置 DMEM prepare、descriptor fill 或 initial submit 所花时间。

H2D/D2H/D2I 等不同 profile 可以放在同一个 testcase 中，但必须保证 endpoint
配置、地址范围和对齐合法；有数据依赖的 descriptor 必须显式等待前序完成。
Host/DMEM/VMEM 路径通常要求 32-byte 地址和长度对齐，涉及 IMEM 的路径要求
256-byte 对齐。`D2V/V2D` 当前上层 testcase 返回 `UNIMPLEMENTED`。

## 4. Open hardware checks

1. 对齐 descriptor-controller DCF range、credit threshold 和 fetch timeout；
   记录目标 RTL revision 的实际 reset value、推荐写值及单位。
2. 用真实 generated CSR header 对齐 direct-DD sync flag 的地址、context 和
   clear 语义，特别是 guide 的 SMF path 与当前 `0x400 + id*4` path。
3. 确认 descriptor fetch address 的 SRAM bank 编码；当前正式实现固定 SRAM1，
   guide 示例中的 generic fetch address 不能直接替换它。
4. 确认 trigger 后 idle 是否必须立即复核，还是下一次 submit 前复核即可。
5. 确认 SRAM MMIO writes 到 descriptor trigger 之间需要哪一种 PCIe readback/
   fence；不要把 C++ `atomic_thread_fence` 当作 posted-write completion。
6. 在硬件上验证 fatal/fetch-timeout 后的 reset/quiesce 以及资源释放边界。

建议的最小上机证据：

- 初始化前后 dump core0/core1 idle、fetch-timeout、全部 DCF range/control/credit
  CSR，附 RTL/CSR header revision；
- 对 flag `1`、`31`、`32`、`255` 做 `clear -> read -> submit -> read -> clear -> read`，
  同时用 guide 的 SMF/bitmap view 读取相同 flag，确认两个 view 是否一致；
- 用一个长 DMA 占用 core0，再提交 core1，记录每次 trigger 前后两个 idle 的时间线；
- 完成 255 个在途 job 后复用 slot 1，先延迟第二个 job 的完成，确认 stale done
  不会让 `poll_completion()` 提前成功。

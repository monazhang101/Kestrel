# reg / dmem 薄接口审查与最小实施方案

> 历史评估记录。后续确认的实施范围为：保留全部 PCIe/HQC/DMA/kernel；删除 TPU identify、DMC 和其它旧 case/Impl；PMU、DDP、ISI 各保留一个 example，只有 ISI 直接调用原生 PHAL linkup。实际接口和使用方法以 [isml_diag/README.md](../../isml_diag/README.md) 为准。本文下方关于移出 DMA、单个共享 example 等早期建议不再作为实施要求。

审查日期：2026-09-20。基于当前工作区，包括已有未提交改动。本次交付是审查和实施方案，没有修改运行代码。

结论：底层 IO 能力接近目标；需要集中修改 testcase 接口、上下文准备和错误语义，同时退出当前默认路径中的 FW/HQC/DMA。无需重建 probe、设备树或测试框架。不能把目前部分 case 返回 OK 当成已实现硬件功能的证据。

根据后续反馈修订：不新增 IoContext 或 Io.h/.cpp；复用模块已有的 DeviceContext ctx_。在 Common.h 提供 common::reg_write/read，通过精确的 using 声明让 case 直接调用 reg_write/read；dmem 同理。短名称不增加转发函数。PHAL 初始化仍由框架负责。

进一步收窄调用链：reg_write/read 直接调用现有 common::bar::write32/read32，不经过 phal_write/read。PHAL 用于 aperture 和 case 明确调用的 component API，其 callback 与普通 reg 路径共用 BAR/MMIO 底层。

## 审查范围与验证边界

审查覆盖 CMake、core、device/module、testcases、implementer 产品实现、产品/用例 policy、CLI、Python binding、mock PHAL；kernel 部分核对其 DMA 分配/ioctl/mmap 边界，不展开此次范围外的 DMA/HQC 协议正确性。golden 和根目录历史归档不作为当前实现。

本机为 Windows，项目 CMake 明确要求 Linux。真实 `isml_ipc/external/ProtonHal` 目录存在，但没有本项目需要的头文件/源码；能读取的是本地 mock。因此没有进行真实 PHAL 构建或硬件验证。对现有 `TestStatus |= TestStatus` 做了独立 C++17 语法检查，编译器确认缺少 `operator|=`。

当前架构技能文档中“case 自己 get_context / 直接计算 BAR offset”的旧约定，与此次需求不同；本方案按此次需求改为框架注入，不保留这部分旧约束。

## 主要发现

| 优先级 | 发现与代码位置 | 对此次目标的影响 |
|---|---|---|
| P1 | `isml_lib/implementer/src/atlas/pmu/pmu_impl.cpp` 的 reg_read/write/check，以及 ISI/DDP/DMC 的多项实现，只打印固定内容并返回 OK；AtlasM 也有类似情况 | PMU 写入没有写硬件，检查甚至把 expected 当 actual 输出。尚未落地的功能应返回 UNIMPLEMENTED 或移出默认注册 |
| P1 | `isml_diag/src/core/HalContext.cpp:443` 可退回只读 mmap，但 `BarMapping` 未记录写权限，`common::bar::write` 只检查 mapped | 三参数 write 可能触发进程异常，不能可靠返回 status。增加 writable 标志并在写入前拒绝 |
| P1 | `PhalBridge.cpp:198` 的 mutex 只保护缓存取得/创建；`TestTarget.cpp` 只锁当前 target；`DevMem.cpp` 的 set/get/payload 没有共同锁 | 同一 TPU 的不同 module 并发时，可能改变同一 PHAL base 或 aperture，使 IO 落到错误地址 |
| P1 | `testcases/isi/link_cases.cpp:28` 固定写入 DMEM 0x10000000，没有保存恢复原值；前置 linkup 实现又是固定成功 | 这个 case 不能证明真实 ISI linkup，还会改变内存；拆成明确的 dmem smoke case，使用确认的 scratch 区域并恢复 |
| P2 | `Common.h` 只有 BAR-relative bool API；模块 base 分散在模块成员和 ModuleImplContext，偏移计算落到 GenericPCIeImpl 等调用方 | 缺少全模块统一的三参数 reg API 和 block context |
| P2 | `DevMem.h` 要求 ti、DeviceContext、Window、offset、buffer、len、error；`DevMem.cpp:58` 在访问时取得 PHAL context | 底层 set/get/MMIO 链已经存在，但需要把配置和生命周期从 case 中移走 |
| P2 | `TestInfo.h:14` 状态是位值，却没有 OR 运算符；`test_status_name` 对组合值返回 UNKNOWN；mock PHAL 的 ERROR=1、INVALID=2 与框架位值不同 | 不能直接 OR bool，不能把 PHAL status 强转成 TestStatus。需要运算符、组合状态格式化及显式映射 |
| P2 | `CMakeLists.txt:127`、`:165` 仍编译 HQC/DMA，PCIeModule 仍注册 dma_data_transfer，PMUModule 注册 IPC | 当前默认功能面大于此次范围 |
| P2 | Python 的 `TestTarget.run_testcase` 直接调用 target，未传 HalContext/policy；binding 还暴露可写 CPU 地址 | 存在绕过 manager 的第二条执行路径。新 context 注入必须统一入口，Python 应通过 manager 运行 case |
| P2 | `HalContext.cpp:588` 先解除 BAR mapping，最后 reset PHAL | 当前 mock deinit 不访问硬件，但真实 PHAL 释放阶段是否访问 BAR 未知。明确顺序为停止测试 → 销毁 PHAL → unmap |

另有几项收尾问题：policy 路径依赖工作目录；产品 policy 缺失可静默变成无设备；mock README 声称自动 fallback，但当前 CMake 要求显式开启 mock；`--backend phal` 实际指向合成 dry-run，不能理解成真实 PHAL 已接通。64 KiB dry-run BAR 也覆盖不到 Atlas 的 0x36000000 等 module base，不能直接用于真实地址布局的验证。

## 保留与收窄

保留：DeviceManager discovery、现有 TPU/module 树、产品地址配置、TestTarget 注册/参数检查、Logger、HalContext 的映射所有权、PhalBridge 生命周期桥接、common::bar/mmio、DevMem 的 aperture set/get 与 payload 逻辑。

第一轮停止让普通 case 经过 `Module → Impl interface → GenericImpl → AtlasImpl` 才完成一次读写。普通 case 直接调用四个 IO 函数，产品差异先由已有 module layout 与 PHAL project 承担。有真实行为差异的实现以后再保留独立函数，不再为占位功能增加虚接口。旧 implementer 文件可以暂留，但不继续作为新 case 的必经层；大规模删除不作为首个可运行版本的前置条件。

DMA、HQC、FW/PMU IPC 从本轮默认注册和诊断构建依赖中移出；isml_kmod 的主机 DMA 分配能力不在 reg/dmem 调用链中。PHAL 内部源码是否能删减，需要真实依赖树决定，不能盲目移除 init 所引用的组件。

## 四个 API 的明确契约

以下是拟新增接口，不是当前已存在的可编译 API：

```cpp
TestStatus reg_write (DeviceContext& ctx, uint64_t addr, uint32_t data);
TestStatus reg_read  (DeviceContext& ctx, uint64_t addr, uint32_t* data);
TestStatus dmem_write(DeviceContext& ctx, uint64_t addr, uint32_t data);
TestStatus dmem_read (DeviceContext& ctx, uint64_t addr, uint32_t* data);
```

这四个函数分别在 common 命名空间定义一次。Common.h 在命名空间外用 `using common::reg_write;`、`using common::reg_read;` 等精确声明提供短名称；不是再写四个全局转发函数，也不使用宏或 `using namespace common`。reg 的薄实现放在 Common.h；dmem 的实现保留在 DevMem.cpp，声明由 DevMem.h 提供并通过 Common.h 引入，PHAL component 头仍留在实现文件。

第一版明确只读写一个 32-bit word，addr 的单位是 byte。read 的 data 是出参：调用方传 `&value`，成功后得到读值；失败不修改原值。先读到内部临时值，成功后再赋给出参。拒绝空指针、不对齐、地址加法溢出、模块越界、映射越界和只读映射上的写操作。

裸 `void* data` 无法表达传输长度。后续如需批量 IO，可用带长度的 buffer view 作为第三参，或显式增加长度重载；不在第一版引入任意类型模板或根据指针推断长度。

寄存器和内存分别定义地址空间：

```text
reg BAR offset = module CSR base + 当前 block offset + addr
CPU address    = mapped BAR virtual base + reg BAR offset

dmem target    = ctx.dmem_base + addr
CPU address    = mapped data BAR virtual base + window BAR offset
               + (dmem target - aperture target base)
```

`module CSR base` 来自当前产品配置；Atlas PCIe 已是 0x36000000。配置里的 DMC base 已是完整 BAR-relative base，不能再加父 DDP base。

`dmem_base` 是独立的设备内存区域起点，不能使用 PCIe/ISI/DDP 的 CSR base 代替。当前仓库没有各 module 的独立 dmem region 表。最小版本先采用明确的公共 scratch region；若需要每个 DDP/ISI 有独立内存视图，再添加经过确认的 base/size 数据。不能由 CSR base 推导内存地址。

block enter/exit 默认只作用于 reg 路径，不隐式移动 dmem_base。

## 上下文与 PHAL 接入

复用 TestTarget 已有的 DeviceContext ctx_，不新增 context 类型、IO 类或接口分发层。HalContext 继续拥有 mapping，PhalBridge 继续拥有 PHAL context。模块的 ctx_ 保存 reg BAR、module 根地址/范围、当前 block offset，以及必要的 dmem 区域信息。供 component API 使用的 module PHAL 指针和供 dmem 使用的 PCIe PHAL 指针由框架准备，均为借用。现有 module 配置在构造/准备阶段填入，case 不再组装另一份上下文。

普通 reg 的 block_ctx_enter/exit 只更新现有 DeviceContext 的 block offset，reg_write/read 用 module base + block offset + addr 计算 BAR offset。PHAL component API 使用自己的 env.base，由 PHAL 的 block 操作管理。这是两条独立调用路径，不同步两边的可变 base，也不把 framework block offset 再加进 PHAL callback。日志信息由现有执行入口/bridge callback 数据承接；借用的测试 logger 仅在本次执行期间有效。

生命周期采用“probe 后、case 开始前初始化”：

1. discovery 发现设备、映射 BAR、解析 module layout；未映射成功的 target 可以展示，但不能作为 ready target 执行 IO。
2. framework 在测试入口确保当前 module context 和该 TPU 的 aperture context 已初始化；缓存复用，不要求 case 调用 init/get_context。
3. 将已初始化的 PHAL 指针绑定到现有模块 ctx_，然后执行 callback。初始化失败直接返回错误，不进入 case。TestInfo 不新增 io 字段。
4. testcase 结束恢复 block scope。清理时先停止正在执行的 case，再销毁 PHAL，最后 unmap。

建议允许 discovery 单独展示设备，不强制每次纯 discovery 都初始化所有 PHAL 组件；初始化位置在统一执行入口即可满足需求。

reg_write/read 在 Common.h 中完成地址计算、必要检查和 bool 到 TestStatus 的转换，直接调用现有 common::bar::write32/read32。PHAL component API 经现有 callback 访问同一个 BAR/MMIO 底层；callback 已收到 env.base + addr，应继续调用 BAR helper，不调用会再次添加 module base 的 reg_write/read。

```text
testcase → reg_read/write → BAR → MMIO
testcase → dmem_read/write → PHAL PCIe aperture set/get → data BAR → MMIO
testcase → PHAL component API → callback → BAR → MMIO
```

用于 aperture 的 PHAL context 必须从 PCIe module root 开始，不能使用 ISI/DDP 的 PHAL root。普通 reg block 不再改变 PHAL env.base，因此 PCIe reg block 内调用 dmem 不会造成 PHAL block 重复叠加。在同 TPU 串行、原生 component API 成对恢复 block 的前提下，可以复用现有按设备/BAR/base/project 缓存的 PCIe PHAL context，无需新增用途 key 或强制创建第二个 PCIe 实例。若 case 手动调用原生 phal_block_ctx_enter，则必须 exit 后才能调用期待 module root 的 API，包括 dmem；不要直接复制未知所有权语义的 phal_ctx_t。

直接 PHAL component API 仍可使用，ctx 中预先注入所需指针：

```cpp
// 示意：调用自己模块的 PHAL API，用 ctx.phal。
// 跨模块调用 PCIe API，用 ctx.pcie_phal（PCIe root）。
auto native = phal_pcie_aperture_get(ctx.pcie_phal, bar, index, &aperture);
status |= from_phal(ctx, native);
```

不为每个 PHAL component 函数建立同名包装，也不引入全局“当前设备”。调用方不需要 init/get_context，但仍需要把正确的 ctx 传给原生函数。组件 API 若在内部 enter block，外部就不能重复 enter 同一 block。

当前 bridge 只安装 read/write callback；只承诺支持依赖这些 callback 的 component API。新 API 若需要 delay/tick/fence 等，再针对实际需求补齐。

## dmem 的最小实现

第一版先固定一个框架自用窗口，复用当前示例候选值：BAR4 / aperture0 / identity0 / BAR offset0，窗口大小候选 1 MiB，scratch base 候选 0x10000000。这些是仓库已有 bring-up 数值，不能视为已验证的平台 ABI；必须确认目标范围可读写且没有占用冲突。

处理一次 32-bit IO：校验 target 和 scratch 范围 → 计算合法 aperture target/窗口内偏移 → phal_pcie_aperture_set → get/readback 验证 → MMIO。任何准备步骤失败都不能执行 payload IO。地址对齐、size 编码和窗口 BAR offset 规则以真实 PHAL/硬件为准。

首版每次调用重新 set/get，不做窗口缓存与切换优化，代码最直接。默认窗口由框架独占；普通 case 不负责保存/恢复 aperture。专门测试多 aperture 的 case 可以使用注入的 PHAL ctx，但必须与普通 dmem case 串行执行，并明确其 aperture 与数据恢复行为。

等出现实际配置需求后，增加带 `const DmemOptions&` 第四参的同名重载。配置至少包含 BAR/aperture/identity，以及必要的合法窗口尺寸/布局；不能允许任意 BAR offset 与任意 aperture 组合却不验证匹配关系。普通 case 始终使用三参数版本。

## block scope、状态与并发

首版提供与示例形态对应的 block_ctx_enter/exit，只在现有 ctx 上累加/减回 block offset，不调用 PHAL。enter 检查溢出与范围并返回 TestStatus；失败不得继续 IO，exit 检查减法下溢，调用方保证同 offset 成对配对。先不新增 BlockScope 类，case 保持成对 enter/exit；有提前返回或异常可能的 case 必须在退出路径恢复 context，框架在 case 边界恢复初始 block offset，不能把残留偏移带入下一个 case。以后如果实际 case 中反复出现复杂清理，再考虑很小的 RAII guard。

拟议 testcase 形态：

```cpp
TestStatus PCIeModule::program_example(TestInfo& ti)
{
    auto& ctx = ctx_;               // 复用当前 module 的 context
    auto status = block_ctx_enter(ctx, ATLAS_CSR_PCIE_AXICLK);
    if (status != TestStatus::OK) return status;
    status |= reg_write(ctx, off_start_h, uint32_t(start >> 32));
    status |= reg_write(ctx, off_start_l, uint32_t(start));
    status |= block_ctx_exit(ctx, ATLAS_CSR_PCIE_AXICLK);
    return status;
}
```

这是用目标中的寄存器名展示最终形态，相关寄存器常量和变量由真实 case 定义。

status 继续使用现有 TestStatus 位值，增加 `operator|` / `operator|=`，日志把组合值输出成如 `TIMEOUT|ERROR`。from_phal 根据真实 PHAL 的位定义逐位映射并保留原始状态日志；未知非零值至少映射 ERROR，绝不直接强转。当前 bool helper 必须显式转换，不能 `status |= bool_result`。

OR 汇总的是错误类别，不是失败步骤和发生次数；每个失败操作需记录 target、地址、操作和原始错误。独立寄存器写可以累积执行；init、block enter、aperture set、读取后作为下一步输入等依赖步骤必须检查成功后再继续。

并发第一版采用每颗 TPU 一个测试执行锁，覆盖其所有子 module 的整个 testcase，包括原生 PHAL 调用。通过 manager 的统一入口执行，关闭/改造 Python 直接 target.run_testcase 的旁路；重新 discover/clear 也不得与执行并发。跨 TPU 可独立执行，不提供同一 TPU 多进程并行访问承诺。以后真有吞吐需求，再拆为 aperture/context 等内部锁。

## 最小修改顺序与验收

| 步骤 | 文件范围 | 完成标准 |
|---|---|---|
| 1 | Platform/DeviceContext、Common.h、PhalBridge、TestTarget/DeviceManager、必要的 module config 接入 | 复用 ctx_，填入 reg layout 并准备所需 PHAL 指针；四个短名称函数与 block enter/exit 契约固定；不新增 IoContext/Io.h/.cpp |
| 2 | Common、Platform/BarMapping、HalContext、PhalBridge | reg 直接调用 BAR/MMIO；统一检查模块边界、对齐、溢出与写权限；PHAL 先于 mapping 释放 |
| 3 | DevMem.h/.cpp | 隐藏旧 Window 组装；默认单窗口单字读写；prepare 失败绝无 payload 访问 |
| 4 | 一组 reg case、一组 dmem smoke、TestStatus formatter、CLI/binding | case 只保留操作、比较和必要的恢复；组合状态可读；所有执行经过同一注入/锁入口 |
| 5 | 注册、CMake、implementer 占位实现、说明文档 | 默认移出 FW/HQC/DMA/IPC；未实现不报 OK；mock 明确标注能力边界 |

必要的软件验收：

- 用本地模拟 BAR 验证 reg 最终地址恰为 module base + block offset + addr；嵌套/异常返回后回到原 base。PHAL callback 地址转换另行验证。
- 在 PCIe reg block 中调用 dmem，确认 aperture PHAL ctx 不受 framework reg block 影响；ISI/DDP 的 dmem 同样走 PCIe root。
- 空出参、未对齐、边界最后一个 word、越界、加法溢出、只读 BAR 均有明确状态；失败 read 不修改出参。
- 注入 aperture set/get 失败，确认没有 data BAR 访问；read/write 失败能正确 OR 汇总。
- 两个同 TPU module 并发请求不能交错执行；case 初始化失败不进入 callback。

普通 reg 和 framework block 的软件验证不依赖补齐 mock 的 phal_read/write/block。mock 是否增加这些接口由后续直接 PHAL component 调用的需要决定。现有 mock 可用于 aperture set/get 软件流程检查，但不证明硬件 aperture 翻译正确；不要把同一个内存数组的写回读当成物理 dmem 验证。

硬件验收：在真实 PHAL 与 Linux 主机上，读取已知只读 CSR；在确认的 scratch 寄存器/内存区域保存原值、写 pattern、读回比较、恢复；分别验证 PCIe/ISI/DDP 的 reg base，以及跨 module dmem 路径。专用多 aperture case 单独验证。

## 工作量与待确认事实

粗估一名熟悉仓库的开发者：接口/context/status 收口约 1–2 人日，case 迁移、默认路径收窄及软件验证约 1–2 人日；真实 PHAL 与硬件联调另计，接口/地址图稳定时预留约 1–2 人日。不包含实现所有子系统的功能测试，也不包含等待硬件和补齐 SDK 的时间。

落地前需要从真实 PHAL/硬件资料确认的事实只有核心几项：phal_read/write/block 的完整契约与状态位，aperture 的对齐/size/地址翻译，框架可独占的 scratch 内存范围，实际可访问的 module CSR 范围。软件结构可以先按上述方案推进，不需要为这些事实预先增加新的 backend/session/调度层。

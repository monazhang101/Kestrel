# PHAL：调用链、地址基准与编译记录

> 整理范围：2026-08-17 至 2026-09-21 的本地 Codex 聊天；当前行为以 2026-09-21 工作区 [CMakeLists.txt](../../CMakeLists.txt) 与 [PhalBridge.cpp](../../spev_diag/src/core/PhalBridge.cpp) 核对。历史中关于“预先构建 `libphal.a`”的建议是过渡方案，不是当前项目构建方式。

## PHAL 的角色

PHAL 提供项目/组件级硬件功能入口，例如 `phal_pcie_aperture_set/get()`、`phal_component_isi_linkup()`。diag 框架在运行 case 前准备 module root 和 PCIe root 两个 PHAL context；case 只在需要组件功能时调用原生 PHAL API。普通寄存器 `reg_read/write` 直接走 `common::bar`，不会绕进 PHAL。DMEM API 则用 PCIe PHAL 配 aperture，之后在 BAR4 数据窗口传输 payload。

```text
原生 PHAL 组件 API
  -> PHAL project implementation
  -> env.hw_ops.read/write
  -> hal_drv_read/write (PhalBridge.cpp)
  -> ihal_read32/write32
  -> common::bar::read32/write32(BAR0, env.base + addr)

普通 reg_read/write -> common::bar(BAR0, module base + block offset + addr)
DMEM read/write     -> PHAL aperture set/get -> common::bar(BAR4, window offset)
```

## context 初始化与地址

`PhalBridge::get_context(ctx, base_offset, project)` 按 BDF、base、project 缓存 `phal_ctx_t`，通过 `phal_init()` 配置 `env_config.base`、`user_data`、read/write `hw_ops` 与 Atlas/AtlasM project。runner 为需要 PHAL 的 case 注入 `ctx.phal`（当前模块 root）及 `ctx.pcie_phal`（PCIe root）；[TestTarget.cpp](../../spev_diag/src/core/TestTarget.cpp) 中 `_add_test(..., true)` 决定是否预备它们。当前 bridge 只安装 read/write callback；某个 PHAL API 如果要求 delay、tick 或 fence，必须先核对真实 PHAL 实现再补。

2026-09-07 的 pattern mismatch 排查定位到 **AXICLK offset 被重复相加**：框架曾提前把 `0x20000` 加到 `env.base`，而 PHAL aperture API 内部又 `block_ctx_enter(AXICLK)`。正确约定是给 PHAL **PCIe module root base**，由组件 API 自己进入内部 block。例如 Atlas PCIe root 为 `0x36000000`，aperture 内部进入 AXICLK 后，控制 CSR 地址是 `0x36020000 + 相对 offset`，而不是 `0x36040000 + 相对 offset`。直接调用别的 PHAL 组件 API 时，应先核对它是否内部进入 block；不要通用地手动预加子块偏移。

原生 `phal_status_t` 与框架 `TestStatus` 不是同一数值体系。[from_phal()](../../spev_diag/src/core/PhalBridge.cpp) 显式把 OK/INVALID 映射为框架状态，其它 native 错误映射为 ERROR，并保留原值日志。ISI example 直接调用 `phal_component_isi_linkup(ctx.phal, 1000000)`；普通 case 不需要自己调用 `get_context()`。

## 当前编译方式

当前根目录 CMake **直接把真实 ProtonHal 源码编译成静态 target `phal`**，最终链接到 `spev_diag`。真实源码必须在 `spev_ipc/external/ProtonHal/`；Atlas CSR 外部依赖通过 `FLOWRA_SOURCES_DIR` 提供，默认是项目上级的 `.flowra_sources`。Flowra manifest 本身不会被本项目 CMake 自动解析或拉取依赖。CMake 配置日志应显示 `PHAL backend: real ProtonHal`。

```sh
cmake -S . -B build-fpga -DSPEV_BUILD_CLI=ON \
  -DSPEV_USE_MOCK_PHAL=OFF \
  -DFLOWRA_SOURCES_DIR=/path/to/.flowra_sources
cmake --build build-fpga --parallel "$(nproc)"
```

`SPEV_USE_MOCK_PHAL` 默认 `OFF`；真实 PHAL 缺失时配置直接失败。`ON` 仅用于本地编译/smoke，不证明硬件功能。复用旧 build 目录时核对 `CMakeCache.txt`，因为它可能缓存过 mock 选项或绝对依赖路径。当前 CLI 构建开关是 `SPEV_BUILD_CLI`，产物为 `build-fpga/spev_diag`；早期文档中的 `SPEV_BUILD_EXAMPLES` / `spev_diag_example` 已过时。项目 CMake 仅支持 Linux，kmod 则另外使用 `make -C spev_kmod`。

构建机器与 tester 都需要完整的顶层 `CMakeLists.txt`、`spev_diag/`、`spev_lib/`、`spev_kmod/` 的 UAPI 头、真实 `spev_ipc/external/ProtonHal/` 以及 Flowra/CSR sources。不要把另一个机器的 CMake build 目录当成可移植源码。2026-09-09 在 tester 上还遇到 CMake 不在 `PATH`、旧 GCC 链接 `std::filesystem` 需 `stdc++fs` 的问题；当前 CMake 对 GCC < 9 已有条件链接。

## 决策中的变化与边界

- 9 月 1 日曾讨论“PHAL 自己构建预编译 `libphal.a`，diag 只链接库”；后续项目实际采用顶层 CMake 编译真实 PHAL 源码，并显式提供 CSR include 路径。不要混用两套构建说明。
- 不能用空的 project ops stub 仅让链接通过，然后把 `phal_init` 成功误当作真实 Atlas aperture 已可用。
- `--backend phal` 的名称不自动证明当前走的是真实 PHAL 硬件路径；以构建日志、运行配置和具体 callback 为准。
- Windows 工作区无法执行顶层真实 Linux/PHAL 构建；本地 mock/软件测试只验证代码调用，不验证 FPGA 地址译码或真实组件行为。

## 会话来源

- 2026-08-17～24：`01a00f3b-df62-7373-8662-6486e3cf5643`、`01a031a2-0a72-7c02-bc60-c5b8fe5c3746`（iHal/pHal 与调用边界）。
- 2026-09-01～04：`01a05be3-e9a8-7713-9c4b-cee492afae0f`、`01a06b42-4b98-76b0-9ef6-269237d3cf03`（flowra/CSR、callback、PHAL context）。
- 2026-09-07：`01a06b42-4b98-76b0-9ef6-269237d3cf03`（AXICLK 双加及修正）。
- 2026-09-09：`01a084ad-1bae-7d50-8772-ab4b400b864f`（tester 编译、real/mock、CLI 名称）。
- 2026-09-20：`01a0bd66-f08c-7b53-98b3-faa2c6812dd3`（最终 reg/DMEM/PHAL 三条调用链）。

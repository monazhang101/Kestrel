# Kernel module：历史决策与当前实现

> 整理范围：2026-07-29 至 2026-09-21 的本地 Codex 聊天。本文记录设计如何收敛、FPGA bring-up 中的发现，并以 2026-09-21 工作区源码核对当前实现。聊天中的建议不等同于硬件验证结果。

## 结论

`spev_drv.ko` 是独立于用户态 CMake 的薄 PCI 驱动。它按 VID/DID probe 设备，启用 PCI 设备与 bus master，为用户态提供 coherent host DMA buffer 的分配、`mmap` 和释放。DMA 命令的构造、HQC 队列提交、完成轮询和数据比较留在用户态；当前 kmod 不启动 FW，也未实现 IRQ。用户态的普通 BAR/MMIO 寄存器读取不经过这个 DMA 分配设备节点。

## 决策演变

| 阶段 | 历史讨论的结论 |
|---|---|
| 7 月：层次划分 | 先把 kernel 压薄，只承接用户态不能安全完成的设备资源获取。曾讨论由 kernel 分配物理内存、request IRQ，UMD/HAL 管 descriptor、ring、doorbell 和 DMA 调度。IRQ 后来从首版范围移出。 |
| 8 月：两种后端 | diag 需要选择 iHal 或 pHal；两者的底层驱动互斥。这个选择属于一次测试进程/设备发现阶段，不应让每个 case 任意切换。 |
| 9 月 8 日：落地方案 | 新增独立 `spev_kmod/`；用 PCI driver 持有正确的 `struct pci_dev`，通过 Linux DMA API 分配 host buffer；UAPI 返回 opaque handle、`device_addr` 和 `mmap_offset`。 |
| 9 月 9–15 日：tester bring-up | 在 tester 上编译并加载 `.ko`，解除旧驱动绑定后测试。经历 D3cold、BAR/HQC 路径问题后，用户报告自研驱动绑定成功，H2D/D2H 在匹配 FW 下完成数据比较。 |

## 当前源码边界

- [spev_diag_main.c](../spev_kmod/spev_diag_main.c) 的 PCI ID 表负责 probe；probe 中调用 `pci_enable_device_mem()`、设置 64-bit DMA mask，失败则回退到 32-bit。
- [spev_diag_uapi.h](../spev_kmod/spev_diag_uapi.h) 定义 ABI v1、`GET_DEVICE_INFO`、`DMA_ALLOC`、`DMA_FREE`，单次 coherent 分配上限为 128 MiB。
- `DMA_ALLOC` 用 `dma_alloc_coherent()` 同时得到内核 CPU 地址与设备可见的 `dma_addr_t`；返回给用户态的 `device_addr` 应写入 HQC descriptor。不要假定它恒等于 CPU 物理地址，即使当前没有启用 IOMMU。
- `mmap` 用 `dma_mmap_coherent()` 映射 buffer；文件关闭时清理未释放的资源。用户态通过 [HalContext.cpp](../spev_diag/src/core/HalContext.cpp) 的 `alloc_host_dma_buffer()` 持有并释放 buffer。
- kmod 不负责 aperture、HQC、FW 启动或数据比较；当前没有 request IRQ/中断等待实现，完成路径采用用户态轮询。

这也解释了几个常混淆的地址：`cpu_base` 是用户态访问 host buffer 的虚拟地址；`device_addr` 是设备 DMA 可见地址；`handle` 是该 buffer 的内核句柄；`fd` 是 `/dev/spev_drvN` 的打开文件描述符。BAR 映射地址和这些地址不是同一概念。

## 构建和加载记录

在**目标 tester 当前运行的 Linux kernel**上，使用匹配的 kernel headers：

```sh
uname -r
test -d "/lib/modules/$(uname -r)/build"
make -C spev_kmod
```

`spev_kmod/Makefile` 通过 `/lib/modules/$(uname -r)/build` 构建模块，生成物放在 `spev_kmod/build/`，不由顶层 CMake 编译。Linux 6.13 及以后使用 Kbuild 的 `MO=` 外部模块输出目录；旧版 Kbuild 没有 `MO=`，Makefile 会把最小模块源码暂存到 `build/` 后用 `M=build/` 编译。`O=` 用于选择内核输出树，不是 `MO=` 的兼容替代。加载前确认 FPGA 的 BDF、VID/DID、电源状态，并确认与它互斥的旧驱动已解绑；加载后用 `readlink /sys/bus/pci/devices/<BDF>/driver`、`lspci -nnk -s <BDF>` 和 `dmesg` 核实实际绑定。若 `insmod` 已自动 probe 成功，再手动 `bind` 会返回 busy，不代表首次绑定失败。修改 `.ko` 后必须卸载旧模块并加载新产物，不能只重新编译用户态程序。

历史里出现过 endpoint 处于 D3cold、无法切到 D0 的 probe 失败，也出现过旧 Proton 驱动重绑或设备复位后的状态变化。`lspci`/`dmesg` 与设备的实际 driver symlink 比命令是否返回成功更有判断价值。后续可考虑 DKMS/deb 安装，但当时决定先完成源码编译和 DMA bring-up；仓库当前仍以 `Makefile` 为主。

## 已验证与待确认

- 历史记录中，用户在 FPGA tester 上确认自研 driver 可绑定、BAR 读取可用；匹配 HQC FW 下 4096-byte H2D/D2H 的 incremental/random pattern 比较通过，random 测试还覆盖了超过 4 GiB 的设备 DMA 地址。这是该 tester/FW 组合的验证记录，不能外推到所有板卡。
- 64-bit DMA mask 表示允许请求高地址；只有日志中的实际 `device_addr >= 0x100000000` 且传输比较通过，才能说高地址路径被测到。地址超过 4 GiB 不表示传输了 4 GiB 数据。
- IRQ、IOMMU 场景、安装包/DKMS、跨进程资源仲裁仍未由当前 kmod 实现或验证。FW/HQC 初始化由别的组件负责。

## 会话来源

可用 `codex resume <session-id>` 查阅原始聊天：

- 2026-07-29～31：`019fa7a7-c995-7e01-858d-451a428eb14d`、`019fb6cd-57b8-76c2-a03a-78bfe7b50b6d`（薄 kernel、UMD/HAL 边界）。
- 2026-09-07～08：`01a07bb5-6fed-7ce0-845b-bd17d8584a4c`（kmod/UAPI/HQC 落地方案）。
- 2026-09-09～10：`01a085b9-3ec5-7901-9be1-6434c683eb7a`（编译、绑定、D3cold 排查）。
- 2026-09-14～15：`01a09e00-a837-7d93-a4a7-f3c8e73fc799`（高地址 DMA 验证、当前薄 kernel 功能总结）。

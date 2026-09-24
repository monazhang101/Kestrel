# ISML Diagnostic Development

`isml_diag` separates target construction, testcase implementation, and
platform configuration:

```text
include/diag/modules/        Module interfaces and testcase declarations
src/modules/                 Module construction and testcase registration
testcases/<target_type>/     Testcase implementations
policies/testcases/          Optional testcase argument ranges
policies/product/            Product topology and module instances
cli/                         Command-line entry point
bindings/                    Python bindings
```

## Select a device and module

CLI targets select a physical TPU: `TPU0`, `TPU1`, etc. The default is `TPU0`;
numbering follows discovery order. Test names include the module prefix:

```sh
./build/isml_diag pcie_example
./build/isml_diag ddp_example --target TPU1
./build/isml_diag pcie_sequential_aperture_mapping --target TPU0
./build/isml_diag soc_example
```

A module testcase's `ctx_` is instance 0 on that TPU. Other instances are
obtained from policy by type and instance ID, not vector position:

```cpp
auto& ddp1 = ti.module("ddp", 1);
auto status = phal_read(ddp1.phal, offset, &value);
```

`ti.module()` returns a prepared `DeviceContext&`; it does not run another
case. Missing instances fail the case. Get contexts before starting worker
threads and join workers before returning. Register block scopes on a shared
PHAL context are not safe for concurrent use.

`SocModule` is a sibling under `TPUDevice`, with cases in `testcases/soc/`.
It has no register base; its cases obtain all hardware contexts via `ti.module()`.

## Add a testcase

1. Declare the testcase method in `include/diag/modules/<Module>.h`.
2. Register its local name, defaults, formats, and callback with `_add_test()`
   in `src/modules/<target_type>/<Module>.cpp`.
3. Implement the method in `testcases/<target_type>/*.cpp` (or the module source
   for a small primitive).
4. Add optional platform limits under the same testcase name in
   `policies/testcases/<target_type>.yaml`.
5. Add a new testcase source file to the root `CMakeLists.txt` when needed.

`_add_test("example", ...)` in DDP registers `ddp_example`. Policy keys remain
local names (`example` in `ddp.yaml`).

Defaults and argument formats belong in `_add_test()`. YAML policy files only
restrict values allowed on a platform; they do not redefine testcase behavior.
The runner prepares both module and PCIe PHAL contexts before every testcase;
`_add_test()` has no PHAL-initialization option. Case code uses `ctx.phal` or
`ctx.pcie_phal` directly without fetching or initializing a context.

## Add a module type

1. Add the module interface under `include/diag/modules/` and its constructor
   under `src/modules/<target_type>/`.
2. Derive the module from `TestTarget` and pass its lowercase target type to the
   base constructor, for example `TestTarget(name, "foo", ctx)`.
3. Add testcase implementations under `testcases/<target_type>/`.
4. If policy limits are needed, add
   `policies/testcases/<target_type>.yaml`. The filename stem is the target type
   used during policy lookup, so `foo.yaml` matches target type `"foo"`.
5. Add the module to the parent target's topology construction and list its
   sources in the root `CMakeLists.txt`.

Adding a module type does not require a target-type enum or a parser change.

## Register and device-memory examples

Register operations use PHAL directly:

```cpp
auto& ctx = ctx_; // This module's instance 0.
phal_block_ctx_enter(ctx.phal, block_offset);
auto status = phal_read(ctx.phal, reg_offset, &actual);
phal_block_ctx_exit(ctx.phal, block_offset);
```

The runner restores every borrowed PHAL context to its module root on return
or exception. Pair block enter/exit before memory IO, especially on PCIe where
module and aperture configuration can share a PHAL context.

Allocate memory before accessing it. The allocation selects the offset;
read/write offsets are relative to that buffer:

```cpp
auto& pcie = ti.module("pcie", 0);
auto buffer = mem_alloc(pcie, HQC_SRAM_0, 1024);
if (!buffer.valid()) return PHAL_STATUS_ERROR;
std::vector<uint8_t> expected(1024, 0xab), actual(1024);
auto status = mem_write(buffer, 0, expected);
if (status != PHAL_STATUS_OK) return status;
status = mem_read(buffer, 0, &actual);
// buffer.release() or automatic release at scope exit.
```

The same API accepts `DMEM`, `DDP_ILM`, `PMU_DLM`, etc. The context must match
the RCF region's owning module: PMU cases use `ti.module("pcie", 0)` for HQC
SRAM. DDP0 and DDP1 have independent pools; DMEM is shared by all modules on
one TPU. Host DMA allocation remains `ti.hal->alloc_host_dma_buffer()`.

- Start addresses, allocation sizes, read/write offsets and lengths use
  4-byte alignment. Zero sizes and unsupported regions fail allocation.
- A per-device mutex protects allocation/free. First fit reuses free gaps;
  insufficient contiguous space returns an invalid buffer without waiting.
- Buffers are move-only. Keep them within the owning HAL/test execution's
  lifetime. Wait for hardware completion before release. Sharing one buffer
  between threads requires caller synchronization.
- RCF pools currently cover whole regions. DMEM's provisional allocatable
  range is `[0x10000000, 0x20000000)`; this is not confirmed physical capacity.
  Atlas tables live in `isml_lib/implementer/src/atlas/memory_regions.cpp`.
- DMEM IO configures BAR4/aperture0 to cover the current test range. A separate
  mutex serializes configuration and payload access. AIC S/I/VMEM allocation
  remains unimplemented.
- `mem_read/write(ctx, region, offset, ...)` remains a low-level fixed-address
  API for firmware protocols. It does not reserve space; ordinary cases use
  allocated buffers. Raw BAR access likewise bypasses the allocation table.

### Run DMA H2D

Rebuild on the FPGA host after syncing source changes:

```sh
cmake --build build-fpga -j 4
# Defaults: TPU0, H2D, 4096 bytes (4 KiB), random pattern, 1000 ms.
sudo ./build-fpga/isml_diag pcie_dma_data_transfer
# 128 MiB H2D, with a 30-second completion timeout:
sudo ./build-fpga/isml_diag pcie_dma_data_transfer \
  --target TPU0 --direction h2d --size-bytes 0x08000000 \
  --pattern incremental --timeout-ms 30000
```

The CLI name includes `pcie_`; `_add_test("dma_data_transfer", ...)` adds that
prefix automatically. Arguments are optional because registration supplies
defaults through `ti.args`. Size is in bytes: use `--size-bytes`, not `--size`.
H2D initializes the host source, submits one DMA transfer, then reads DMEM once
through MMIO and compares the data. It does no preliminary DMEM write/read or
backup/restore. Host DMA requests are limited to 128 MiB, subject to allocation
success; `timeout_ms` is not a time limit for the final MMIO verification.
MMIO transfers of at least 1 MiB print progress at start, every 10 seconds, and
at completion. Progress is checked every 64 KiB and is visible at info level.

The DMA case allocates its DMEM source/destination instead of accepting
`device_offset` or `return_offset`. FW still requires DMA lengths in 32-byte
units. Failed DMA requests retain their device allocations because completion
is uncertain; do not rediscover/reuse the device until hardware has stopped.
The existing host-buffer timeout/drain behavior is unchanged.

The aperture case allocates space for its 15 ranges (plus alignment padding),
then holds the aperture mutex through all explicit configuration and IO.
Runner testcase execution remains serialized. The allocator and normal DMEM
IO support threads inside a case; no parallel testcase scheduler is added.

TODO: FW reserved ranges, confirmed DMEM capacity, concurrent aperture windows,
and multi-process coordination. The existing HQC/FW prototype is left as-is;
it is scheduled for replacement, so dynamic queue integration is deferred.

PMU, DDP and ISI use hardcoded register examples:

- PMU first writes `0x5a5a5a5a` at its module offset `0x0` to unlock host
  access. It then enters PLL at block offset `0x2100000`, reads offset `0x800`,
  and expects `0x00141e01`. The internal read address is
  `0x18000000 + 0x2100000 + 0x800 = 0x1a100800`; the Atlas BAR0 offset is
  `0x3a100800`.
- Every DDP target enters DMC0 at block offset `0x80800`, reads DID/VID at
  offset `0x0`, and expects `0xabcd16c3`. For DDP0 this corresponds to the
  documented internal register address `0x12080800`; the configured BAR0
  offset is `0x32080800`. The mapping between those address spaces must be
  confirmed on the platform. This access has stalled the `pcie-b7` tester,
  so do not rerun it there until the PCIe/FPGA path is fixed.
- Every ISI target reads offset `0x1c` directly from its module base, without
  block entry, and expects `0x202020`.
- PCIe enters block offset `0x100000` from its module base, reads offset
  `0x0`, and expects `0xabcd16c3`. For Atlas this is BAR0 offset
  `0x36100000`; that register has returned the expected value on `pcie-b7`.

PCIe, PMU, DDP and ISI return ERROR on a value mismatch. PMU calls
`phal_write` once to unlock host register access.
Their examples take no arguments. Each allocates one DMEM word, writes
`0x12345678`, checks it and restores the original word before releasing it.
The PCIe example also allocates and checks one HQC SRAM0 word.

For raw BAR0 debugging, `pcie_bar_read32` takes a PCIe-module-relative `--offset`
and adds the PCIe base. `pcie_bar_read32_abs` takes a required `--offset` and
reads that exact BAR0 offset without adding any module base. Like every case,
the runner prepares PHAL first, but the raw BAR read itself does not call it.
For example, `pcie_bar_read32_abs --target TPU0 --offset 0x36100000`
checks the known PCIe DID/VID location. It can also address DDP/ISI offsets
through the PCIe target, but it issues the same MMIO read as `phal_read` at that
location. In particular, do not read `0x32080800` on `pcie-b7` until the
hardware access that stalled that server is resolved.



## Software verification

On Linux, make sure flowra_sources folder is cloned outside isml_suite2. Configure
the normal build with `-DISML_BUILD_TESTS=ON`, build by execute:

```sh
make -C isml_kmod
cmake -S . -B build
cmake --build build
```

To unbind ProtonHal VFIO driver, remove DKMS package first, then reboot and
load our isml_kmod driver:

```sh
sudo dpkg -P proton-drv-vfio-pci-kernel-dkms
sudo reboot

sudo insmod ./isml_kmod/isml_diag.ko
```

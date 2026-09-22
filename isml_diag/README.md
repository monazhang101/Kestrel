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

## Add a testcase

1. Declare the testcase method in `include/diag/modules/<Module>.h`.
2. Register its public name, defaults, formats, and callback with `_add_test()`
   in `src/modules/<target_type>/<Module>.cpp`.
3. Implement the method in `testcases/<target_type>/*.cpp` (or the module source
   for a small primitive).
4. Add optional platform limits under the same testcase name in
   `policies/testcases/<target_type>.yaml`.
5. Add a new testcase source file to the root `CMakeLists.txt` when needed.

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

PCIe, PMU, DDP and ISI each register one `example` testcase in their own
`testcases/<module>/example.cpp`. Register operations call PHAL directly;
the framework prepares `ctx.phal` before the case begins:

```cpp
auto& ctx = ctx_;  // This target's existing DeviceContext; prepared by the runner.
phal_block_ctx_enter(ctx.phal, block_offset);
auto status = phal_read(ctx.phal, reg_offset, &actual);
phal_block_ctx_exit(ctx.phal, block_offset);
status |= dmem_read(ctx, dmem_offset, &actual);
```

Each access above moves one 32-bit word; offsets are byte offsets. PHAL starts
at the module's BAR0 base. Its block enter/exit adjusts `ctx.phal->env.base`,
so the callback reaches `module base + block offset + reg_offset` in BAR0.
`phal_read/write` call the registered PHAL 32-bit callbacks, which terminate
at `common::bar::read32/write32`. Callback logs show `env_base`, `offset`, the
complete `bar0_offset`, value, and native status at debug level; failures log
at error level. Pair block enter/exit before any dependent DMEM call, especially
on PCIe where module and aperture contexts can be the same PHAL instance.
The runner restores cached PHAL context bases after the testcase returns or
throws. `TestStatus` aliases `phal_status_t`, so PHAL and DMEM results can be
combined directly with `|` without status conversion.

DMEM uses a separate scratch base, currently `0x10000000`, and a 256 MiB window
on BAR4/aperture0/identity0. Its PCIe PHAL context is initialized at the PCIe
root in BAR0, independently of the current module. PHAL register callbacks
always access BAR0, including aperture configuration. DMEM payloads use BAR4
by default (or BAR2/BAR4 through the configurable window API). Every operation sets and verifies
the aperture before accessing the data window. The three-argument DMEM API
itself is unchanged. These bring-up scratch settings still need confirmation
on the hardware.

The same three-argument DMEM names also accept byte vectors:

```cpp
std::vector<uint8_t> expected(1024, 0xab);
std::vector<uint8_t> actual(expected.size());
auto status = dmem_write(ctx, 0x10200, expected);
if (status != PHAL_STATUS_OK) return status;
status = dmem_read(ctx, 0x10200, &actual);
```

A buffer call transfers exactly `vector.size()` bytes with one aperture setup,
not one setup per word. The caller sizes the read buffer beforehand; the API
does not resize it. Byte buffers support unaligned byte offsets and lengths;
the uint32 overloads still require 4-byte alignment. Empty/null buffers and
out-of-window ranges return INVALID; mapping/PHAL failures return ERROR.
Failed reads leave the output unchanged. The accessed range must also fit the
actual mapped BAR; a 256 MiB aperture does not enlarge the host BAR mapping.

PMU, DDP and ISI use hardcoded, read-only register examples:

- PMU enters EFUSE CTRL at block offset `0x2106000`, reads revision at
  offset `0x0`, and expects `0x01010001`. Its internal address is
  `0x18000000 + 0x2106000 = 0x1a106000`; the Atlas BAR0 policy base is
  `0x38000000`, so the actual BAR0 offset is `0x3a106000`.
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

PCIe, PMU, DDP and ISI return ERROR on a value mismatch and never call `phal_write`.
Their examples take no arguments. Each reads DMEM offset `0x0`, writes
`0x12345678`, reads it back, checks the value and restores the original word.
Run these examples only when that DMEM range is usable for writes.

For raw BAR0 debugging, `bar_read32` takes a PCIe-module-relative `--offset`
and adds the PCIe base. `bar_read32_abs` takes a required `--offset` and
reads that exact BAR0 offset without adding any module base. Like every case,
the runner prepares PHAL first, but the raw BAR read itself does not call it.
For example, `bar_read32_abs --target PCIE_0_0 --offset 0x36100000`
checks the known PCIe DID/VID location. It can also address DDP/ISI offsets
through the PCIe target, but it issues the same MMIO read as `phal_read` at that
location. In particular, do not read `0x32080800` on `pcie-b7` until the
hardware access that stalled that server is resolved.

```sh
./build/isml_diag example --target DDP_0_0
./build/isml_diag example --target DDP_0_1
./build/isml_diag example --target PCIE_0_0
./build/isml_diag example --target PMU_0_0
./build/isml_diag example --target ISI_0_0
```

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

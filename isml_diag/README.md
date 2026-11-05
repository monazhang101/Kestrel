# ISML Diagnostic Development

Commands below run from the repository root.

## Build

Linux builds require the complete ProtonHal/CSR tree under
`isml_ipc/external/`.

```bash
cmake -S . -B build-fpga
cmake --build build-fpga -j
# Both entrypoints use the shared module/testcase sources:
cmake --build build-fpga --target isml_diag isml_uart -j
```

For host DMA through `isml_drv`, build/load the driver on the tester:

```bash
make -C isml_kmod
sudo insmod isml_kmod/build/isml_drv.ko
```

## Write a testcase

1. Declare the method in `isml_diag/include/diag/modules/<Module>.h`.
2. Register its name, arguments and callback in
   the module registration using `_add_test()` (PMU file registration below).
3. Put examples in that module's `example.cpp`; add other source files to
   `CMakeLists.txt` when needed.

Registration defines defaults and argument formats. The module prefix is added
implicitly: `uart_read32_example` in PMU becomes `pmu_uart_read32_example`.

```cpp
_add_test("uart_read32_example", {{"addr", "", "u64"}},
          [this](TestInfo& ti) { return uart_read32_example(ti); });
```

Use `ti.args` (`common::args::get_u64/get_string`) for arguments and `ti.logger`
(`error/info/flow/regio`) for logs. Return a `PHAL_STATUS_*` value.

- Register IO: `phal_read/write(ti.phal_ctx, addr, ...)`. `isml_diag` supplies
  PCIe ctx; `isml_uart` supplies UART ctx.
- FW IPC IO: register `{CtxResource::ipc(fw)}` and use `ti.hal->ipc_context(fw)`.
- Device memory: allocate with `ti.hal->device_mem_alloc(region, size)`, then
  `mem_read/write(buffer, offset, ...)`.
- Host DMA memory: allocate with `ti.hal->host_mem_alloc(size)`.

PCIe testcase setup configures the default Atlas DMEM aperture: BAR4/aperture 0,
identity 0, target `0x10000000`, size `0x8000000000`. DMEM IO does not
reconfigure it. To change the mapping:

```cpp
aperture_set(*ti.hal, 4, 0, 0x10000000, 0x8000000000);
```

The testcase owns the mapping after reconfiguration.

Include `diag/core/HalContext.h` only for PCIe/DMA resources. Shared register
cases use `ti.phal_ctx`; `isml_uart` provides no bound PCIe HAL resources.

For PMU files, put all registrations in the `PMUModule` constructor using
`_add_test()`, as with the other modules. The same module and testcase sources
are linked into both executables; the executable selects the active backend.
Case owners ensure resource compatibility and adapt addresses using
`ti.transport`.

## Run testcases

```bash
./build-fpga/isml_diag discover
./build-fpga/isml_diag ddp_example --target TPU0 --log-level regio
./build-fpga/isml_diag pcie_dma_h2d_example --target TPU0
./build-fpga/isml_diag pcie_bar_read32 --target TPU0 --offset 0x36100000
```

`--target` defaults to `TPU0`. Use `--log-level error|info|flow|regio` and
`--policy-dir <path>` as needed. Multiword CLI flags use kebab case;
`--size-bytes` becomes `ti.args["size_bytes"]`.

For ordered parameter runs, add entries under the local testcase name in
`isml_diag/policies/testcases/<module>.yaml`, then run
`isml_diag <module_test_name> -all`. CMake copies policies beside the binaries.

## Run PMU UART examples

One terminal is sufficient. The tool automatically opens the unique CP210x
port at 115200 8N1. Zero or multiple matches produce an error; arrange the
tester mapping so one PMU endpoint is identifiable. No PCI scan, BAR mapping,
chip initialization, socket service, or separate console is needed.

```bash
./build-uart/isml_uart list
./build-uart/isml_uart pmu_uart_example --uart-log
./build-uart/isml_uart pmu_uart_read32_example --addr 0x02105060
./build-uart/isml_uart pmu_uart_write32_example --addr 0x02105060 --value 0x12345678
# The same source/case runs through PCIe:
./build-fpga/isml_diag pmu_uart_read32_example --addr 0x02105060
```

These example cases accept PMU-local byte addresses and adapt them to the
PCIe PMU RCF window when run by `isml_diag`. UART forwards addresses unchanged.
The fixed example checks PMU-ready and writes/reads REG0; firmware must permit
those addresses. This example intentionally overwrites REG0.

`--retry-count N` (default 3) means one send plus N retries;
`--retry-interval-ms` defaults to 100 and `--response-timeout-ms` to 5000.
Only a missing matching response causes a retry; retries reuse the same frame
and IDs. Firmware errors finish the request. Port loss fails without reconnect.

`--uart-log` saves current-session PMU LOG text and printable boot text to a
new `logs/pmu_uart_<timestamp>_<pid>_<session>.log` per run. Without the flag,
logs are drained and discarded. Responses return to the case; `--log-level regio` prints register traffic. LOG/RSP session filtering follows the reference
script; current-session LOG bodies are retained. Logging continues between
cases and during retry waits, and flushes on normal exit. File errors or queue
overflow are reported.

For a standalone release, ship `isml_uart`; include `policies/` if using `-all`.
Do not ship the old console executable or require the PCIe kernel driver.

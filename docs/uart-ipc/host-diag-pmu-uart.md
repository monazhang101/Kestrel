# Host Diagnostic PMU UART 设计与使用说明

本文描述当前 `spev_uart` 的已实现架构、功能、接口、协议和使用方式。它面向
host diagnostic 开发者和 testcase 开发者，以当前源码和上机验证结果为准。

> 当前方案是单进程、单终端、单串口所有者。`spev_uart` 直接打开 PMU UART，
> 同一进程内完成 testcase 执行、PMU LOG 接收、日志落盘和交互式寄存器调试。
> 不需要独立 console 进程、Unix socket、client-server 或其他进程间通信。

## 1. 目标与设计结论

PMU UART 功能需要同时覆盖三类使用场景：

1. 不运行 testcase，直接进入交互 console，执行寄存器读写。
2. 通过 UART 运行一个 testcase，完成后退出，可选保存 PMU UART 日志。
3. 通过 UART 运行 testcase，完成后保持同一进程和 UART session，切换到交互
   console 继续调试。

当前设计的核心结论如下：

- `spev_uart` 是唯一入口，也是串口的唯一 owner。
- testcase 和 console 共用一个 `UartBridge`、一个串口 fd 和一个 UART session。
- testcase 继续使用框架统一的 `phal_read()`/`phal_write()` API；UART 只是 PHAL
  register backend 的另一种实现。
- `spev_uart` 保留完整的 TPU module/testcase catalog。testcase 是否能通过 UART
  运行，由 testcase 所需资源和实现决定，而不是由其所属 module 决定。
- PMU UART 原始 LOG、testcase 的 `Logger` 输出和 CMD/RSP 是三类不同数据，显示
  与保存策略相互独立。
- 串口事务串行执行，避免多个 caller 交错 CMD、重试和 RSP 匹配。
- 当前不引入常驻服务、socket 或多进程仲裁。一个进程退出后释放串口；下一次
  运行创建新的 session。

## 2. 总体架构

```text
                         one spev_uart process
+---------------------------------------------------------------------+
| uart_main.cpp                                                       |
|  - CLI/mode selection                                               |
|  - testcase catalog and execution                                   |
|  - console live-log display policy                                  |
|                                                                     |
| testcase path                         console path                   |
| Testcase -> phal_read/write           UartRepl -> RD/WR              |
|             |                              |                         |
|             v                              v                         |
|       Uart PHAL adapter --------------> UartBridge                   |
|       - PHAL hw_ops                     - serial ownership           |
|       - status mapping                  - RX worker                  |
|                                         - transaction/retry          |
|                                         - LOG callback/file writer   |
|                                                 |                   |
|                                         UartProtocol                 |
|                                         - COBS/CRC/frame             |
|                                         - session/request match      |
+-------------------------------------------------|-------------------+
                                                  |
                                      CP210x / 115200 8N1
                                                  |
                                             PMU firmware
                                      CMD -> register access
                                      RSP <- result/value
                                      LOG <- firmware text
```

### 2.1 组件职责

| 组件 | 主要职责 |
| --- | --- |
| `spev_diag/cli/uart_main.cpp` | 参数解析、三种模式选择、构造 testcase catalog、创建并复用 `UartBridge`、控制 PMU LOG 是否显示到 terminal |
| `spev_diag/include/diag/core/UartBridge.h` | 对上层暴露串口 session、32-bit read/write、日志路径和诊断信息 |
| `spev_diag/src/core/UartBridge.cpp` | CP210x 发现、串口独占、termios 配置、RX worker、事务串行化、重试、文件日志 |
| `spev_diag/src/uart/UartProtocol.*` | UART IPC 帧编码/解码、session/request 过滤、transaction 状态机 |
| `spev_diag/include/diag/core/UartRepl.h`、`src/uart/UartRepl.cpp` | `uart> ` 交互循环、`RD/WR/HELP/EXIT` 解析和输出 |
| `spev_diag/src/core/UartPhalBridge.cpp` | 将 PHAL `read/write` callback 适配为 `UartBridge::read/write`，并映射状态码 |
| `abios/common/uart_ipc/` | host 与 PMU firmware 共用的 UART IPC wire protocol 定义和 C 编解码实现 |

### 2.2 为什么不采用 client-server

当前需求是一个开发者、一个 terminal、一个前台运行过程。串口本身也是只能被一个
owner 安全控制的有状态资源。在这个边界内，client-server 不会增加 testcase 或
console 的能力，反而会增加服务生命周期、socket 协议、权限、错误传播和部署成本。

因此，当前 `UartBridge` 的生命周期就是本次运行的 session 边界。只有以后明确需要
多个进程共享同一串口、后台常驻采集或跨用户访问时，才需要重新评估 daemon 和 IPC。

## 3. 三种运行模式

### 3.1 模式一：纯交互 console

```bash
./build-uart/spev_uart console
./build-uart/spev_uart console --uart-log
```

行为：

- 不创建或运行 testcase。
- 打开 UART 后立即显示 `uart> `。
- 当前 session 的 PMU LOG 立即显示到 terminal。
- `--uart-log` 可选；启用后，从串口打开直到 `EXIT` 的 PMU LOG 和可打印 boot
  text 都写入文件。
- `EXIT` 结束 REPL、flush 日志、关闭串口并退出进程。

适用于现场寄存器检查、固件 bring-up 和系统异常后的人工调试。

### 3.2 模式二：只运行 testcase

```bash
./build-uart/spev_uart pmu_uart_example
./build-uart/spev_uart pmu_uart_example --uart-log
./build-uart/spev_uart pmu_uart_read32_example --addr 0x02105060
```

行为：

- CLI 参数先完成校验；真正执行 testcase 时才打开串口。
- testcase 通过 `ti.phal_ctx` 发起 UART register transaction。
- terminal 只显示 testcase 的 `error/info/flow/regio` 日志和最终
  `[PASS]`/失败状态，不显示原始 PMU UART LOG。
- 使用 `--uart-log` 时，原始 PMU LOG 在后台持续写文件，但仍不显示到 terminal。
- testcase 完成后 flush 日志、关闭串口并退出。

适用于自动化测试、CI 或只关心 testcase 结果的运行。

### 3.3 模式三：testcase 完成后进入 console

```bash
./build-uart/spev_uart pmu_uart_example --uart-console
./build-uart/spev_uart pmu_uart_example --uart-console --uart-log
```

行为：

- testcase 阶段与模式二相同。
- testcase 返回后不关闭串口，不创建新进程，也不创建新 session。
- 无论 testcase 是 PASS、失败还是 timeout，只要 UART 已成功打开，就进入 console。
- 切换时先显示 `uart> `，testcase 尾部迟到的 PMU LOG 不显示到 terminal。
- 用户提交第一条结构完整、数值可解析的 `RD` 或 `WR` 后，才开启 console 的
  PMU live LOG。
- `--uart-log` 的文件记录不受 terminal 显示门控影响，会连续覆盖 testcase、切换
  边界和 console，直到 `EXIT`。

这个模式用于“先复现 testcase，再在同一硬件状态和同一 UART session 中继续调试”。

### 3.4 三种模式的行为对比

| 行为 | 纯 console | testcase only | testcase + console |
| --- | --- | --- | --- |
| 运行 testcase | 否 | 是 | 是 |
| testcase 后进程退出 | `EXIT` 后 | 是 | 否，先进入 console |
| UART session | console 全程一个 | testcase 全程一个 | testcase 与 console 共用一个 |
| terminal 显示 testcase Logger | 不适用 | 是 | 是 |
| testcase 阶段显示 PMU LOG | 不适用 | 否 | 否 |
| console 显示 PMU LOG | 立即显示 | 不适用 | 第一条可解析的 `RD/WR` 后显示 |
| `--uart-log` | 支持，记录到 `EXIT` | 支持，记录到 testcase 结束 | 支持，连续记录到 `EXIT` |

## 4. Console 命令

命令不区分大小写，数值支持 C/C++ 风格的十进制或 `0x` 十六进制输入。

```text
RD <addr>
WR <addr> <value>
HELP
EXIT
```

示例：

```text
uart> RD 0x02105060
RD 0x2105060 = 0x12345678
uart> WR 0x02105060 0xaabbccdd
WR 0x2105060 = 0xaabbccdd OK
uart> EXIT
```

约束：

- `RD` 和 `WR` 都是 32-bit register 操作。
- `addr` 必须 4-byte aligned。
- `value` 必须在 `uint32_t` 范围内。
- 空行只重新显示 prompt；非法命令或参数会打印 `ERROR:`，不会退出 console。
- `on_command` 只在命令结构完整、数值可解析的 `RD/WR` 上触发，`HELP`、空行、
  未知命令和参数解析失败不会开启转换模式的 PMU live LOG。地址 alignment 和实际
  transaction 结果由随后调用的 `UartBridge` 判断，因此可解析但未对齐的地址仍会
  先触发 `on_command`，再返回 `Invalid`。

## 5. CLI 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--target TPU0` | `TPU0` | 当前 `spev_uart` 只暴露逻辑 target `TPU0` |
| `--uart-console` | 关闭 | testcase 完成后进入 console；不能与 `console` 子命令同时使用 |
| `--uart-log` | 关闭 | 将 PMU LOG/可打印 boot text 保存到独立文件 |
| `--retry-count N` | `3` | 初次发送之后最多再重试 N 次 |
| `--retry-interval-ms N` | `100` | 未收到匹配响应时，两次发送之间的等待时间 |
| `--response-timeout-ms N` | `5000` | 每次发送等待匹配 RSP 的时间 |
| `--log-level LEVEL` | `info` | testcase/host 日志级别：`error/info/flow/regio` |
| `--policy-dir PATH` | executable 附近的 policies | `-all` 参数组合的 policy 根目录；纯 console 不接受 |
| `-all` | 关闭 | 按 testcase policy 顺序执行多组参数；同一 UART session 跨所有 run 复用 |

`--uart-log` 和 `--log-level` 控制不同的数据：

- `--uart-log`：PMU firmware 通过 UART 发来的 LOG 和可打印 boot text。
- `--log-level`：host testcase/框架的 `Logger` 输出；`regio` 还会显示成功的
  `uart_read`/`uart_write` host transaction 摘要。

## 6. Testcase 执行模型

### 6.1 Testcase catalog 与 module

UART 是 register transport，不是 testcase module。`spev_uart` 构造一个轻量的
`TPUDevice` catalog，其中包含：

- `PCIE_0`
- `PMU_0`
- `DDP_0` 到 `DDP_3`
- `ISI_0` 到 `ISI_7`

因此，各 module 注册的 testcase 都可以出现在 `spev_uart list` 中，并按
`<module>_<local_test_name>` 路由到所属 module。是否能够真正通过 UART 执行取决于
case 的资源需求：

- 只依赖 register `phal_read/write`，并正确处理 UART 地址语义的 case 可以支持。
- 依赖 BAR mapping、DMA buffer、device memory、PCI discovery 或其他 PCIe-only
  `HalContext` 资源的 case 不能因为出现在 catalog 中就自动获得 UART 能力。
- 不支持 UART 的 case 应明确返回 `PHAL_STATUS_UNIMPLEMENTED` 或其他准确错误，
  不能静默走错误的 backend。

### 6.2 同一个 testcase 适配 PCIe 与 UART

两个 executable 链接同一份 module/testcase 源码：

```text
spev_diag -> PCIe PHAL context -> BAR/RCF
spev_uart -> UART PHAL context -> PMU firmware UART IPC
```

框架通过 `TestInfo` 提供：

```cpp
phal_ctx_t* phal_ctx;
RegisterTransport transport;  // Pcie or Uart
```

共享 case 继续调用：

```cpp
uint32_t value = 0;
auto status = phal_read(ti.phal_ctx, address, &value);
status = phal_write(ti.phal_ctx, address, value);
```

如果 PCIe 和 UART 的地址视图不同，地址转换属于 testcase/硬件语义，而不是
`UartBridge`。例如当前 PMU UART examples 接收 PMU-local absolute byte address：

- UART：地址原样发送，UART PHAL context 的 `env.base = 0`。
- PCIe：case 增加 PMU core/RCF window offset，再通过 PCIe PHAL context 访问。

这保证 transport 层不隐式添加 host BAR translation。

### 6.3 Testcase 运行生命周期

```text
validate CLI/testcase args
        |
open UartBridge once (first run)
        |
for each policy run:
    create_uart_context()       <- per-case PHAL context
    set ti.phal_ctx
    set ti.transport = Uart
    execute testcase
    destroy PHAL context
        |
optional: enter console         <- same UartBridge/session
        |
close bridge, flush log, report diagnostics
```

串口 session 跨 `-all` 的多组 testcase run 复用，但 PHAL context 每个 run 独立创建
和销毁。这样既保留 request ID/session 连续性，也避免 testcase context 生命周期泄漏。

## 7. Host API

### 7.1 `uart::RetryConfig`

```cpp
struct RetryConfig {
    uint32_t retry_count = 3;
    uint32_t retry_interval_ms = 100;
    uint32_t response_timeout_ms = 5000;
    void validate() const;
};
```

`retry_count` 是“初次发送后的重试次数”，所以默认最多发送 4 次。配置必须保证
timeout 非零且完整时间计算不会溢出。

### 7.2 `uart::Config`

```cpp
struct Config {
    uint32_t baud = 115200;
    RetryConfig retry;
    bool capture_log = false;
    std::function<void(const std::string&)> on_log;
    std::string port;
};
```

- `capture_log` 控制文件保存。
- `on_log` 控制 live consumer，当前 CLI 用它显示 terminal；回调在 RX worker
  线程且 bridge state lock 内执行，必须保持短小、线程安全，并且不能重入
  `UartBridge::read/write/close`。
- 文件保存和 `on_log` 完全独立。terminal 门控不会丢失已经启用的文件日志。
- CLI 将 `port` 留空，由实现自动选择唯一 CP210x endpoint；测试可注入 PTY。

### 7.3 `UartBridge`

```cpp
bool open(const uart::Config&, std::string* error = nullptr);
void close();
UartResponse read(uint64_t address);
UartResponse write(uint64_t address, uint32_t value);
std::string port() const;
std::string log_path() const;
std::string diagnostics() const;
```

语义：

- `open()` 创建一个 session、独占并配置串口、启动 RX worker 和可选 log worker。
- `read()`/`write()` 是同步接口，直到收到匹配 RSP、timeout 或 transport failure。
- 所有 caller 由 transaction mutex 串行化；调用方不会在一个串口上并发交错请求。
- `close()` 停止 worker、结束未完成事务、flush log、恢复 termios 并释放串口。
- `diagnostics()` 汇总串口故障、坏帧、超长帧、旧 session 帧和日志丢字节。
- 一个 `UartBridge` 实例只承载一次 open/close 生命周期；`close()` 后需要新建实例，
  不能在同一对象上再次 `open()`。

`UartResponse` 将 host transport 结果与 firmware status 分开：

```cpp
struct UartResponse {
    UartResult result;
    uint32_t firmware_status;
    uint32_t value;
    std::string error;
};
```

| `UartResult` | 含义 |
| --- | --- |
| `Ok` | 收到匹配 RSP 且 firmware status 为 OK |
| `Timeout` | 所有发送和重试均未收到匹配 RSP |
| `Invalid` | host 请求非法，例如地址未 4-byte aligned |
| `Transport` | 串口未打开、断开、写失败、session 关闭或 request ID 耗尽 |
| `Firmware` | 收到匹配 RSP，但 firmware 返回非零 status |

### 7.4 UART PHAL adapter

```cpp
PhalBridge::ScopedContext create_uart_context(
    const DeviceContext&,
    PhalProject,
    std::string* error,
    std::shared_ptr<UartBridge> uart);
```

adapter 创建 `env.base = 0` 的 PHAL context，并提供：

- `hw_ops.read` -> `UartBridge::read()`
- `hw_ops.write` -> `UartBridge::write()`
- `hw_ops.delay_us/delay_ns` -> host sleep

wire status 到 PHAL status 的映射如下：

| UART/firmware 结果 | PHAL status |
| --- | --- |
| host `Ok` / firmware `OK(0)` | `PHAL_STATUS_OK` |
| host timeout | `PHAL_STATUS_TIMEOUT` |
| host invalid | `PHAL_STATUS_INVALID` |
| firmware `INVALID(1)` | `PHAL_STATUS_INVALID` |
| firmware `TIMEOUT(2)` | `PHAL_STATUS_TIMEOUT` |
| firmware `UNKNOWN(4)` / `BUSY(8)` | `PHAL_STATUS_ERROR` |
| transport failure | `PHAL_STATUS_ERROR` |

成功的 register transaction 只在 `regio` 级别输出；错误始终以 error 级别输出。

### 7.5 `uart::run_repl`

```cpp
void run_repl(UartBridge&,
              std::istream&,
              std::ostream&,
              PromptStateHandler on_prompt_state = {},
              CommandHandler on_command = {});
```

- `on_prompt_state(true/false)` 告诉 CLI 当前是否已打印 prompt 并等待输入，便于异步
  PMU LOG 到达时清行、打印 LOG、再恢复 `uart> `。
- `on_command()` 在结构完整且数值可解析的 `RD/WR` 调用 bridge 前触发。转换模式
  利用它建立 testcase 尾部与 console live LOG 之间的确定性边界；它不表示随后
  的 transaction 一定通过 alignment 检查或执行成功。

## 8. UART IPC 协议

协议权威定义在 `abios/common/uart_ipc/uart_ipc_proto.h`，host 与 firmware 共用。

### 8.1 Wire frame

```text
0x00 | COBS(header + payload + CRC32) | 0x00
```

- 多字节字段使用 little-endian。
- CRC 为 CRC-32/ISO-HDLC，覆盖 header + payload。
- 最大 payload 为 256 bytes，最大 wire frame 为 280 bytes。

Header 固定 16 bytes：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `version` | `u8` | 当前为 1 |
| `type` | `u8` | `CMD=1`、`RSP=2`、`LOG=3` |
| `payload_len` | `u16` | CMD=16、RSP=8、LOG=8..256 |
| `session_id` | `u64` | 本次 `UartBridge::open()` 的非零 session |
| `request_id` | `u32` | CMD/RSP 非零；LOG 必须为 0 |

Payload：

```text
CMD: opcode u32 | address u64 | data u32
RSP: status u32 | data u32
LOG: sequence u32 | dropped_total u32 | text bytes
```

`opcode=0` 为 read，`opcode=1` 为 write。

### 8.2 Session 与 request matching

- 每次 `open()` 生成新的非零随机 session，并避开协议保留的 V1 固定值。
- request ID 从 1 开始，在同一 session 内单调增加且不复用。
- RSP 必须同时匹配当前 session 和当前 request ID 才能完成 transaction。
- LOG 必须匹配当前 session，且 request ID 为 0，才会进入 live/file log。
- 其他 session 的 LOG/RSP 被丢弃并计入 `stale_session_frames`。
- 坏 CRC、非法类型/长度、错误 request ID 规则或超长帧被丢弃并计数。

### 8.3 Retry 语义

只有“没有收到匹配 RSP”会触发重试。重试原样发送同一个 wire frame，即 session、
request ID 和 payload 全部不变，从而利用 firmware 去重语义避免重复执行 write。

以下情况不重试：

- firmware 返回明确的非零 status；
- 串口断开或写失败；
- host 参数非法。

timeout 后命令结果可能未知，特别是 write：host 无法区分命令从未执行，还是 firmware
已经执行但 RSP 丢失。因此错误文本明确包含 `command outcome may be unknown`。

## 9. 串口与线程模型

### 9.1 Port discovery 与独占

CLI 在 Linux `/sys/class/tty` 中向上查找绑定 `cp210x` driver 的 endpoint：

- 恰好一个：自动使用对应 `/dev/tty*`。
- 零个：报 `no CP210x UART endpoint found`。
- 多个：拒绝猜测并列出候选，tester 必须提供可唯一识别的映射。

串口使用 `flock(LOCK_EX | LOCK_NB)` 和 `TIOCEXCL` 双重独占。参数为 raw mode、8 data
bits、no parity、1 stop bit、无软/硬件流控；默认 baud 为 115200。关闭时恢复原 termios。

当前 production serial transport 仅支持 Linux。

### 9.2 Worker 与锁

一个打开的 `UartBridge` 包含：

- 调用线程：执行同步 `read/write` 并等待结果。
- RX worker：`poll/read` 串口、解帧、分发 LOG/RSP、推进 timeout/retry 状态机。
- 可选 log worker：异步将 PMU text 写入文件，避免文件 IO 阻塞 UART RX。

关键同步边界：

- `callers` mutex：一个完整 transaction，包括等待和重试期间，都不允许另一 caller
  插入。
- `state` mutex + condition variable：保护 transaction、停止状态和 transport failure。
- log mutex + bounded pending buffer：保护异步日志队列。

## 10. PMU LOG 与 terminal 显示

### 10.1 文件日志

启用 `--uart-log` 后创建：

```text
logs/pmu_uart_<YYYYMMDD_HHMMSS>_<pid>_<session-hex>.log
```

文件包含当前 session 的 PMU LOG body 和识别出的可打印 boot text，不包含 CMD/RSP、
testcase Logger 前缀或 `uart> ` prompt。

pending buffer 上限为 64 KiB。文件写失败或生产速度超过写盘速度时不会无限增长内存；
丢失字节数会在关闭后的 diagnostics 中报告。

### 10.2 Live LOG 与 prompt 重绘

PMU LOG 从 RX worker 异步到达。console 正在等待输入时，CLI 会：

1. 使用 `\r\033[2K` 清除当前 prompt 行；
2. 输出 PMU LOG，并在必要时补换行；
3. 重新输出 `uart> `。

纯 console 从一开始就允许 live LOG。testcase + console 使用两个 atomic state：

- `console_active`：决定 `on_log` 是否向 terminal 输出。
- `prompt_visible`：决定是否需要清行和恢复 prompt。

转换模式调用 `enter_console(true)`，先令 `console_active=false`；第一条可解析的
`RD/WR` 触发 `on_command()` 后才设为 true。这个边界不依赖固定 delay，文件日志
也不受影响。

## 11. 构建、发布与使用示例

### 11.1 构建

```bash
cmake -S . -B build-uart
cmake --build build-uart --target spev_uart -j
```

UART transport 由两个静态库组成：

- `spev_uart_protocol`：共享 C 协议、CRC32 和 COBS。
- `spev_uart_transport`：`UartBridge`、`UartProtocol` 和 `UartRepl`。

### 11.2 查询 testcase

```bash
./build-uart/spev_uart list
```

### 11.3 常见运行方式

```bash
# 只进入 console
./build-uart/spev_uart console --uart-log

# testcase 完成后退出
./build-uart/spev_uart pmu_uart_example --uart-log

# testcase 完成后保留现场并进入 console
./build-uart/spev_uart pmu_uart_example --uart-console --uart-log

# 单独读写 PMU-local register
./build-uart/spev_uart pmu_uart_read32_example \
    --addr 0x02105060 --log-level regio
./build-uart/spev_uart pmu_uart_write32_example \
    --addr 0x02105060 --value 0x12345678 --log-level regio

# 按 policy 运行多组参数，然后进入 console
./build-uart/spev_uart <module_test_name> -all --uart-console --uart-log
```

当前已知 PMU example 地址：

| 地址 | 含义 | 已知值/用途 |
| --- | --- | --- |
| `0x0210205c` | PMU system-ready | 期望 `0x5b5b5b5b` |
| `0x02105060` | PMU_DIAG REG0 | read/write example；`pmu_uart_example` 会写 `0x12345678` |

这些是 PMU-local absolute byte addresses。写寄存器前必须确认 firmware 和硬件允许访问；
`pmu_uart_example` 会覆盖 REG0，不会恢复原值。

### 11.4 发布边界

独立 UART 工具发布至少包含：

- `spev_uart`
- 其动态依赖和 ProtonHal/CSR 运行依赖
- 使用 `-all` 时所需的 `policies/`

不需要发布旧的独立 console executable，不需要启动 socket service，也不依赖 PCIe
kernel driver。

## 12. 错误处理与排障

| 现象 | 可能原因/检查方向 |
| --- | --- |
| 找不到 UART | 检查 CP210x 枚举、driver binding 和 `/sys/class/tty` |
| 检测到多个 UART | 当前 CLI 不猜测 PMU port；调整 tester mapping，使目标唯一可识别 |
| `UART already owned` | 另一个进程持有 flock/TIOCEXCL；关闭旧工具或查找 owner |
| RD/WR timeout | firmware 未响应、地址不可访问、RX 链路异常；检查 `--uart-log` 和 diagnostics |
| write timeout | 结果可能未知；不要盲目认为 write 没有执行 |
| `stale_session_frames` 增长 | 收到旧 session 或其他 owner 遗留帧；这些帧不会完成当前请求 |
| `bad_frames`/`oversized_frames` 增长 | 检查 baud、线路质量、firmware 协议版本和 frame 边界 |
| PMU LOG 未显示但文件存在 | testcase 阶段或转换模式第一条可解析 RD/WR 前，这是预期的 terminal 门控 |
| testcase 出现在 list 但 UART 运行失败 | case 可能依赖 PCIe-only 资源，需实现 UART path 或明确返回 UNIMPLEMENTED |

串口 open 成功只证明 host 获得了 endpoint，不证明 PMU firmware 已经能收发。若一直没有
任何 RX 字节，应先检查 firmware 初始化、物理链路和 baud，再检查 host 的 RSP matching。

## 13. 验证范围

当前实现具有以下自动化覆盖：

- COBS/CRC golden vectors 和 CMD frame 编码。
- LOG/RSP 解码、坏帧/超长帧/旧 session 过滤。
- timeout、retry、duplicate/old RSP 和 firmware error 状态机。
- UART/PCIe 共享 testcase 的地址转换约定。
- REPL prompt 状态和命令解析。
- Linux PTY 上的真实 termios、串口独占、PHAL read/write、重试、异步 LOG、文件日志、
  多 caller 串行化、断线和重新打开 session。
- CLI 参数组合与 testcase catalog。

三种运行模式及 testcase 到 console 的切换路径已经在 tester 上完成验证。

## 14. 当前限制

- 仅支持 Linux production serial transport。
- CLI 当前只支持逻辑 target `TPU0`，并要求自动发现唯一 CP210x endpoint。
- 一个进程只维护一个 UART session；断线后不自动 reconnect。
- register transaction 全局串行，不提供并行 CMD pipeline。
- console 只提供 `RD/WR/HELP/EXIT`，不在 REPL 内动态运行 testcase。
- testcase catalog 是全量的，但 UART backend 不会自动提供 BAR、DMA、device memory 等
  PCIe-only 能力。
- 当前方案不支持多进程共享 UART；若未来需要 daemon/client，必须先定义 ownership、
  session、request attribution、log fan-out 和故障恢复契约，而不是直接在现有接口外套 socket。

## 15. 相关源码与文档

- `spev_diag/cli/uart_main.cpp`
- `spev_diag/include/diag/core/UartBridge.h`
- `spev_diag/include/diag/core/UartRepl.h`
- `spev_diag/src/core/UartBridge.cpp`
- `spev_diag/src/core/UartPhalBridge.cpp`
- `spev_diag/src/uart/UartProtocol.h/.cpp`
- `spev_diag/src/uart/UartRepl.cpp`
- `abios/common/uart_ipc/uart_ipc_proto.h`
- `abios/common/uart_ipc/uart_ipc_rx.h`
- `docs/uart-ipc/host_cpp_guide.md`

其中 `uart_ipc_proto.h` 是 wire protocol 的权威定义；本文是 host diagnostic 当前架构和
运行行为的权威说明。`host_cpp_guide.md` 可用于理解底层 C 协议如何接入其他 C++ host。

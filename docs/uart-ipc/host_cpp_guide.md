# C++ Host 如何对接 PMU UART IPC

给 PC 侧 C++ 程序（上位机/测试工具）看的对接说明：怎么把我们的 C 协议代码编进 C++ 工程、怎么调用、串口上怎么收发。协议本身的权威定义在 `common/uart_ipc/uart_ipc_proto.h`（固件与 host 同源共用），本文只讲“怎么用”。

## 1. 拿哪些源码

协议层是纯 C、无固件依赖（不碰 FreeRTOS、不碰寄存器），直接把下面 4 组文件加进 C++ 工程一起编译（`abios/` = 本仓库根下的 `abios/` 目录）：

| 文件 | 作用 |
| --- | --- |
| `abios/common/uart_ipc/uart_ipc_proto.c/.h` | 帧编解码（encode/decode） |
| `abios/common/uart_ipc/uart_ipc_rx.c/.h` | 收帧状态机（找 0x00 边界，可选） |
| `abios/common/crc/crc32.c/.h` | CRC-32 校验 |
| `abios/external/cobs/src/cobs.c`、`abios/external/cobs/src/include/cobs.h` | COBS 编码 |

include 路径要加 3 个目录（cobs 的头在 `src/include/` 下，不在 `src/` 旁边）：

```text
-I<仓库根>/abios/common/uart_ipc
-I<仓库根>/abios/common/crc
-I<仓库根>/abios/external/cobs/src/include
```

## 2. extern "C"——最关键的适配点

4 个头文件里只有 `cobs.h` 自带 `extern "C"` guard，`uart_ipc_proto.h` 和 `crc32.h` 都没有。C++ 直接 include 会按 C++ 名字修饰去找符号，链接报 undefined reference。在自己工程里包一层：

```cpp
extern "C" {
#include "uart_ipc_proto.h"
#include "crc32.h"
#include "cobs.h"
}
```

注意：不要去改仓库里的头文件加 guard——固件构建（纯 C）不需要，host 侧在自己工程里包就够。（`uart_ipc.h` 有 guard，但它是固件侧通道模块的头，host 不需要它。）

## 3. 帧格式（速查）

```text
wire = 0x00 | COBS(header 16B + payload + crc 4B) | 0x00
```

- 所有多字节字段小端；起始/结束各一个 `0x00` 做帧边界。
- CRC-32/ISO-HDLC 算 header+payload（和 zlib.crc32 同结果）。
- header：`version u8(=1) / type u8 / payload_len u16 / session_id u64 / request_id u32`。
- type：1=CMD (payload 16B)、2=RSP (8B)、3=LOG (8..256B)。
- CMD payload：`opcode u32(0=RD 1=WR) / addr_low u32 / addr_high u32 / data u32`。
- RSP payload：`status u32 / data u32`。
- 错误码：0=OK，1=INVALID，2=TIMEOUT，4=UNKNOWN，8=BUSY。

## 4. 发一条 RD 命令并收响应（完整例子）

```cpp
#include <cstdint>
#include <cstring>
extern "C" {
#include "uart_ipc_proto.h"
}

// session_id：一次连接一个值。重开串口/程序重启时必须换新值，
// 固件靠它识别“新的一轮”，去重表才会清零，request_id 才能从头计。
// V1 固定用这个值；重连时把它 +1。
static uint64_t s_session = UART_IPC_SESSION_V1;  /* 0x0000000100000001 */

bool send_cmd_rd(uint32_t request_id, uint64_t addr,
                 uint8_t *wire_out, size_t *wire_len)
{
    uart_ipc_hdr_t hdr = {};
    hdr.version     = UART_IPC_VERSION;
    hdr.type        = UART_IPC_TYPE_CMD;
    hdr.payload_len = 16;
    hdr.session_id  = s_session;
    hdr.request_id  = request_id;      /* 必须非零；同一轮连接内递增 */

    uart_ipc_cmd_t cmd = {};
    cmd.opcode   = 0;                  /* 0=RD */
    cmd.addr_low = (uint32_t)(addr & 0xffffffffu);
    cmd.addr_high = (uint32_t)(addr >> 32);

    /* payload 按小端逐字段摆进 16B 缓冲（不要 memcpy 结构体：
       对齐/填充不可控） */
    uint8_t payload[16];
    auto put32 = [&](size_t off, uint32_t v) {
        payload[off]     = (uint8_t)(v);
        payload[off + 1] = (uint8_t)(v >> 8);
        payload[off + 2] = (uint8_t)(v >> 16);
        payload[off + 3] = (uint8_t)(v >> 24);
    };
    put32(0, cmd.opcode);
    put32(4, cmd.addr_low);
    put32(8, cmd.addr_high);
    put32(12, cmd.data);

    /* wire 缓冲必须 >= UART_IPC_MAX_WIRE(280) */
    return uart_ipc_encode(&hdr, payload, wire_out,
                           UART_IPC_MAX_WIRE, wire_len) == UART_IPC_OK;
}
```

收的方向：固件回的也是 `0x00 ... 0x00` 定界的 wire 帧。host 侧先按 `0x00` 找边界（同源状态机 `uart_ipc_rx.h` 可直接用），对两个 `0x00` 之间的部分做 `cobs_decode` 得到 raw，再：

```cpp
extern "C" {
#include "uart_ipc_proto.h"
#include "cobs.h"
}

bool parse_rsp(const uint8_t *raw, size_t raw_len,
               uint32_t *status, uint32_t *data, uint32_t *request_id)
{
    uart_ipc_hdr_t hdr;
    uint8_t payload[UART_IPC_MAX_PAYLOAD];
    if (uart_ipc_decode(raw, raw_len, &hdr, payload,
                        sizeof payload) != UART_IPC_OK) {
        return false;                  /* 版本/CRC/长度不对，坏帧 */
    }
    if (hdr.type != UART_IPC_TYPE_RSP) {
        return false;                  /* LOG 帧不是响应，走日志分支 */
    }
    /* payload 前 4B=status，后 4B=data（小端读出） */
    ...
    *request_id = hdr.request_id;      /* 拿它对上是哪条命令 */
    return true;
}
```

## 5. 串口对接

- 参数：**115200 8N1 无流控**。
- 发：把 `uart_ipc_encode` 出来的整段 wire（含首尾 `0x00`）原样 write 到串口。
- 收：串口是字节流，自己按 `0x00` 定界攒帧；攒够一帧再解。
- host 与固件之间没有“握手指令”——固件上电初始化完就在收，host 发第一条 CMD 就行。

## 6. request_id / session_id 规矩（踩坑高发区）

- `request_id` 必须非零，**同一轮连接内只增不重**（跳着增没关系，回头重复旧 id 会被固件去重表拦掉不执行）。
- 想让固件重发某个结果：原样重发同一帧（同 id 同内容），固件回缓存的结果，不会重复执行。
- 串口重开/程序重启：`session_id` 换新值（如 +1），`request_id` 从头计。固件看到新 session 会清去重表。
- LOG 帧 `request_id=0`，host 别用它做关联。

## 7. 现成参考（在 PC 上跑过的）

- **Python 版完整 host**（本次板上验收就是它跑的，发帧/收帧/判定全套）：`abios/pmu/test/pmu/common/uart_ipc_host_sim.py`（用法：`python3 uart_ipc_host_sim.py --port /dev/ttyUSBx --mode echo`，支持 echo / crc-error / dedup / burst / stress / session-rebuild；`--self-test` 不接板子自测帧格式）
- 帧格式有疑问时，协议权威定义只有一个：`abios/common/uart_ipc/uart_ipc_proto.h`（固件与 host 同源共用，不存在第二份描述）。

写 C++ host 时拿 py 脚本当行为基准、C 测试当 API 用法样例，两边都和固件同源，不存在三份真相。

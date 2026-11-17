# PCIE_DMA_HAS

## Appendix 5.10 HQC 操作流程

### HQC Queue 寄存器寻址规则（重要）

HQC queue 寄存器没有固定地址的寄存器，而是对每个 queue 先配置一个基址
`queue*_reg_addr`：

- MEM queue 基址指针为 `MEM_CP_QUEUE_i.mem_queue_regs_addr_i`。
- COMP queue 基址指针为 `COMP_CP_QUEUE_i.comp_queue_regs_addr_i`。
- Golden 源为 `PCIE_REG_DE/reg_src/PCIE_HQC_REG.xlsx`。

随后各 queue 配置寄存器按下表中的固定偏移寻址。

| 偏移（`queue*_reg_addr +`） | 寄存器 | 说明 |
|---|---|---|
| `'h0` | `queue_fetch_direction` | 指示 fetch 的搬运方向（0: H2S；1: S2H；2: D2S；3: S2D） |
| `'h4` | `queue_doorbell_addr (low)` | doorbell 地址（生产者维护），软件写此地址视为 doorbell |
| `'h8` | `queue_doorbell_addr (high)` | doorbell 地址高 32 bit |
| `'hC` | `queue_doorbell_credit` | doorbell 信用值，value 非 0 代表软件可以 doorbell |
| `'h10` | `queue_fetch_src_addr (low)` | 预取数据地址，fetch_ctrl 据此读取数据（生产者维护） |
| `'h14` | `queue_fetch_src_addr (high)` | 预取数据地址高 32 bit |
| `'h18` | `queue_fetch_src_size` | 生产者预取数据大小寄存器（生产者维护） |
| `'h1C` | `queue_fetch_dst_ring_base (low)` | 消费者将预取数据写到的目的 ring 基地址（消费者初始化配置） |
| `'h20` | `queue_fetch_dst_ring_base (high)` | 目的 ring 基地址高 32 bit |
| `'h24` | `queue_fetch_dst_ring_size` | 消费者将预取数据写到的目的 ring 大小（消费者初始化配置） |
| `'h28` | `queue_fetch_dst_wptr` | 消费者写指针寄存器（fetch 硬件维护） |
| `'h2C` | `queue_fetch_dst_rdptr` | 消费者读指针寄存器（消费者维护） |

Golden 中 queue0 实例：

- MEM：`MEM_O_QUEUE_0_REG_*`，地址 `'hD8..'h104`。
- COMP：`COMPUTE_O_QUEUE_0_REG_*`，地址 `'h19C..'h1C8`。

它们对应 `queue0_reg_addr + 'h0..'h2C`。queue1、queue2、queue3 实例按
`0x30` stride 排布，均由各自的 `queue*_reg_addr + offset` 寻址。下文代码以
queue0（`MEM_O_QUEUE_0_REG_*` / `COMPUTE_O_QUEUE_0_REG_*`）为示例；各 field
名按 golden 命名，多 queue 场景下替换实例索引并按上述规则寻址。

## Appendix 5.10.1 Doorbell 配置初始化

```c
// Module: HQC 内部寄存器（doorbell 空间 remap 地址，位于 BAR01 空间）
// Golden: PCIE_REG_DE/reg_src/PCIE_HQC_REG.xlsx
// MEM queue 基址指针: MEM_CP_QUEUE_i.mem_queue_regs_addr_i (i = 0..3)
// COMP queue 基址指针: COMP_CP_QUEUE_i.comp_queue_regs_addr_i (i = 0..3)
// 每个 queue 的配置寄存器 = queue*_reg_addr + 固定偏移
//   ('h0/'h4/'h8/'hC/'h10/'h14/'h18/'h1C/'h20/'h24/'h28/'h2C)

phal_enter(ctx, TOP.U_PCIE.U_HQC);

// MEM Queue Doorbell（高 128 MB 空间）
for (uint32_t i = 0; i < 4; i++) {
    // 基址: MEM_CP_QUEUE_i.mem_queue_regs_addr_i（由 DE 展开 i 实例）
    // 下列寄存器地址 = queue*_reg_addr + 偏移，以 queue0 为示例。

    // queue*_reg_addr + 'hC: doorbell credit，值为 0 表示不可下发
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_DB_CRD.mem_o_queue_0_reg_db_crd,
        0x0010); // i 实例索引待展开

    // queue*_reg_addr + 'h10/'h14: fetch src addr（生产者维护）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_SRC_ADDR_LOW.mem_o_queue_0_reg_fetch_src_addr_low,
        0x80000000); // i 待展开
    // phal_fld_write(ctx,
    //     MEM_O_QUEUE_0_REG_FETCH_SRC_ADDR_HIGH.mem_o_queue_0_reg_fetch_src_addr_high,
    //     0x00000000); // + 'h14

    // queue*_reg_addr + 'h18: fetch src size（生产者维护）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_SRC_SIZE.mem_o_queue_0_reg_fetch_src_size,
        0x00000100); // i 待展开

    // queue*_reg_addr + 'h1C/'h20: dst ring base（消费者初始化配置）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_DST_ADDR_LOW.mem_o_queue_0_reg_fetch_dst_addr_low,
        0xA0000000); // i 待展开
    // phal_fld_write(ctx,
    //     MEM_O_QUEUE_0_REG_FETCH_DST_ADDR_HIGH.mem_o_queue_0_reg_fetch_dst_addr_high,
    //     0x00000000); // + 'h20

    // queue*_reg_addr + 'h24: dst ring size（消费者初始化配置）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_DST_SIZE.mem_o_queue_0_reg_fetch_dst_size,
        0x00000100); // i 待展开

    // queue*_reg_addr + 'h28: dst 写指针（fetch 硬件维护，初始化清 0）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_DST_WR_PTR.mem_o_queue_0_reg_dst_wr_ptr,
        0x00000000); // i 待展开

    // queue*_reg_addr + 'h2C: dst 读指针（消费者维护，初始化清 0）
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_DST_RD_PTR.mem_o_queue_0_reg_dst_rd_ptr,
        0x00000000); // i 待展开
}

// Compute Queue Doorbell（低 128 MB 空间）
for (uint32_t i = 0; i < 4; i++) {
    // 基址: COMP_CP_QUEUE_i.comp_queue_regs_addr_i（由 DE 展开 i 实例）
    // 下列寄存器地址 = queue*_reg_addr + 偏移，以 queue0 为示例。

    // queue*_reg_addr + 'h0: fetch 方向（bit[2] 中断方向包含 fetch_direction field）
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_FETCH_DIRECTION.compute_o_queue_0_reg_fetch_direction,
        0x1); // i 待展开

    // queue*_reg_addr + 'hC: doorbell credit
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_DB_CRD.compute_o_queue_0_reg_db_crd,
        0x0010); // i 待展开

    // queue*_reg_addr + 'h10/'h14: fetch src addr
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_FETCH_SRC_ADDR_LOW.compute_o_queue_0_reg_fetch_src_addr_low,
        0x80000000); // i 待展开
    // phal_fld_write(ctx,
    //     COMPUTE_O_QUEUE_0_REG_FETCH_SRC_ADDR_HIGH.compute_o_queue_0_reg_fetch_src_addr_high,
    //     0x00000000); // + 'h14

    // queue*_reg_addr + 'h18: fetch src size
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_FETCH_SRC_SIZE.compute_o_queue_0_reg_fetch_src_size,
        0x00000100); // i 待展开

    // queue*_reg_addr + 'h1C/'h20: dst ring base
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_FETCH_DST_ADDR_LOW.compute_o_queue_0_reg_fetch_dst_addr_low,
        0xA0000000); // i 待展开
    // phal_fld_write(ctx,
    //     COMPUTE_O_QUEUE_0_REG_FETCH_DST_ADDR_HIGH.compute_o_queue_0_reg_fetch_dst_addr_high,
    //     0x00000000); // + 'h20

    // queue*_reg_addr + 'h24: dst ring size
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_FETCH_DST_SIZE.compute_o_queue_0_reg_fetch_dst_size,
        0x00000100); // i 待展开

    // queue*_reg_addr + 'h28: dst 写指针
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_DST_WR_PTR.compute_o_queue_0_reg_dst_wr_ptr,
        0x00000000); // i 待展开

    // queue*_reg_addr + 'h2C: dst 读指针
    phal_fld_write(ctx,
        COMPUTE_O_QUEUE_0_REG_DST_RD_PTR.compute_o_queue_0_reg_dst_rd_ptr,
        0x00000000); // i 待展开
}
```

说明：

- Host 作为生产者时，queue 地址需放在 BAR01 aperture 内。
- 下发 doorbell 的地址为 `queue*_reg_addr + 'h4`（doorbell addr）。
- PCIe 内部总线基址为 `BAR01_base`。
- 上述 `MEM_O_QUEUE_0_REG_*` / `COMPUTE_O_QUEUE_0_REG_*` 已对齐 golden
  `PCIE_HQC_REG`。queue1、queue2、queue3 实例按各自生成的寄存器名访问；基址
  不同，固定偏移相同。

## Appendix 5.10.2 HQC 基于 Doorbell 的 Fetch 流程

```c
phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 生产者读取 doorbell_credit；为 0 则禁止下发（queue*_reg_addr + 'hC）。
uint32_t credit = 0x0;
phal_fld_read(ctx,
    MEM_O_QUEUE_0_REG_DB_CRD.mem_o_queue_0_reg_db_crd,
    &credit);
if (credit == 0x0) {
    // 不可下发 doorbell
}

// 2. 生产者配置 src_addr（queue*_reg_addr + 'h10）。
phal_fld_write(ctx,
    MEM_O_QUEUE_0_REG_FETCH_SRC_ADDR_LOW.mem_o_queue_0_reg_fetch_src_addr_low,
    0x80000000);

// 3. 生产者配置 src_size（queue*_reg_addr + 'h18）。
phal_fld_write(ctx,
    MEM_O_QUEUE_0_REG_FETCH_SRC_SIZE.mem_o_queue_0_reg_fetch_src_size,
    0x00000100);

// 4. 消费者配置 dst_addr / dst_size（queue*_reg_addr + 'h1C / 'h24）。

// 5. 生产者按 doorbell_addr 下发 MWr 请求，数据内容为 1。
// doorbell 地址 = queue*_reg_addr + 'h4（MEM_O_QUEUE_0_REG_DB_ADDR_LOW/HIGH）。
// Golden:
//   MEM_O_QUEUE_0_REG_DB_ADDR_LOW.mem_o_queue_0_reg_db_addr_low  (+ 'h4)
//   MEM_O_QUEUE_0_REG_DB_ADDR_HIGH.mem_o_queue_0_reg_db_addr_high (+ 'h8)
// 软件对该地址写 0x00000001 即视为 doorbell。
phal_write(ctx, MEM_O_QUEUE_0_REG_DB_ADDR_LOW, 0x00000001);

// 6. HQC Fetch 完成后更新 dst wptr（硬件维护，queue*_reg_addr + 'h28）。
// 7. HQC 上报中断通知消费者。
// 8. 消费者接收中断后读取 dst wptr。
uint32_t wptr = 0x0;
phal_fld_read(ctx,
    MEM_O_QUEUE_0_REG_DST_WR_PTR.mem_o_queue_0_reg_dst_wr_ptr,
    &wptr);

// 9. 消费者 load 数据完成后配置 dst rptr 更新（queue*_reg_addr + 'h2C）。
phal_fld_write(ctx,
    MEM_O_QUEUE_0_REG_DST_RD_PTR.mem_o_queue_0_reg_dst_rd_ptr,
    0x00000000);
```

## Appendix 5.10.3 Doorbell 连续下发流程

```c
// Module: HQC
phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 生产者读取 doorbell_credit（queue*_reg_addr + 'hC）。
uint32_t credit = 0x0;
phal_fld_read(ctx,
    MEM_O_QUEUE_0_REG_DB_CRD.mem_o_queue_0_reg_db_crd,
    &credit);

uint32_t count = credit; // 可下发 doorbell 的数目

// 2--4 可重复执行，次数不超过第一步读取的 doorbell 数目。
for (uint32_t n = 0; n < count; n++) {
    // 2. 配置 src_addr（queue*_reg_addr + 'h10）。
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_SRC_ADDR_LOW.mem_o_queue_0_reg_fetch_src_addr_low,
        0x80000000);

    // 3. 配置 src_size（queue*_reg_addr + 'h18）。
    phal_fld_write(ctx,
        MEM_O_QUEUE_0_REG_FETCH_SRC_SIZE.mem_o_queue_0_reg_fetch_src_size,
        0x00000100);

    // 4. 按 doorbell_addr 下发 MWr，数据内容为 1。
    // doorbell 地址 = queue*_reg_addr + 'h4。
    phal_write(ctx, MEM_O_QUEUE_0_REG_DB_ADDR_LOW, 0x00000001);
}
```

## Appendix 5.10.4 Doorbell 配置重新初始化

```c
phal_enter(ctx, TOP.U_PCIE.U_HQC);

uint32_t credit = 0x0;
uint32_t wptr = 0x0;
uint32_t rptr = 0x0;

// Step 1: 等待 doorbell_credit == 16（queue*_reg_addr + 'hC）。
status = phal_fld_polling(
    ctx,
    MEM_O_QUEUE_0_REG_DB_CRD.mem_o_queue_0_reg_db_crd,
    0x10,
    PHAL_TIMEOUT_MS(10)
);

// Step 2: 等待 wptr == rptr。
// wptr = queue*_reg_addr + 'h28；rptr = queue*_reg_addr + 'h2C。
// Golden 中 DST_WR_PTR 与 DST_RD_PTR 各自为单 field，需软件分别读出比较。
if (status == OK) {
    phal_fld_read(ctx,
        MEM_O_QUEUE_0_REG_DST_WR_PTR.mem_o_queue_0_reg_dst_wr_ptr,
        &wptr);
    phal_fld_read(ctx,
        MEM_O_QUEUE_0_REG_DST_RD_PTR.mem_o_queue_0_reg_dst_rd_ptr,
        &rptr);
    while (wptr != rptr) {
        phal_fld_read(ctx,
            MEM_O_QUEUE_0_REG_DST_WR_PTR.mem_o_queue_0_reg_dst_wr_ptr,
            &wptr);
        phal_fld_read(ctx,
            MEM_O_QUEUE_0_REG_DST_RD_PTR.mem_o_queue_0_reg_dst_rd_ptr,
            &rptr);
    }
}

// Step 3: wptr / rptr 清零。
phal_fld_write(ctx,
    MEM_O_QUEUE_0_REG_DST_WR_PTR.mem_o_queue_0_reg_dst_wr_ptr,
    0x0);
phal_fld_write(ctx,
    MEM_O_QUEUE_0_REG_DST_RD_PTR.mem_o_queue_0_reg_dst_rd_ptr,
    0x0);

// Step 4: 按 5.10.1 / 5.10.2 重新配置初始化并执行 Fetch（调用公共 sequence）。
```

## Appendix 5.10.5 Descriptor Control 配置初始化

```c
// Golden:
//   HQC_DESC_CTRL_DCF_RANG0..3_CTRL.hqc_desc_ctrl_dcf_rang_crd_enable
//   HQC_DESC_CTRL_DCF_RANG0..3_STAR_ADDR_LOW/HIGH.hqc_desc_ctrl_dcf_rang_star_addr_low/high
//   HQC_DESC_CTRL_DCF_RANG0..3_END_ADDR_LOW/HIGH.hqc_desc_ctrl_dcf_rang_end_addr_low/high
//   HQC_DESC_CTRL_DCF_CREDIT_THRESH.hqc_desc_ctrl_dcf_credit_thresh
//   CORE0/1_DESC_CTRL_FETCH_TIMEOUT_THRESH.core0/1_desc_ctrl_fetch_timeout_thresh

phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 配置 DCF Range 起始地址（i = 0..3，有效地址 13 bit）。
// Golden: HQC_DESC_CTRL_DCF_RANG0..3_STAR_ADDR_LOW (i = 0..3)
for (uint32_t i = 0; i < 4; i++) {
    phal_fld_write(ctx,
        HQC_DESC_CTRL_DCF_RANG0_STAR_ADDR_LOW.hqc_desc_ctrl_dcf_rang_star_addr_low,
        0x1000); // i 实例索引待展开
}

// 2. 配置 DCF Range 结束地址。
// Golden: HQC_DESC_CTRL_DCF_RANG0..3_END_ADDR_LOW (i = 0..3)
for (uint32_t i = 0; i < 4; i++) {
    phal_fld_write(ctx,
        HQC_DESC_CTRL_DCF_RANG0_END_ADDR_LOW.hqc_desc_ctrl_dcf_rang_end_addr_low,
        0x1FFF); // i 实例索引待展开
}

// 3. 配置 DCF Credit 阈值。
phal_fld_write(ctx,
    HQC_DESC_CTRL_DCF_CREDIT_THRESH.hqc_desc_ctrl_dcf_credit_thresh,
    0x8);

// 4. 配置 Descriptor fetch timeout 阈值。
phal_fld_write(ctx,
    CORE0_DESC_CTRL_FETCH_TIMEOUT_THRESH.core0_desc_ctrl_fetch_timeout_thresh,
    0x3E8);
phal_fld_write(ctx,
    CORE1_DESC_CTRL_FETCH_TIMEOUT_THRESH.core1_desc_ctrl_fetch_timeout_thresh,
    0x3E8);
```

## Appendix 5.10.6 Descriptor 配置流程

```c
phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 等待 core0/1_desc_ctrl_idle == 1。
status = phal_fld_polling(
    ctx,
    CORE0_DESC_CTRL_IDLE.core0_desc_ctrl_idle,
    0x1,
    PHAL_TIMEOUT_MS(100)
);

if (status == TIMEOUT) {
    // Report idle timeout
}

// 2. 配置 fetch 地址寄存器（MCU SRAM 地址）。
phal_fld_write(ctx,
    CORE0_DESC_CTRL_FETCH_ADDR_LOW.core0_desc_ctrl_fetch_addr_low,
    0x20000000);

// 3. 配置 trig = 1，触发 HW 搬运 MCU SRAM 内的 descriptor。
phal_fld_write(ctx,
    CORE0_DESC_CTRL_TRIG.core0_desc_ctrl_trig,
    0x1);

// 4. HW 按地址带大小搬运 descriptor（idle 置 0），完成后返回 1（硬件行为）。
// descriptor 内 station id 路由至 PCIE_BS/DCF。
// 下一次配置前需等待 CORE0_DESC_CTRL_IDLE 再次为 1。
status = phal_fld_polling(
    ctx,
    CORE0_DESC_CTRL_IDLE.core0_desc_ctrl_idle,
    0x1,
    PHAL_TIMEOUT_MS(100)
);
```

## Appendix 5.10.7 Sync flag 发送配置流程

```c
// Golden: PCIE_REG_DE/reg_src/PCIE_HQC_REG.xlsx
//   HQC_SEND_SMF_RDY_S0.csr0_rdy / HQC_SEND_SMF_VLD_S0.csr0_vld
//   HQC_SEND_SMF_ADDR_S0.s0_addr_byte / s0_sync_flag_id / s0_station_id
//   HQC_SEND_SMF_COUNT_S0.s0_count / HQC_SEND_SMF_ORIGIN_TYPE_S0.s0_origin_type

phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 读取硬件 ready 寄存器，为 1 时继续。
status = phal_fld_polling(
    ctx,
    HQC_SEND_SMF_RDY_S0.csr0_rdy,
    0x1,
    PHAL_TIMEOUT_MS(100)
);

// 2. 配置 sync flag 包。
phal_fld_write(ctx,
    HQC_SEND_SMF_ADDR_S0.s0_addr_byte,
    0x00000000); // addr byte
phal_fld_write(ctx,
    HQC_SEND_SMF_ADDR_S0.s0_station_id,
    0x202010); // 示例，如发往 AIC 则填 station_id
phal_fld_write(ctx,
    HQC_SEND_SMF_ORIGIN_TYPE_S0.s0_origin_type,
    0x0); // SW origin
phal_fld_write(ctx,
    HQC_SEND_SMF_COUNT_S0.s0_count,
    0x1); // count（示意）

// Dst chip id 依据使用场景配置；trace_id 默认与 desc 保持一致。

// 3. 触发硬件发送 sync flag 包。
phal_fld_write(ctx,
    HQC_SEND_SMF_VLD_S0.csr0_vld,
    0x1);
```

## Appendix 5.10.8 Sync flag 读取

MCU 通过 AXI 读取 `sfmem` 中的 sync flag entry。地址排布如下：

- `addr[9:2] = sync flag number`
- `addr[11:10] = 2'b00`
- `addr[13:12] = 00` 表示 CNT，`01` 表示 done
- `addr[16:14] = 3'b000`

每笔读请求只能返回 CNT 或 done 中的一个；读取完整 entry 需要读取两次。为确保
值稳定，CNT/done 需要至少读取两次。

`SMF_READ_ADDR` / `SMF_READ_DATA` 不在 PCIe golden Excel 内。Golden 中仅有
`SYNC_FLAG_DONE_*_BIT_STATUS` / `HQC_SYNC_FLAG_CLR`，需与对应寄存器库核对。

```c
phal_enter(ctx, TOP.U_PCIE.U_SMF_MEM);

uint32_t cnt_val = 0x0;
uint32_t done_val = 0x0;

// 读取 CNT：设置 addr[13:12] = 00。
// TODO: SMF_READ_ADDR / FIELD_SEL / SMF_READ_DATA.CNT 不在 PCIe golden Excel 内，
//       待与对应寄存器库核对。
// phal_fld_write(ctx, SMF_READ_ADDR.FIELD_SEL, 0x0);
// phal_fld_read(ctx, SMF_READ_DATA.CNT, &cnt_val);

// 读取 done：设置 addr[13:12] = 01。
// phal_fld_write(ctx, SMF_READ_ADDR.FIELD_SEL, 0x1);
// phal_fld_read(ctx, SMF_READ_DATA.DONE, &done_val);

// 替代方案：通过 golden 中可见的寄存器读取 done bit。
phal_fld_read(ctx,
    SYNC_FLAG_DONE_31_0_BIT_STATUS.o_sync_flag_done_31_0_bit_status,
    &done_val);
```

## Appendix 5.10.9 Sync flag Done Clear 流程

HQC FW 通过 `apb_slv0_ratio_mem` / `apb_slv1_ratio_mem` 访问 `SYNCFLAG`。空间排布：

- `0x0..0x3FF`：CNT
- `0x400..0x7FF`：DONE bit
- `0x800..0xBFF`：PA bit

`SYNCFLAG_CNT_0` / `SYNCFLAG_DONE_0` 内存空间寄存器不在 PCIe golden Excel 内。
Golden 中仅有 `HQC_SYNC_FLAG_CLR.hqc_sync_flag_clr` 与
`SYNC_FLAG_DONE_*_BIT_STATUS`，用于 done 状态读取/清除。

```c
phal_enter(ctx, TOP.U_PCIE.U_HQC_SYNCFLAG);

// 读取 CNT（golden 无独立 CNT 寄存器，由 SMF 内存空间读取）。
// TODO: SYNCFLAG_CNT_0 不在 PCIe golden Excel 内，待与对应寄存器库核对。
// phal_read(ctx, SYNCFLAG_CNT_0, &data);

// 读取/清除 DONE bit。
// Golden:
//   SYNC_FLAG_DONE_31_0_BIT_STATUS.o_sync_flag_done_31_0_bit_status
//   HQC_SYNC_FLAG_CLR.hqc_sync_flag_clr
phal_fld_write(ctx,
    HQC_SYNC_FLAG_CLR.hqc_sync_flag_clr,
    0x1); // 清除 done bit
```

> 原始材料未提供 Appendix 5.10.10，本文保留其原始编号空缺。

## Appendix 5.10.11 HQC SW 发送中断

```c
// Golden: PCIE_REG_DE/reg_src/PCIE_HQC_REG.xlsx
//   HQC_SEND_INTR_RDY_S0.intr_csr0_rdy / HQC_SEND_INTR_VLD_S0.intr_csr0_vld
//   HQC_SEND_INTR_ADDR_S0.intr_csr0_addr
//   HQC_SEND_INTR_DATA0_LOW_S0.intr_csr0_data0_l
//   HQC_SEND_INTR_DATA0_HIGH_S0.intr_csr0_data0_h
//   HQC_SEND_INTR_DATA1_LOW_S0.intr_csr0_data1_l
//   HQC_SEND_INTR_DATA1_HIGH_S0.intr_csr0_data1_h

phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 1. 等待 HQC_SEND_INTR_RDY 就绪。
status = phal_fld_polling(
    ctx,
    HQC_SEND_INTR_RDY_S0.intr_csr0_rdy,
    0x1,
    PHAL_TIMEOUT_MS(100)
);

if (status == TIMEOUT) {
    // Report ready timeout
}

// 2. 配置中断数据。
phal_fld_write(ctx,
    HQC_SEND_INTR_DATA0_LOW_S0.intr_csr0_data0_l,
    0x00000001);
phal_fld_write(ctx,
    HQC_SEND_INTR_DATA0_HIGH_S0.intr_csr0_data0_h,
    0x00000000);
phal_fld_write(ctx,
    HQC_SEND_INTR_DATA1_LOW_S0.intr_csr0_data1_l,
    0x00000000);
phal_fld_write(ctx,
    HQC_SEND_INTR_DATA1_HIGH_S0.intr_csr0_data1_h,
    0x00000000);

// 3. 触发硬件发送 intr 包。
phal_fld_write(ctx,
    HQC_SEND_INTR_VLD_S0.intr_csr0_vld,
    0x1);
```

## Appendix 5.10.12 Host Doorbell Register 流程（ECR）

- Host 视角地址：Doorbell Space + `0x2000..0x207F`。
- HQC FW 视角地址：base addr + `0x2000..0x207F`。
- Golden 源：`PCIE_REG_DE/reg_src/PCIE_HQC_REG.xlsx` 中的
  `HOST_WR_MASK.host_wr_mask`。

```c
phal_enter(ctx, TOP.U_PCIE.U_HQC);

// 配置 Host 写屏蔽（仅作示范，按需屏蔽）。
phal_fld_write(ctx,
    HOST_WR_MASK.host_wr_mask,
    0x00000000);

// Host 下发对应写请求后会触发中断给 HQC FW（中断处理另述）。
```

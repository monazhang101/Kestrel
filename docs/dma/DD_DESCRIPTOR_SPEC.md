# DD DMA Descriptor Specification

本文整理自用户提供的 Confluence `DD` 页面截图，按原始页码 8、9、10、11、12、13
连续编排。它作为当前仓库分析 DMA descriptor contract 的主要资料来源；截图未覆盖
的页面和被截断的内容不在本文中推断补全。

## Source and authority

- Source: `https://wiki.infiscale.dev/spaces/~juanjuan/pages/70815009/DD`
- Source title: `DD - 卷卷 - Confluence`
- Source timestamp shown in screenshots: `2026/9/17 15:24`
- Captured pages: 8--13 of 30
- Original field-description reference: `AIC_DMA_AR_DD_WORD.md`
- DCF flit-layout reference: `AIC_DMA_COMMON.sv`
  (`dcf_flit_a_ts`, `dcf_flit_b_ts`, and `dcf_flit_c_ts`), with the latest
  `dev` revision of the `AIC_DMA_DE` repository taking precedence.

When this extract conflicts with an implementation comment or a software test,
the original specification and the referenced RTL/golden sources take
precedence. Product- or stepping-specific restrictions still require the
corresponding RTL owner or errata.

## 1. Packet types (page 8)

A DMA descriptor packet contains one to three 32-byte flits. Fields not sent by
a shorter packet are treated as their defaults. The source lists three packet
types:

| Packet type | Size | Intended use | Flits | Rate |
|---|---:|---|---|---:|
| Compact | 32 B | Fast back-to-back sequential local/remote | A | 1 desc/cycle |
| Outer-Strided | 64 B | Partial-strided local/remote | A then B | 0.5 desc/cycle |
| Full | 96 B | All features | A then B then C | 0.33 desc/cycle |

### 1.1 Flit A

Flit A carries most common information used by local, remote, and host DMA. It
also carries the information needed to route a descriptor packet through the
NoC. Flit A is always the first flit. It has 44 reserved bits, and its valid
fields do not occupy the channel's complete 32-byte payload.

| Field | Width | Descriptor word |
|---|---:|---:|
| `trace_en` | 1 | Word 0 |
| `dma_type` | 2 | Word 0 |
| `thread_select` | 2 | Word 0 |
| `sync_mode` | 1 | Word 0 |
| `execution_order_required` | 1 | Word 0 |
| `dst_chip_id` | 20 | Word 0 |
| `src_opcode` | 2 | Word 1 |
| `src_mem_core_id` | 2 | Word 1 |
| `src_mem_mem_id` | 2 | Word 1 |
| `src_sync_flag_core_id` | 2 | Word 1 |
| `src_sync_flag_id` | 14 | Word 1 |
| `dst_opcode` | 2 | Word 1 |
| `dst_mem_core_id` | 2 | Word 1 |
| `dst_mem_mem_id` | 2 | Word 1 |
| `dst_sync_flag_0_core_id` | 2 | Word 2 |
| `dst_sync_flag_0_id` | 14 | Word 2 |
| `dst_sync_flag_1_core_id` | 2 | Word 2 |
| `dst_sync_flag_1_id` | 14 | Word 2 |
| `src_mem_start_offset_word_upper` | 12 | Word 3 |
| `dst_mem_start_offset_word_upper` | 12 | Word 3 |
| `length` | 32 | Word 4 |
| `src_mem_start_offset_word_lower` | 32 | Word 5 |
| `dst_mem_start_offset_word_lower` | 32 | Word 6 |
| `steps_level_4` | 5 | Word 18 |
| `reserved_0` | 44 | N/A |

For DCF, `reserved_0` carries a 1-bit `supervisor` field and a 43-bit
`trace_id`. The authoritative DCF layout is the referenced RTL definition.

## 2. Flits B and C (page 9)

### 2.1 Flit B

Flit B carries the next three outer stride-level values. It has 64 reserved
bits, so its valid fields do not occupy the channel's complete 32-byte payload.

| Field | Width | Descriptor word | Default |
|---|---:|---:|---:|
| `src_stride_level_0` | 32 | Word 7 | `32'd16` |
| `dst_stride_level_0` | 32 | Word 8 | `32'd16` |
| `steps_level_1` | 32 | Word 9 | `32'd1` |
| `src_stride_level_1` | 32 | Word 10 | `32'd1` |
| `dst_stride_level_1` | 32 | Word 11 | `32'd1` |
| `steps_level_2` | 32 | Word 12 | `32'd1` |
| `reserved_1` | 64 | N/A | `64'd0` |

For DCF, the reserved area includes a 1-bit `supervisor` field and a 43-bit
`trace_id`; use `dcf_flit_b_ts` as the layout authority.

### 2.2 Flit C

Flit C carries the outermost stride-level information and the upper part of the
host IOVA. It has 64 reserved bits.

| Field | Width | Descriptor word | Default |
|---|---:|---:|---:|
| `src_stride_level_2` | 32 | Word 13 | `32'd1` |
| `dst_stride_level_2` | 32 | Word 14 | `32'd1` |
| `steps_level_3` | 32 | Word 15 | `32'd1` |
| `src_stride_level_3` | 32 | Word 16 | `32'd1` |
| `dst_stride_level_3` | 32 | Word 17 | `32'd1` |
| `host_upper_iova` | 32 | Word 19 | `32'd0` |
| `reserved_2` | 64 | N/A | `64'd0` |

For DCF, the reserved area includes a 1-bit `supervisor` field and a 43-bit
`trace_id`; use `dcf_flit_c_ts` as the layout authority.

## 3. Descriptor layout and common fields (page 10)

The logical DMA descriptor contains 20 32-bit words, for a total of 640 bits
(80 bytes). The packet format may pad or omit flits according to the packet type
described above.

- `steps_level_0` is derived from `length` and is not represented separately.
- `src_stride_level_4` and `dst_stride_level_4` are fixed to 1 and are not
  represented separately.
- Reserved fields must not be assigned arbitrary values. Doing so can cause
  translation-time or memory-access errors.

The word-level layout is:

| Word | Fields |
|---:|---|
| 0 | `trace_en`, `dma_type`, `thread_select`, `sync_mode`, `execution_order_required`, reserved bits, `dst_chip_id` |
| 1 | source/destination opcode and memory IDs, `src_sync_flag_core_id`, `src_sync_flag_id` |
| 2 | destination sync flag 0 and 1 core IDs and flag IDs |
| 3 | source/destination upper start offsets and reserved bits |
| 4 | `length` |
| 5 | `src_mem_start_offset_word_lower` |
| 6 | `dst_mem_start_offset_word_lower` |
| 7--8 | source/destination stride level 0 |
| 9 | steps level 1 |
| 10--11 | source/destination stride level 1 |
| 12 | steps level 2 |
| 13--14 | source/destination stride level 2 |
| 15 | steps level 3 |
| 16--17 | source/destination stride level 3 |
| 18 | `steps_level_4` and reserved bits |
| 19 | `host_upper_iova` |

### 3.1 `trace_en` (1 bit)

| Value | Meaning |
|---:|---|
| 0 | Do not generate a debug trace for this DMA transaction. |
| 1 | Generate a debug trace if tracing is enabled. |

### 3.2 `dma_type` (2 bits)

| Value | Meaning |
|---:|---|
| 0 | Local/Host |
| 1 | Remote Write Unicast |
| 2 | Not visible in the supplied screenshots |
| 3 | Remote Read Unicast |

### 3.3 `thread_select` (2 bits)

The field selects one thread from a set of eligible threads to execute the
current DMA transaction.

| Value | Meaning |
|---:|---|
| 0 | Thread 0 |
| 1 | Thread 1 |
| 2 | Thread 2 |
| 3 | Thread 3 |

There is no ordering between any two DMA transactions whose `thread_select`
values differ.

### 3.4 `sync_mode` (1 bit)

This field selects how the sync flag is incremented while DMA executes.

| Value | Meaning |
|---:|---|
| 0 | Count words. Increment by the number of 32-byte words transferred. Set the Done bit when DMA completes. |
| 1 | Count dones. Increment by 1 when DMA completes and set the Done bit. |

## 4. Ordering and opcodes (page 11)

### 4.1 `execution_order_required` (1 bit)

| Value | Meaning |
|---:|---|
| 0 | Strict Cross-DMA Ordering |
| 1 | Relaxed Ordering |

When `execution_order_required` is 1:

- Ordering between two DMA transfers A and B must be implemented with sync
  flags, even when the descriptor initiators are the same (both `ACC_CS` or
  both `HQC_CS`), the source station IDs match, the destination station IDs
  match, the thread matches, and the sync-flag address matches.
- Software must not assume that data within one DMA is transferred in order.
  In count mode, the sync-flag count only reports how much data has moved; it
  cannot identify a safely completed prefix because partially completed data
  may be out of order.
- The sync flag's Done signal still indicates completion of the DMA transfer.

**Atlas only supports `execution_order_required = 1`.**

### 4.2 `src_opcode` (2 bits)

| Value | Meaning |
|---:|---|
| 0 | Read |
| 1 | Reserved |
| 2 | Instruction memset |
| 3 | Data memset |

Host DMA only supports `src_opcode == Read`. Any other value produces a
malformed-descriptor fatal error.

### 4.3 `dst_opcode` (2 bits)

| Value | Meaning |
|---:|---|
| 0 | Write |
| 1 | Write 4B for AIC SMEM; reserved for all other destinations |
| 2 | Write Special 0: Atomic Compare and Swap for AIC SMEM; reserved elsewhere |
| 3 | Write Special 1: Atomic Read and Add for AIC SMEM; reserved elsewhere |

For Write 4B, the first four source bytes are copied to a 4-byte-aligned
destination address and DMA `length` must be one 32-byte word. The source still
performs a 32-byte read, so a Write 4B starting in the final seven 4-byte words
of AIC SMEM causes a fatal error.

Write Special 0 and Write Special 1 return a response message to the destination
memory:

- The message sets the Done bit.
- The message value is the original 4-byte value at offset `+0` of the
  referenced destination memory.
- The message address is read as 4-byte data from the source-memory location.
- The Write Special 0 message address comes from source offset `+8`.
- The Write Special 1 message address comes from source offset `+4`.
- Message address bits `[33:14]` contain the destination chip ID.
- Bits `[13:0]` carry the upper 14 bits of a 16-bit message-word address; bits
  1 and 0 are fixed to zero for 4-byte message-word alignment.

Only AIC supports special-write destination opcodes. Other destination memory
types use `Write`; unsupported opcodes cause fatal errors.

### 4.4 `dst_chip_id` (20 bits)

- The field is ignored for `Local/Host` DMA.
- For `Remote Write Unicast` or `Remote Read Unicast`, it denotes the remote
  destination chip, but may also name the local chip.
- For requests originating from AIC, `fix_descriptor_field_s0` automatically
  detects the local case and converts `dma_type` to `Local/Host`.
- For requests originating from HQC, the DMA transaction follows the ISI route;
  actual loopback behavior depends on the ISI routing-table configuration.

## 5. Core, memory, and sync-flag IDs (page 12)

### 5.1 `{src/dst}_{mem/sync_flag}_core_id` (2 bits)

| Value | Core class | Meaning |
|---:|---|---|
| 0 | Reserved | Represents `self` only in ISA instruction decode. |
| 1 | Non-Core | For memory: DMEM or Host according to `mem_id`. For messages, sync flags, or tracing: HQC. |
| 2 | AIC | AIC memory/core. |
| 3 | Reserved | Reserved. |

ISA uses core ID 0 as `self` and replaces it with the actual current core ID
while processing the DMA instruction. In a memory-system descriptor, core ID 0
is reserved because the DMA hardware does not modify descriptors.

If the memory system receives a DMA using a reserved core ID, it raises a
malformed-descriptor fatal error and discards the descriptor. A descriptor
created by an AIC ISA DMA instruction is corrected by `fix_descriptor_fields`
before it enters the memory system.

When `{src/dst}_sync_flag_{0/1}_id` is zero, the corresponding sync-flag core ID
can be ignored.

### 5.2 `{src/dst}_mem_mem_id` (2 bits)

| Core ID | Memory ID | Meaning |
|---:|---:|---|
| 0 (`self`, reserved in a descriptor) | 0--3 | Reserved |
| 1 (Non-Core) | 0 | DMEM |
| 1 (Non-Core) | 1 | Host |
| 1 (Non-Core) | 2--3 | Reserved |
| 2 (AIC) | 0 | VMEM |
| 2 (AIC) | 1 | SMEM |
| 2 (AIC) | 2 | IMEM |
| 2 (AIC) | 3 | Reserved |
| 3 | 0--3 | Reserved |

The following cause a malformed-descriptor fatal error and descriptor discard:

- Using a reserved `mem_id`.
- Using Host as source or destination in remote DMA.
- Using Host as both source and destination.

### 5.3 `{src/dst}_sync_flag_{0/1}_id` (14 bits)

The field identifies a 4-byte offset in the target sync-flag memory.

Each SFM has a sync flag at offset 0, but the Null value prevents a DMA
transaction from using SFM sync flag 0 to report DMA progress. Other mechanisms
may still use sync flag 0, including Atomic Remote Set/Add instructions and the
response to an Atomic Compare and Swap DMA instruction.

A sync-flag ID beyond the sync-flag-memory address boundary does not wrap. It
can address reserved space and cause a fatal error from SFM access; that error
does not originate from the DMA engine's descriptor-processing path.

## 6. Length, address, stride, and host IOVA (page 13)

### 6.1 `length` (uint32)

The transfer length is expressed in 32-byte units. If `dst_opcode` is not
`Write`, `length` must be 1.

The source lists legal lengths as:

- 0 bytes; or
- `n * 32` bytes.

It states a maximum DMA length of 64 GiB (`2^36` bytes). An illegal length
causes a malformed-descriptor fatal error.

### 6.2 Start offsets

`{src/dst}_mem_start_offset_word_upper` is the high part of the start offset.
It is concatenated with the low part to calculate the byte offset in the DF
address.

`{src/dst}_mem_start_offset_word_lower` is the low part. It is concatenated
with the high part and multiplied by the memory word size.

- For Host memory, word size is 1 byte. The lower field carries the low 32 bits
  of the 64-bit host IOVA. Although the writable word size is 1 byte, a Host
  start offset must be 32-byte aligned or data corruption can occur.
- For other memory types, word size is 4 bytes.
- For source-memory memset opcodes, the 32-bit value written to memory is held
  in `src_mem_start_offset_word_lower`.
- The source notes that no error check is performed for a Host access that is
  not 32-byte aligned.

### 6.3 Strides and steps

`{src/dst}_stride_level_{0/1/2/3}` is a signed 32-bit stride size measured in
32-byte units; negative strides are allowed.

`steps_level_{1/2/3/4}` gives the number of groups visited at the current
stride level before advancing to the next stride level. For example,
`steps_level_2` is the number of `stride_level_2` steps performed before one
`stride_level_1` advance.

### 6.4 `host_upper_iova`

For Host DMA, this field contains the upper 32 bits of the 64-bit Host IOVA.

- Host-to-chip DMA takes the lower 32 bits from
  `src_mem_start_offset_word_lower`.
- Chip-to-host DMA takes the lower 32 bits from
  `dst_mem_start_offset_word_lower`.

## 7. Supported DMA combinations (pages 9 and 13)

General restrictions:

- Host-to-Host is illegal and causes a DMA-engine fatal error.
- Remote-memory to remote-memory is illegal.
- A transaction combining one AIC Host Memory endpoint with another chip's
  Host/Local Memory endpoint is illegal.
- Instruction and data types must be distinguished. A source/destination
  data-type mismatch causes a destination-memory fatal error.
- An implementation may split a memory access into multiple reads or writes.
- The table describes functional support, not the highest-performance setup.

Atlas support:

| Memory | Endpoint role | Length | Start alignment |
|---|---|---|---:|
| Any | Source/destination | 0 B | N/A |
| DMEM | Source/destination | Non-zero multiple of 32 B | 32 B |
| Host | Source/destination | Non-zero multiple of 32 B | 32 B |
| VMEM | Source/destination | Non-zero multiple of 32 B | 32 B |
| SMEM | Source/destination | Non-zero multiple of 32 B | 4 B |
| SMEM | Destination | Exactly 4 B with special-write opcodes | 4 B |
| IMEM | Source/destination | Non-zero multiple of 256 B | 256 B |

For local DMA involving these memory types, both source and destination must
satisfy their respective alignment constraints.

## 8. Authoritative conclusion about `thread_select = 3`

The supplied DD specification does **not** say that Thread 3 is reserved,
unsupported, or unusable. It explicitly defines the complete 2-bit encoding:

- 0: Thread 0
- 1: Thread 1
- 2: Thread 2
- 3: Thread 3

It also states that DMA transactions using different `thread_select` values
have no ordering relationship. This wording treats the four values as distinct
eligible execution threads.

The only explicit Atlas-specific field restriction in the supplied pages is
that Atlas supports `execution_order_required = 1` only. No Atlas-specific
restriction on `thread_select` appears in pages 8--13.

Therefore, a transfer that succeeds when forced to Thread 0 but times out on
Thread 3 indicates one of the following, rather than a documented DD rule:

- a product/stepping-specific RTL limitation or erratum absent from this
  generic DD document;
- missing initialization, enablement, routing, or credit for execution thread
  3;
- an RTL/firmware/document-version mismatch; or
- another field or resource correlated with Thread 3 in the test sequence.

The next authority to consult is the referenced `AIC_DMA_AR_DD_WORD.md`, the
latest `AIC_DMA_COMMON.sv`/`AIC_DMA_DE` source for the target RTL revision, and
any Atlas-specific errata or thread-enable/register documentation.

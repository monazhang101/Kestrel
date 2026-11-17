# CP Queue Copy

CP queue copy 是 direct-DD DMA 的独立路径。它直接配置 HQC MEM/COMP queue，
通过 queue pointer 判断 payload 完成；不使用 80-byte DMA descriptor，也不使用
direct-DD sync flag。

## Contract

- directions: `h2sram`, `sram2h`, `d2sram`, `sram2d`;
- queues: `mem0..mem3`, `comp0..comp3`;
- payload address and length: 128-byte aligned;
- HQC SRAM banks: `HQC_SRAM_0..3`, each 512 KiB, allocation cannot cross banks;
- Host DMA address must come from the HAL allocator;
- DMEM-to-SRAM paths require the platform Q128 translation enable bit;
- selected queue is owned exclusively by diag while the operation runs;
- current API is synchronous and serialized by `cp_queue_mutex`.

The 128-byte rule is a CP payload rule. It is independent of the default allocator
512-byte placement and direct-DD descriptor slot stride.

## Register sequence

The backend obtains the canonical queue image from the generated HQC offsets and
uses the queue remap to reach the writable doorbell alias. Per-queue fields are:

```text
+0x00 direction
+0x04 doorbell
+0x0c credit
+0x10/+0x14 source low/high
+0x18 source size
+0x1c/+0x20 destination low/high
+0x24 destination size
+0x28 wptr
+0x2c rptr
```

The operation is:

```text
read remap
wait credit == default and wptr == rptr
initialize and verify remap when it is zero
clear and verify wptr/rptr
write direction, source, destination and sizes
read back configuration and credit
write doorbell = 1
poll wptr - rptr >= transfer_bytes
write rptr = wptr
drain and clear pointers
```

The guide's queue initialization and doorbell examples match this sequence. The
implementation additionally reads back configuration before the doorbell and
drains the queue before returning.

## Failure ownership

The queue is marked faulted after a touched operation fails. If doorbell may have
reached hardware and completion is unknown, the source and destination allocations
are retained. There is no automatic cancel or queue reset; reset/quiesce ownership
must be established by the platform before reusing the queue.

## CLI

```sh
spev_diag cp_queue_data_transfer --target TPU0 \
  --direction h2sram --queue mem3 --size-bytes 512 --sram-bank 0
```

The testcase prepares and verifies payload data separately. A successful queue
return means the pointer protocol completed; the testcase's payload comparison is
what establishes data correctness.

## Not covered

This path does not implement asynchronous CP jobs, sync-flag completion, host
interrupt delivery, or parallel submissions to multiple queues. Those require a
separate queue ownership and completion contract.

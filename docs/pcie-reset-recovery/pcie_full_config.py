#!/usr/bin/env python3
"""Automatically snapshot/replay a PCIe Endpoint's full configuration image.

Keep pcie_config_restore.py beside this file for its Linux PCI sysfs backend.
Unknown register semantics cannot be inferred from reads; --raw-unknown opts
into replaying changed opaque bytes, with known status/trigger bits excluded.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import sys

from pcie_config_restore import (
    LinuxDevice, PCIError, require, valid_bdf, valid_identity,
    check_transfer_sizes, device_lock, save_snapshot, wait_ready,
)


FORMAT = "pcie-full-config-v1"


class Image:
    def __init__(self, data):
        self.data = data

    def read(self, offset, width):
        require(0 <= offset and offset + width <= len(self.data),
                f"Truncated configuration register at 0x{offset:x}")
        return int.from_bytes(self.data[offset: offset + width], "little")


class Layout:
    def __init__(self, image):
        self.image = image
        self.known = bytearray(len(image.data))
        self.zero = bytearray(len(image.data))
        self.rw = bytearray(len(image.data))
        self.fields = []
        self.fixed = []
        self.caps = []
        self.interrupts = []

    def cover(self, offset, size):
        self.image.read(offset, size)
        self.known[offset:offset + size] = b"\x01" * size

    def clear_bits(self, offset, width, mask):
        for i, byte in enumerate(mask.to_bytes(width, "little")):
            self.zero[offset + i] |= byte

    def check(self, offset, width, mask, label):
        self.fixed.append((offset, width, mask, self.image.read(offset, width) & mask, label))

    def add(self, offset, width, mask, label, stage=30, zero=0):
        require(width in (1, 2, 4) and offset % width == 0, "Unaligned replay field")
        self.cover(offset, width)
        self.clear_bits(offset, width, zero)
        self.fields.append({"offset": offset, "width": width, "mask": mask,
                            "value": self.image.read(offset, width) & mask,
                            "label": label, "stage": stage})
        for i, byte in enumerate(mask.to_bytes(width, "little")):
            self.rw[offset + i] |= byte


def walk_caps(image):
    standard, extended, seen = [], [], set()
    offset = image.read(0x34, 1) if image.read(6, 2) & 0x10 else 0
    while offset:
        require(0x40 <= offset <= 0xfc and offset % 4 == 0 and offset not in seen,
                "Invalid standard capability chain")
        seen.add(offset)
        ident = image.read(offset, 1)
        require(ident not in (0, 0xff), "Invalid standard capability ID")
        standard.append((offset, ident))
        offset = image.read(offset + 1, 1)
    if len(image.data) > 256:
        offset, seen = 0x100, set()
        while offset:
            require(0x100 <= offset <= 0xffc and offset % 4 == 0 and offset not in seen,
                    "Invalid extended capability chain")
            seen.add(offset)
            value = image.read(offset, 4)
            if offset == 0x100 and value == 0:
                break
            ident, version = value & 0xffff, (value >> 16) & 0xf
            require(ident not in (0, 0xffff) and version != 0,
                    "Unreadable extended capability header")
            extended.append((offset, ident, version))
            offset = (value >> 20) & 0xfff
    return standard, extended


def layout_for(data):
    image, layout = Image(data), Layout(Image(data))
    require(valid_identity(image.read(0, 4)), "Invalid Endpoint identity")
    require(image.read(0x0e, 1) == 0, "Only single-function Type 0 Endpoints are supported")
    layout.cover(0, 64)
    for offset in (0, 8, 0x2c):
        layout.check(offset, 4, 0xffffffff, "identity/class/subsystem")
    layout.check(0x0e, 1, 0xff, "header type")
    layout.add(4, 2, 0x7ff, "original Command (BME/decoding last)", 100)
    layout.clear_bits(6, 2, 0xffff)
    layout.add(0x0c, 1, 0xff, "Cache Line Size", 20)
    layout.add(0x0d, 1, 0xff, "Latency Timer", 20)
    layout.clear_bits(0x0f, 1, 0x40)
    index = 0
    while index < 6:
        offset = 0x10 + 4 * index
        low = image.read(offset, 4)
        is_io, kind = bool(low & 1), (low >> 1) & 3
        require(is_io or kind in (0, 2), "Unsupported memory BAR type")
        is64 = not is_io and kind == 2
        require(not is64 or index < 5, "Invalid 64-bit BAR")
        attrs = 3 if is_io else 15
        layout.check(offset, 4, attrs, f"BAR[{index}] attributes")
        if is64:
            layout.add(offset + 4, 4, 0xffffffff, f"BAR[{index}] high", 10)
        layout.add(offset, 4, 0xffffffff ^ attrs, f"BAR[{index}] address", 10)
        index += 2 if is64 else 1
    layout.add(0x30, 4, 0xfffff800, "Expansion ROM address", 10)
    layout.add(0x30, 4, 1, "Expansion ROM enable", 80)
    layout.add(0x3c, 1, 0xff, "Interrupt Line", 20)
    standard, extended = walk_caps(image)
    layout.caps = [standard, extended]
    require(sum(ident == 0x10 for _, ident in standard) == 1,
            "Exactly one PCIe capability is required")
    for ident in (1, 5, 0x11):
        require(sum(c == ident for _, c in standard) <= 1, "Duplicate control capability")

    for offset, ident in standard:
        limit = min([p for p, _ in standard if p > offset] + [256])

        def span(size):
            require(offset + size <= limit, f"Overlapping/truncated capability at 0x{offset:x}")
            layout.cover(offset, size)

        span(2)
        layout.check(offset, 2, 0xffff, "capability header")
        if ident == 1:
            span(8)
            require(image.read(offset + 4, 2) & 3 == 0, "Full replay currently requires D0")
            layout.check(offset + 2, 2, 0xffff, "PM Capabilities")
            layout.add(offset + 4, 2, 0x1f03, "PMCSR configuration", zero=0x8000)
        elif ident == 3:  # VPD access registers are a command interface, not static state.
            span(8)
        elif ident == 5:
            flags = image.read(offset + 2, 2)
            cap64, masking = bool(flags & 0x80), bool(flags & 0x100)
            count, enabled_count = (flags >> 1) & 7, (flags >> 4) & 7
            require(count <= 5 and enabled_count <= count, "Invalid MSI queue size")
            span(24 if cap64 and masking else (16 if cap64 else 12))
            layout.check(offset + 2, 2, 0x018e, "MSI capabilities")
            layout.add(offset + 2, 2, 0x70, "MSI Multiple Message Enable", 40)
            layout.add(offset + 4, 4, 0xfffffffc, "MSI Address Low", 40)
            if cap64:
                layout.add(offset + 8, 4, 0xffffffff, "MSI Address High", 40)
            data_offset = offset + (12 if cap64 else 8)
            layout.add(data_offset, 2, 0xffff, "MSI Message Data", 40)
            if masking:
                mask = (1 << (1 << count)) - 1
                layout.add(offset + (16 if cap64 else 12), 4, mask, "MSI vector mask", 40)
            layout.add(offset + 2, 2, 1, "MSI Enable", 90)
            layout.interrupts.append((offset + 2, 1, 0, "disable MSI"))
        elif ident == 0x10:
            flags = image.read(offset + 2, 2)
            version, kind = flags & 0xf, (flags >> 4) & 0xf
            require(version in (1, 2) and kind in (0, 1), "Unsupported PCIe capability type/version")
            span(0x34 if version == 2 else 0x14)
            layout.check(offset + 2, 2, 0xffff, "PCIe flags")
            layout.check(offset + 4, 4, 0xffffffff, "Device Capabilities")
            layout.check(offset + 12, 4, 0xffffffff, "Link Capabilities")
            check_transfer_sizes(image.read(offset + 8, 2), image.read(offset + 4, 4))
            layout.add(offset + 8, 2, 0x7fff, "Device Control", zero=0x8000)
            layout.clear_bits(offset + 10, 2, 0xffff)
            layout.add(offset + 0x10, 2, 0x0fcb, "Link Control", 60, zero=0x20)
            if version == 2:
                layout.check(offset + 0x24, 4, 0xffffffff, "Device Capabilities 2")
                layout.check(offset + 0x2c, 4, 0xffffffff, "Link Capabilities 2")
                layout.add(offset + 0x28, 2, 0x7f1f, "Device Control 2", zero=0x8000)
                layout.add(offset + 0x30, 2, 0xfbef, "Link Control 2", 60, zero=0x0410)
        elif ident == 0x11:
            span(12)
            layout.check(offset + 2, 2, 0x7ff, "MSI-X table size")
            layout.check(offset + 4, 4, 0xffffffff, "MSI-X table location")
            layout.check(offset + 8, 4, 0xffffffff, "MSI-X PBA location")
            layout.add(offset + 2, 2, 0xc000, "MSI-X Enable/Function Mask", 90)
            layout.interrupts.append((offset + 2, 2, 0xc000, "disable/mask MSI-X"))
        elif ident == 0x13:  # Advanced Features: FLR command/status, no static RW config.
            span(6)
        elif ident == 9:
            length = image.read(offset + 2, 1)
            require(3 <= length <= limit - offset, "Invalid vendor capability length")
            span(3)
            layout.check(offset + 2, 1, 0xff, "vendor capability length")

    for offset, ident, version in extended:
        limit = min([p for p, _, _ in extended if p > offset] + [len(data)])

        def span(size):
            require(offset + size <= limit, f"Overlapping/truncated extended capability at 0x{offset:x}")
            layout.cover(offset, size)

        span(4)
        layout.check(offset, 4, 0xffffffff, "extended capability header")
        if ident == 1:  # AER Endpoint controls, status/logs are excluded.
            span(min(0x48, limit - offset))
            require(limit - offset >= 0x2c, "Truncated AER capability")
            layout.add(offset + 8, 4, 0xffffffff, "AER Uncorrectable Mask")
            layout.add(offset + 12, 4, 0xffffffff, "AER Uncorrectable Severity")
            layout.add(offset + 20, 4, 0x0000f1c1, "AER Correctable Mask")
            layout.check(offset + 24, 4, 0xa80, "AER capabilities")
            layout.add(offset + 24, 4, 0x540, "AER ECRC/MHDR controls")
            layout.clear_bits(offset + 4, 4, 0xffffffff)
            layout.clear_bits(offset + 16, 4, 0xffffffff)
        elif ident == 3:
            span(12)
            layout.check(offset + 4, 4, 0xffffffff, "Device Serial Number low")
            layout.check(offset + 8, 4, 0xffffffff, "Device Serial Number high")
        elif ident == 4:
            span(16)
            layout.add(offset + 4, 1, 0xff, "Power Budgeting Data Select")
        elif ident == 0x18:
            span(8)
            layout.add(offset + 4, 2, 0x1fff, "LTR Max Snoop Latency")
            layout.add(offset + 6, 2, 0x1fff, "LTR Max No-Snoop Latency")
        elif ident in (0x0e, 0x0f, 0x1b):
            span(8)
            layout.check(offset + 4, 2, 0xffff, "Translation/ARI/PASID capabilities")
            mask = {0x0e: 0x73, 0x0f: 0x801f, 0x1b: 7}[ident]
            layout.add(offset + 6, 2, mask, "ARI/ATS/PASID Control", 50)
        elif ident == 0x15:
            first = image.read(offset + 8, 4)
            count = (first >> 5) & 7
            require(1 <= count <= 6, "Invalid Resizable BAR count")
            span(4 + count * 8)
            for index in range(count):
                entry = offset + 4 + index * 8
                capability, control = image.read(entry, 4), image.read(entry + 4, 4)
                size_code = (control >> 8) & 0x1f
                require(size_code <= 27 and capability & (1 << (size_code + 4)),
                        "Unsupported Resizable BAR size encoding")
                layout.check(entry, 4, 0xffffffff, "Resizable BAR sizes")
                layout.check(entry + 4, 4, 0xe7, "Resizable BAR index/count")
                layout.add(entry + 4, 4, 0x1f00, "Resizable BAR size", 5)
    return layout


def read_image(dev, allow_bound_driver=False):
    dev.check_unmanaged(require_unbound=not allow_bound_driver)
    environment = dev.environment(require_unbound=not allow_bound_driver)
    size = environment["config_size"]
    require(size in (256, 4096), f"Unexpected config size: {size}")
    raw = b"".join(dev.read(offset, 4).to_bytes(4, "little") for offset in range(0, size, 4))
    require(dev.environment(require_unbound=not allow_bound_driver) == environment,
            "Host environment changed during full configuration read")
    return raw, environment


def capture(dev):
    # Capture is read-only, so it can record the current state before the
    # user stops/unbinds a driver. Restore remains strict and requires no
    # driver to be bound.
    data, environment = read_image(dev, allow_bound_driver=True)
    layout_for(data)
    return {"format": FORMAT, "bdf": dev.bdf, "environment": environment,
            "config_hex": data.hex(),
            "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}


def decode_snapshot(snapshot):
    require(isinstance(snapshot, dict) and snapshot.get("format") == FORMAT,
            "Use a full-configuration snapshot made by pcie_full_config.py")
    require(isinstance(snapshot.get("bdf"), str), "Missing snapshot BDF")
    valid_bdf(snapshot["bdf"])
    raw = snapshot.get("config_hex")
    require(isinstance(raw, str) and len(raw) in (512, 8192), "Malformed complete configuration image")
    try:
        data = bytes.fromhex(raw)
    except ValueError as exc:
        raise PCIError("Configuration image is not valid hexadecimal") from exc
    require(len(data) in (256, 4096), "Malformed complete configuration length")
    env = snapshot.get("environment")
    require(isinstance(env, dict) and env.get("config_size") == len(data),
            "Snapshot environment/image size mismatch")
    return data, layout_for(data)


def matched_layout(dev, snapshot, timeout=60, ready_confirmed=False):
    data, layout = decode_snapshot(snapshot)
    require(dev.bdf == snapshot["bdf"], "Requested BDF differs from snapshot")
    identity = Image(data).read(0, 4)
    if not ready_confirmed:
        wait_ready(dev, identity, timeout)
    current, environment = read_image(dev)
    require(environment == snapshot["environment"], "Host resources/path/device object changed")
    live = layout_for(current)
    require(live.caps == layout.caps, "Capability layout changed")
    now = Image(current)
    for offset, width, mask, expected, label in layout.fixed:
        require(now.read(offset, width) & mask == expected,
                f"{label} changed at 0x{offset:x}")
    return data, layout, current


def unknown_changes(layout, current):
    old, now = layout.image, Image(current)
    changes = []
    for offset in range(0, len(current), 4):
        mask = sum((0xff if not layout.known[offset + i] else 0) << (8 * i) for i in range(4))
        if mask and (old.read(offset, 4) ^ now.read(offset, 4)) & mask:
            changes.append({"offset": offset, "width": 4, "mask": mask,
                            "value": old.read(offset, 4) & mask,
                            "label": "opaque/private bytes (raw replay)", "stage": 45})
    return changes


def compare(dev, snapshot, timeout=60, ready_confirmed=False):
    data, layout, current = matched_layout(dev, snapshot, timeout, ready_confirmed)
    now = Image(current)
    mismatches = []
    for field in layout.fields:
        if now.read(field["offset"], field["width"]) & field["mask"] != field["value"]:
            mismatches.append({"offset": field["offset"], "label": field["label"]})
    other = []
    for offset, (old, new) in enumerate(zip(data, current)):
        if layout.known[offset] and (old ^ new) & ~layout.rw[offset] & 0xff:
            other.append(offset)
    return {"config_mismatches": mismatches,
            "unknown_mismatches": [f["offset"] for f in unknown_changes(layout, current)],
            "other_differences": other}


def execute_field(dev, identity, layout, field):
    dev.check_unmanaged()
    require(dev.read(0, 4) == identity, "Endpoint became inaccessible before write")
    offset, width, mask = field["offset"], field["width"], field["mask"]
    zero = int.from_bytes(layout.zero[offset:offset + width], "little")
    value = (dev.read(offset, width) & ~mask & ~zero) | field["value"]
    dev.write(offset, width, value)
    actual = dev.read(offset, width)
    require(actual & mask == field["value"],
            f"Readback failed at 0x{offset:x} ({field['label']}): write 0x{value:x}, read 0x{actual:x}")


def restore(dev, snapshot, raw_unknown=False, dry_run=False, timeout=60,
            ready_confirmed=False, report=None):
    data, layout, current = matched_layout(dev, snapshot, timeout, ready_confirmed)
    opaque = unknown_changes(layout, current)
    require(raw_unknown or not opaque,
            "Opaque/private configuration differs at " +
            ", ".join(f"0x{f['offset']:03x}" for f in opaque[:12]) +
            "; use --raw-unknown to replay these bytes without a register list")
    identity = Image(data).read(0, 4)
    prefix = [{"offset": 4, "width": 2, "mask": 0x407, "value": 0x400,
               "label": "disable decode/BME; mask INTx", "stage": 0}]
    for offset, mask, value, label in layout.interrupts:
        prefix.append({"offset": offset, "width": 2, "mask": mask,
                       "value": value, "label": label, "stage": 0})
    plan = prefix + sorted(layout.fields + opaque, key=lambda f: f["stage"])
    if report:
        report(f"Image: {len(data)} bytes; opaque DWORDs to replay: {len(opaque)}")
        for f in plan:
            report(f"stage={f['stage']:3} 0x{f['offset']:03x}/{8*f['width']:2}b mask=0x{f['mask']:x} "
                   f"saved=0x{f['value']:x}  {f['label']}")
    if dry_run:
        return plan
    began = False
    try:
        began = True
        for field in plan:
            if field["stage"] >= 80:
                # Do not enable ROM/interrupts/BME if FW changed an earlier value
                # after its immediate write-readback check.
                for prior in plan:
                    if 0 < prior["stage"] < field["stage"]:
                        actual = dev.read(prior["offset"], prior["width"])
                        require(actual & prior["mask"] == prior["value"],
                                f"Configuration changed before enable: {prior['label']} "
                                f"at 0x{prior['offset']:x}")
            if field["stage"] == 100:
                require(dev.environment() == snapshot["environment"],
                        "Host allocation changed before restoring Command")
            elif field["stage"] > 0:
                require(dev.read(4, 2) & 7 == 0, "Decode/BME changed during restore")
            execute_field(dev, identity, layout, field)
        result = compare(dev, snapshot, timeout=0, ready_confirmed=ready_confirmed)
        require(not result["config_mismatches"] and not result["unknown_mismatches"],
                f"Final configuration comparison failed: {result}")
    except (Exception, KeyboardInterrupt):
        if began:
            for field in prefix:
                try:
                    execute_field(dev, identity, layout, field)
                except (PCIError, OSError) as exc:
                    if report:
                        report(f"Cleanup could not confirm {field['label']}: {exc}")
        raise
    return plan


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    save = sub.add_parser("save", help="Read all available 256B/4KB configuration bytes")
    save.add_argument("--bdf", required=True)
    save.add_argument("--output", required=True, type=Path)
    for name in ("restore", "compare"):
        cmd = sub.add_parser(name)
        cmd.add_argument("--bdf", required=True)
        cmd.add_argument("--input", required=True, type=Path)
        cmd.add_argument("--timeout", type=float, default=60)
        if name == "restore":
            cmd.add_argument("--dry-run", action="store_true")
            cmd.add_argument("--ready-confirmed", action="store_true",
                             help="FW_DONE/LTSSM=L0 was confirmed externally; skip config polling")
            cmd.add_argument("--raw-unknown", action="store_true",
                             help="Replay changed opaque/private bytes using their saved values")
    args = parser.parse_args(argv)
    try:
        require(sys.platform.startswith("linux"), "Run on the Linux PCIe Host")
        require(os.geteuid() == 0, "Use sudo/root for full configuration access")
        dev = LinuxDevice(args.bdf)
        with device_lock(dev.bdf):
            if args.operation == "save":
                snapshot = capture(dev)
                save_snapshot(args.output, snapshot)
                print(f"Saved {len(snapshot['config_hex']) // 2} bytes for {dev.bdf} to {args.output}")
                return 0
            require(args.input.stat().st_size <= 1024 * 1024, "Snapshot is too large")
            snapshot = json.loads(args.input.read_text(encoding="utf-8"))
            if args.operation == "restore":
                restore(dev, snapshot, raw_unknown=args.raw_unknown, dry_run=args.dry_run,
                        timeout=args.timeout, ready_confirmed=args.ready_confirmed, report=print)
                print("No hardware writes performed." if args.dry_run else
                      "Configuration comparison passed; original Command/interrupt enables restored.")
                return 0
            result = compare(dev, snapshot, timeout=args.timeout)
            print(json.dumps(result, indent=2))
            return int(bool(result["config_mismatches"] or result["unknown_mismatches"]))
    except (PCIError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; check hardware state before retrying.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())

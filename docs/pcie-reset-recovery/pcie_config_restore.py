#!/usr/bin/env python3
"""Save/restore an unbound PCIe Endpoint for post-SoC-reset MMIO testing.

Hardware is accessed only through Linux PCI sysfs. This is deliberately not
a generic byte-for-byte PCI configuration replay or a DMA recovery tool.
"""
import argparse
import contextlib
import datetime
import json
import math
import os
from pathlib import Path
import re
import sys
import time


FORMAT = 1
MPS_MRRS = 0x70e0
BDF_RE = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-1][0-9a-f]\.[0-7]\Z")
SYSFS = Path("/sys/bus/pci/devices")


class PCIError(Exception):
    """A prerequisite, PCI access or readback check failed."""


def require(condition, message):
    if not condition:
        raise PCIError(message)


def valid_bdf(value):
    value = value.lower()
    require(bool(BDF_RE.fullmatch(value)),
            "Use a full BDF, for example 0000:01:00.0")
    return value


def valid_identity(value):
    return (value & 0xffff) not in (0, 1, 0xffff)


class LinuxDevice:
    def __init__(self, bdf):
        self.bdf = valid_bdf(bdf)
        self.path = SYSFS / self.bdf

    def check_unmanaged(self, require_unbound=True):
        require(self.path.is_dir(), "Device disappeared from sysfs; no rescan performed")
        if require_unbound:
            require(not (self.path / "driver").is_symlink(),
                    "A Host driver is bound; stop the case and unbind it first")
        require(not (self.path / "physfn").is_symlink(), "VFs are unsupported")
        sriov = self.path / "sriov_totalvfs"
        require(not sriov.exists() or int(sriov.read_text().strip()) == 0,
                "This first version does not support SR-IOV devices")
        siblings = list(SYSFS.glob(self.bdf.rsplit(".", 1)[0] + ".*"))
        require(len(siblings) == 1,
                "Multiple functions share this device; coordinated recovery is required")

    def read(self, offset, width):
        require(width in (1, 2, 4) and offset >= 0 and offset % width == 0,
                "Invalid configuration access width/alignment")
        try:
            fd = os.open(self.path / "config", os.O_RDONLY | os.O_CLOEXEC)
            try:
                data = os.pread(fd, width, offset)
            finally:
                os.close(fd)
        except OSError as exc:
            raise PCIError(f"Config read {self.bdf}+0x{offset:x}: {exc}") from exc
        require(len(data) == width, f"Short config read at 0x{offset:x}")
        return int.from_bytes(data, "little")

    def write(self, offset, width, value):
        self.check_unmanaged()
        require(width in (1, 2, 4) and offset % width == 0,
                "Invalid configuration write width/alignment")
        require(0 <= value < 1 << (width * 8), "Configuration value out of range")
        try:
            fd = os.open(self.path / "config", os.O_RDWR | os.O_CLOEXEC)
            try:
                count = os.pwrite(fd, value.to_bytes(width, "little"), offset)
            finally:
                os.close(fd)
        except OSError as exc:
            raise PCIError(f"Config write {self.bdf}+0x{offset:x}: {exc}") from exc
        require(count == width, f"Short config write at 0x{offset:x}")

    def environment(self, require_unbound=True):
        self.check_unmanaged(require_unbound=require_unbound)
        rows = (self.path / "resource").read_text().splitlines()
        require(len(rows) >= 6, "PCI resource table is incomplete")
        resources = [[int(word, 16) for word in row.split()] for row in rows[:6]]
        require(all(len(row) == 3 for row in resources), "Invalid PCI resource table")
        real_path = self.path.resolve(strict=True)
        bridges = []
        # Read only routing/window registers, never secondary-status bits.
        window_regs = [(4, 2), (0x18, 4), (0x1c, 2), (0x20, 4),
                       (0x24, 4), (0x28, 4), (0x2c, 4), (0x30, 4)]
        for parent in real_path.parents:
            if BDF_RE.fullmatch(parent.name):
                bridge = LinuxDevice(parent.name)
                require(bridge.read(0x0e, 1) & 0x7f == 1,
                        f"Unexpected upstream header: {parent.name}")
                bridges.append({"bdf": parent.name,
                                "windows": [bridge.read(o, w) for o, w in window_regs]})
        return {
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "inode": self.path.stat().st_ino,
            "path": str(real_path),
            "config_size": (self.path / "config").stat().st_size,
            "resources": resources, "bridges": bridges,
        }


def standard_caps(dev, header):
    if not header[1] & (0x10 << 16):
        return []
    offset = header[13] & 0xff
    seen = set()
    caps = []
    while offset:
        require(0x40 <= offset <= 0xfc and offset % 4 == 0 and offset not in seen,
                "Invalid/looping standard capability chain")
        seen.add(offset)
        value = dev.read(offset, 2)
        cap_id = value & 0xff
        require(cap_id not in (0, 0xff), "Unreadable capability header")
        caps.append([offset, cap_id])
        offset = (value >> 8) & 0xff
    for cap_id in (1, 5, 0x10, 0x11):
        require(sum(c[1] == cap_id for c in caps) <= 1,
                "Duplicate standard capabilities are unsupported")
    return caps


def extended_caps(dev, config_size):
    if config_size <= 256:
        return []
    offset, seen, caps = 0x100, set(), []
    while offset:
        require(0x100 <= offset <= 0xffc and offset % 4 == 0 and offset not in seen,
                "Invalid/looping extended capability chain")
        seen.add(offset)
        value = dev.read(offset, 4)
        if value == 0 and offset == 0x100:
            break
        require(value not in (0, 0xffffffff) and value & 0xffff not in (0, 0xffff),
                "Unreadable extended capability header")
        cap_id = value & 0xffff
        require(cap_id not in (0x10, 0x15, 0x24),
                "SR-IOV/Resizable BAR recovery is unsupported by this version")
        caps.append([offset, cap_id])
        offset = (value >> 20) & 0xfff
    return caps


def cap_offset(caps, cap_id):
    return next((offset for offset, ident in caps if ident == cap_id), None)


def check_transfer_sizes(control, devcap):
    mps, mrrs, supported = (control >> 5) & 7, (control >> 12) & 7, devcap & 7
    require(mps <= 5 and mrrs <= 5 and supported <= 5,
            "Reserved MPS/MRRS/Max Payload Supported encoding")
    require(mps <= supported, "MPS exceeds the Endpoint's Max Payload Supported")


def inspect_device(dev):
    dev.check_unmanaged()
    environment = dev.environment()
    header = [dev.read(i * 4, 4) for i in range(16)]
    require(valid_identity(header[0]), "Endpoint not ready (invalid Vendor ID or CRS)")
    require((header[3] >> 16) & 0xff == 0,
            "Only single-function Type 0 Endpoints are supported")
    require(not header[1] & 4, "Bus Master is enabled; stop DMA and clear BME first")
    require(not header[12] & 1, "Expansion ROM decoding must be disabled")
    caps = standard_caps(dev, header)
    exp = cap_offset(caps, 0x10)
    require(exp is not None and exp <= 0xf4, "Missing/truncated PCIe capability")
    flags = dev.read(exp + 2, 2)
    require((flags >> 4) & 0xf in (0, 1), "Device is not a PCIe Endpoint")
    control = dev.read(exp + 8, 2)
    require(not control & 0x8000, "PCIe FLR/BCR trigger is active")
    devcap = dev.read(exp + 4, 4)
    check_transfer_sizes(control, devcap)
    pm = cap_offset(caps, 1)
    if pm is not None:
        require(pm <= 0xf8, "Truncated PM capability")
        require(dev.read(pm + 4, 2) & 3 == 0, "Endpoint is not in D0")
    msi = cap_offset(caps, 5)
    if msi is not None:
        require(not dev.read(msi + 2, 2) & 1, "MSI must be disabled for MMIO-only recovery")
    msix = cap_offset(caps, 0x11)
    if msix is not None:
        require(not dev.read(msix + 2, 2) & 0x8000,
                "MSI-X must be disabled for MMIO-only recovery")
    snapshot = {
        "format": FORMAT, "bdf": dev.bdf, "environment": environment,
        "header": header, "capabilities": caps,
        "extended_capabilities": extended_caps(dev, environment["config_size"]),
        "pcie_flags": flags, "pcie_control": control, "pcie_devcap": devcap,
    }
    memory_bars(snapshot, require_address=False)
    require(dev.environment() == environment,
            "Host device/resources changed while inspecting configuration")
    return snapshot


def memory_bars(snapshot, require_address=True):
    header = snapshot["header"]
    resources = snapshot["environment"]["resources"]
    bars, index = [], 0
    while index < 6:
        low = header[4 + index]
        start, end, flags = resources[index]
        require(not low & 1, "I/O BARs are unsupported by this MMIO version")
        kind = (low >> 1) & 3
        require(kind in (0, 2), "Unsupported memory BAR type")
        is64 = kind == 2
        require(not is64 or index < 5, "64-bit BAR cannot start at BAR5")
        high = header[5 + index] if is64 else None
        if (start, end, flags) != (0, 0, 0):
            require(flags & 0x200 and not flags & 0x30000000 and start > 0 and end >= start,
                    f"BAR{index} has no usable Host memory allocation")
            size = end - start + 1
            require(size >= 16 and size & (size - 1) == 0,
                    f"BAR{index} has an invalid allocation size")
            address = (low & ~0xf) | ((high or 0) << 32)
            if require_address:
                require(address != 0 and address % size == 0,
                        f"BAR{index} had no valid pre-reset bus address")
            bars.append({"index": index, "offset": 0x10 + index * 4,
                         "low": low, "high": high})
        else:
            require(low & ~0xf == 0 and (high or 0) == 0,
                    f"Unallocated BAR{index} has an unexpected bus address")
        if is64:
            require(resources[index + 1] == [0, 0, 0],
                    "High half of a 64-bit BAR unexpectedly has a separate resource")
        index += 2 if is64 else 1
    require(bars, "No assigned memory BARs found")
    return bars


def capture(dev):
    snapshot = inspect_device(dev)
    memory_bars(snapshot)
    snapshot["created_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    return snapshot


def uint(value, bits):
    return type(value) is int and 0 <= value < 1 << bits


def validate_snapshot(snapshot):
    require(isinstance(snapshot, dict) and type(snapshot.get("format")) is int
            and snapshot["format"] == FORMAT, "Unsupported snapshot format")
    require(isinstance(snapshot.get("bdf"), str), "Missing snapshot BDF")
    valid_bdf(snapshot["bdf"])
    header = snapshot.get("header")
    require(isinstance(header, list) and len(header) == 16
            and all(uint(v, 32) for v in header), "Malformed snapshot header")
    require(valid_identity(header[0]) and (header[3] >> 16) & 0xff == 0
            and not header[1] & 4, "Snapshot contains unsupported/active hardware state")
    require(uint(snapshot.get("pcie_control"), 16) and
            not snapshot["pcie_control"] & 0x8000, "Invalid saved PCIe Device Control")
    require(uint(snapshot.get("pcie_flags"), 16), "Invalid saved PCIe flags")
    require(uint(snapshot.get("pcie_devcap"), 32), "Invalid saved PCIe Device Capabilities")
    check_transfer_sizes(snapshot["pcie_control"], snapshot["pcie_devcap"])
    for name, low, high, bits in [("capabilities", 0x40, 0xfc, 8),
                                  ("extended_capabilities", 0x100, 0xffc, 16)]:
        caps = snapshot.get(name)
        require(isinstance(caps, list), f"Invalid saved {name}")
        offsets = set()
        for entry in caps:
            require(isinstance(entry, list) and len(entry) == 2
                    and uint(entry[0], 12) and low <= entry[0] <= high
                    and entry[0] % 4 == 0 and entry[0] not in offsets
                    and uint(entry[1], bits), f"Invalid saved {name} entry")
            offsets.add(entry[0])
    require(sum(c[1] == 0x10 for c in snapshot["capabilities"]) == 1,
            "Snapshot must contain exactly one PCIe capability")
    environment = snapshot.get("environment")
    require(isinstance(environment, dict), "Missing saved Host environment")
    resources = environment.get("resources")
    require(isinstance(resources, list) and len(resources) == 6
            and all(isinstance(row, list) and len(row) == 3
                    and all(uint(v, 64) for v in row) for row in resources),
            "Invalid saved resource table")
    memory_bars(snapshot)


def wait_ready(dev, identity, timeout):
    require(isinstance(timeout, (int, float)) and math.isfinite(timeout)
            and 0 <= timeout <= 3600, "Timeout must be between 0 and 3600 seconds")
    deadline = time.monotonic() + timeout
    reason = "Endpoint not ready"
    while True:
        try:
            dev.check_unmanaged()
            value = dev.read(0, 4)
        except PCIError as exc:
            reason = str(exc)
        else:
            if valid_identity(value):
                require(value == identity,
                        f"Identity changed: expected {identity:08x}, read {value:08x}")
                return
            reason = f"Endpoint config returned {value:08x} (not ready/CRS)"
        remaining = deadline - time.monotonic()
        require(remaining > 0, f"Timed out waiting for original BDF: {reason}")
        time.sleep(min(0.2, remaining))


def check_same_device(saved, live):
    require(saved["environment"] == live["environment"],
            "Host boot/device object/path/resources/upstream windows changed; refusing replay")
    for index in (0, 2, 11):
        require(saved["header"][index] == live["header"][index],
                "Endpoint identity/class/subsystem changed")
    for name in ("capabilities", "extended_capabilities", "pcie_flags", "pcie_devcap"):
        require(saved[name] == live[name], f"Endpoint {name} layout changed")
    index = 0
    while index < 6:
        old = saved["header"][4 + index]
        new = live["header"][4 + index]
        require(old & 0xf == new & 0xf, f"BAR{index} type/prefetch attributes changed")
        index += 2 if (old >> 1) & 3 == 2 else 1


def write_verified(dev, identity, offset, width, value, mask=None):
    dev.check_unmanaged()
    require(dev.read(0, 4) == identity, "Endpoint became inaccessible/changed before write")
    dev.write(offset, width, value)
    actual = dev.read(offset, width)
    if mask is None:
        mask = (1 << (width * 8)) - 1
    require(actual & mask == value & mask,
            f"Readback mismatch at 0x{offset:x}: wrote {value:0{width * 2}x}, "
            f"read {actual:0{width * 2}x}")


def restore(dev, snapshot, dry_run=False, timeout=60, report=None):
    validate_snapshot(snapshot)
    require(dev.bdf == snapshot["bdf"], "Requested BDF differs from snapshot BDF")
    identity = snapshot["header"][0]
    wait_ready(dev, identity, timeout)
    live = inspect_device(dev)
    check_same_device(snapshot, live)
    bars = memory_bars(snapshot)
    exp = cap_offset(live["capabilities"], 0x10)
    # MPS/MRRS only: never include Device Status or the FLR trigger.
    control = (live["pcie_control"] & ~MPS_MRRS) | (snapshot["pcie_control"] & MPS_MRRS)
    disabled = (live["header"][1] & 0x7ff & ~7) | 0x400
    final_command = (snapshot["header"][1] & 0x7ff & ~0x407) | 0x402
    plan = [(4, 2, disabled, "disable decode/BME; mask INTx")]
    for bar in bars:
        if bar["high"] is not None:
            plan.append((bar["offset"] + 4, 4, bar["high"], f"BAR{bar['index']} high"))
        plan.append((bar["offset"], 4, bar["low"], f"BAR{bar['index']} low"))
    plan += [(exp + 8, 2, control, "restore MPS/MRRS"),
             (4, 2, final_command, "enable MMIO; keep BME/I/O off and INTx masked")]
    if report:
        for offset, width, value, label in plan:
            report(f"  +0x{offset:03x}/{width * 8:2} <- 0x{value:0{width * 2}x}  {label}")
    if dry_run:
        return plan
    began = False
    try:
        began = True
        write_verified(dev, identity, 4, 2, disabled)
        for offset, width, value, _ in plan[1:-1]:
            require(dev.read(4, 2) & 7 == 0, "Decode/BME changed during restore")
            write_verified(dev, identity, offset, width, value)
        # Check the complete BAR pairs again before opening memory decoding.
        for bar in bars:
            require(dev.read(bar["offset"], 4) == bar["low"], "BAR changed before MMIO enable")
            if bar["high"] is not None:
                require(dev.read(bar["offset"] + 4, 4) == bar["high"],
                        "64-bit BAR changed before MMIO enable")
        require(dev.environment() == snapshot["environment"],
                "Host environment changed during restore")
        require(dev.read(4, 2) & 7 == 0, "Decode/BME changed before MMIO enable")
        write_verified(dev, identity, 4, 2, final_command)
    except (Exception, KeyboardInterrupt):
        if began:
            try:
                dev.check_unmanaged()
                require(dev.read(0, 4) == identity, "Endpoint unavailable for cleanup")
                safe = (dev.read(4, 2) & 0x7ff & ~7) | 0x400
                write_verified(dev, identity, 4, 2, safe)
            except (PCIError, OSError) as cleanup:
                if report:
                    report(f"  Could not confirm decode/BME disabled: {cleanup}")
        raise
    return plan


@contextlib.contextmanager
def device_lock(bdf):
    import fcntl
    lock_path = Path("/run/lock") / f"pcie-config-restore-{bdf}.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise PCIError("Another save/restore process holds the device lock") from exc
        yield
    finally:
        os.close(fd)


def load_snapshot(path):
    require(path.stat().st_size <= 1024 * 1024, "Snapshot file is unexpectedly large")
    with path.open("r", encoding="utf-8") as stream:
        snapshot = json.load(stream)
    validate_snapshot(snapshot)
    return snapshot


def save_snapshot(path, snapshot):
    # Never overwrite a previous pre-reset snapshot with post-reset values.
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(snapshot, stream, indent=2, ensure_ascii=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    save = sub.add_parser("save", help="Save pre-reset config without hardware writes")
    save.add_argument("--bdf", required=True, help="Full BDF, e.g. 0000:01:00.0")
    save.add_argument("--output", required=True, type=Path, help="New JSON snapshot path")
    replay = sub.add_parser("restore", help="Restore memory BARs and MMIO configuration")
    replay.add_argument("--bdf", required=True)
    replay.add_argument("--input", required=True, type=Path)
    replay.add_argument("--timeout", type=float, default=60, help="Config-ready timeout in seconds")
    replay.add_argument("--dry-run", action="store_true", help="Inspect/print plan; no config writes")
    args = parser.parse_args(argv)
    try:
        require(sys.platform.startswith("linux"), "Run this script on the Linux Host")
        require(os.geteuid() == 0, "Use sudo/root for PCI configuration access")
        dev = LinuxDevice(args.bdf)
        with device_lock(dev.bdf):
            if args.operation == "save":
                snapshot = capture(dev)
                save_snapshot(args.output, snapshot)
                print(f"Saved {dev.bdf} to {args.output}; no hardware configuration was changed.")
            else:
                snapshot = load_snapshot(args.input)
                print(f"{'DRY RUN' if args.dry_run else 'RESTORE'} {dev.bdf}")
                restore(dev, snapshot, dry_run=args.dry_run, timeout=args.timeout, report=print)
                if args.dry_run:
                    print("Inspection complete; no configuration writes performed.")
                else:
                    print("Configuration readback passed. BME=0, MSE=1, INTx masked.")
                    print("Wait for FW MMIO readiness, remap the BARs, then run your BAR case.")
        return 0
    except (PCIError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; check device state before retrying.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())

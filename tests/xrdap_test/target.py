"""Target profiles (tests/targets/*.toml) and the test fixture the tool writes into the
target's scratch RAM, so that no special target program is needed."""

import os
import tomllib
from dataclasses import dataclass

from .vendor import Item

TARGET_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "targets")

# Cortex-M system registers.
CPUID = 0xE000ED00
DHCSR, DEMCR = 0xE000EDF0, 0xE000EDFC
AIRCR = 0xE000ED0C
DWT_CTRL, DWT_CYCCNT = 0xE0001000, 0xE0001004
DBGKEY = 0xA05F0000
S_HALT, S_RESET_ST = 1 << 17, 1 << 25

CORES = {0xC20: "Cortex-M0", 0xC60: "Cortex-M0+", 0xC23: "Cortex-M3", 0xC24: "Cortex-M4",
         0xC27: "Cortex-M7", 0xD20: "Cortex-M23", 0xD21: "Cortex-M33", 0xD22: "Cortex-M55"}


@dataclass
class Region:
    base: int
    size: int


@dataclass
class Target:
    name: str
    scratch: Region
    dpidr: int | None = None
    fault_address: int = 0xFFFFF000
    counter: int | None = None  # address of a u32 the target program increments
    dump: Region | None = None
    flash_test: Region | None = None
    openocd_target: str | None = None
    pyocd_target: str | None = None
    probe_rs_chip: str | None = None
    path: str = ""

    # Fixture layout, offsets from scratch.base (the first 0x400 bytes; blocks use the rest).
    V_U8, V_U16, V_U32, V_U64 = 0x18, 0x1A, 0x1C, 0x20
    CHAIN_FINAL, CHAIN_P3, CHAIN_P2, CHAIN_P1 = 0x28, 0x2C, 0x30, 0x34
    NULL_P1, BAD_P1, RW = 0x38, 0x3C, 0x40
    WORDS = 0x100  # 32 u32 words for the 1/4/8/32-variable sets
    BLOCK = 0x400

    CONSTANTS = {V_U8: (1, 0xA5), V_U16: (2, 0x1234), V_U32: (4, 0x89ABCDEF),
                 V_U64: (8, 0x0123456789ABCDEF), CHAIN_FINAL: (4, 0xCAFEBABE)}

    def at(self, offset):
        return self.scratch.base + offset

    def fixture_bytes(self):
        """The fixture image written at scratch.base."""
        img = bytearray(0x400)

        def put(off, size, value):
            img[off:off + size] = (value & (1 << 8 * size) - 1).to_bytes(size, "little")

        for off, (size, value) in self.CONSTANTS.items():
            put(off, size, value)
        put(self.CHAIN_P3, 4, self.at(self.CHAIN_FINAL))
        put(self.CHAIN_P2, 4, self.at(self.CHAIN_P3))
        put(self.CHAIN_P1, 4, self.at(self.CHAIN_P2))
        put(self.NULL_P1, 4, 0)
        put(self.BAD_P1, 4, self.fault_address)
        put(self.RW, 4, 0x11111111)
        for k in range(32):
            put(self.WORDS + 4 * k, 4, 0x5A000000 | k)
        return bytes(img)

    def constant_items(self):
        """v_u8, v_u16, v_u32, v_u64 and the 3-level chain as READ items with expectations."""
        return [
            (Item(self.at(self.V_U8), 1), 0xA5),
            (Item(self.at(self.V_U16), 2), 0x1234),
            (Item(self.at(self.V_U32), 4), 0x89ABCDEF),
            (Item(self.at(self.V_U64), 8), 0x0123456789ABCDEF),
            (Item(self.at(self.CHAIN_P1), 4, (0, 0, 0)), 0xCAFEBABE),
        ]

    def word_items(self, n):
        return [Item(self.at(self.WORDS + 4 * (k % 32)), 4) for k in range(n)]


def _region(value):
    return Region(int(value["base"]), int(value["size"])) if value else None


def load_target(name_or_path, overrides=None):
    path = name_or_path
    if not os.path.exists(path):
        path = os.path.join(TARGET_DIR, f"{name_or_path}.toml")
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    raw.update(overrides or {})
    return Target(
        name=raw.get("name", os.path.splitext(os.path.basename(path))[0]),
        scratch=_region(raw["scratch"]),
        dpidr=raw.get("dpidr"),
        fault_address=raw.get("fault_address", 0xFFFFF000),
        counter=raw.get("counter"),
        dump=_region(raw.get("dump")),
        flash_test=_region(raw.get("flash_test")),
        openocd_target=raw.get("openocd_target"),
        pyocd_target=raw.get("pyocd_target"),
        probe_rs_chip=raw.get("probe_rs_chip"),
        path=path,
    )


def list_targets():
    return sorted(os.path.splitext(f)[0] for f in os.listdir(TARGET_DIR) if f.endswith(".toml"))

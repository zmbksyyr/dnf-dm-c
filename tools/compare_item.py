"""Compare the C item codec with DnfItemSlot extracted from local Python references.

Only the class AST is executed; reference GUI startup and database code are not run.
"""
import argparse
import ast
from pathlib import Path
import random
import struct
import subprocess

from db_integration import OUT, PROBE, ROOT, build


def check(reference, raw, lines):
    source = reference.read_text(encoding="utf-8-sig")
    tree = ast.parse(source, filename=str(reference))
    node = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "DnfItemSlot")
    namespace = {"struct": struct}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(reference), "exec"), namespace)
    cls = namespace["DnfItemSlot"]
    for i, line in enumerate(lines):
        slot = cls(raw[i * 61:(i + 1) * 61])
        parts = line.split()
        fields = [slot.isSeal, slot.type, slot.id, slot.enhancementLevel, slot.sealCnt,
                  slot.num_grade, slot.durability, slot.increaseType, slot.increaseValue,
                  slot.orb, slot.forgeLevel]
        for offset in (0, 3, 6, 10):
            fields += [slot.magicSeal[offset], int.from_bytes(slot.magicSeal[offset+1:offset+3], "big")]
        assert list(map(int, parts[:-1])) == fields, (reference, i, "decoded fields differ")
        slot.isSeal = 1
        slot.id = 0x12345678
        slot.enhancementLevel = 23
        slot.sealCnt = 5
        slot.num_grade = 0x98765432
        slot.durability = 54321
        slot.increaseType = 3
        slot.increaseValue = 12345
        slot.orb = 0xABCDEF01
        slot.forgeLevel = 9
        slot.otherworld = struct.pack("<H", 0xA5B6)
        slot.magicSeal = slot.buildMagicSeal((slot.coverMagic,
                            [[j + 21, "", j * 1000 + 321] for j in range(4)]))
        assert slot.build_bytes().hex() == parts[-1], (reference, i, "rebuilt bytes differ")
    print("PASS", reference, len(lines), "slots: decoded fields and all 61 rebuilt bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--reference", action="append", type=Path)
    args = parser.parse_args()
    if args.build:
        build()
    references = args.reference or [ROOT.parent / "DNF_pvf_python/dnfpkgtool/sqlManager2.py",
                                   *ROOT.parent.glob("*/2026.3.14*/dnfpkgtool/sqlManager2.py")]
    references = [p for p in references if p.is_file()]
    if not references:
        parser.error("No Python reference found; supply --reference path/to/sqlManager2.py")
    rng = random.Random(20261112)
    raw = bytearray()
    for kind in (0, 1, 2, 3, 4, 5, 6, 7, 10, 255):
        for _ in range(16):
            slot = bytearray(rng.randbytes(61))
            slot[1] = kind
            raw += slot
    path = OUT / "item-codec-fixtures.bin"
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        path.write_bytes(raw)
        result = subprocess.run([str(PROBE), "item-codec", str(path)],
                                capture_output=True, text=True, check=True)
        lines = result.stdout.splitlines()
        assert len(lines) == len(raw) // 61
        for reference in references:
            check(reference, raw, lines)
    finally:
        path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()

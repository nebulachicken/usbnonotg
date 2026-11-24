#!/usr/bin/env python3
"""Generate the shared C signatures, or scan a flat little-endian ARM memory dump."""

import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "hooks" / "patterns.json"
GENERATED = ROOT / "hooks" / "patterns.inc"
MAX_CANDIDATES = 16


def load_patterns():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for pattern in manifest["patterns"]:
        pattern["reference_address"] = int(pattern["reference_address"], 16)
        for word in pattern["words"]:
            for key in ("value", "mask", "reference"):
                word[key] = int(word[key], 16)
            if word["reference"] & word["mask"] != word["value"]:
                raise ValueError(f"Invalid mask in {pattern['id']}")
        if not 1 <= len(pattern["words"]) <= 32:
            raise ValueError(f"Invalid pattern length: {pattern['id']}")
    return manifest


def enum_name(pattern):
    return "USB_HOOK_" + pattern["id"].upper()


def generated_source(manifest):
    lines = [
        "/* Generated from hooks/patterns.json by tools/hook_scan.py. */", "",
        f"const char usb_hook_reference_os[] = {json.dumps(manifest['reference_os'])};",
        f"const char usb_hook_reference_sha256[] = {json.dumps(manifest['reference_sha256'])};", "",
    ]
    for pattern in manifest["patterns"]:
        lines.append(f"static const struct usb_hook_word pattern_{pattern['id']}[] = {{")
        for word in pattern["words"]:
            lines.append(f"    {{ 0x{word['value']:08x}, 0x{word['mask']:08x} }},")
        lines.extend(["};", ""])
    lines.append("const struct usb_hook_pattern usb_hook_patterns[USB_HOOK_COUNT] = {")
    for pattern in manifest["patterns"]:
        name = enum_name(pattern)
        lines.extend([
            f"    [{name}] = {{",
            f"        .id = {name},",
            f"        .name = \"{pattern['name']}\",",
            f"        .enum_name = \"{name}\",",
            f"        .reference_address = 0x{pattern['reference_address']:08x},",
            f"        .word_count = {len(pattern['words'])},",
            f"        .words = pattern_{pattern['id']}",
            "    },",
        ])
    lines.extend(["};", ""])
    return "\n".join(lines)


def matches_at(data, offset, pattern):
    length = len(pattern["words"]) * 4
    if offset < 0 or offset + length > len(data):
        return False
    for index, word in enumerate(pattern["words"]):
        actual = struct.unpack_from("<I", data, offset + index * 4)[0]
        if actual & word["mask"] != word["value"]:
            return False
    return True


def scan(data, base, patterns):
    results = {}
    for pattern in patterns:
        first = pattern["words"][0]
        if first["mask"] != 0xFFFFFFFF:
            raise ValueError("Offline search requires an exact first instruction")
        needle = struct.pack("<I", first["value"])
        hits = []
        offset = data.find(needle)
        while offset != -1:
            if not (base + offset) & 3 and matches_at(data, offset, pattern):
                hits.append(base + offset)
            offset = data.find(needle, offset + 1)
        results[pattern["id"]] = hits
    return results


def read_word(data, base, address):
    offset = address - base
    if address & 3 or offset < 0 or offset + 4 > len(data):
        raise ValueError(f"Address 0x{address:08x} is outside the dump")
    return struct.unpack_from("<I", data, offset)[0]


def decode_branch(address, instruction):
    if instruction & 0x0E000000 != 0x0A000000 or instruction >> 28 == 15:
        raise ValueError("Not an ARM B/BL instruction")
    displacement = instruction & 0xFFFFFF
    if displacement & 0x800000:
        displacement -= 0x1000000
    target = address + 8 + displacement * 4
    if not 0 <= target <= 0xFFFFFFFF:
        raise ValueError("Branch target overflow")
    return target


def decode_literal(address, instruction):
    if instruction & 0x0F7F0000 != 0x051F0000:
        raise ValueError("Not a supported ARM literal LDR")
    displacement = instruction & 0xFFF
    if not instruction & 0x800000:
        displacement = -displacement
    target = address + 8 + displacement
    if not 0 <= target <= 0xFFFFFFFF or target & 3:
        raise ValueError("Invalid literal address")
    return target


def relationships(data, base, results):
    """Read reference-layout relations without executing discovered functions."""
    lines = ["RELATIONSHIPS (reference instruction/structure layout only)"]
    hits = results["findmount"]
    if len(hits) != 1:
        return lines + ["Skipped: findmount is missing or ambiguous."]
    address = hits[0]
    try:
        call = address + 0x18
        parsedrive = decode_branch(call, read_word(data, base, call))
        lines.append(f"findmount BL +0x18 -> parsedrive 0x{parsedrive:08x}")
        lines.append(f"parsedrive relation matches scan: {results['parsedrive'] == [parsedrive]}")
        load = address + 0x2C
        literal = decode_literal(load, read_word(data, base, load))
        table = read_word(data, base, literal)
        lines.append(f"findmount LDR +0x2c -> literal 0x{literal:08x} -> mount table 0x{table:08x}")
        for index in range(20):
            mount = table + index * 0x28
            flags = read_word(data, base, mount + 0x0C)
            letter_and_drive = read_word(data, base, mount + 0x14)
            if flags & 1 and letter_and_drive & 0xFFFF == 0:
                ops = read_word(data, base, mount)
                lines.append(f"A mount 0x{mount:08x}, drive {letter_and_drive >> 16}, ops 0x{ops:08x}")
                for slot in range(29):
                    value = read_word(data, base, ops + slot * 4)
                    lines.append(f"  ops[{slot:02d}] = 0x{value:08x}")
                for hook, slot in (("allocate_fsnode", 24), ("deallocate_fsnode", 25),
                                   ("fsnode_to_string", 26), ("rename", 17)):
                    value = read_word(data, base, ops + slot * 4)
                    lines.append(f"  {hook} relation matches scan: {value in results[hook]}")
                break
        else:
            lines.append("No active A mount in the reference-layout table.")
    except ValueError as error:
        lines.append(f"Relationship read stopped: {error}")
    return lines


def report(manifest, data, base, results, os_id):
    lines = [
        "usb-storage hook scan, format 1",
        f"os_id = {os_id if os_id is not None else 'unknown'}",
        f"scan = [0x{base:08x}, 0x{base + len(data):08x})",
        f"reference = {manifest['reference_os']}",
        f"reference_sha256 = {manifest['reference_sha256']}",
        "Candidates are not ABI verification. No candidate code was executed.",
        "Masks: branch destinations and literal-load displacements are ignored.",
        "Matching rule: (instruction & mask) == value, ARM LE, 4-byte aligned.",
        "",
    ]
    for pattern in manifest["patterns"]:
        hits = results[pattern["id"]]
        status = "UNIQUE" if len(hits) == 1 else "MISSING" if not hits else "AMBIGUOUS"
        lines.extend([f"HOOK {pattern['name']} [{status}] count={len(hits)}",
                      f"reference_address = 0x{pattern['reference_address']:08x}", "pattern value/mask:"])
        for index, word in enumerate(pattern["words"]):
            lines.append(f"  +0x{index * 4:02x}: {word['value']:08x}/{word['mask']:08x}")
        for address in hits[:MAX_CANDIDATES]:
            lines.append(f"candidate = 0x{address:08x}")
            context = [read_word(data, base, address + index * 4)
                       for index in range(len(pattern["words"]))]
            lines.append("  actual words: " + " ".join(f"{word:08x}" for word in context))
        if len(hits) > MAX_CANDIDATES:
            lines.append("Additional addresses omitted; match count is complete.")
        lines.append("")
    lines.extend(relationships(data, base, results))
    lines.extend(["", "PROFILE CANDIDATES -- merge into os_profiles.c only after review", "{",
                  f"    .os_id = {os_id or 0},", "    .reviewed = 0,",
                  "    .abi = USB_OS_ABI_UNREVIEWED,", "    .hooks = {"])
    for pattern in manifest["patterns"]:
        hits = results[pattern["id"]]
        value = hits[0] if len(hits) == 1 else 0
        lines.append(f"        [{enum_name(pattern)}] = 0x{value:08x}, /* {len(hits)} matches */")
    lines.extend(["    }", "    /* Review USB offsets, callback signatures, DSTAT and mount layout. */", "}", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate", help="Regenerate the C pattern table")
    generate.add_argument("--check", action="store_true", help="Check that the shipped table is current")
    offline = commands.add_parser("scan", help="Scan a flat SDRAM/code image, not an OS upgrade container")
    offline.add_argument("dump", type=Path)
    offline.add_argument("--base", type=lambda value: int(value, 0), default=0x10000000)
    offline.add_argument("--os-id", type=int)
    offline.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifest = load_patterns()
    if args.command == "generate":
        source = generated_source(manifest)
        if args.check:
            if not GENERATED.exists() or GENERATED.read_text(encoding="utf-8") != source:
                parser.error("hooks/patterns.inc is stale; run generate")
            print("Shared C pattern table is current.")
        else:
            GENERATED.write_text(source, encoding="utf-8", newline="\n")
        return 0
    data = args.dump.read_bytes()
    if not data or args.base & 3 or args.base < 0 or args.base + len(data) > 0x100000000:
        parser.error("Expected a nonempty, aligned, 32-bit ARM memory image")
    results = scan(data, args.base, manifest["patterns"])
    text = report(manifest, data, args.base, results, args.os_id)
    if args.output:
        args.output.write_text(text, encoding="utf-8", newline="\n")
    else:
        print(text, end="")
    return 0 if all(len(hits) == 1 for hits in results.values()) else 2


if __name__ == "__main__":
    sys.exit(main())

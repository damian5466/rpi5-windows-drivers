#!/usr/bin/env python3
"""Build the exact-version Pi 5 ARM64 StorNVMe DMA compatibility patch.

Input: Microsoft stornvme.sys 10.0.26100.9539 with the pinned SHA256 below.
Output is unsigned; test-sign it and regenerate its OEM catalog before use.
This is a copy-only build recipe; the input files are never modified.
Run with original stornvme.sys, matching stornvme.inf, and a new output directory.
Pass --hardware-id with the target controller PCI hardware ID, or --any-device
to match any standard PCIe NVMe controller.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import re

SOURCE_SHA256 = "10330d2cf54a03208c677713332c77e38ce5c0029d87cf7a0fd9af33e121d249"
OUTPUT_SHA256 = "f647dd0e011997ef8c652d3cd9a887963cb31f202a7d0f0505c6221e787a8530"
PRP_SITES = {0xB654, 0xB838, 0x1FD1C, 0x2389C, 0x38B28}
HELPER_RVA = 0x39F90
# mov x9,x2; add x10,x9,#4096; dc cvac,x9; add x9,x9,#64;
# cmp x9,x10; b.lo -12; dsb sy; br x8 (StorPortGetPhysicalAddress).
HELPER = bytes.fromhex("e90302aa2a054091297a0bd5290101913f010aeba3ffff549f3f03d500011fd6")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def patch(source):
    require(hashlib.sha256(source).hexdigest() == SOURCE_SHA256,
            "Source is not the supported original ARM64 driver")
    data = bytearray(source)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    require(data[pe:pe + 4] == b"PE\0\0", "Invalid PE signature")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    require(machine == 0xAA64, "Not ARM64")
    opt = pe + 24
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    require(struct.unpack_from("<H", data, opt)[0] == 0x20B, "Not PE32+")
    sections = []
    for i in range(count):
        pos = opt + opt_size + i * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, pos + 8)
        sections.append((pos, virtual_size, rva, raw_size, raw))

    def offset(rva):
        for _, _, start, size, raw in sections:
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError(f"RVA outside file: {rva:#x}")

    def replace_instruction(rva, old, new):
        pos = offset(rva)
        require(struct.unpack_from("<I", data, pos)[0] == old,
                f"Unexpected instruction at {rva:#x}")
        struct.pack_into("<I", data, pos, new)

    for rva, reg in [(0x483C, 6), (0x489C, 6), (0x5518, 6), (0x5564, 6),
                     (0x55D8, 4), (0x6568, 4), (0x6798, 4), (0x6E58, 4)]:
        replace_instruction(rva, 0x52800020 | reg, 0x52800000 | reg)
    pos = offset(HELPER_RVA)
    require(not any(data[pos:pos + len(HELPER)]), "Helper space is occupied")
    data[pos:pos + len(HELPER)] = HELPER
    text = sections[0]
    require(text[2] == 0x1000 and HELPER_RVA + len(HELPER) <= text[2] + text[3],
            "Helper does not fit in .text")
    struct.pack_into("<I", data, text[0] + 8, HELPER_RVA + len(HELPER) - text[2])
    for site in PRP_SITES:
        branch = 0x94000000 | (((HELPER_RVA - site) // 4) & 0x3FFFFFF)
        replace_instruction(site, 0xD63F0100, branch)

    # The installed OS optimizes imported calls using this metadata. Leaving
    # the original entries would replace our BL instructions and skip the helper.
    load_config_rva = struct.unpack_from("<I", data, opt + 112 + 10 * 8)[0]
    lc = offset(load_config_rva)
    table_offset = struct.unpack_from("<I", data, lc + 0xE0)[0]
    section_index = struct.unpack_from("<H", data, lc + 0xE4)[0]
    start = sections[section_index - 1][4] + table_offset
    version, size = struct.unpack_from("<II", data, start)
    require(version == 1 and size == 5316, "Unexpected dynamic relocation table")
    end = start + 8 + size
    cursor = start + 8
    records, removed = [], []
    while cursor < end:
        symbol, length = struct.unpack_from("<QI", data, cursor)
        require(cursor + 12 + length <= end, "Truncated relocation record")
        payload = bytes(data[cursor + 12:cursor + 12 + length])
        if symbol == 8:  # IMAGE_DYNAMIC_RELOCATION_ARM64_KERNEL_IMPORT_CALL_TRANSFER
            blocks, at = [], 0
            while at < len(payload):
                page, block_size = struct.unpack_from("<II", payload, at)
                require(block_size >= 8 and block_size % 4 == 0
                        and at + block_size <= len(payload), "Invalid relocation block")
                entries = []
                for entry_at in range(at + 8, at + block_size, 4):
                    entry = struct.unpack_from("<I", payload, entry_at)[0]
                    site = page + 4 * (entry & 0x3FF)
                    if site in PRP_SITES:
                        require((entry >> 10) & 1 == 1 and (entry >> 11) & 31 == 8
                                and (entry >> 16) & 1 == 0 and entry >> 17 == 28,
                                "Unexpected import-call relocation")
                        removed.append(site)
                    else:
                        entries.append(struct.pack("<I", entry))
                if entries:
                    blocks.append(struct.pack("<II", page, 8 + 4 * len(entries))
                                  + b"".join(entries))
                at += block_size
            payload = b"".join(blocks)
        records.append(struct.pack("<QI", symbol, len(payload)) + payload)
        cursor += 12 + length
    require(cursor == end and len(removed) == 5 and set(removed) == PRP_SITES,
            "Did not remove exactly the five conflicting relocations")
    payload = b"".join(records)
    require(len(payload) == size - 20, "Unexpected relocation size change")
    data[start:end] = struct.pack("<II", version, len(payload)) + payload + bytes(20)

    # Remove the invalidated Microsoft signature from the output copy.
    security = opt + 112 + 4 * 8
    cert_offset, cert_size = struct.unpack_from("<II", data, security)
    require(cert_offset + cert_size == len(data), "Unexpected certificate layout")
    del data[cert_offset:]
    struct.pack_into("<II", data, security, 0, 0)
    checksum_offset = opt + 64
    struct.pack_into("<I", data, checksum_offset, 0)
    checksum = 0
    for i in range(0, len(data), 2):
        checksum += int.from_bytes(data[i:i + 2], "little")
        checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = (checksum & 0xFFFF) + (checksum >> 16)
    struct.pack_into("<I", data, checksum_offset, checksum + len(data))
    require(hashlib.sha256(data).hexdigest() == OUTPUT_SHA256,
            "Output differs from the validated patch artifact")
    return data


INF_SOURCE_SHA256 = "8cfb0401ac12f2ce0a9a2dfe80a3eaba999997cb4e7c293ab03be62ae9e1fed1"


def package_inf(source, hardware_id):
    require(re.fullmatch(r"PCI\\[A-Z0-9_&]+", hardware_id) is not None,
            "Expected a PCI hardware ID containing only letters, digits, _, and &")
    require(hashlib.sha256(source).hexdigest() == INF_SOURCE_SHA256,
            "INF is not the supported original ARM64 StorNVMe INF")
    text = source.decode("utf-16").replace("\r\n", "\n")
    text = text.replace("Provider    = %MSFT%", "Provider    = %Pi5Provider%\nCatalogFile = pi5-stornvme.cat")
    text = text.replace("DriverVer = 06/21/2006,10.0.26100.9539", "DriverVer = 09/19/2026,10.0.26100.9539")
    text = re.sub(r"(?ms)(^\[NVME\.NTarm64\]\n).*?(?=^\[)",
                  lambda m: m[1] + f"%Pi5DeviceDesc% = Stornvme_Inst, {hardware_id}\n\n", text)
    text = text.replace('[Strings]\n', '[Strings]\nPi5Provider = "RPi5 UEFI project"\nPi5DeviceDesc = "NVMe Controller (Pi5 DMA compatibility)"\n')
    result = b"\xff\xfe" + text.replace("\n", "\r\n").encode("utf-16-le")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Original Microsoft ARM64 stornvme.sys")
    parser.add_argument("inf", type=Path, help="Matching original stornvme.inf")
    parser.add_argument("output", type=Path, help="New package directory; must not exist")
    parser.add_argument("--hardware-id", help="PCI hardware ID of the target NVMe controller (required unless --any-device is set)")
    parser.add_argument("--any-device", action="store_true",
                        help="Match any standard PCIe NVMe controller using PCI\\CC_010802; overrides --hardware-id")
    args = parser.parse_args()
    if not args.any_device and not args.hardware_id:
        parser.error("provide --hardware-id or --any-device")
    hardware_id = r"PCI\CC_010802" if args.any_device else args.hardware_id.upper()
    try:
        driver = patch(args.source.read_bytes())
        inf = package_inf(args.inf.read_bytes(), hardware_id)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "stornvme.sys").write_bytes(driver)
        (args.output / "pi5-stornvme.inf").write_bytes(inf)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Error: {exc}\n")
    print(f"Unsigned driver SHA256: {OUTPUT_SHA256}")
    print(f"INF SHA256: {hashlib.sha256(inf).hexdigest()}")


if __name__ == "__main__":
    main()

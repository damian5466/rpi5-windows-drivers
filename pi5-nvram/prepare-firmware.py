#!/usr/bin/env python3
"""Assemble firmware build outputs and fresh file-backed variable stores.

Existing stores are validated and preserved, never reseeded by a rebuild.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import uuid
import zlib

HEADER = 4096
PAYLOAD = 0x20000
SIZE = HEADER + PAYLOAD

def relative_path(name, prefix=False):
    path = name.replace('\\', '/')
    parts = path.rstrip('/').split('/') if prefix and path.endswith('/') else path.split('/')
    if (not (prefix and not path) and any(p in ('', '.', '..') for p in parts)) or any(
            ord(c) < 32 or ord(c) > 126 or c in ':*?"<>|' for c in name) or '//' in path:
        raise ValueError('Paths must be relative to the boot volume, without traversal')
    if len(path) >= 128:
        raise ValueError('Paths must be shorter than 128 ASCII bytes')
    return path.encode('ascii')

def make(fd, name, store, prefix=''):
    if len(fd) != 0x1f0000 or fd[0x1d0028:0x1d002c] != b'_FVH':
        raise ValueError('Unexpected firmware/variable-volume layout')
    path = relative_path(name)
    nvram_prefix = relative_path(prefix, prefix=True)
    if not path.lower().endswith(b'.fd') or len(path.split(b'/')[-1]) < 4:
        raise ValueError('Expected a relative .fd path shorter than 128 ASCII bytes')
    payload = fd[-PAYLOAD:]
    if (struct.unpack_from('<Q', payload, 32)[0] != PAYLOAD or
            payload[16:32] != bytes.fromhex('8d2bf1ff96768b4ca9852747075b4f50') or
            payload[48:50] != b'\x48\x00' or
            sum(struct.unpack_from('<36H', payload)) & 0xffff):
        raise ValueError('Invalid firmware variable-volume header')
    header = bytearray(HEADER)
    struct.pack_into('<8sIIIIQ16sIIII', header, 0, b'RPINV001', 1, HEADER, PAYLOAD,
                     int(bool(nvram_prefix)), 1, store.bytes, zlib.crc32(payload), 0, len(fd), len(fd)-PAYLOAD)
    header[64:64+len(path)] = path
    header[192:192+len(nvram_prefix)] = nvram_prefix
    struct.pack_into('<I', header, 52, zlib.crc32(header))
    return bytes(header) + payload


def validate_store(data, firmware):
    if len(data) != SIZE:
        raise ValueError("Existing NVRAM file has the wrong size")
    fields = struct.unpack_from('<8sIIIIQ16sIIII', data)
    magic, version, hs, ds, flags, seq, sid, dc, hc, fs, off = fields
    h = bytearray(data[:HEADER]); h[52:56] = bytes(4)
    if (magic != b'RPINV001' or version != 1 or hs != HEADER or ds != PAYLOAD
            or flags != 0 or not 0 < seq < 0xffffffffffffffff or not any(sid)
            or zlib.crc32(h) != hc or zlib.crc32(data[HEADER:]) != dc
            or fs != len(firmware) or off != len(firmware) - PAYLOAD
            or data[64:192] != b'RPI_EFI.fd' + bytes(118)
            or any(data[192:HEADER])):
        raise ValueError("Existing NVRAM file is invalid or belongs to a different layout/path")
    # Also check the FV format, using its saved payload rather than the new defaults.
    make(firmware[:-PAYLOAD] + data[HEADER:], 'RPI_EFI.fd', uuid.UUID(bytes=sid))
    return sid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('config', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--file-nvram', choices=['0', '1'], default='1')
    args = parser.parse_args()
    try:
        firmware = args.firmware.read_bytes()
        config = args.config.read_text()
        config = '\n'.join(line for line in config.splitlines()
                           if not line.strip().startswith('initramfs RPI_NV')) + '\n'
        if args.file_nvram == '1':
            config += 'initramfs RPI_NV0.bin,RPI_NV1.bin 0x04000000\n'
        targets = [args.output / f'RPI_NV{n}.bin' for n in range(2)]
        data = None
        if args.file_nvram == '1':
            # Validate the new firmware layout even when reusing saved stores.
            data = make(firmware, 'RPI_EFI.fd', uuid.uuid4())
            if any(t.exists() for t in targets):
                if not all(t.is_file() and not t.is_symlink() for t in targets):
                    raise ValueError('An incomplete NVRAM pair exists; use a new output directory')
                ids = [validate_store(t.read_bytes(), firmware) for t in targets]
                if ids[0] != ids[1]:
                    raise ValueError('Existing NVRAM files have different store identities')
                data = None
                print('Preserving the existing NVRAM pair.')
        else:
            config = '\n'.join(line for line in config.splitlines()
                               if not line.startswith('initramfs RPI_NV')) + '\n'
        args.output.mkdir(parents=True, exist_ok=True)
        if data is not None:
            for target in targets:
                with target.open('xb') as stream:
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
        shutil.copyfile(args.firmware, args.output / 'RPI_EFI.fd')
        (args.output / 'config.txt').write_text(config)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'Error: {exc}\n')
    print(f'Firmware boot files: {args.output}')


if __name__ == '__main__':
    main()

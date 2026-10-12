#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Minimal writer for Android boot images, header version 0: the format the
# Fairphone 3 stock LK loads from the boot partition.
#
# usage: mkbootimg-v0.py --kernel K [--ramdisk R] [--cmdline C]
#                        [--base 0x80000000] [--pagesize 2048] --output O
import argparse
import hashlib
import struct

parser = argparse.ArgumentParser()
parser.add_argument("--kernel", required=True)
parser.add_argument("--ramdisk")
parser.add_argument("--cmdline", default="")
parser.add_argument("--base", type=lambda x: int(x, 0), default=0x80000000)
parser.add_argument("--pagesize", type=int, default=2048)
parser.add_argument("--output", required=True)
args = parser.parse_args()

kernel = open(args.kernel, "rb").read()
ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""
cmdline = args.cmdline.encode()
if len(cmdline) > 511:
    raise SystemExit("cmdline too long")

# Default mkbootimg offsets from the base address
kernel_addr = args.base + 0x00008000
ramdisk_addr = args.base + 0x01000000
second_addr = args.base + 0x00f00000
tags_addr = args.base + 0x00000100

sha = hashlib.sha1()
for blob in (kernel, ramdisk, b""):
    sha.update(blob)
    sha.update(struct.pack("<I", len(blob)))
image_id = sha.digest() + b"\0" * 12

header = struct.pack(
    "<8s10I16s512s32s1024s",
    b"ANDROID!",
    len(kernel), kernel_addr,
    len(ramdisk), ramdisk_addr,
    0, second_addr,
    tags_addr, args.pagesize,
    0,  # header version
    0,  # os version
    b"",  # board name
    cmdline,
    image_id,
    b"",  # extra cmdline
)


def pad(data):
    return data + b"\0" * (-len(data) % args.pagesize)


with open(args.output, "wb") as out:
    out.write(pad(header))
    out.write(pad(kernel))
    out.write(pad(ramdisk))

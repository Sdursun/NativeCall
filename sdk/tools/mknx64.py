#!/usr/bin/env python3
"""mknx64.py <module.narm> <module.dll> <out.nx64>

An x64 module for WinUAE (x64/README.md): a 32-byte big-endian header and the DLL.

    0  'NX64'
    4  version (1)
    8  CRC32 of the .narm built from the same kernels (its build ID): nativecall.library
       matches the two, so the x64 module of a program is the one of the same build
   12  kernels (as in the .narm)
   16  offset of the DLL (32)
   20  size of the DLL
   24  0, 0
"""
import struct
import sys
import zlib

narm = open(sys.argv[1], 'rb').read()
dll = open(sys.argv[2], 'rb').read()
if narm[:4] != b'NARM':
    sys.exit('mknx64: %s is not a .narm module' % sys.argv[1])
count = struct.unpack('>I', narm[8:12])[0]

# The PE time stamp and checksum: ld derives the "time stamp" from the image, debug paths
# included, so it differs between build directories. Windows does not check either in a DLL;
# zeroed, the same kernels give the same .nx64 wherever they are built.
dll = bytearray(dll)
if dll[:2] == b'MZ':
    pe = struct.unpack('<I', dll[0x3c:0x40])[0]
    if dll[pe:pe + 4] == b'PE\0\0':
        dll[pe + 8:pe + 12] = b'\0\0\0\0'               # COFF TimeDateStamp
        dll[pe + 24 + 64:pe + 24 + 68] = b'\0\0\0\0'    # optional header CheckSum
dll = bytes(dll)
header = struct.pack('>4sIIIIIII', b'NX64', 1, zlib.crc32(narm) & 0xffffffff, count, 32, len(dll), 0, 0)
open(sys.argv[3], 'wb').write(header + dll)

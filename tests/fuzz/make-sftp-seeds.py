#!/usr/bin/env python3
# @file      make-sftp-seeds.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Seeds for sftp_fuzz (devplan/network.md §24): request sequences as the target reads them (each a
# 2-byte length, big end first, then the request; the INIT is the target's own). The first handle
# opened is index 0, generation 1.
import os
import struct
import sys


def s(b):
    return struct.pack('>I', len(b)) + b


def pkt(t, request_id, body=b''):
    return bytes([t]) + struct.pack('>I', request_id) + body


def frame(p):
    return struct.pack('>H', len(p)) + p


H0 = b'\0\0\0\0\0\0\0\1'
SEEDS = {
    'open-read-close': [pkt(3, 1, s(b'/ram0/p0/f.txt') + struct.pack('>II', 1, 0)),
                        pkt(5, 2, s(H0) + struct.pack('>QI', 0, 32768)), pkt(4, 3, s(H0))],
    'write': [pkt(3, 1, s(b'out.bin') + struct.pack('>III', 0x1a, 4, 0o644)),
              pkt(6, 2, s(H0) + struct.pack('>Q', 0) + s(b'x' * 1000)),
              pkt(10, 3, s(H0) + struct.pack('>IQ', 1, 5))],
    'dir': [pkt(11, 1, s(b'.')), pkt(12, 2, s(H0)), pkt(12, 3, s(H0)), pkt(4, 4, s(H0))],
    'paths': [pkt(16, 1, s(b'../a/./b//c/..')), pkt(17, 2, s(b'/x')), pkt(7, 3, s(b'y')),
              pkt(19, 4, s(b'link')), pkt(20, 5, s(b'target') + s(b'link'))],
    'change': [pkt(14, 1, s(b'd') + struct.pack('>I', 0)), pkt(18, 2, s(b'a') + s(b'b')), pkt(13, 3, s(b'a')),
               pkt(15, 4, s(b'd')), pkt(9, 5, s(b'f') + struct.pack('>IQII', 0x9, 3, 1, 2))],
    'ext': [pkt(200, 1, s(b'limits@openssh.com')), pkt(200, 2, s(b'statvfs@openssh.com') + s(b'/')),
            pkt(200, 3, s(b'expand-path@openssh.com') + s(b'~/x')),
            pkt(200, 4, s(b'posix-rename@openssh.com') + s(b'a') + s(b'b')),
            pkt(200, 5, s(b'fsync@openssh.com') + s(H0))],
}

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: make-sftp-seeds.py DIR')
    os.makedirs(sys.argv[1], exist_ok=True)
    for name, packets in SEEDS.items():
        with open(os.path.join(sys.argv[1], name), 'wb') as f:
            f.write(b''.join(frame(p) for p in packets))

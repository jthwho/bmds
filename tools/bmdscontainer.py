# @file      bmdscontainer.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# The signed container of update packages (.bmup), packages (.bmpk) and package indexes (.bmpi):
# components/bmds_core/include/bmds/signedcontainer.h's format, written and read. Used by
# tools/mkupdate and tools/mkpkg. Ed25519 keys come from the cryptography package (in IDF's
# Python); BLAKE2s from hashlib.

import hashlib
import os
import struct
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

PREFIX = '<4sHHII8s'
PREFIX_BYTES = 24
SECTION = '<IIII32s'
SECTION_BYTES = 48
SIGNATURE_BYTES = 64
MAX_SECTIONS = 16
KEY_ID_BYTES = 8

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
KEYS = os.path.join(ROOT, 'keys')
RAW = (serialization.Encoding.Raw, serialization.PublicFormat.Raw)


class ContainerError(Exception):
    pass


def load_key(path):
    """The Ed25519 private key in PEM file path."""
    with open(path, 'rb') as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ContainerError('%s is not an Ed25519 key' % path)
    return key


def public_bytes(key):
    """The 32 bytes of a private key's public half."""
    return key.public_key().public_bytes(*RAW)


def key_id(public):
    """A public key's id: the first 8 bytes of BLAKE2s-256 of its 32 bytes."""
    return hashlib.blake2s(public).digest()[:KEY_ID_BYTES]


def load_pubs(paths):
    """(public key bytes, a name: its file and comment) for each key in the files, in order."""
    keys = []
    for path in paths:
        for line in open(path):
            parts = line.split(None, 2)
            if len(parts) >= 2 and parts[0] == 'ed25519':
                name = os.path.basename(path) + (': ' + parts[2].strip() if len(parts) > 2 else '')
                keys.append((bytes.fromhex(parts[1]), name))
    return keys


def default_pubs():
    """keys/dev.pub and keys/trusted.pub, those that exist."""
    return [p for p in (os.path.join(KEYS, 'dev.pub'), os.path.join(KEYS, 'trusted.pub')) if os.path.exists(p)]


def keygen(key_path, pub_path, comment):
    """A new key: the private half to key_path (PEM, mode 600, never overwritten), the public to pub_path."""
    if os.path.exists(key_path):
        raise ContainerError('%s exists: not overwriting a key' % key_path)
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(pem)
    with open(pub_path, 'w') as f:
        f.write('ed25519 %s %s\n' % (public_bytes(key).hex(), comment))


def text_field(text, size, what):
    """text as a NUL-padded field of size bytes: printable ASCII only."""
    data = text.encode('ascii', 'replace')
    if len(data) > size or any(b < 0x20 or b > 0x7e for b in data):
        raise ContainerError('%s "%s" must be at most %d printable ASCII characters' % (what, text, size))
    return data.ljust(size, b'\0')


def build(magic, fmt, body, sections, key):
    """The bytes of a container: magic, format, the kind's header fields (body), sections as
    (type, bytes), signed with key."""
    if len(sections) > MAX_SECTIONS:
        raise ContainerError('%d sections: at most %d' % (len(sections), MAX_SECTIONS))
    header_bytes = PREFIX_BYTES + len(body)
    header = struct.pack(PREFIX, magic, fmt, header_bytes, 0, len(sections), key_id(public_bytes(key))) + body
    offset = header_bytes + len(sections) * SECTION_BYTES + SIGNATURE_BYTES
    table = b''
    for kind, data in sections:
        table += struct.pack(SECTION, kind, 0, offset, len(data), hashlib.blake2s(data).digest())
        offset += len(data)
    signed = header + table
    return signed + key.sign(signed) + b''.join(data for _, data in sections)


class Container:
    """A container read from data, checked as far as its structure: magic, format, sections inside
    the file. body is the kind's header fields; sections are (type, offset, size, hash good)."""

    def __init__(self, data, magic, fmt, header_bytes, what):
        if data[:4] != magic:
            raise ContainerError('not %s' % what)
        if len(data) < PREFIX_BYTES:
            raise ContainerError('cut short')
        _, got_fmt, got_header, flags, count, kid = struct.unpack_from(PREFIX, data, 0)
        if got_fmt != fmt or got_header != header_bytes or flags != 0 or count > MAX_SECTIONS:
            raise ContainerError('format %d, header %d, flags %d, %d sections: not one this tool reads'
                                 % (got_fmt, got_header, flags, count))
        self.data = data
        self.key_id = kid
        self.body = data[PREFIX_BYTES:header_bytes]
        self.signed_bytes = header_bytes + count * SECTION_BYTES
        if len(data) < self.signed_bytes + SIGNATURE_BYTES:
            raise ContainerError('cut short')
        self.signature = data[self.signed_bytes:self.signed_bytes + SIGNATURE_BYTES]
        self.sections = []
        for i in range(count):
            kind, sflags, offset, size, digest = struct.unpack_from(SECTION, data, header_bytes + i * SECTION_BYTES)
            ok = (sflags == 0 and offset >= self.signed_bytes + SIGNATURE_BYTES and offset + size <= len(data)
                  and hashlib.blake2s(data[offset:offset + size]).digest() == digest)
            self.sections.append((kind, offset, size, ok))

    def section(self, kind):
        """The bytes of the first section of kind, or None."""
        for k, offset, size, _ in self.sections:
            if k == kind:
                return self.data[offset:offset + size]
        return None

    def hashes_good(self):
        return all(ok for _, _, _, ok in self.sections)

    def signer(self, pubs):
        """The name of the key in pubs ((bytes, name), load_pubs()) that signed, or None."""
        for public, name in pubs:
            if key_id(public) != self.key_id:
                continue
            try:
                Ed25519PublicKey.from_public_bytes(public).verify(self.signature, self.data[:self.signed_bytes])
                return name
            except InvalidSignature:
                pass
        return None


def fail(tool, message):
    sys.exit('%s: %s' % (tool, message))

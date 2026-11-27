"""SARC names are read only up to their terminator, not by copying/splitting the archive tail. Synthetic archives; no game files needed."""
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def sarc(entries):
    """Build a little-endian SARC from (name or None, payload) pairs, preserving their order."""
    names, nodes, data = bytearray(), bytearray(), bytearray()
    for name, payload in entries:
        attr = 0
        if name is not None:
            names += bytes(-len(names) % 4)
            attr = 0x01000000 | (len(names) // 4)
            names += (name.encode('utf-8') if isinstance(name, str) else name) + b'\0'
        start = len(data); data += payload
        nodes += struct.pack('<4I', 0, attr, start, len(data))
    names += bytes(-len(names) % 4)
    doff = 0x14 + 12 + len(nodes) + 8 + len(names)
    header = struct.pack('<4sHHIIHH', b'SARC', 0x14, 0xfeff, doff + len(data), doff, 0x100, 0)
    return header + struct.pack('<4sHHI', b'SFAT', 12, len(entries), 0x65) + nodes + struct.pack('<4sHH', b'SFNT', 8, 0) + names + data


def legacy_members(d):
    """Pre-optimization reader, retained as a valid-input oracle and benchmark baseline."""
    hl, bom, size, doff = struct.unpack_from('<HHII', d, 4)
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, start, end = struct.unpack_from('<4I', d, hl + 12 + 16 * i)
        name = d[names + (attr & 0xffffff) * 4:].split(b'\0')[0].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'
        yield name, d[doff + start:doff + end]


class BoundedNames(bytes):
    """Deterministic complexity regression check: no suffix slices or unbounded filename searches."""
    def __getitem__(self, key):
        if isinstance(key, slice) and key.stop is None:
            raise AssertionError('copied the archive tail instead of just the filename')
        return super().__getitem__(key)

    def find(self, sub, start=0, end=None):
        doff = struct.unpack_from('<I', self, 12)[0]
        if sub == b'\0' and (end is None or end > doff):
            raise AssertionError('filename search escaped the name table')
        return super().find(sub, start, end)


class SarcMembers(unittest.TestCase):
    def test_names_order_payloads_and_utf8_match_legacy(self):
        entries = [('last.bin', b'a\0b'), (None, b'unnamed'), ('caf\u00e9/\u96ea.bin', b'utf8'),
                   (b'bad\xff.bin', b'invalid UTF-8 name'), ('', b''), ('last.bin', b'duplicate')]
        d = sarc(entries)
        expected = [('last.bin', b'a\0b'), ('#1', b'unnamed'), ('caf\u00e9/\u96ea.bin', b'utf8'),
                    ('bad\ufffd.bin', b'invalid UTF-8 name'), ('', b''), ('last.bin', b'duplicate')]
        self.assertEqual(list(legacy_members(d)), expected)
        self.assertEqual(list(ss.sarc_members(d)), expected)

    def test_empty_archive(self):
        self.assertEqual(list(ss.sarc_members(sarc([]))), [])

    def test_unnamed_archive_needs_no_name_table_bytes(self):
        self.assertEqual(list(ss.sarc_members(sarc([(None, b'abc'), (None, b'')]))), [('#0', b'abc'), ('#1', b'')])

    def test_shared_name_offsets(self):
        d = bytearray(sarc([('shared', b'first'), ('ignored', b'second')]))
        struct.pack_into('<I', d, 0x14 + 12 + 16 + 4, 0x01000000)
        self.assertEqual(list(ss.sarc_members(bytes(d))), [('shared', b'first'), ('shared', b'second')])

    def test_many_names_with_large_trailing_payload_do_not_copy_the_tail(self):
        entries = [(f'file{i}', b'x') for i in range(128)] + [('large.bin', bytes(2 << 20))]
        self.assertEqual(list(ss.sarc_members(BoundedNames(sarc(entries)))), entries)

    def test_nested_archive_reaches_the_shader_scanner(self):
        shader = bytearray(0x68)
        shader[:8] = b'BNSH\0\0\0\0'; shader[0x60:0x64] = b'grsc'
        struct.pack_into('<I', shader, 0x1c, len(shader))
        inner = sarc([('shader.bnsh', bytes(shader)), (None, b'plain member')])
        outer = sarc([('inner.pack', inner)])
        self.assertEqual(list(ss.sarc_members(outer)), list(legacy_members(outer)))
        found, result = [('synthetic shader', 'vs', b'control', b'code')], []
        with patch.object(ss, 'bnsh_shaders', return_value=found) as read:
            ss.scan_blob(outer, '/fixture', result)
        read.assert_called_once_with(bytes(shader), 0, '/fixture/inner.pack/shader.bnsh')
        self.assertEqual(result, found)

    def test_name_offset_must_stay_before_file_data(self):
        original = sarc([('name', b'payload\0')])
        names = 0x14 + 12 + 16 + 8
        doff = struct.unpack_from('<I', original, 12)[0]
        for offset in ((doff - names) // 4, 0xffffff):
            with self.subTest(offset=offset):
                d = bytearray(original)
                struct.pack_into('<I', d, 0x14 + 12 + 4, 0x01000000 | offset)
                with self.assertRaisesRegex(ValueError, 'SARC member 0: filename offset'):
                    list(ss.sarc_members(bytes(d)))

    def test_name_terminator_cannot_come_from_the_payload(self):
        d = bytearray(sarc([('name', b'\0payload')]))
        names = 0x14 + 12 + 16 + 8
        doff = struct.unpack_from('<I', d, 12)[0]
        d[names:doff] = b'x' * (doff - names)
        with self.assertRaisesRegex(ValueError, 'SARC member 0: unterminated filename'):
            list(ss.sarc_members(BoundedNames(d)))

    def test_name_table_bounds_are_validated(self):
        original = sarc([('name', b'payload')])
        names = 0x14 + 12 + 16 + 8
        for doff in (names - 1, len(original) + 4):
            with self.subTest(data_offset=doff):
                d = bytearray(original); struct.pack_into('<I', d, 12, doff)
                with self.assertRaisesRegex(ValueError, 'SARC: invalid filename table bounds'):
                    list(ss.sarc_members(bytes(d)))


if __name__ == '__main__':
    unittest.main()

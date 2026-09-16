#!/usr/bin/env python3
"""Focused regression tests for the classic-Mac pilot layout conversion."""

import unittest

import convert_mac_pilot as converter


class ConvertMacPilotTest(unittest.TestCase):
    def test_primary_block_removes_each_mission_padding_group(self) -> None:
        plain = bytearray(converter.MAC_BLOCK1_SIZE)
        plain[0:2] = b"\x00\x6a"
        plain[0x295E:0x297E] = bytes(range(0x20))
        plain[0x297E:0x2980] = b"XX"
        plain[0x2980:0x2993] = bytes(range(0x20, 0x33))
        plain[0x2993] = 0xEE
        plain[0x2994:0x3245] = bytes(i & 0xFF for i in range(0x8B1))
        plain[0x3245:0x3248] = b"YYY"
        plain[0xE9AE:0xE9B2] = b"\x00\x00\x7c\xeb"

        converted = converter.convert_primary_block(
            converter.simple_crypt(plain)
        )
        decoded = converter.simple_crypt(converted)

        self.assertEqual(len(decoded), converter.BLOCK1_SIZE)
        self.assertEqual(decoded[0:2], b"\x00\x6a")
        self.assertEqual(decoded[0x295E:0x297E], bytes(range(0x20)))
        self.assertEqual(decoded[0x297E:0x2991], bytes(range(0x20, 0x33)))
        self.assertEqual(decoded[0xE94E:0xE952], b"\x00\x00\x7c\xeb")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Metadata-only regression tests: no Mach-O fixture is linked or executed."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(sys.argv.pop(1)).resolve()
REAL_OBJECT = os.environ.get("MAC2IOS_REAL_OBJECT")
CPUS = (0x01000007, 0x0100000C)
LEGACY = {0x24, 0x25, 0x2F, 0x30}
BUILD = 0x32


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def legacy():
    return struct.pack("<4I", 0x24, 16, 0x000A0700, 0x000E0400)


def modern(tools=True):
    command = struct.pack("<6I", BUILD, 32 if tools else 24,
                          6, 0x000D0100, 0x00110400, int(tools))
    return command + (struct.pack("<2I", 1, 0x00150108) if tools else b"")


def object_bytes(cpu, versions):
    # A valid data-only MH_OBJECT with an external symbol, section and symtab.
    command_size = 152 + sum(map(len, versions)) + 24
    data_offset = 32 + command_size
    payload = bytes(range(32))
    symbol_offset = data_offset + len(payload)
    strings = b"\0_fixture\0"
    string_offset = symbol_offset + 16
    header = struct.pack("<8I", 0xFEEDFACF, cpu, 3 if cpu == CPUS[0] else 0,
                         1, 2 + len(versions), command_size, 0, 0)
    segment = struct.pack("<2I16s4Q4I", 0x19, 152, b"", 0, len(payload),
                          data_offset, len(payload), 7, 3, 1, 0)
    section = struct.pack("<16s16s2Q8I", b"__data", b"__DATA", 0, len(payload),
                          data_offset, 3, 0, 0, 0, 0, 0, 0)
    symtab = struct.pack("<6I", 2, 24, symbol_offset, 1, string_offset, len(strings))
    symbol = struct.pack("<IBBHQ", 1, 0x0F, 1, 0, 0)
    return header + segment + section + b"".join(versions) + symtab + payload + symbol + strings


def load_commands(data):
    count, size = struct.unpack_from("<2I", data, 16)
    result, offset = [], 32
    for _ in range(count):
        command, length = struct.unpack_from("<2I", data, offset)
        assert length >= 8 and offset + length <= 32 + size
        result.append((command, offset, data[offset:offset + length]))
        offset += length
    assert offset == 32 + size
    return result


def archive(members):
    result = bytearray(b"!<arch>\n")
    offsets = []
    for name, data in members:
        # BSD extended names include NUL padding, as actual Darwin ar emits.
        encoded = name.encode() + b"\0"
        encoded += b"\0" * (-len(encoded) % 8)
        body = encoded + data
        header = (f"#1/{len(encoded)}".ljust(16) + "0".ljust(12) +
                  "0".ljust(6) + "0".ljust(6) + "644".ljust(8) +
                  str(len(body)).ljust(10) + "`\n").encode()
        assert len(header) == 60
        offsets.append((len(result) + 60 + len(encoded), len(data)))
        result += header + body + (b"\n" if len(body) % 2 else b"")
    return bytes(result), offsets


class ConverterTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def convert(self, original, simulator=False, success=True, filename="fixture.a"):
        path = self.root / filename
        path.write_bytes(original)
        result = subprocess.run([str(TOOL), *(["-s"] if simulator else []), str(path)],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stderr.decode())
        return path.read_bytes()

    def check_object(self, before, after, platform):
        self.assertEqual(len(before), len(after))
        old_commands, new_commands = load_commands(before), load_commands(after)
        old_size, new_size = u32(before, 20), u32(after, 20)
        builds = [c for c in new_commands if c[0] == BUILD]
        old_builds = [c for c in old_commands if c[0] == BUILD]
        legacies = [c for c in new_commands if c[0] in LEGACY]
        if old_builds:
            self.assertEqual(len(builds), 1)
            self.assertEqual(legacies, [])
            self.assertEqual(u32(builds[0][2], 8), platform)
            self.assertEqual(builds[0][2][12:], old_builds[0][2][12:])
        else:
            self.assertEqual(builds, [])
            self.assertTrue(all(c[0] == 0x25 for c in legacies))
        # All unrelated load commands and every byte outside the command table
        # retain their original offsets/content, including section/symtab fields.
        self.assertEqual([c[2] for c in new_commands if c[0] not in LEGACY | {BUILD}],
                         [c[2] for c in old_commands if c[0] not in LEGACY | {BUILD}])
        self.assertEqual(after[32 + old_size:], before[32 + old_size:])
        self.assertEqual(after[32 + new_size:32 + old_size], b"\0" * (old_size - new_size))
        self.assertEqual(before[:16], after[:16])
        self.assertEqual(before[24:32], after[24:32])

    def apple_accepts(self, data):
        if sys.platform != "darwin" or not shutil.which("xcrun"):
            return
        path = self.root / "data-only.o"
        path.write_bytes(data)
        for tool, args in (("nm", ["-g"]), ("otool", ["-l"])):
            result = subprocess.run(["xcrun", tool, *args, str(path)],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertNotIn(b"malformed", result.stderr)

    def test_both_cpus_and_version_orders(self):
        for cpu in CPUS:
            for versions in ([legacy()], [modern()], [legacy(), modern()],
                             [modern(), legacy()], [legacy(), modern(False)],
                             [modern(False), legacy()]):
                for simulator in (False, True):
                    with self.subTest(cpu=cpu, versions=len(versions), simulator=simulator):
                        obj = object_bytes(cpu, versions)
                        original, ((offset, length),) = archive([("fixture.o", obj)])
                        changed = self.convert(original, simulator)
                        self.assertEqual(changed[:offset], original[:offset])
                        self.assertEqual(changed[offset + length:], original[offset + length:])
                        output = changed[offset:offset + length]
                        self.check_object(obj, output, 7 if simulator else 2)
                        self.apple_accepts(output)
                        self.apple_accepts(changed)
                        self.assertEqual(self.convert(changed, simulator), changed)

    def test_direct_objects_both_cpus_and_version_orders(self):
        for cpu in CPUS:
            for versions in ([legacy()], [modern()], [legacy(), modern()],
                             [modern(), legacy()], [legacy(), modern(False)],
                             [modern(False), legacy()]):
                for simulator in (False, True):
                    with self.subTest(cpu=cpu, versions=len(versions), simulator=simulator):
                        original = object_bytes(cpu, versions)
                        changed = self.convert(original, simulator, filename="direct.o")
                        self.check_object(original, changed, 7 if simulator else 2)
                        self.apple_accepts(changed)
                        self.assertEqual(self.convert(changed, simulator, filename="direct.o"), changed)

    def test_dispatch_uses_content_not_filename(self):
        obj = object_bytes(CPUS[0], [legacy(), modern()])
        original, _ = archive([("fixture.o", obj)])
        for filename in ("direct.a", "direct.bin"):
            self.check_object(obj, self.convert(obj, filename=filename), 2)
        for filename in ("archive.o", "archive.bin"):
            self.apple_accepts(self.convert(original, filename=filename))
        for cpu in CPUS:
            obj = object_bytes(cpu, [])
            self.assertEqual(self.convert(obj, filename="no-version.o"), obj)

    def test_invalid_option_does_not_mutate_input(self):
        obj = object_bytes(CPUS[0], [legacy(), modern()])
        wrapped, _ = archive([("fixture.o", obj)])
        for original in (obj, wrapped):
            for option in ("-S", "-x", "-z", "-sx"):
                with self.subTest(archive=original.startswith(b"!<arch>"), option=option):
                    path = self.root / "invalid-option.bin"
                    path.write_bytes(original)
                    result = subprocess.run([str(TOOL), option, str(path)],
                                            capture_output=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(path.read_bytes(), original)

    def test_invalid_direct_input_is_unchanged(self):
        invalid = [b"", b"x", b"not a Mach-O object", b"!<arch>",
                   bytes.fromhex("feedfacf") + bytes(32),  # Big-endian magic.
                   bytes.fromhex("cafebabe") + bytes(32)] # Fat input is unsupported.
        for cpu in CPUS:
            obj = object_bytes(cpu, [legacy(), modern()])
            invalid.extend((obj[:3], obj[:4], obj[:24], obj[:32 + u32(obj, 20) - 1]))
            for offset, value in ((20, len(obj)), (16, 1000), (36, 0),
                                  (32 + 152 + 16 + 20, 100)):
                broken = bytearray(obj)
                struct.pack_into("<I", broken, offset, value)
                invalid.append(bytes(broken))
        for original in invalid:
            with self.subTest(length=len(original), magic=original[:4].hex()):
                self.assertEqual(self.convert(original, success=False, filename="invalid.o"), original)

    def test_missing_version_is_unchanged(self):
        for cpu in CPUS:
            original, _ = archive([("no-version.o", object_bytes(cpu, []))])
            self.assertEqual(self.convert(original), original)

    def test_archive_layout_and_odd_member_padding(self):
        one = object_bytes(CPUS[0], [legacy(), modern()])
        two = object_bytes(CPUS[1], [modern(), legacy()])
        original, offsets = archive([("__.SYMDEF SORTED", b"index"),
                                     ("odd-data", b"abc"), ("one.o", one), ("two.o", two)])
        changed = self.convert(original)
        expected = bytearray(original)
        for (offset, length), obj in zip(offsets[2:], (one, two)):
            self.check_object(obj, changed[offset:offset + length], 2)
            expected[offset:offset + length] = changed[offset:offset + length]
        self.assertEqual(changed, bytes(expected))
        self.assertEqual(self.convert(changed), changed)

    def test_malformed_commands_leave_failing_member_unchanged(self):
        obj = object_bytes(CPUS[0], [legacy(), modern()])
        variants = []
        def corrupt(offset, value):
            data = bytearray(obj)
            struct.pack_into("<I", data, offset, value)
            variants.append(bytes(data))
        corrupt(20, len(obj))                  # Table extends past member.
        corrupt(16, 3)                         # Command count leaves table bytes.
        corrupt(16, 1000)                      # Impossible command count.
        corrupt(32 + 4, 151)                   # Unaligned command size.
        corrupt(32 + 4, 0)                     # Zero-sized command.
        corrupt(32 + 4, len(obj))              # Oversized command.
        corrupt(32 + 152 + 4, 8)               # Truncated legacy command.
        corrupt(32 + 152 + 16 + 20, 100)       # Invalid ntools.
        corrupt(32 + 152 + 16 + 4, 16)         # Truncated modern command.
        variants.append(object_bytes(CPUS[0], [modern(), modern()]))
        variants.append(obj[:24])              # Truncated Mach-O header.
        for malformed in variants:
            with self.subTest(length=len(malformed), sha=malformed[-4:].hex()):
                original, _ = archive([("bad.o", malformed)])
                self.assertEqual(self.convert(original, success=False), original)
        # A failure after an earlier member was normalized is not an archive-wide
        # transaction; the failing member and its header/padding still stay intact.
        original, offsets = archive([("good.o", obj), ("bad.o", variants[0])])
        changed = self.convert(original, success=False)
        bad_start = offsets[1][0] - 60 - 8  # BSD name occupies eight bytes.
        self.assertEqual(changed[bad_start:], original[bad_start:])

    def test_archive_member_bounds(self):
        obj = object_bytes(CPUS[0], [legacy()])
        original, _ = archive([("fixture.o", obj)])
        odd, _ = archive([("fixture.o", obj + b"x")])
        for malformed in (original[:-1], original[:30], original[:8] + b"short", odd[:-1]):
            self.assertEqual(self.convert(malformed, success=False), malformed)

    def test_odd_extended_name_padding(self):
        # BSD extended names may have odd length; the ar pad covers the whole
        # member, name included, and the following member must stay reachable.
        first = object_bytes(CPUS[0], [legacy(), modern()])
        second = object_bytes(CPUS[1], [modern(), legacy()])
        result = bytearray(b"!<arch>\n")
        offsets = []
        for name, data in (("abcd", first), ("second.o", second)):
            encoded = name.encode() + b"\0"  # 5 and 9 bytes: no 8-byte padding.
            body = encoded + data
            header = (f"#1/{len(encoded)}".ljust(16) + "0".ljust(12) +
                      "0".ljust(6) + "0".ljust(6) + "644".ljust(8) +
                      str(len(body)).ljust(10) + "`\n").encode()
            assert len(header) == 60
            offsets.append((len(result) + 60 + len(encoded), len(data)))
            result += header + body + (b"\n" if len(body) % 2 else b"")
        original = bytes(result)
        changed = self.convert(original)
        for (offset, length), obj in zip(offsets, (first, second)):
            self.check_object(obj, changed[offset:offset + length], 2)
        self.assertEqual(self.convert(changed), changed)


    def test_table_fills_buffer(self):
        # A first command consuming the whole 64kb table plus an inflated
        # ncmds must be rejected without reading past the buffer.
        obj = struct.pack("<8I", 0xFEEDFACF, CPUS[0], 3, 1, 2, 65504, 0, 0)
        obj += struct.pack("<2I", 0x19, 65504).ljust(65504, b"\0")
        original, _ = archive([("big.o", obj)])
        self.assertEqual(self.convert(original, success=False), original)


    def test_empty_trailing_member(self):
        # A valid zero-length member after a converted object must not be
        # judged from the previous member's leftover buffer contents.
        obj = object_bytes(CPUS[0], [legacy(), modern()])
        result = bytearray(b"!<arch>\n")
        start, offset = None, None
        for name, data in (("obj.o", obj), ("empty.o", b"")):
            encoded = name.encode() + b"\0"
            encoded += b"\0" * (-len(encoded) % 8)
            body = encoded + data
            header = (f"#1/{len(encoded)}".ljust(16) + "0".ljust(12) +
                      "0".ljust(6) + "0".ljust(6) + "644".ljust(8) +
                      str(len(body)).ljust(10) + "`\n").encode()
            start = len(result) if not data else start
            offset = offset if not data else len(result) + 60 + len(encoded)
            result += header + body + (b"\n" if len(body) % 2 else b"")
        original = bytes(result)
        changed = self.convert(original)
        self.check_object(obj, changed[offset:offset + len(obj)], 2)
        self.assertEqual(changed[start:], original[start:])
        self.assertEqual(self.convert(changed), changed)

    @unittest.skipUnless(shutil.which("cc"), "requires cc to build the fault-injection shim")
    def test_io_fault_injection(self):
        # Interpose the tool's stdio calls and check every injected I/O fault
        # escapes as a nonzero exit instead of a silently mangled file.
        tests_dir = Path(__file__).resolve().parent
        shim = self.root / ("faultinj.dylib" if sys.platform == "darwin" else "faultinj.so")
        flags = ["-O1", "-dynamiclib"] if sys.platform == "darwin" else ["-O1", "-shared", "-fPIC"]
        build = subprocess.run(["cc", *flags, "-o", str(shim), str(tests_dir / "faultinj.c")],
                               capture_output=True, timeout=60)
        self.assertEqual(build.returncode, 0, build.stderr.decode())
        inserted = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"

        def fault_env(fault):
            env = dict(os.environ)
            env[inserted] = str(shim)
            env["MAC2IOS_FAULT"] = fault
            return env

        obj = object_bytes(CPUS[0], [legacy(), modern()])
        original, _ = archive([("fixture.o", obj)])
        # Two canaries on different faults: skip only when both are inert, so
        # sabotaging any single guard still turns its subTest into a failure
        # rather than a skip (each canary is blind only to its own guard).
        alive = []
        for canary_fault in ("fread", "fclose"):
            canary = self.root / f"canary-{canary_fault}.a"
            canary.write_bytes(original)
            probe = subprocess.run([str(TOOL), str(canary)], capture_output=True, timeout=10,
                                   env=fault_env(canary_fault))
            alive.append(probe.returncode != 0)
        if not any(alive):
            self.skipTest("fault injection unavailable in this environment")
        for fault in ("fread", "fwrite", "fseek", "fclose"):
            with self.subTest(fault=fault):
                path = self.root / f"fault-{fault}.a"
                path.write_bytes(original)  # every fault starts from a fresh copy
                result = subprocess.run([str(TOOL), str(path)], capture_output=True, timeout=10,
                                        env=fault_env(fault))
                self.assertNotEqual(result.returncode, 0, result.stderr.decode())
                if fault in ("fread", "fwrite"):
                    # The failed read precedes any write and the failed write
                    # shims to a no-op, so both must leave the file untouched.
                    self.assertEqual(path.read_bytes(), original)

    @unittest.skipUnless(REAL_OBJECT, "set MAC2IOS_REAL_OBJECT for the artifact-copy regression")
    def test_real_compiler_rt_member(self):
        obj = Path(REAL_OBJECT).read_bytes()
        original, ((offset, length),) = archive([("absvdi2.c.o", obj)])
        changed = self.convert(original)
        output = changed[offset:offset + length]
        self.check_object(obj, output, 2)
        self.apple_accepts(output)
        self.apple_accepts(changed)
        self.assertEqual(self.convert(changed), changed)
        direct = self.convert(obj, filename="absvdi2.o")
        self.check_object(obj, direct, 2)
        self.apple_accepts(direct)
        self.assertEqual(self.convert(direct, filename="absvdi2.o"), direct)


if __name__ == "__main__":
    unittest.main(verbosity=2)

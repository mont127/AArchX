"""Exercise the audit predicate and framing without a translator or macOS.

Synthetic records deliberately perturb addresses, instructions, descriptors,
boundaries and ordering so the oracle cannot silently compare nothing. The
real offset/low corpus remains the integration test for writer and build paths.
"""
import contextlib
import io
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from audit import AuditError, Block, HEADER, HERE, INSN, MAGIC, RELOC, compare, diagnostics, main, normalized, records
from x64 import block_sets, cache_coverage, compare_sets


def block(address=0x1122334455667788, register=3):
    words = [0xD2800000 | ((address & 0xFFFF) << 5) | register]
    words += [0xF2800000 | (i << 21) | (((address >> (16 * i)) & 0xFFFF) << 5) | register
              for i in range(1, 4)]
    words += [address & 0xFFFFFFFF, address >> 32, 0xD503201F]
    return Block(0x204000, struct.pack("<7I", *words),
                 ((0, 1, 0, 2), (4, 11, 1, 0x204040)), ((0x204000, 1, "nop"),))


def encode(value):
    out = HEADER.pack(value.rip, len(value.code) // 4, len(value.relocs), len(value.insns))
    out += value.code
    out += b"".join(RELOC.pack(*reloc) for reloc in value.relocs)
    for pc, length, text in value.insns:
        text = text.encode()
        out += INSN.pack(pc, length, len(text)) + text
    return out


class AuditTests(unittest.TestCase):
    def compare_blocks(self, left, right):
        with tempfile.TemporaryDirectory() as tmp:
            reference, candidate = Path(tmp) / "reference", Path(tmp) / "candidate"
            reference.write_bytes(MAGIC + b"".join(map(encode, left)))
            candidate.write_bytes(MAGIC + b"".join(map(encode, right)))
            examples = []
            result = compare(reference, candidate, examples, "offset")
            return result, examples

    def test_only_relocation_payloads_are_ignored(self):
        left, right = block(), block(0xFEDCBA9876543210)
        self.assertNotEqual(left.code, right.code)
        self.assertEqual(normalized(left), normalized(right))
        self.assertEqual(self.compare_blocks([left], [right])[0], (1, 0, 7))

    def test_unrelocated_instruction_change_has_diagnostics(self):
        left, right = block(), block()
        right.code = right.code[:-4] + struct.pack("<I", 0xD503205F)
        result, examples = self.compare_blocks([left], [right])
        self.assertEqual(result, (1, 1, 7))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            diagnostics(examples)
        for text in ("reference guest rip=0x204000", "candidate guest rip=0x204000",
                     "x86 disassembly", "nop", "1f 20 03 d5", "5f 20 03 d5"):
            self.assertIn(text, out.getvalue())

    def test_relocation_register_bits_are_not_masked(self):
        self.assertEqual(self.compare_blocks([block()], [block(register=4)])[0][1], 1)

    def test_relocation_shape_corruption_is_reported(self):
        bad = block()
        bad.code = struct.pack("<I", 0xD503201F) + bad.code[4:]
        result, examples = self.compare_blocks([block()], [bad])
        self.assertEqual(result[1], 1)
        self.assertIn("invalid candidate relocation", examples[0][2][0])

    def test_relocation_argument_kind_and_form_are_checked(self):
        for reloc in ((0, 1, 0, 3), (0, 2, 0, 2), (0, 1, 1, 2)):
            with self.subTest(reloc=reloc):
                right = block()
                right.relocs = (reloc, right.relocs[1])
                self.assertEqual(self.compare_blocks([block()], [right])[0][1], 1)

    def test_instruction_boundaries_and_guest_rip_are_checked(self):
        for change in ("rip", "insns"):
            right = block()
            if change == "rip":
                right.rip += 1
            else:
                right.insns = ((0x204000, 2, "nop"),)
            self.assertEqual(self.compare_blocks([block()], [right])[0][1], 1)

    def test_disassembly_formatting_is_not_an_oracle(self):
        right = block()
        right.insns = ((0x204000, 1, "NOP"),)
        self.assertEqual(self.compare_blocks([block()], [right])[0][1], 0)

    def test_missing_block_is_not_a_match(self):
        result, examples = self.compare_blocks([block(), block()], [block()])
        self.assertEqual(result[:2], (2, 1))
        self.assertIsNone(examples[0][4])

    def test_only_first_three_mismatches_are_printed(self):
        result, examples = self.compare_blocks([block()] * 5, [block(register=4)] * 5)
        self.assertEqual(result[1], 5)
        self.assertEqual(len(examples), 3)

    def test_unknown_overlapping_and_out_of_bounds_relocations_are_rejected(self):
        for relocs in (((6, 1, 0, 2),), ((0, 1, 2, 2),), ((0, 99, 0, 2),),
                       ((0, 1, 0, 2), (0, 1, 0, 2))):
            with self.subTest(relocs=relocs):
                value = block()
                value.relocs = relocs
                with self.assertRaises(AuditError):
                    normalized(value)

    def test_empty_bad_and_truncated_streams_are_rejected(self):
        for data in (b"", MAGIC, b"NOJITA01", MAGIC + b"abc", MAGIC + encode(block())[:-1]):
            with self.subTest(data=data[:20]), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "bad"
                path.write_bytes(data)
                with self.assertRaises(AuditError):
                    list(records(path))

    def test_failed_corpus_cannot_report_match_even_with_equal_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            record = Path(tmp) / "stream"
            record.write_bytes(MAGIC + encode(block()))
            out, err = io.StringIO(), io.StringIO()
            with patch("audit.platform.system", return_value="Darwin"), \
                    patch("audit.platform.machine", return_value="arm64"), \
                    patch("audit.build", return_value=Path("unused")), \
                    patch("audit.corpus_run", return_value=(record, "corpus failed")), \
                    patch.dict(os.environ, {"OCERZ_JIT_AUDIT_DIR": tmp}), \
                    contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                result = main([str(HERE.parent.parent), "--corpus", "i386"])
            self.assertEqual(result, 2)
            self.assertNotIn("MATCH", out.getvalue())
            self.assertIn("corpus failed", err.getvalue())

    def test_bad_artifact_directory_is_an_error_not_a_mismatch(self):
        with tempfile.NamedTemporaryFile() as file:
            out = io.StringIO()
            with patch("audit.platform.system", return_value="Darwin"), \
                    patch("audit.platform.machine", return_value="arm64"), \
                    patch.dict(os.environ, {"OCERZ_JIT_AUDIT_DIR": file.name}), \
                    contextlib.redirect_stderr(out):
                self.assertEqual(main([str(HERE.parent.parent)]), 2)
            self.assertIn("cannot create audit directory", out.getvalue())

    def compare_sets(self, left, right):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / "reference", Path(tmp) / "candidate"]
            for path, blocks in zip(paths, (left, right)):
                path.write_bytes(MAGIC + b"".join(map(encode, blocks)))
            examples = []
            return compare_sets(*(block_sets([p]) for p in paths), examples, "x64"), examples

    def test_x64_ignores_order_multiplicity_and_relocation_payloads(self):
        other = block(register=7)
        result, examples = self.compare_sets([block(), other, block()], [other, block(address=123)])
        self.assertEqual(result, (2, 0, 14))
        self.assertEqual(examples, [])

    def test_x64_does_not_ignore_extra_or_missing_variants(self):
        for left, right in (([block(), block(register=7)], [block()]),
                            ([block()], [block(), block(register=7)])):
            self.assertEqual(self.compare_sets(left, right)[0][:2], (2, 1))

    def test_x64_instruction_and_metadata_changes_are_not_masked(self):
        for attr, value in (("code", block().code[:-4] + struct.pack("<I", 0xD503203F)),
                            ("insns", ((0x204000, 2, "nop"),)),
                            ("relocs", ((0, 1, 0, 3), (4, 11, 1, 0x204040)))):
            right = block()
            setattr(right, attr, value)
            result, examples = self.compare_sets([block()], [right])
            self.assertEqual(result[:2], (1, 1))
            self.assertIsNotNone(examples[0][3])
            self.assertIsNotNone(examples[0][4])

    def test_x64_missing_stream_is_an_error(self):
        with self.assertRaises(AuditError):
            block_sets([])

    def test_only_one_sided_shared_cache_rips_can_be_coverage_noise(self):
        common = {0x7ff802001000: {b"variant": block()}}
        left = {**common, 0x7ff802001010: {b"variant": block()}, 0x100000010: {b"guest": block()}}
        right = {**common, 0x7ff802001020: {b"variant": block()}}
        (a, b), coverage = cache_coverage(left, right)
        self.assertEqual(coverage, [[0x7ff802001010], [0x7ff802001020]])
        self.assertIn(0x100000010, a)
        self.assertIn(0x7ff802001000, a)
        self.assertIn(0x7ff802001000, b)
        self.assertEqual(compare_sets(a, b, [], "x64")[1], 1)

    def test_native_fastcall_only_masks_validated_pointer_payload(self):
        a = Block(0x204000, struct.pack("<4I", 0xd2800010, 0xf2a00010, 0xf2c00030, 0xd63f0200),
                  ((0, 128, 2, (3 << 32) | 1),), block().insns)
        b = Block(a.rip, struct.pack("<4I", 0xd2855550, 0xf2a66670, 0xf2c00030, 0xd63f0200), a.relocs, a.insns)
        self.assertEqual(normalized(a), normalized(b))
        for code, relocs in ((a.code[:-4] + bytes(4), a.relocs),
                             (a.code, ((0, 128, 2, (3 << 32) | 2),)),
                             (a.code, ((0, 128, 2, (5 << 32) | 1),)),
                             (a.code, ((0, 128, 2, (3 << 32) | 1), (0, 1, 0, 0)))):
            with self.assertRaises(AuditError):
                normalized(Block(a.rip, code, relocs, a.insns))


if __name__ == "__main__":
    unittest.main()

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from chip8asm import AsmError, assemble, to_c_header  # noqa: E402


def words(source):
    rom = assemble(source)
    return [int.from_bytes(rom[i : i + 2], "big") for i in range(0, len(rom), 2)]


class EncodingTests(unittest.TestCase):
    def check(self, line, expected):
        self.assertEqual(words(line), [expected], line)

    def test_no_operand_instructions(self):
        self.check("CLS", 0x00E0)
        self.check("RET", 0x00EE)

    def test_jumps_and_calls(self):
        self.check("SYS 0x123", 0x0123)
        self.check("JP 0x345", 0x1345)
        self.check("CALL 0x456", 0x2456)
        self.check("JP V0, 0x300", 0xB300)

    def test_skips(self):
        self.check("SE V3, 0x42", 0x3342)
        self.check("SNE V3, 0x42", 0x4342)
        self.check("SE V1, V2", 0x5120)
        self.check("SNE V1, V2", 0x9120)

    def test_loads(self):
        self.check("LD V5, 0x7F", 0x657F)
        self.check("LD V1, V2", 0x8120)
        self.check("LD I, 0x2AB", 0xA2AB)
        self.check("LD V4, DT", 0xF407)
        self.check("LD V4, K", 0xF40A)
        self.check("LD DT, V4", 0xF415)
        self.check("LD ST, V4", 0xF418)
        self.check("LD F, V4", 0xF429)
        self.check("LD B, V4", 0xF433)
        self.check("LD [I], V4", 0xF455)
        self.check("LD V4, [I]", 0xF465)

    def test_adds(self):
        self.check("ADD V2, 0x08", 0x7208)
        self.check("ADD V2, V3", 0x8234)
        self.check("ADD I, V2", 0xF21E)

    def test_alu(self):
        self.check("OR V1, V2", 0x8121)
        self.check("AND V1, V2", 0x8122)
        self.check("XOR V1, V2", 0x8123)
        self.check("SUB V1, V2", 0x8125)
        self.check("SUBN V1, V2", 0x8127)
        self.check("SHR V1", 0x8106)
        self.check("SHR V1, V2", 0x8126)
        self.check("SHL V1", 0x810E)
        self.check("SHL V1, V2", 0x812E)

    def test_misc(self):
        self.check("RND V6, 0x0F", 0xC60F)
        self.check("DRW V0, V1, 5", 0xD015)
        self.check("SKP V7", 0xE79E)
        self.check("SKNP V7", 0xE7A1)

    def test_case_and_number_formats(self):
        self.check("ld v0, 255", 0x60FF)
        self.check("LD VA, 0b101", 0x6A05)
        self.check("ld vF, 0xff", 0x6FFF)


class LabelAndDataTests(unittest.TestCase):
    def test_backward_and_forward_labels(self):
        src = """
        top:  JP end        ; forward
              JP top        ; backward
        end:  CLS
        """
        self.assertEqual(words(src), [0x1204, 0x1200, 0x00E0])

    def test_label_on_its_own_line(self):
        src = "loop:\n  JP loop"
        self.assertEqual(words(src), [0x1200])

    def test_db_and_dw(self):
        self.assertEqual(assemble("DB 1, 2, 0xFF"), bytes([1, 2, 255]))
        self.assertEqual(assemble("DW 0x1234, 5"), bytes([0x12, 0x34, 0x00, 0x05]))

    def test_label_addresses_account_for_data(self):
        src = "JP after\nDB 1, 2, 3\nafter: CLS"
        # JP (2 bytes) + 3 data bytes -> 'after' is at 0x200 + 5
        self.assertEqual(words("JP after\nDB 1, 2, 3, 4\nafter: CLS")[0], 0x1206)
        self.assertEqual(assemble(src)[:2], bytes([0x12, 0x05]))

    def test_comments_and_blank_lines(self):
        self.assertEqual(words("; nothing\n\n  CLS ; clear\n"), [0x00E0])


class ErrorTests(unittest.TestCase):
    def fails(self, source, fragment, line=None):
        with self.assertRaises(AsmError) as ctx:
            assemble(source)
        self.assertIn(fragment, str(ctx.exception))
        if line is not None:
            self.assertEqual(ctx.exception.line_no, line)

    def test_unknown_instruction(self):
        self.fails("CLS\nFROB V0", "unknown instruction", line=2)

    def test_bad_register(self):
        self.fails("LD VG, 1", "must be a register", line=1)

    def test_values_out_of_range(self):
        self.fails("LD V0, 256", "out of range", line=1)
        self.fails("DRW V0, V1, 16", "out of range", line=1)
        self.fails("JP 0x1000", "out of range", line=1)
        self.fails("DB 300", "out of range", line=1)

    def test_undefined_label(self):
        self.fails("JP nowhere", "undefined label", line=1)

    def test_duplicate_label(self):
        self.fails("a: CLS\na: CLS", "duplicate label", line=2)

    def test_register_names_are_not_labels(self):
        self.fails("V1: CLS", "can't be used as a label", line=1)

    def test_wrong_operand_count(self):
        self.fails("CLS V0", "takes 0 operand", line=1)
        self.fails("DRW V0, V1", "takes 3 operand", line=1)

    def test_wrong_operand_type(self):
        self.fails("ADD 5, V1", "must be a register", line=1)
        self.fails("JP V1, 0x300", "JP V0", line=1)

    def test_program_too_large(self):
        self.fails("DB " + ",".join(["0"] * 4000), "too large")


class ExampleProgramTests(unittest.TestCase):
    def test_zero_example_matches_the_hand_made_rom(self):
        with open(os.path.join(ROOT, "examples", "zero.asm"), encoding="utf-8") as f:
            rom = assemble(f.read())
        rom_path = os.path.join(ROOT, "..", "cpu-emulator", "roms", "zero.ch8")
        with open(rom_path, "rb") as f:
            self.assertEqual(rom, f.read())


    def test_zeros_example_assembles_to_known_bytes(self):
        # Verified by running it: three glyphs at x = 0, 8, 16.
        with open(os.path.join(ROOT, "examples", "zeros.asm"), encoding="utf-8") as f:
            rom = assemble(f.read())
        expected = bytes.fromhex(
            "6000 6100 6203 2212 7008 72ff 3200 1206 1210 a218 d015 00ee"
            " f0 90 90 90 f0"
        )
        self.assertEqual(rom, expected)


class CHeaderTests(unittest.TestCase):
    def test_header_contains_bytes_and_size(self):
        text = to_c_header(bytes([0xA2, 0x06, 0xD0]), "demo", "x.asm")
        self.assertIn("0xA2, 0x06, 0xD0,", text)
        self.assertIn("static const uint8_t demo[]", text)
        self.assertIn("demo_size = sizeof(demo)", text)


if __name__ == "__main__":
    unittest.main()

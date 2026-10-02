"""Native host regression coverage for string arguments and protocol validation."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "build/release/tools/microexec_evnova/host/evnova_microexec_host"


@unittest.skipUnless(HOST.exists(), "microexec host is not built")
class HostStringsTests(unittest.TestCase):
    def host(self, text):
        result = subprocess.run([str(HOST)], input=text, text=True, capture_output=True, check=True)
        return result.stdout

    def test_description_and_conditions(self):
        self.assertIn("returns bool state:state condition:string", self.host("describe\n"))
        cases = [(b"", 1), (b"unknown", 0), (b"G", 1), (b"!G", 0),
                 (b"((G))", 1), (b"(G & !G)", 0), (b"\xff\n", 0)]
        text = "".join(f"case {i} 0x00447f20\narg-bytes condition {data.hex() or '-'}\nend\n"
                       for i, (data, _) in enumerate(cases))
        output = self.host(text)
        for i, (_, expected) in enumerate(cases):
            self.assertIn(f"result {i} ok {expected}\n", output)

    def test_malformed_missing_and_wrong_type_arguments(self):
        for command in ("arg-bytes condition 0", "arg-bytes condition zz", "arg-bytes condition 47 junk",
                        "arg condition 1", "arg-bytes unknown 47", ""):
            with self.subTest(command=command):
                self.assertIn("result 1 error ", self.host(f"case 1 0x00447f20\n{command}\nend\n"))
        output = self.host("case 2 0x0046bc90\narg-bytes govt_a 33\narg govt_b 5\nend\n")
        self.assertIn("result 2 error byte string value for a numeric port field", output)


if __name__ == "__main__":
    unittest.main()

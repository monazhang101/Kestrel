import json
import sys
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from spev_diag.cicd.run_regression import (  # noqa: E402
    ConfigError,
    ProcessOutcome,
    RegressionRunner,
    load_suite,
)


class RegressionConfigTests(unittest.TestCase):
    def _write(self, directory: Path, text: str) -> Path:
        path = directory / "EMU_db1.yaml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_loads_ordered_testcases_and_default_run(self):
        with tempfile.TemporaryDirectory() as temp:
            path = self._write(
                Path(temp),
                """\
version: 1
testcases:
  pcie_dma_data_transfer:
    - direction=h2d, size_bytes=4096, pattern=incremental
  isi_example:
    - {}
""",
            )
            profile = load_suite(path)

        self.assertEqual(
            [run.testcase for run in profile.runs],
            ["pcie_dma_data_transfer", "isi_example"],
        )
        self.assertEqual(profile.runs[0].arguments["size_bytes"], "4096")
        self.assertEqual(profile.runs[1].arguments, {})
        self.assertRegex(
            profile.runs[0].case_name,
            r"^pcie_dma_data_transfer\[[0-9a-f]{12}\]$",
        )
        self.assertEqual(profile.runs[1].case_name, "isi_example[default]")

    def test_rejects_duplicate_testcase_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            path = self._write(
                Path(temp),
                """\
version: 1
testcases:
  isi_example: [{}]
  isi_example:
    - {}
""",
            )
            with self.assertRaisesRegex(ConfigError, "duplicate YAML key"):
                load_suite(path)

    def test_rejects_empty_suite(self):
        with tempfile.TemporaryDirectory() as temp:
            path = self._write(Path(temp), "version: 1\ntestcases: {}\n")
            with self.assertRaisesRegex(ConfigError, "non-empty"):
                load_suite(path)

    def test_rejects_duplicate_parameter_combinations(self):
        with tempfile.TemporaryDirectory() as temp:
            path = self._write(
                Path(temp),
                """\
version: 1
testcases:
  isi_example:
    - links=all, timeout_ms=1000
    - timeout_ms=1000, links=all
""",
            )
            with self.assertRaisesRegex(ConfigError, "duplicates an earlier run"):
                load_suite(path)


class RegressionRunnerTests(unittest.TestCase):
    def test_continues_after_failure_and_writes_reports(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            profile_path = root / "FPGA_db2.yaml"
            profile_path.write_text(
                """\
version: 1
testcases:
  pcie_dma_data_transfer:
    - direction=h2d, size_bytes=4096
  isi_example:
    - {}
""",
                encoding="utf-8",
            )
            profile = load_suite(profile_path)
            binary = root / "spev_diag"
            binary.touch()
            commands = []

            def fake_process(command, timeout_s):
                commands.append((list(command), timeout_s))
                if command[1] == "pcie_dma_data_transfer":
                    return ProcessOutcome(returncode=2, stderr="DMA failed")
                return ProcessOutcome(returncode=0, stdout="OK")

            output = root / "artifacts"
            runner = RegressionRunner(
                profile=profile,
                binary=binary,
                target="TPU0",
                output_dir=output,
                timeout_s=123,
                policy_dir=root / "policies",
                process_runner=fake_process,
            )
            suite, exit_code = runner.run()

            self.assertEqual(exit_code, 1)
            self.assertEqual(suite["status"], "fail")
            self.assertEqual(len(commands), 2)
            self.assertIn("--size-bytes", commands[0][0])
            self.assertIn("--policy-dir", commands[0][0])
            self.assertEqual(commands[1][0][1], "isi_example")

            report = json.loads((output / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(report["summary"]["fail"], 1)
            junit = ElementTree.parse(output / "ct.xml").getroot()
            self.assertEqual(junit.attrib["tests"], "2")
            self.assertEqual(junit.attrib["failures"], "1")
            failure = junit.find("testcase/failure")
            self.assertIsNotNone(failure)
            self.assertIn("DMA failed", failure.text)
            testcase = junit.findall("testcase")[0]
            self.assertEqual(testcase.attrib["status"], "fail")
            self.assertIn("duration_s:", testcase.find("system-out").text)
            self.assertIn('"direction": "h2d"', testcase.find("system-out").text)
            self.assertTrue((output / "0001_pcie_dma_data_transfer.log").is_file())


if __name__ == "__main__":
    unittest.main()

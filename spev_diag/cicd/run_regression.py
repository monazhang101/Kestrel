#!/usr/bin/env python3
"""Run a platform regression suite through the spev_diag CLI."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence
from xml.etree import ElementTree


PROFILE_VERSION = 1
NAME_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")
MAX_FAILURE_DETAIL = 8192


class ConfigError(ValueError):
    """The regression suite does not satisfy the public file contract."""


@dataclass(frozen=True)
class TestRun:
    testcase: str
    arguments: dict[str, str]
    testcase_run: int

    @property
    def case_name(self) -> str:
        if not self.arguments:
            return f"{self.testcase}[default]"
        normalized = json.dumps(
            self.arguments,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        identity = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:12]
        return f"{self.testcase}[{identity}]"


@dataclass(frozen=True)
class RegressionProfile:
    name: str
    path: Path
    runs: list[TestRun]


@dataclass(frozen=True)
class ProcessOutcome:
    returncode: int | None
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    launch_error: str | None = None


def _parse_argument_line(text: str, context: str) -> dict[str, str]:
    try:
        assignments = next(csv.reader([text], skipinitialspace=True))
    except csv.Error as error:
        raise ConfigError(f"{context} has invalid CSV-style arguments: {error}") from error

    arguments: dict[str, str] = {}
    for assignment in assignments:
        if "=" not in assignment:
            raise ConfigError(f"{context} argument must use name=value: {assignment}")
        name, value = (part.strip() for part in assignment.split("=", 1))
        if not NAME_PATTERN.fullmatch(name):
            raise ConfigError(f"{context} has invalid argument name: {name}")
        if not value:
            raise ConfigError(f"{context} argument has an empty value: {name}")
        if name in arguments:
            raise ConfigError(f"{context} repeats argument: {name}")
        arguments[name] = value
    if not arguments:
        raise ConfigError(f"{context} must use {{}} for a default-argument run")
    return arguments


def _strip_inline_comment(line: str) -> str:
    quote: str | None = None
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if character == "\\" and quote is not None:
            escaped = True
            continue
        if character in ("'", '"'):
            if quote == character:
                quote = None
            elif quote is None:
                quote = character
            continue
        if character == "#" and quote is None and (
            index == 0 or line[index - 1].isspace()
        ):
            return line[:index].rstrip()
    return line.rstrip()


def load_suite(path: Path) -> RegressionProfile:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ConfigError(f"cannot read regression suite {path}: {error}") from error

    runs: list[TestRun] = []
    version_seen = False
    testcases_seen = False
    in_testcases = False
    current_testcase: str | None = None
    run_counts: dict[str, int] = {}
    seen_runs: dict[str, set[tuple[tuple[str, str], ...]]] = {}

    def add_run(testcase: str, arguments: dict[str, str], line_number: int) -> None:
        identity = tuple(sorted(arguments.items()))
        if identity in seen_runs[testcase]:
            raise ConfigError(
                f"line {line_number}: {testcase} duplicates an earlier run"
            )
        seen_runs[testcase].add(identity)
        run_counts[testcase] += 1
        runs.append(TestRun(testcase, arguments, run_counts[testcase]))

    for line_number, raw_line in enumerate(lines, start=1):
        if "\t" in raw_line:
            raise ConfigError(f"line {line_number}: tabs are not allowed")
        line = _strip_inline_comment(raw_line)
        if not line.strip():
            continue

        indent = len(line) - len(line.lstrip(" "))
        text = line.strip()
        if indent == 0:
            current_testcase = None
            in_testcases = False
            if text.startswith("version:"):
                if version_seen:
                    raise ConfigError("duplicate YAML key: version")
                version_seen = True
                if text != f"version: {PROFILE_VERSION}":
                    raise ConfigError(f"suite version must be {PROFILE_VERSION}")
            elif text in ("testcases:", "testcases: {}"):
                if testcases_seen:
                    raise ConfigError("duplicate YAML key: testcases")
                testcases_seen = True
                in_testcases = text == "testcases:"
            else:
                field = text.split(":", 1)[0]
                raise ConfigError(f"line {line_number}: unknown suite field: {field}")
            continue

        if not in_testcases:
            raise ConfigError(f"line {line_number}: content must be under testcases")
        if indent == 2:
            match = re.fullmatch(r"([a-z][a-z0-9_]*):(?:\s*(.*))?", text)
            if match is None:
                raise ConfigError(f"line {line_number}: invalid testcase entry")
            current_testcase = match.group(1)
            if current_testcase in run_counts:
                raise ConfigError(f"duplicate YAML key: {current_testcase}")
            run_counts[current_testcase] = 0
            seen_runs[current_testcase] = set()
            inline_runs = (match.group(2) or "").strip()
            if inline_runs:
                if inline_runs != "[{}]":
                    raise ConfigError(
                        f"line {line_number}: only [{{}}] is supported inline"
                    )
                add_run(current_testcase, {}, line_number)
            continue

        if indent == 4 and current_testcase is not None:
            if not text.startswith("- "):
                raise ConfigError(f"line {line_number}: testcase run must start with '- '")
            configured_run = text[2:].strip()
            if configured_run == "{}":
                arguments = {}
            elif configured_run:
                arguments = _parse_argument_line(
                    configured_run,
                    f"line {line_number} ({current_testcase})",
                )
            else:
                raise ConfigError(f"line {line_number}: empty testcase run")
            add_run(current_testcase, arguments, line_number)
            continue

        raise ConfigError(f"line {line_number}: invalid indentation")

    if not version_seen:
        raise ConfigError("missing required suite field: version")
    if not testcases_seen or not run_counts:
        raise ConfigError("testcases must be a non-empty mapping")
    for testcase, count in run_counts.items():
        if count == 0:
            raise ConfigError(f"{testcase} must define at least one run")

    return RegressionProfile(name=path.stem, path=path.resolve(), runs=runs)


def run_process(command: Sequence[str], timeout_s: float) -> ProcessOutcome:
    try:
        completed = subprocess.run(
            list(command),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_s,
            check=False,
        )
        return ProcessOutcome(
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        return ProcessOutcome(
            returncode=None,
            stdout=stdout,
            stderr=stderr,
            timed_out=True,
        )
    except OSError as error:
        return ProcessOutcome(returncode=None, launch_error=str(error))


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def _command_text(command: Sequence[str]) -> str:
    return subprocess.list2cmdline(list(command))


def _failure_reason(
    outcome: ProcessOutcome, error_code: str, diagnosis: str
) -> str:
    details = [diagnosis, f"error_code: {error_code}"]
    output = "\n".join(
        value.strip()
        for value in (outcome.stderr, outcome.stdout)
        if value and value.strip()
    )
    if output:
        details.extend(["process output:", output])
    reason = "\n".join(details)
    if len(reason) > MAX_FAILURE_DETAIL:
        reason = "..." + reason[-(MAX_FAILURE_DETAIL - 3):]
    return reason


def _xml_safe(text: str) -> str:
    return "".join(
        character
        for character in text
        if character in "\t\n\r" or ord(character) >= 0x20
    )


def _argument_flag(name: str) -> str:
    return "--" + name.replace("_", "-")


class RegressionRunner:
    def __init__(
        self,
        profile: RegressionProfile,
        binary: Path,
        target: str,
        output_dir: Path,
        timeout_s: float,
        policy_dir: Path | None = None,
        log_level: str = "info",
        process_runner: Callable[[Sequence[str], float], ProcessOutcome] = run_process,
    ) -> None:
        self.profile = profile
        self.binary = binary.resolve()
        self.target = target
        self.output_dir = output_dir.resolve()
        self.timeout_s = timeout_s
        self.policy_dir = policy_dir.resolve() if policy_dir else None
        self.log_level = log_level
        self.process_runner = process_runner

    def _test_command(self, run: TestRun) -> list[str]:
        command = [
            str(self.binary),
            run.testcase,
            "--target",
            self.target,
            "--log-level",
            self.log_level,
        ]
        if self.policy_dir is not None:
            command.extend(["--policy-dir", str(self.policy_dir)])
        for name, value in run.arguments.items():
            command.extend([_argument_flag(name), value])
        return command

    def _execute(
        self,
        kind: str,
        name: str,
        case_name: str,
        run_number: int,
        command: list[str],
        timeout_s: float,
        log_index: int,
        arguments: dict[str, str] | None = None,
    ) -> dict[str, object]:
        start_text = _utc_timestamp()
        start = time.monotonic()
        outcome = self.process_runner(command, timeout_s)
        duration_s = time.monotonic() - start
        end_text = _utc_timestamp()

        if outcome.timed_out:
            status = "timeout"
            error_code = "PROCESS_TIMEOUT"
            diagnosis = f"process exceeded {timeout_s:g} seconds"
        elif outcome.launch_error is not None:
            status = "error"
            error_code = "PROCESS_LAUNCH_ERROR"
            diagnosis = outcome.launch_error
        elif outcome.returncode == 0:
            status = "pass"
            error_code = "OK"
            diagnosis = ""
        else:
            status = "fail"
            error_code = f"EXIT_{outcome.returncode}"
            diagnosis = f"spev_diag exited with code {outcome.returncode}"

        failure_reason = (
            ""
            if status == "pass"
            else _failure_reason(outcome, error_code, diagnosis)
        )

        safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
        log_name = f"{log_index:04d}_{safe_name}.log"
        log_path = self.output_dir / log_name
        log_path.write_text(
            "\n".join(
                [
                    f"command: {_command_text(command)}",
                    f"status: {status}",
                    f"returncode: {outcome.returncode}",
                    "",
                    "[stdout]",
                    outcome.stdout,
                    "[stderr]",
                    outcome.stderr,
                    "[launch_error]",
                    outcome.launch_error or "",
                ]
            ),
            encoding="utf-8",
        )

        return {
            "name": name,
            "case_name": case_name,
            "type": kind,
            "target": self.target,
            "run": run_number,
            "arguments": arguments or {},
            "status": status,
            "start_time": start_text,
            "end_time": end_text,
            "duration_s": round(duration_s, 6),
            "returncode": outcome.returncode,
            "error_code": error_code,
            "diagnosis": diagnosis,
            "failure_reason": failure_reason,
            "command": command,
            "artifacts": [log_name],
        }

    def run(self) -> tuple[dict[str, object], int]:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        suite_start_text = _utc_timestamp()
        suite_start = time.monotonic()
        results: list[dict[str, object]] = []

        total = len(self.profile.runs)
        for index, test_run in enumerate(self.profile.runs, start=1):
            label = test_run.case_name
            print(f"[RUN {index}/{total}] {label}", flush=True)
            result = self._execute(
                kind="testcase",
                name=test_run.testcase,
                case_name=test_run.case_name,
                run_number=test_run.testcase_run,
                command=self._test_command(test_run),
                timeout_s=self.timeout_s,
                log_index=index,
                arguments=test_run.arguments,
            )
            results.append(result)
            print(f"[{str(result['status']).upper()}] {label}", flush=True)

        suite_status = "pass" if all(
            result["status"] == "pass" for result in results
        ) else "fail"
        suite = {
            "name": self.profile.name,
            "type": "regression",
            "status": suite_status,
            "suite": str(self.profile.path),
            "binary": str(self.binary),
            "target": self.target,
            "start_time": suite_start_text,
            "end_time": _utc_timestamp(),
            "duration_s": round(time.monotonic() - suite_start, 6),
            "summary": {
                status: sum(1 for result in results if result["status"] == status)
                for status in ("pass", "fail", "timeout", "error")
            },
            "results": results,
        }
        write_json_report(suite, self.output_dir / "result.json")
        write_junit_report(suite, self.output_dir / "ct.xml")
        print(
            f"[SUMMARY] status={suite_status} "
            f"passed={suite['summary']['pass']} "
            f"failed={suite['summary']['fail']} "
            f"timeout={suite['summary']['timeout']} "
            f"error={suite['summary']['error']}",
            flush=True,
        )
        return suite, 0 if suite_status == "pass" else 1


def write_json_report(suite: dict[str, object], path: Path) -> None:
    path.write_text(
        json.dumps(suite, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def write_junit_report(suite: dict[str, object], path: Path) -> None:
    results = suite["results"]
    failures = sum(1 for result in results if result["status"] == "fail")
    errors = sum(
        1 for result in results if result["status"] in ("timeout", "error")
    )
    root = ElementTree.Element(
        "testsuite",
        {
            "name": str(suite["name"]),
            "tests": str(len(results)),
            "failures": str(failures),
            "errors": str(errors),
            "skipped": "0",
            "time": f"{float(suite['duration_s']):.6f}",
        },
    )
    properties = ElementTree.SubElement(root, "properties")
    ElementTree.SubElement(
        properties, "property", {"name": "target", "value": str(suite["target"])}
    )
    ElementTree.SubElement(
        properties,
        "property",
        {"name": "suite", "value": str(suite["suite"])},
    )

    for result in results:
        case = ElementTree.SubElement(
            root,
            "testcase",
            {
                "classname": str(suite["name"]),
                "name": str(result["case_name"]),
                "status": str(result["status"]),
                "time": f"{float(result['duration_s']):.6f}",
            },
        )
        if result["status"] == "fail":
            failure = ElementTree.SubElement(
                case, "failure", {"message": str(result["diagnosis"])}
            )
            failure.text = _xml_safe(str(result["failure_reason"]))
        elif result["status"] in ("timeout", "error"):
            error = ElementTree.SubElement(
                case, "error", {"message": str(result["diagnosis"])}
            )
            error.text = _xml_safe(str(result["failure_reason"]))
        system_out = ElementTree.SubElement(case, "system-out")
        system_out.text = _xml_safe(
            "\n".join(
                [
                    f"status: {result['status']}",
                    f"duration_s: {result['duration_s']}",
                    "arguments: " + json.dumps(
                        result["arguments"], sort_keys=True, ensure_ascii=True
                    ),
                    f"log: {result['artifacts'][0]}",
                ]
            )
        )

    ElementTree.indent(root, space="  ")
    ElementTree.ElementTree(root).write(
        path, encoding="utf-8", xml_declaration=True
    )


def _positive_timeout(value: str) -> float:
    timeout = float(value)
    if timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return timeout


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a platform regression suite through spev_diag"
    )
    parser.add_argument("--suite", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--target", default="TPU0")
    parser.add_argument("--policy-dir", type=Path)
    parser.add_argument(
        "--log-level", choices=("error", "info", "flow", "regio"), default="info"
    )
    parser.add_argument("--timeout-s", type=_positive_timeout, default=600.0)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        profile = load_suite(args.suite)
        if not args.binary.is_file():
            raise ConfigError(f"spev_diag binary does not exist: {args.binary}")
        if args.policy_dir is not None and not args.policy_dir.is_dir():
            raise ConfigError(f"policy directory does not exist: {args.policy_dir}")
        reports_path = os.environ.get("FLOWRA_REPORTS_PATH") or "reports"
        runner = RegressionRunner(
            profile=profile,
            binary=args.binary,
            target=args.target,
            output_dir=Path(reports_path),
            timeout_s=args.timeout_s,
            policy_dir=args.policy_dir,
            log_level=args.log_level,
        )
        _, exit_code = runner.run()
        return exit_code
    except ConfigError as error:
        print(f"configuration error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"runner I/O error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

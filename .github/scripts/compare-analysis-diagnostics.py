#!/usr/bin/env python3
"""Fail incremental analysis only when it introduces new diagnostics."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DIAGNOSTIC = re.compile(
    r"^(?P<path>.+?):\d+:(?:\d+:)?\s*"
    r"(?P<level>warning|error|note|style|performance|portability|information):\s*"
    r"(?P<message>.*)$"
)
CODE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".def",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".inl",
    ".ipp",
    ".m",
    ".mm",
    ".tcc",
    ".tpp",
}


def normalize_path(repo: Path, value: str) -> str:
    path = Path(value)
    if path.is_absolute():
        try:
            return path.resolve().relative_to(repo).as_posix()
        except ValueError:
            return path.as_posix()
    return value


def diagnostics(report: Path, repo: Path) -> set[str]:
    try:
        lines = report.read_text(errors="replace").splitlines()
    except OSError:
        return set()
    result: set[str] = set()
    for raw_line in lines:
        line = ANSI_ESCAPE.sub("", raw_line)
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        path = normalize_path(repo, match.group("path"))
        level = match.group("level")
        message = match.group("message")
        result.add(f"{path}|{level}|{message}")
    return result


def load_baseline(path: Path) -> dict[str, set[str]] | None:
    try:
        payload = json.loads(path.read_text())
        return {
            tool: set(values)
            for tool, values in payload.get("diagnostics", {}).items()
        }
    except (OSError, ValueError, TypeError):
        return None


def write_baseline(path: Path, values: dict[str, set[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "diagnostics": {
                    tool: sorted(entries) for tool, entries in sorted(values.items())
                },
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def changed_code_paths(path: Path) -> set[str]:
    try:
        return {
            line.strip()
            for line in path.read_text().splitlines()
            if line.strip() and Path(line.strip()).suffix.lower() in CODE_SUFFIXES
        }
    except OSError:
        return set()


def diagnostic_path(value: str) -> str:
    return value.split("|", 1)[0]


def diagnostic_level(value: str) -> str:
    parts = value.split("|", 2)
    return parts[1] if len(parts) > 1 else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--report", action="append", nargs=2, metavar=("TOOL", "PATH"), required=True)
    parser.add_argument("--outcome", action="append", nargs=2, metavar=("TOOL", "OUTCOME"), default=[])
    parser.add_argument("--has-files", choices=("true", "false"), required=True)
    parser.add_argument("--changed-files", type=Path, required=True)
    parser.add_argument("--analyzed-files", type=Path)
    parser.add_argument("--new-diagnostics", type=Path, required=True)
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    reports = {
        tool: diagnostics(Path(report), repo)
        for tool, report in args.report
    }
    outcomes = dict(args.outcome)
    refreshed_paths = changed_code_paths(
        args.analyzed_files or args.changed_files
    )
    baseline_path = args.cache_dir / "diagnostic-baseline.json"
    baseline = load_baseline(baseline_path)
    failed_tools = [
        tool
        for tool in reports
        if args.has_files == "true" and outcomes.get(tool) == "failure"
    ]
    failed_tools.extend(
        tool
        for tool, current in reports.items()
        if args.has_files == "true"
        and any(diagnostic_level(value) == "error" for value in current)
        and tool not in failed_tools
    )
    if baseline is None:
        if failed_tools:
            args.new_diagnostics.parent.mkdir(parents=True, exist_ok=True)
            args.new_diagnostics.write_text(
                "\n".join(
                    f"{tool}: {value}"
                    for tool, current in reports.items()
                    if tool in failed_tools
                    for value in sorted(current)
                )
                + "\n"
            )
            for tool in failed_tools:
                print(f"::error::{tool} failed or emitted a compiler error.")
            return 1
        write_baseline(baseline_path, reports)
        args.new_diagnostics.parent.mkdir(parents=True, exist_ok=True)
        args.new_diagnostics.write_text("baseline initialized\n")
        print("Initialized the static-analysis diagnostic baseline.")
        return 0

    new_diagnostics: list[str] = []
    for tool, current in reports.items():
        for diagnostic in sorted(current - baseline.get(tool, set())):
            new_diagnostics.append(f"{tool}: {diagnostic}")

    args.new_diagnostics.parent.mkdir(parents=True, exist_ok=True)
    args.new_diagnostics.write_text(
        "\n".join(new_diagnostics) + ("\n" if new_diagnostics else "")
    )
    if failed_tools:
        for tool in failed_tools:
            print(f"::error::{tool} failed or emitted a compiler error.")
        return 1
    if new_diagnostics:
        for diagnostic in new_diagnostics:
            print(f"::error::New static-analysis diagnostic: {diagnostic}")
        return 1

    for tool, current in reports.items():
        baseline[tool] = {
            diagnostic
            for diagnostic in baseline.get(tool, set())
            if diagnostic_path(diagnostic) not in refreshed_paths
        }
        baseline[tool].update(current)
    write_baseline(baseline_path, baseline)
    print("No new static-analysis diagnostics.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

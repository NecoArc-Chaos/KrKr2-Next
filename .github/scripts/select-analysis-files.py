#!/usr/bin/env python3
"""Select the translation units affected since the last successful analysis."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
HEADER_SUFFIXES = {
    ".def",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".inl",
    ".ipp",
    ".tcc",
    ".tpp",
}
FULL_FILES = {
    ".clang-tidy",
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
}
FULL_PREFIXES = (
    ".github/scripts/",
    ".github/workflows/",
    "cmake/",
    "vcpkg/",
)
CODE_PREFIXES = ("apps/", "bridge/", "cpp/", "platforms/", "tools/")


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=False,
        capture_output=True,
        text=True,
    )


def repo_path(repo: Path, value: str, base: Path | None = None) -> str | None:
    if not value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = (base or repo) / path
    try:
        return path.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        return None


def stable_path(repo: Path, value: str, base: Path | None = None) -> str | None:
    path = repo_path(repo, value, base)
    if path:
        return path
    if not value:
        return None
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = (base or repo) / candidate
    return candidate.resolve().as_posix()


def source_path(repo: Path, entry: dict) -> str | None:
    directory = Path(entry.get("directory", ""))
    if not directory.is_absolute():
        directory = repo / directory
    return stable_path(repo, str(entry.get("file", "")), directory)


def changed_paths(repo: Path, base: str, head: str) -> set[str] | None:
    result = git(repo, "diff", "--name-only", "-z", base, head)
    if result.returncode != 0:
        return None
    return {item for item in result.stdout.split("\0") if item}


def is_ancestor(repo: Path, base: str, head: str) -> bool:
    return git(repo, "merge-base", "--is-ancestor", base, head).returncode == 0


def object_exists(repo: Path, value: str) -> bool:
    return git(repo, "cat-file", "-e", f"{value}^{{commit}}").returncode == 0


def full_trigger(path: str) -> bool:
    return path in FULL_FILES or path.endswith("CMakeLists.txt") or path.endswith(
        ".cmake"
    ) or path.startswith(FULL_PREFIXES)


def source_change(path: str) -> bool:
    return Path(path).suffix.lower() in SOURCE_SUFFIXES


def header_change(path: str) -> bool:
    return Path(path).suffix.lower() in HEADER_SUFFIXES


def scanner_path() -> str | None:
    for name in ("clang-scan-deps", "clang-scan-deps-20", "clang-scan-deps-19", "clang-scan-deps-18"):
        found = shutil.which(name)
        if found:
            return found
    for candidate in sorted(Path("/usr/bin").glob("clang-scan-deps-*")):
        if candidate.is_file() and candidate.stat().st_mode & 0o111:
            return str(candidate)
    return None


def compile_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_dependencies(cache_file: Path, database_hash: str) -> dict[str, set[str]] | None:
    try:
        payload = json.loads(cache_file.read_text())
        if payload.get("compile_commands_sha256") != database_hash:
            return None
        return {
            source: set(dependencies)
            for source, dependencies in payload.get("dependencies", {}).items()
        }
    except (OSError, ValueError, TypeError):
        return None


def scan_dependencies(repo: Path, compile_commands: Path, cache_file: Path) -> dict[str, set[str]] | None:
    scanner = scanner_path()
    if not scanner:
        print("clang-scan-deps is unavailable; falling back to a full analysis", file=sys.stderr)
        return None
    result = subprocess.run(
        [
            scanner,
            "-compilation-database",
            str(compile_commands),
            "-format",
            "experimental-full",
        ],
        cwd=repo,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print("clang-scan-deps failed; falling back to a full analysis", file=sys.stderr)
        if result.stderr:
            print(result.stderr[-4000:], file=sys.stderr)
        return None
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        print("clang-scan-deps returned invalid JSON; falling back to a full analysis", file=sys.stderr)
        return None

    dependencies: dict[str, set[str]] = {}
    for unit in payload.get("translation-units", []):
        for command in unit.get("commands", []):
            source_value = command.get("input-file") or unit.get("input-file")
            source = stable_path(repo, str(source_value or ""))
            if not source:
                continue
            files = {
                dependency
                for dependency in (
                    stable_path(repo, str(value))
                    for value in command.get("file-deps", [])
                )
                if dependency
            }
            files.add(source)
            dependencies.setdefault(source, set()).update(files)

    if not dependencies:
        print("clang-scan-deps found no translation units; falling back to a full analysis", file=sys.stderr)
        return None
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    cache_file.write_text(
        json.dumps(
            {
                "compile_commands_sha256": compile_hash(compile_commands),
                "dependencies": {
                    source: sorted(files) for source, files in sorted(dependencies.items())
                },
            },
            sort_keys=True,
            indent=2,
        )
        + "\n"
    )
    return dependencies


def select_base(repo: Path, cache_dir: Path, requested_base: str, head: str) -> tuple[str | None, str]:
    if requested_base:
        if object_exists(repo, requested_base) and is_ancestor(repo, requested_base, head):
            return requested_base, "event base"
        return None, "event base is unavailable"

    marker = cache_dir / "last-successful.sha"
    try:
        cached_base = marker.read_text().strip()
    except OSError:
        cached_base = ""
    if cached_base and object_exists(repo, cached_base) and is_ancestor(repo, cached_base, head):
        return cached_base, "last successful analysis"
    return None, "no usable successful-analysis marker"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--base-sha", default="")
    parser.add_argument("--head-sha", default="HEAD")
    parser.add_argument("--force-full", action="store_true")
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    compile_commands = args.compile_commands.resolve()
    cache_dir = args.cache_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)

    try:
        entries = json.loads(compile_commands.read_text())
    except (OSError, ValueError) as error:
        print(f"unable to read compilation database: {error}", file=sys.stderr)
        return 1
    if not isinstance(entries, list):
        print("compilation database must be a JSON array", file=sys.stderr)
        return 1

    source_entries: dict[str, list[dict]] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        source = source_path(repo, entry)
        if source:
            source_entries.setdefault(source, []).append(entry)

    head = args.head_sha or "HEAD"
    base, base_reason = select_base(repo, cache_dir, args.base_sha, head)
    full = args.force_full or base is None
    changed: set[str] = set()
    reason = "forced full analysis" if args.force_full else base_reason
    if not (cache_dir / "diagnostic-baseline.json").is_file():
        full = True
        reason = "no diagnostic baseline"
    if not full:
        diff = changed_paths(repo, base, head)
        if diff is None:
            full = True
            reason = "unable to compute changed paths"
        else:
            changed = diff
            if any(full_trigger(path) for path in changed):
                full = True
                reason = "analysis or build configuration changed"
            else:
                reason = f"changes since {base}"

    selected: set[str]
    if full:
        selected = set(source_entries)
    else:
        selected = {path for path in changed if source_change(path) and path in source_entries}
        headers = {path for path in changed if header_change(path)}
        unknown_code = {
            path
            for path in changed
            if path.startswith(CODE_PREFIXES)
            and not source_change(path)
            and not header_change(path)
        }
        if unknown_code:
            full = True
            reason = "unknown file under a compiled source tree changed"
            selected = set(source_entries)
        elif headers:
            dependency_file = cache_dir / "clang-dependencies.json"
            dependencies = load_dependencies(dependency_file, compile_hash(compile_commands))
            if dependencies is None:
                dependencies = scan_dependencies(repo, compile_commands, dependency_file)
            if dependencies is None:
                full = True
                reason = "header impact could not be resolved"
                selected = set(source_entries)
            else:
                selected.update(
                    source
                    for source, files in dependencies.items()
                    if files.intersection(headers)
                )

    if full:
        mode = "full"
    elif selected:
        mode = "incremental"
    else:
        mode = "none"

    selected_entries = [
        entry
        for source in sorted(selected)
        for entry in source_entries.get(source, [])
    ]
    (output_dir / "clang-tidy-files.txt").write_text(
        "".join(f"{source}\n" for source in sorted(selected))
    )
    (output_dir / "cppcheck-compile_commands.json").write_text(
        json.dumps(selected_entries, indent=2) + "\n"
    )
    (output_dir / "changed-files.txt").write_text(
        "".join(f"{path}\n" for path in sorted(changed))
    )
    (output_dir / "affected-files.txt").write_text(
        "".join(f"{path}\n" for path in sorted(selected))
    )
    (output_dir / "mode").write_text(mode + "\n")
    (output_dir / "reason").write_text(reason + "\n")
    (output_dir / "base").write_text((base or "none") + "\n")

    print(f"analysis mode: {mode}")
    print(f"analysis base: {base or 'none'} ({base_reason})")
    print(f"changed files: {len(changed)}")
    print(f"affected translation units: {len(selected)}")
    if selected:
        print("affected files:")
        print("\n".join(sorted(selected)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

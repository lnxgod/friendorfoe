#!/usr/bin/env python3
"""Reject build inputs that cross the backend-firmware isolation boundary."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
from typing import Iterable, Sequence


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
EXCLUDED_NAMES = {"VENDOR_MANIFEST.json", "VENDORED_SHA256.json"}
QUOTED_PATH = re.compile(r"[\"']([^\"']+)[\"']")
INCLUDE_PATH = re.compile(
    r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE
)
ANGLE_FILTER_PATH = re.compile(r"[+-]?<([^>]+)>")
INCLUDE_FLAG = re.compile(r"(?:^|\s)(?:-I|-isystem\s+)([^\s]+)")
UNQUOTED_PATH = re.compile(
    r"(?<![A-Za-z0-9_}$%])((?:\.\.?/|/)[A-Za-z0-9_./@+~-]+)"
)
BARE_PATH = re.compile(
    r"(?<![A-Za-z0-9_$%{}])"
    r"([A-Za-z0-9_.@+~-]+(?:/[A-Za-z0-9_.*?@+~-]+)+)"
)
KNOWN_ROOT_VARIABLES = (
    "${CMAKE_SOURCE_DIR}",
    "${PROJECT_SOURCE_DIR}",
    "${PROJECT_DIR}",
    "$PROJECT_DIR",
    "%PROJECT_DIR%",
)
KNOWN_LOCAL_VARIABLES = (
    "${CMAKE_CURRENT_LIST_DIR}",
    "${CMAKE_CURRENT_SOURCE_DIR}",
)


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _discover_tool_roots(root: Path, repository_root: Path) -> list[Path]:
    roots: set[Path] = set()
    platformio_packages = Path.home() / ".platformio" / "packages"
    if platformio_packages.exists():
        roots.add(platformio_packages.resolve())
    for variable in ("IDF_PATH", "PLATFORMIO_CORE_DIR"):
        value = os.environ.get(variable)
        if value:
            roots.add(Path(value).expanduser().resolve(strict=False))
    for executable in ("cc", "gcc", "clang", "c++", "g++"):
        found = shutil.which(executable)
        if found:
            roots.add(Path(found).resolve().parent)
    safe_roots: list[Path] = []
    filesystem_root = Path(repository_root.anchor)
    for candidate in roots:
        if (
            candidate == filesystem_root
            or _is_within(repository_root, candidate)
            or (
                _is_within(candidate, repository_root)
                and not _is_within(candidate, root)
            )
        ):
            continue
        safe_roots.append(candidate)
    return sorted(safe_roots, key=str)


def _has_symlink_component(path: Path, repository_root: Path) -> bool:
    absolute = path.absolute()
    try:
        relative = absolute.relative_to(repository_root.absolute())
    except ValueError:
        return False
    current = repository_root.absolute()
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            return True
    return False


def _clean_candidate(candidate: str) -> str:
    return candidate.strip().strip("\"'<>(),;[]")


def _expand_known_variables(candidate: str, root: Path, base_directory: Path) -> str:
    expanded = candidate
    for variable in KNOWN_ROOT_VARIABLES:
        expanded = expanded.replace(variable, str(root))
    for variable in KNOWN_LOCAL_VARIABLES:
        expanded = expanded.replace(variable, str(base_directory))
    return expanded


def _finding_for_path(
    *,
    display_file: str,
    candidate: str,
    base_directory: Path,
    root: Path,
    repository_root: Path,
    allowed_tool_roots: Sequence[Path],
) -> str | None:
    original_candidate = _clean_candidate(candidate)
    if not original_candidate or original_candidate.startswith(("http://", "https://")):
        return None
    candidate = _expand_known_variables(original_candidate, root, base_directory)
    if "$" in candidate or "%" in candidate:
        return f"{display_file}: unresolved build path {original_candidate}"

    lexical = Path(candidate).expanduser()
    was_absolute = lexical.is_absolute()
    if not was_absolute:
        lexical = base_directory / lexical

    if _has_symlink_component(lexical, repository_root):
        return f"{display_file}: repository symlink build path {original_candidate}"

    canonical = lexical.resolve(strict=False)
    if _is_within(canonical, root / "vendor"):
        return f"{display_file}: vendor source compiled {original_candidate}"
    if _is_within(canonical, root):
        return None
    if _is_within(canonical, repository_root):
        return f"{display_file}: protected or escaping build path {original_candidate}"
    if any(_is_within(canonical, tool_root) for tool_root in allowed_tool_roots):
        return None
    if not was_absolute:
        return f"{display_file}: protected or escaping build path {original_candidate}"
    return f"{display_file}: absolute build path outside allowed roots {original_candidate}"


def _config_candidates(path: Path, text: str) -> Iterable[str]:
    if path.suffix.lower() in SOURCE_SUFFIXES:
        yield from INCLUDE_PATH.findall(text)
        return
    yield from QUOTED_PATH.findall(text)
    yield from ANGLE_FILTER_PATH.findall(text)
    yield from INCLUDE_FLAG.findall(text)
    yield from UNQUOTED_PATH.findall(text)
    yield from BARE_PATH.findall(text)


def _build_files(root: Path) -> Iterable[Path]:
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if (
            "vendor" in relative.parts
            or ".pio" in relative.parts
            or "__pycache__" in relative.parts
            or path.name in EXCLUDED_NAMES
        ):
            continue
        if relative.as_posix() == "tools/vendor_snapshot.py":
            continue
        if (
            path.name in {"CMakeLists.txt", "platformio.ini"}
            or path.suffix.lower() == ".cmake"
            or path.suffix.lower() in SOURCE_SUFFIXES
        ):
            yield path


def _compile_argument_paths(arguments: Sequence[str]) -> Iterable[str]:
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument in {"-I", "-isystem", "-include", "-iquote"}:
            if index + 1 < len(arguments):
                yield arguments[index + 1]
                index += 2
                continue
        if argument.startswith("-I") and len(argument) > 2:
            yield argument[2:]
        elif argument.startswith("-isystem") and len(argument) > len("-isystem"):
            yield argument[len("-isystem") :]
        elif not argument.startswith("-") and Path(argument).suffix.lower() in SOURCE_SUFFIXES:
            yield argument
        index += 1


def _compile_database_findings(
    database: Path,
    *,
    root: Path,
    repository_root: Path,
    allowed_tool_roots: Sequence[Path],
) -> list[str]:
    display_file = database.relative_to(root).as_posix()
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{display_file}: invalid compile database: {exc}"]
    if not isinstance(entries, list):
        return [f"{display_file}: invalid compile database: expected a list"]

    findings: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            findings.append(f"{display_file}: invalid compile database entry")
            continue
        directory = Path(entry.get("directory", root)).expanduser()
        if not directory.is_absolute():
            directory = root / directory
        candidates: list[str] = []
        file_value = entry.get("file")
        if isinstance(file_value, str):
            candidates.append(file_value)
        arguments = entry.get("arguments")
        if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
            candidates.extend(_compile_argument_paths(arguments))
        elif isinstance(entry.get("command"), str):
            try:
                candidates.extend(_compile_argument_paths(shlex.split(entry["command"])))
            except ValueError as exc:
                findings.append(f"{display_file}: invalid compile command: {exc}")
        for candidate in candidates:
            finding = _finding_for_path(
                display_file=display_file,
                candidate=candidate,
                base_directory=directory,
                root=root,
                repository_root=repository_root,
                allowed_tool_roots=allowed_tool_roots,
            )
            if finding is not None:
                findings.append(finding)
    return findings


def audit_tree(
    root: Path,
    allowed_tool_roots: Sequence[Path] | None = None,
) -> list[str]:
    root = root.expanduser().resolve(strict=True)
    repository_root = root.parent.resolve(strict=True)
    if allowed_tool_roots is None:
        tool_roots = _discover_tool_roots(root, repository_root)
    else:
        tool_roots = [Path(path).expanduser().resolve(strict=False) for path in allowed_tool_roots]

    findings: list[str] = []
    seen: set[str] = set()
    for path in sorted(_build_files(root), key=lambda item: item.relative_to(root).as_posix()):
        display_file = path.relative_to(root).as_posix()
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            finding = f"{display_file}: unreadable build input: {exc}"
            if finding not in seen:
                findings.append(finding)
                seen.add(finding)
            continue
        for candidate in _config_candidates(path, text):
            finding = _finding_for_path(
                display_file=display_file,
                candidate=candidate,
                base_directory=path.parent,
                root=root,
                repository_root=repository_root,
                allowed_tool_roots=tool_roots,
            )
            if finding is not None and finding not in seen:
                findings.append(finding)
                seen.add(finding)

    for database in sorted(root.rglob("compile_commands.json")):
        for finding in _compile_database_findings(
            database,
            root=root,
            repository_root=repository_root,
            allowed_tool_roots=tool_roots,
        ):
            if finding not in seen:
                findings.append(finding)
                seen.add(finding)
    return findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    arguments = parser.parse_args(argv)
    findings = audit_tree(arguments.root)
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        return 1
    print("source isolation: PASS (0 findings)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Require SP_VERSION at the PR head to advance main's numeric MAJOR.MINOR pair."""

import argparse
import json
import re
import subprocess
from dataclasses import dataclass

VERSION_PATH = "src/host.h"
_C_TOKENS = re.compile(
    r""""(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|(?P<comment>//[^\r\n]*|/\*.*?(?:\*/|\Z))""",
    re.DOTALL,
)
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+SP_VERSION\b([^\r\n]*)", re.MULTILINE)
_SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


@dataclass(frozen=True)
class VersionReading:
    value: str
    parts: tuple[int, int, int] | None = None
    error: str | None = None


def parse_header(text: str) -> VersionReading:
    """Read exactly one literal wide-string definition, without running C or PR code."""
    # Preserve quoted literals so comment-like text inside a version stays invalid.
    text = _C_TOKENS.sub(
        lambda match: re.sub(r"[^\r\n]", " ", match.group())
        if match.group("comment") is not None else match.group(),
        text,
    )
    definitions = _DEFINE.findall(text)
    if not definitions:
        return VersionReading("<missing>", error="SP_VERSION definition is missing")
    if len(definitions) != 1:
        return VersionReading("<multiple definitions>", error="SP_VERSION must be defined exactly once")
    definition = definitions[0].strip()
    literal = re.fullmatch(r'L"([^"\r\n]*)"', definition)
    if literal is None:
        return VersionReading(definition, error='expected one literal L"MAJOR.MINOR.PATCH"')
    value = literal.group(1)
    if _SEMVER.fullmatch(value) is None:
        return VersionReading(value, error="expected MAJOR.MINOR.PATCH, with no leading zeros or suffixes")
    try:
        major, minor, patch = (int(part) for part in value.split("."))
    except ValueError:
        return VersionReading(value, error="a numeric version component is too long")
    return VersionReading(value, (major, minor, patch))


def read_git_version(ref: str) -> VersionReading:
    """Read the committed blob, not a worktree file, symlink target, or merge base."""
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}"],
            check=True, capture_output=True, timeout=20,
        ).stdout.decode("ascii").strip()
        contents = subprocess.run(
            ["git", "cat-file", "blob", f"{commit}:{VERSION_PATH}"],
            check=True, capture_output=True, timeout=20,
        ).stdout
    except (OSError, subprocess.SubprocessError, UnicodeError):
        return VersionReading(
            "<unavailable>",
            error=f"cannot read {VERSION_PATH} from Git ref {ref!r}; ensure the file exists and main is fetched",
        )
    try:
        return parse_header(contents.decode("utf-8-sig"))
    except UnicodeError:
        return VersionReading("<invalid encoding>", error=f"{VERSION_PATH} must be UTF-8 text")


def evaluate(base: VersionReading, head: VersionReading) -> tuple[bool, str]:
    versions = f"main={json.dumps(base.value)}; PR={json.dumps(head.value)}."
    if base.parts is not None and head.parts is not None and head.parts[:2] > base.parts[:2]:
        return True, f"{versions} The PR advances the numeric MAJOR.MINOR pair."

    problems = "; ".join(
        f"{label}: {reading.error}"
        for label, reading in (("main", base), ("PR", head)) if reading.error is not None
    )
    guidance = (
        'Advance by at least 1 minor release: set SP_VERSION in src/host.h to one literal '
        'L"MAJOR.MINOR.PATCH" with non-negative integers, no leading zeros, and a greater '
        '(MAJOR, MINOR) pair than main. Unchanged, patch-only, and lower versions do not qualify; '
        'a major increase is acceptable.'
    )
    if base.parts is not None:
        major, minor, _ = base.parts
        guidance += f" For example, use {major}.{minor + 1}.0 or {major + 1}.0.0."
    elif base.error is not None:
        guidance += " Restore a valid version on main before rerunning this check."
    guidance += " Keep src/app.rc and src/app.manifest in sync with SP_VERSION."
    return False, " ".join(part for part in (versions, problems, guidance) if part)


def escape_annotation(message: str) -> str:
    """Do not let version text create additional GitHub workflow commands."""
    return message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-ref", default="refs/remotes/origin/main", help="fetched target main ref")
    parser.add_argument("--head-ref", default="HEAD", help="exact PR head commit, not a synthetic merge")
    args = parser.parse_args(argv)
    print(f"Version sources: main={json.dumps(args.base_ref)}; PR={json.dumps(args.head_ref)}.")
    passed, message = evaluate(read_git_version(args.base_ref), read_git_version(args.head_ref))
    if passed:
        print(message)
        return 0
    print(f"::error title=Minor version bump required::{escape_annotation(message)}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

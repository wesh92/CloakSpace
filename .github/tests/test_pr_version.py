"""Run with: python3 -I -B -m unittest discover -s .github/tests -p test_pr_version.py -v."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "scripts" / "check_pr_version.py"
SPEC = importlib.util.spec_from_file_location("check_pr_version", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


def header(version):
    return f'#ifndef SP_HOST_H\n#define SP_HOST_H\n#define SP_VERSION L"{version}"\n#endif\n'


class WorkflowTests(unittest.TestCase):
    def test_version_check_runs_before_helper_suite(self):
        workflow = (ROOT / ".github" / "workflows" / "pr-version.yml").read_text(
            encoding="utf-8"
        )
        self.assertLess(
            workflow.index("python3 -I -B .github/scripts/check_pr_version.py"),
            workflow.index(
                "python3 -I -B -m unittest discover -s .github/tests -p test_pr_version.py -v"
            ),
            "Version validation must run before helper tests so header errors show both versions.",
        )


class HeaderTests(unittest.TestCase):
    def test_reads_current_authoritative_header(self):
        version = checker.parse_header((ROOT / "src" / "host.h").read_text(encoding="utf-8"))
        self.assertIsNotNone(version.parts)
        self.assertIsNone(version.error)

    def test_plain_numeric_release_versions(self):
        for value in ("0.0.0", "1.1.0", "1.10.27", "20.0.0", "12345678901234567890.2.3"):
            with self.subTest(value=value):
                result = checker.parse_header(header(value))
                self.assertEqual(result.value, value)
                self.assertEqual(result.parts, tuple(int(part) for part in value.split(".")))
                self.assertIsNone(result.error)

    def test_c_spacing_crlf_and_comments(self):
        text = (
            '/* #define SP_VERSION L"9.9.9"\r\nignored */\r\n'
            '// #define SP_VERSION L"8.8.8"\r\n'
            'const char *example = "/* not a comment */";\r\n'
            '# \tdefine\tSP_VERSION\tL"1.2.3" /* release */ // version\r\n'
        )
        self.assertEqual(checker.parse_header(text).parts, (1, 2, 3))

    def test_rejects_invalid_semver(self):
        for value in (
            "", "1", "1.2", "1.2.3.4", "v1.2.3", "01.2.3", "1.02.3", "1.2.03",
            "-1.2.3", "+1.2.3", "1.2.-3", "1.2.3-rc.1", "1.2.3+build.7",
            "1.2.3 ", " 1.2.3", "1.\u0662.3", "1.2.3/*ignored*/", "1.2.3//ignored",
            "1.2.3\\n::error::injected",
        ):
            with self.subTest(value=value):
                result = checker.parse_header(header(value))
                self.assertIsNone(result.parts)
                self.assertIsNotNone(result.error)

    def test_rejects_missing_duplicate_or_nonliteral_definitions(self):
        for text in (
            "", '// #define SP_VERSION L"1.2.3"\n',
            '/*\n#define SP_VERSION L"1.2.3"\n*/\n',
            header("1.2.3") + '#define SP_VERSION L"2.0.0"\n',
            '#define SP_VERSION "1.2.3"\n',
            '#define SP_VERSION OTHER_VERSION\n',
            '#define SP_VERSION(x) L"1.2.3"\n',
            '#define SP_VERSION L"1.2.3" garbage\n',
            '#define SP_VERSION L"1.2.3" "4"\n',
        ):
            with self.subTest(text=text):
                self.assertIsNotNone(checker.parse_header(text).error)

    def test_extremely_long_component_fails_clearly(self):
        result = checker.parse_header(header("9" * 5000 + ".2.3"))
        self.assertIsNone(result.parts)
        self.assertIsNotNone(result.error)


class ComparisonTests(unittest.TestCase):
    def test_release_version_must_advance_numerically(self):
        cases = (
            ("0.0.0", "0.0.1", True),
            ("1.1.0", "1.1.1", True),
            ("1.1.0", "1.1.7", True),
            ("1.1.9", "1.1.10", True),
            ("1.1.0", "1.2.0", True),
            ("1.1.999", "1.2.0", True),
            ("1.1.0", "1.5.7", True),
            ("1.9.8", "1.10.0", True),
            ("1.99.99", "2.0.0", True),
            ("0.0.9", "0.1.0", True),
            ("0.0.0", "0.0.0", False),
            ("1.1.0", "1.1.0", False),
            ("1.1.9", "1.1.8", False),
            ("1.1.10", "1.1.9", False),
            ("1.10.0", "1.9.999", False),
            ("2.0.0", "1.999.999", False),
            ("2.2.0", "2.1.999", False),
        )
        for base, head, expected in cases:
            with self.subTest(base=base, head=head):
                passed, message = checker.evaluate(
                    checker.parse_header(header(base)), checker.parse_header(header(head))
                )
                self.assertEqual(passed, expected)
                self.assertIn(base, message)
                self.assertIn(head, message)
                if not passed:
                    self.assertIn("patch, minor, or major", message)
                    self.assertIn("(MAJOR, MINOR, PATCH)", message)
                    self.assertIn("src/host.h", message)

    def test_nonincreasing_error_suggests_next_patch_minor_and_major(self):
        version = checker.parse_header(header("1.1.9"))
        passed, message = checker.evaluate(version, version)
        self.assertFalse(passed)
        for example in ("1.1.10", "1.2.0", "2.0.0"):
            with self.subTest(example=example):
                self.assertIn(example, message)

    def test_both_invalid_versions_are_reported_together(self):
        passed, message = checker.evaluate(
            checker.parse_header(header("bad-base")), checker.parse_header(header("bad-pr"))
        )
        self.assertFalse(passed)
        self.assertIn("bad-base", message)
        self.assertIn("bad-pr", message)
        self.assertIn("main", message)
        self.assertIn("PR", message)
        self.assertIn("MAJOR.MINOR.PATCH", message)

    def test_error_annotation_escaping(self):
        self.assertEqual(checker.escape_annotation("a%\r\nb"), "a%25%0D%0Ab")


class GitIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cloakspace-pr-version-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Version Check Tests")
        self.git("config", "user.email", "version-check@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.commit_header(header("1.1.0"))
        self.base_sha = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/origin/main", self.base_sha)
        # A misleading branch name and commit subject must not affect the policy.
        self.git("checkout", "-b", "release-99.0.0")

    def git(self, *args):
        result = subprocess.run(
            ["git", *args], cwd=self.repo, check=True, capture_output=True,
            text=True, encoding="utf-8", timeout=20,
        )
        return result.stdout.strip()

    def commit_header(self, contents):
        path = self.repo / "src" / "host.h"
        path.parent.mkdir(exist_ok=True)
        if contents is None:
            path.unlink()
        elif isinstance(contents, bytes):
            path.write_bytes(contents)
        else:
            path.write_text(contents, encoding="utf-8", newline="\n")
        self.git("add", "src/host.h")
        self.git("commit", "--allow-empty", "-m", "release: pretend version 99.0.0")
        return self.git("rev-parse", "HEAD")

    def run_check(self, *args):
        return subprocess.run(
            [sys.executable, "-I", "-B", str(SCRIPT), *args], cwd=self.repo,
            check=False, capture_output=True, text=True, encoding="utf-8", timeout=30,
        )

    def test_cli_accepts_patch_minor_and_major_bumps(self):
        for version in ("1.1.1", "1.1.10", "1.2.0", "1.10.0", "2.0.0"):
            with self.subTest(version=version):
                head = self.commit_header(header(version))
                result = self.run_check("--head-ref", head)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("1.1.0", result.stdout)
                self.assertIn(version, result.stdout)

    def test_cli_rejects_unchanged_and_downgrade(self):
        for version in ("1.1.0", "1.0.999", "0.99.99"):
            with self.subTest(version=version):
                self.commit_header(header(version))
                result = self.run_check()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("::error title=Version bump required::", result.stdout)
                self.assertIn("main=\"1.1.0\"", result.stdout)
                self.assertIn(f'PR="{version}"', result.stdout)
                self.assertIn("1.1.1", result.stdout)
                self.assertIn("1.2.0", result.stdout)
                self.assertIn("patch, minor, or major", result.stdout)
                self.assertIn("src/host.h", result.stdout)

    def test_reads_main_not_merge_base_or_worktree(self):
        head = self.commit_header(header("1.2.0"))
        self.git("checkout", "main")
        main = self.commit_header(header("1.2.0"))
        self.git("update-ref", "refs/remotes/origin/main", main)
        self.git("checkout", "release-99.0.0")
        (self.repo / "src" / "host.h").write_text(header("99.0.0"), encoding="utf-8")
        result = self.run_check("--head-ref", head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('main="1.2.0"; PR="1.2.0"', result.stdout)
        self.assertNotIn('PR="99.0.0"', result.stdout)

    def test_explicit_head_not_checked_out_merge_result(self):
        head = self.commit_header(header("1.1.0"))
        self.commit_header(header("99.0.0"))
        result = self.run_check("--head-ref", head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('PR="1.1.0"', result.stdout)

    def test_explicit_patch_head_passes_despite_invalid_checked_out_header(self):
        head = self.commit_header(header("1.1.1"))
        self.commit_header(header("invalid"))
        result = self.run_check("--head-ref", head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PR="1.1.1"', result.stdout)

    def test_missing_or_invalid_pr_version_fails_with_both_sides(self):
        for contents in (
            header("1.2.bad"), header("01.1.1"), header("1.01.1"), header("1.1.01"),
            header("1.1.-1"), header("1.1.1-rc.1"),
            header("1.1.1") + '#define SP_VERSION L"1.1.2"\n',
            '#define SP_VERSION L"1.1.1" L"0"\n',
            "/* no version */\n", b"\xff", None,
        ):
            with self.subTest(contents=contents):
                self.commit_header(contents)
                result = self.run_check()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('main="1.1.0"; PR=', result.stdout)
                self.assertIn("MAJOR.MINOR.PATCH", result.stdout)

    def test_missing_or_invalid_main_version_fails_with_both_sides(self):
        self.commit_header(header("2.0.0"))
        head = self.git("rev-parse", "HEAD")
        for contents in (header("bad-base"), "/* no version */\n", b"\xff", None):
            with self.subTest(contents=contents):
                self.git("checkout", "main")
                base = self.commit_header(contents)
                self.git("update-ref", "refs/remotes/origin/main", base)
                result = self.run_check("--head-ref", head)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("main=", result.stdout)
                self.assertIn('PR="2.0.0"', result.stdout)
                self.assertIn("main:", result.stdout)
                self.assertIn("MAJOR.MINOR.PATCH", result.stdout)
                # Restore the tracked file before the next subcase can delete it.
                self.commit_header(header("1.1.0"))

    def test_missing_main_ref_fails_closed(self):
        self.commit_header(header("1.2.0"))
        self.git("update-ref", "-d", "refs/remotes/origin/main")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("refs/remotes/origin/main", result.stdout)
        self.assertIn('PR="1.2.0"', result.stdout)

    def test_option_like_ref_cannot_become_a_git_option(self):
        result = self.run_check("--head-ref=--help")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("PR:", result.stdout)
        self.assertNotIn("Traceback", result.stderr)


class GitFailureTests(unittest.TestCase):
    def test_git_unavailable_fails_closed(self):
        with patch.object(checker.subprocess, "run", side_effect=FileNotFoundError):
            self.assertIsNotNone(checker.read_git_version("HEAD").error)

    def test_git_timeout_fails_closed(self):
        with patch.object(
            checker.subprocess, "run", side_effect=subprocess.TimeoutExpired("git", 20)
        ):
            self.assertIsNotNone(checker.read_git_version("HEAD").error)


if __name__ == "__main__":
    unittest.main()

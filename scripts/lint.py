#!/usr/bin/env python3
"""
chttp2 lint & auto-fix tool

Runs clang-format and clang-tidy against project sources.

Usage:
  python3 scripts/lint.py                  # check all sources (dry-run)
  python3 scripts/lint.py --fix            # auto-fix all issues
  python3 scripts/lint.py --format         # clang-format only
  python3 scripts/lint.py --tidy           # clang-tidy only
  python3 scripts/lint.py --fix --format   # auto-format only
  python3 scripts/lint.py --fix --tidy     # auto-fix tidy issues only
  python3 scripts/lint.py src/http2_protocol.cpp  # check specific files
  python3 scripts/lint.py --diff           # check git-modified files only
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_ROOT / "build"
COMPILE_COMMANDS = BUILD_DIR / "compile_commands.json"

SOURCE_DIRS = ["src", "include"]
SOURCE_EXTENSIONS = {".cpp", ".hpp", ".h"}

# Prefer Homebrew LLVM on macOS; fall back to PATH.
_BREW_LLVM = Path("/opt/homebrew/opt/llvm/bin")
CLANG_FORMAT = (
    str(_BREW_LLVM / "clang-format") if (_BREW_LLVM / "clang-format").exists()
    else "clang-format"
)
CLANG_TIDY = (
    str(_BREW_LLVM / "clang-tidy") if (_BREW_LLVM / "clang-tidy").exists()
    else "clang-tidy"
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def find_sources(roots: list[str]) -> list[Path]:
    """Recursively collect all C++ source files."""
    sources: list[Path] = []
    for root in roots:
        root_path = PROJECT_ROOT / root
        if not root_path.exists():
            continue
        for path in sorted(root_path.rglob("*")):
            if path.suffix in SOURCE_EXTENSIONS and path.is_file():
                sources.append(path)
    return sources


def git_modified_files() -> list[Path]:
    """Return C++ files modified in the working tree (staged + unstaged)."""
    result = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", "HEAD"],
        capture_output=True, text=True, cwd=PROJECT_ROOT,
    )
    staged = subprocess.run(
        ["git", "diff", "--name-only", "--cached", "--diff-filter=ACMR"],
        capture_output=True, text=True, cwd=PROJECT_ROOT,
    )
    # merge and deduplicate
    files = set(result.stdout.strip().splitlines() + staged.stdout.strip().splitlines())
    paths = []
    for f in sorted(files):
        p = PROJECT_ROOT / f
        if p.suffix in SOURCE_EXTENSIONS and p.is_file():
            paths.append(p)
    return paths


def check_tool(tool: str, name: str) -> bool:
    """Return True if *tool* is available on the system."""
    try:
        subprocess.run([tool, "--version"], capture_output=True, check=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        print(f"error: {name} not found: {tool}", file=sys.stderr)
        return False


_SKIP_PREFIXES = ("/", "include/", "single_include/")

def _skip_diagnostic(rel_line: str) -> bool:
    """Return True if a diagnostic line should be suppressed."""
    return any(rel_line.startswith(p) for p in _SKIP_PREFIXES)


def relative(path: Path) -> str:
    """Return *path* relative to the project root."""
    try:
        return str(path.relative_to(PROJECT_ROOT))
    except ValueError:
        return str(path)


# ---------------------------------------------------------------------------
# clang-format
# ---------------------------------------------------------------------------

def run_clang_format(files: list[Path], fix: bool) -> int:
    """Run clang-format.  Returns the number of files with problems."""
    if not files:
        return 0

    problems = 0

    if fix:
        # format in-place
        cmd = [CLANG_FORMAT, "-i"] + [str(f) for f in files]
        subprocess.run(cmd, cwd=PROJECT_ROOT)
        print(f"  formatted {len(files)} file(s)")
    else:
        # dry-run: report files that would change
        for f in files:
            result = subprocess.run(
                [CLANG_FORMAT, "--dry-run", "--Werror", str(f)],
                capture_output=True, text=True, cwd=PROJECT_ROOT,
            )
            if result.returncode != 0:
                problems += 1
                print(f"  [!] {relative(f)}")
                # show diff
                diff_result = subprocess.run(
                    [CLANG_FORMAT, str(f)],
                    capture_output=True, text=True, cwd=PROJECT_ROOT,
                )
                original = f.read_text()
                if diff_result.stdout != original:
                    import difflib
                    diff = difflib.unified_diff(
                        original.splitlines(keepends=True),
                        diff_result.stdout.splitlines(keepends=True),
                        fromfile=relative(f),
                        tofile=relative(f) + " (formatted)",
                        n=2,
                    )
                    diff_lines = list(diff)
                    # truncate to 30 lines to avoid flooding the terminal
                    for line in diff_lines[:30]:
                        print(f"    {line}", end="")
                    if len(diff_lines) > 30:
                        print(f"    ... {len(diff_lines) - 30} more line(s)")

        if problems == 0:
            print("  all files formatted correctly")

    return problems


# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------

def run_clang_tidy(files: list[Path], fix: bool) -> int:
    """Run clang-tidy.  Returns the number of files with problems."""
    if not files:
        return 0

    if not COMPILE_COMMANDS.exists():
        print(f"  [!] {relative(COMPILE_COMMANDS)} not found", file=sys.stderr)
        print("  generate it first:", file=sys.stderr)
        print("    cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON", file=sys.stderr)
        return 1

    # clang-tidy operates on .cpp files; headers are checked indirectly.
    cpp_files = [f for f in files if f.suffix == ".cpp"]
    header_files = [f for f in files if f.suffix in {".hpp", ".h"}]

    if not cpp_files:
        if header_files:
            print("  (i) clang-tidy only analyses .cpp files; "
                  "headers are checked indirectly)")
        return 0

    # Only allow clang-tidy to apply fixes inside src/ and test/.
    # Diagnostics from include/ headers are still reported but never auto-fixed.
    header_filter = r"^(" + re.escape(str(PROJECT_ROOT)) + r"/(src|test)/.*)"

    problems = 0

    for f in cpp_files:
        cmd = [
            CLANG_TIDY,
            "-p", str(BUILD_DIR),
            "--quiet",
            f"--header-filter={header_filter}",
        ]
        if fix:
            cmd.append("--fix")
            cmd.append("--fix-errors")
        cmd.append(str(f))

        result = subprocess.run(
            cmd,
            capture_output=True, text=True, cwd=PROJECT_ROOT,
        )

        # clang-tidy prints diagnostics to stdout.
        output = result.stdout.strip()
        has_warnings = False

        # Only flag diagnostics from project src/ and test/ files;
        # skip anything from include/, single_include/, or system headers.
        target_dir = str(PROJECT_ROOT) + "/"
        if output:
            for line in output.splitlines():
                if ": warning:" not in line and ": error:" not in line:
                    continue
                rel = line.replace(target_dir, "")
                if _skip_diagnostic(rel):
                    continue
                has_warnings = True
                break

        if has_warnings:
            problems += 1
            action = "fixed" if fix else "issues found"
            print(f"  [!] {relative(f)} ({action})")
            for line in output.splitlines():
                if (": warning:" not in line
                        and ": error:" not in line
                        and ": note:" not in line):
                    continue
                short = line.replace(target_dir, "")
                if _skip_diagnostic(short):
                    continue
                print(f"    {short}")

    if problems == 0:
        verb = "fix" if fix else "check"
        print(f"  clang-tidy {verb} passed ({len(cpp_files)} file(s))")

    return problems


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="chttp2 lint & auto-fix tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "files", nargs="*",
        help="files to check (default: all sources)",
    )
    parser.add_argument(
        "--fix", action="store_true",
        help="auto-fix (clang-format -i + clang-tidy --fix)",
    )
    parser.add_argument(
        "--format", action="store_true",
        help="run clang-format only",
    )
    parser.add_argument(
        "--tidy", action="store_true",
        help="run clang-tidy only",
    )
    parser.add_argument(
        "--diff", action="store_true",
        help="check git-modified files only",
    )
    args = parser.parse_args()

    os.chdir(PROJECT_ROOT)

    # decide which checks to run
    run_format = True
    run_tidy = True
    if args.format and not args.tidy:
        run_tidy = False
    if args.tidy and not args.format:
        run_format = False

    # verify tools are available
    if run_format and not check_tool(CLANG_FORMAT, "clang-format"):
        return 1
    if run_tidy and not check_tool(CLANG_TIDY, "clang-tidy"):
        return 1

    # collect files
    if args.files:
        files = [Path(f).resolve() for f in args.files]
        for f in files:
            if not f.exists():
                print(f"error: file not found: {f}", file=sys.stderr)
                return 1
    elif args.diff:
        files = git_modified_files()
        if not files:
            print("(i) no modified C++ files")
            return 0
        print(f"checking {len(files)} modified file(s)")
    else:
        files = find_sources(SOURCE_DIRS)
        print(f"checking {len(files)} file(s)")

    if not files:
        print("(i) no files to check")
        return 0

    mode = "auto-fix mode" if args.fix else "check mode (dry-run)"
    print(f"\n{mode}\n")

    total_problems = 0

    if run_format:
        print("--- clang-format ---")
        total_problems += run_clang_format(files, args.fix)
        print()

    if run_tidy:
        print("--- clang-tidy ---")
        total_problems += run_clang_tidy(files, args.fix)
        print()

    # summary
    if args.fix:
        print("done.")
        if total_problems > 0:
            print(f"  {total_problems} file(s) had issues (now fixed)")
            print("  please review changes and rebuild to verify")
        return 0
    else:
        if total_problems > 0:
            print(f"{total_problems} file(s) with issues")
            print("  run: python3 scripts/lint.py --fix")
            return 1
        else:
            print("all checks passed")
            return 0


if __name__ == "__main__":
    sys.exit(main())

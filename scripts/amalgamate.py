#!/usr/bin/env python3
"""Merge all chttp2 headers and sources into a single-header library.

Output: single_include/chttp2.hpp

Usage in downstream projects:

    // In exactly ONE .cpp file:
    #define CHTTP2_IMPLEMENTATION
    #include "chttp2.hpp"

    // In all other files:
    #include "chttp2.hpp"

You still need to link against OpenSSL (-lssl -lcrypto) and pthreads (-lpthread).
"""

import os
import re
import sys
from collections import defaultdict, deque

PROJ_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
INCLUDE_DIR = os.path.join(PROJ_ROOT, "include")
SRC_DIR = os.path.join(PROJ_ROOT, "src")
OUT_DIR = os.path.join(PROJ_ROOT, "single_include")
OUT_FILE = os.path.join(OUT_DIR, "chttp2.hpp")

INTERNAL_INCLUDE_RE = re.compile(r'^\s*#include\s+"chttp2/[^"]+"\s*$')
PRAGMA_ONCE_RE = re.compile(r'^\s*#pragma\s+once\s*$')
SYSTEM_INCLUDE_RE = re.compile(r'^\s*#include\s+<([^>]+)>\s*$')
INTERNAL_REF_RE = re.compile(r'^\s*#include\s+"(chttp2/[^"]+)"')

HEADERS = []
SOURCES = []


def discover_files():
    for root, _, files in os.walk(INCLUDE_DIR):
        for f in sorted(files):
            if f.endswith(".hpp"):
                rel = os.path.relpath(os.path.join(root, f), INCLUDE_DIR)
                HEADERS.append(rel)
    for root, _, files in os.walk(SRC_DIR):
        for f in sorted(files):
            if f.endswith(".cpp") and "_test" not in f and "main.cpp" not in f and "integrate" not in f:
                rel = os.path.relpath(os.path.join(root, f), SRC_DIR)
                SOURCES.append(rel)


def build_dep_graph():
    deps = defaultdict(list)
    for hdr in HEADERS:
        path = os.path.join(INCLUDE_DIR, hdr)
        with open(path, "r") as fh:
            for line in fh:
                m = INTERNAL_REF_RE.search(line)
                if m:
                    deps[hdr].append(m.group(1))
    return deps


def topo_sort(headers, deps):
    in_degree = defaultdict(int)
    rev = defaultdict(list)
    header_set = set(headers)

    for h in headers:
        for d in deps.get(h, []):
            if d in header_set:
                in_degree[h] += 1
                rev[d].append(h)

    queue = deque(sorted(h for h in headers if in_degree[h] == 0))
    order = []
    while queue:
        n = queue.popleft()
        order.append(n)
        for m in sorted(rev[n]):
            in_degree[m] -= 1
            if in_degree[m] == 0:
                queue.append(m)

    if len(order) != len(headers):
        missing = sorted(set(headers) - set(order))
        print(f"WARNING: could not resolve order for: {missing}", file=sys.stderr)
        order.extend(missing)

    return order


def collect_system_includes_from_headers(sorted_headers):
    """Collect #include <...> only from headers (unconditional includes)."""
    includes = set()
    for hdr in sorted_headers:
        path = os.path.join(INCLUDE_DIR, hdr)
        # Simple heuristic: skip includes inside #if blocks.
        depth = 0
        with open(path, "r") as fh:
            for line in fh:
                stripped = line.strip()
                if stripped.startswith("#if"):
                    depth += 1
                elif stripped.startswith("#endif"):
                    depth = max(0, depth - 1)
                elif stripped.startswith("#elif") or stripped.startswith("#else"):
                    pass  # same depth
                elif depth == 0:
                    m = SYSTEM_INCLUDE_RE.match(line)
                    if m:
                        includes.add(m.group(1))
    return sorted(includes)


def strip_header(path):
    """Strip internal includes, #pragma once, and unconditional system includes.

    System includes inside #if/#ifdef blocks are preserved so that
    platform-conditional headers (e.g. <unistd.h> in the POSIX branch of
    platform.hpp) survive into the amalgamated output.
    """
    lines = []
    depth = 0
    with open(path, "r") as fh:
        for line in fh:
            if INTERNAL_INCLUDE_RE.match(line):
                continue
            if PRAGMA_ONCE_RE.match(line):
                continue
            stripped = line.strip()
            if stripped.startswith("#if"):
                depth += 1
            elif stripped.startswith("#endif"):
                depth = max(0, depth - 1)
            # Only strip system includes at the top level; keep conditional ones.
            if depth == 0 and SYSTEM_INCLUDE_RE.match(line):
                continue
            lines.append(line)
    while lines and lines[0].strip() == "":
        lines.pop(0)
    while lines and lines[-1].strip() == "":
        lines.pop()
    return lines


def strip_source(path):
    """Strip only internal chttp2/ includes from a source file. Keep system includes."""
    lines = []
    with open(path, "r") as fh:
        for line in fh:
            if INTERNAL_INCLUDE_RE.match(line):
                continue
            lines.append(line)
    while lines and lines[0].strip() == "":
        lines.pop(0)
    while lines and lines[-1].strip() == "":
        lines.pop()
    return lines


def main():
    discover_files()

    deps = build_dep_graph()
    sorted_headers = topo_sort(HEADERS, deps)
    system_includes = collect_system_includes_from_headers(sorted_headers)

    os.makedirs(OUT_DIR, exist_ok=True)

    with open(OUT_FILE, "w") as out:
        out.write("// chttp2 -- single-header amalgamation\n")
        out.write("// Generated by scripts/amalgamate.py -- do not edit by hand.\n")
        out.write("//\n")
        out.write("// Usage:\n")
        out.write("//   In exactly ONE .cpp file, before including this header:\n")
        out.write("//     #define CHTTP2_IMPLEMENTATION\n")
        out.write("//     #include \"chttp2.hpp\"\n")
        out.write("//\n")
        out.write("//   In all other files:\n")
        out.write("//     #include \"chttp2.hpp\"\n")
        out.write("//\n")
        out.write("//   Link against: -lssl -lcrypto -lpthread\n")
        out.write("//\n")
        out.write("// License: MIT\n")
        out.write("\n")
        out.write("#ifndef CHTTP2_SINGLE_INCLUDE_HPP\n")
        out.write("#define CHTTP2_SINGLE_INCLUDE_HPP\n")
        out.write("\n")

        for inc in system_includes:
            out.write(f"#include <{inc}>\n")
        out.write("\n")

        for hdr in sorted_headers:
            path = os.path.join(INCLUDE_DIR, hdr)
            lines = strip_header(path)
            out.write(f"// --- {hdr} ---\n")
            for line in lines:
                out.write(line)
            out.write("\n\n")

        out.write("// " + "=" * 76 + "\n")
        out.write("// Implementation\n")
        out.write("// " + "=" * 76 + "\n\n")
        out.write("#ifdef CHTTP2_IMPLEMENTATION\n\n")

        for src in SOURCES:
            path = os.path.join(SRC_DIR, src)
            lines = strip_source(path)
            out.write(f"// --- {src} ---\n")
            for line in lines:
                out.write(line)
            out.write("\n\n")

        out.write("#endif // CHTTP2_IMPLEMENTATION\n\n")
        out.write("#endif // CHTTP2_SINGLE_INCLUDE_HPP\n")

    # Generate forwarding headers so existing #include "chttp2/foo.hpp" works.
    fwd_count = 0
    for hdr in sorted_headers:
        fwd_path = os.path.join(OUT_DIR, hdr)
        fwd_dir = os.path.dirname(fwd_path)
        os.makedirs(fwd_dir, exist_ok=True)

        depth = hdr.count(os.sep)
        rel_prefix = "../" * depth
        with open(fwd_path, "w") as fwd:
            fwd.write(f"// Forwarding header — includes the amalgamated single header.\n")
            fwd.write(f"#include \"{rel_prefix}chttp2.hpp\"\n")
        fwd_count += 1

    total = sum(1 for _ in open(OUT_FILE))
    print(f"Generated {OUT_FILE}")
    print(f"  Headers: {len(sorted_headers)}")
    print(f"  Sources: {len(SOURCES)}")
    print(f"  Forwarding headers: {fwd_count}")
    print(f"  Total lines: {total}")


if __name__ == "__main__":
    main()

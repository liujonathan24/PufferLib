#!/usr/bin/env python3
"""Audit NetHack source files for mutable globals (file-scope and
function-local statics).

A "global" here means writable state that PERSISTS across multiple calls.
That includes both:
  (a) file-scope mutable variables (the classic "global"), and
  (b) function-local `static T x;` declarations — those live for the
      lifetime of the process, are shared across all callers, and survive
      function exit. In a multi-env single-process refactor they race the
      same way file-scope globals do.

What we report:
    <file>:<line> [<kind>] <type> <name> [in <function>]

  kind ∈ {
    "static"      — file-scope `static T x ...`
    "extern"      — file-scope declaration `T x ...` without storage class
                    (becomes external/linker-visible; the dual problem)
    "thread"      — __thread (per-thread; OK if 1 env/thread is pinned)
    "E-decl"      — uses NetHack `E` macro (header forward decl)
    "func-static" — `static T x ...` INSIDE a function body. These persist
                    across calls and yields, same hazard as file-scope.
                    Requires --func-statics to be reported.
  }

We DO NOT flag:
  - const / `const T x = ...` (read-only)
  - typedef, struct, enum, union declarations
  - `extern T x;` declarations (no storage, no body)
  - macro `#define` lines
  - function declarations / definitions
  - non-static locals (`int i;` inside a function — not persistent)

This is a heuristic, not a parser. It will miss things and over-report.
The user should sanity-check each line. The goal is to cut the audit
surface from ~5000 lines/file to ~tens of lines/file.

Usage:
    ./find_globals.py vendor/nle/src/src/*.c
    ./find_globals.py --summary vendor/nle/src/src/*.c
    ./find_globals.py --func-statics vendor/nle/src/src/shk.c
    ./find_globals.py --csv --func-statics vendor/nle/src/src/*.c > all.csv
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict


# Token patterns
TYPE_TOKENS = r"(?:const\s+)?(?:unsigned\s+|signed\s+|long\s+|short\s+|static\s+|register\s+|volatile\s+|struct\s+\w+|union\s+\w+|enum\s+\w+|[A-Za-z_]\w*(?:\s*\*+)?)"

# Heuristic regex: a global declaration line at file scope looks like
#   <maybe-storage-class> <type> <name>[ = ...]; or <name>[N];
# We use a permissive match and then filter by storage class.
STORAGE_TOKENS = {
    "static", "extern", "register", "volatile",
    "STATIC_VAR", "STATIC_DCL", "NEARDATA", "__thread", "E",
}
TYPE_TOKENS_SET = {
    "unsigned", "signed", "long", "short", "const",
    "char", "int", "float", "double", "void",
    "boolean", "xchar", "schar", "uchar", "uint", "uschar",
    "size_t", "ssize_t", "time_t", "off_t",
    "winid", "anything", "coord",  # nethack types
}

# Decl line: optional storage tokens, type tokens, optional `*`s, IDENT,
# optional [N], optional = init, terminating ;
DECL_RE = re.compile(
    r"^\s*"
    r"(?P<head>"
        r"(?:[A-Za-z_]\w*\s+|\*+\s*)+"   # storage + type tokens, with optional stars
    r")"
    r"(?P<name>[A-Za-z_]\w*)"
    r"\s*"
    r"(?P<rest>(?:\[[^;{]*\]\s*)?(?:=\s*[^;]*)?)"
    r"\s*;\s*(?://.*|/\*.*\*/)?\s*$"
)

FUNC_RE = re.compile(r"^\s*[A-Za-z_].*\([^)]*\)\s*(\{|;)?\s*$")

KEYWORDS_NOT_VARS = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "return", "goto", "break", "continue", "sizeof", "typedef", "struct",
    "union", "enum", "void", "FDECL", "STATIC_PTR",
}


def is_const(text: str) -> bool:
    """Detect const-correctness in the line — read-only data is OK."""
    return bool(re.search(r"\bconst\b", text))


def strip_comments(line: str, in_block_comment: bool) -> tuple[str, bool]:
    """Strip /* ... */ and // comments, tracking multi-line block state."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                return "".join(out), True
            i = end + 2
            in_block_comment = False
            continue
        if line[i:i+2] == "/*":
            end = line.find("*/", i + 2)
            if end == -1:
                return "".join(out), True
            i = end + 2
            continue
        if line[i:i+2] == "//":
            break
        out.append(line[i])
        i += 1
    return "".join(out), False


def find_globals(path: str, include_func_statics: bool = False) -> list[dict]:
    """Scan a single C file and return a list of suspected globals.

    If `include_func_statics` is True, also report `static T x;` lines
    inside function bodies (they persist across calls and races same as
    file-scope state).
    """
    with open(path, "rb") as f:
        data = f.read()
    text = data.decode("utf-8", errors="replace")
    lines = text.split("\n")

    findings: list[dict] = []
    brace_depth = 0
    paren_depth = 0
    in_block_comment = False
    in_preproc = False
    # NetHack uses K&R style: `T name(args)\n T arg1;\n T arg2;\n {...}`
    # When we see `name(...)` at depth 0 without semicolon, the lines that
    # follow until `{` are K&R parameter declarations — skip them.
    in_knr_params = False
    # Track the most recent function name we entered (best-effort).
    # Updated when we see `T name(...)` at depth 0 (start of K&R or ANSI
    # function definition). Cleared back to "" when brace_depth returns
    # to 0 from > 0.
    current_function = ""
    pending_function = ""  # set when we see the signature, committed on `{`

    for lineno, raw in enumerate(lines, start=1):
        stripped, in_block_comment = strip_comments(raw, in_block_comment)
        s = stripped.strip()

        # Track preprocessor continuation lines
        if in_preproc:
            if not raw.rstrip().endswith("\\"):
                in_preproc = False
            continue
        if s.startswith("#"):
            if raw.rstrip().endswith("\\"):
                in_preproc = True
            continue

        # If we just saw a function signature, everything until `{` is K&R
        # parameter declarations — skip until we see the opening brace.
        if in_knr_params:
            for ch in re.findall(r"[{}()]", s):
                if ch == "{":
                    brace_depth += 1
                    in_knr_params = False
                    # Commit pending function name on entering its body.
                    if pending_function:
                        current_function = pending_function
                        pending_function = ""
                elif ch == "}":
                    brace_depth = max(0, brace_depth - 1)
                elif ch == "(":
                    paren_depth += 1
                elif ch == ")":
                    paren_depth = max(0, paren_depth - 1)
            continue

        # Update brace depth from the stripped line (ignoring strings/chars).
        # Simple: count { and } not preceded by backslash.
        prev_depth = brace_depth
        for ch in re.findall(r"[{}()]", s):
            if ch == "{":
                brace_depth += 1
                # Commit any pending function-name on first brace open.
                if brace_depth == 1 and pending_function:
                    current_function = pending_function
                    pending_function = ""
            elif ch == "}":
                brace_depth = max(0, brace_depth - 1)
                if brace_depth == 0:
                    current_function = ""
            elif ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth = max(0, paren_depth - 1)

        # === Function-local statics: only when explicitly enabled ===
        if include_func_statics and brace_depth > 0 and paren_depth == 0:
            # Be strict: only consider lines that START with `static` (with
            # optional NEARDATA/STATIC_VAR prefix), to avoid false positives
            # on regular locals. Also skip `static const`.
            if re.match(r"^\s*(NEARDATA\s+)?(STATIC_VAR\s+)?static\s+", s) \
                    and not re.match(r"^\s*static\s+const\b", s):
                m = DECL_RE.match(s)
                if m:
                    head = m.group("head") or ""
                    name = m.group("name") or ""
                    if name not in KEYWORDS_NOT_VARS:
                        head_tokens = re.findall(r"[A-Za-z_]\w*", head)
                        storage_set = {t for t in head_tokens if t in STORAGE_TOKENS}
                        type_tokens = [t for t in head_tokens
                                       if t not in STORAGE_TOKENS]
                        if type_tokens and "const" not in type_tokens \
                                and "const" not in storage_set:
                            findings.append({
                                "file": path,
                                "line": lineno,
                                "kind": "func-static",
                                "type": " ".join(type_tokens),
                                "name": name,
                                "rest": m.group("rest").strip(),
                                "function": current_function or "?",
                            })
            continue

        # Only look at file-scope (depth 0) lines for non-func-static reporting
        if brace_depth != 0:
            continue
        if paren_depth != 0:
            continue
        if not s:
            continue

        # Skip lines that look like function prototypes/definitions:
        # `T name(args)` with no `=` and not a variable declaration.
        if "(" in s and "=" not in s.split("(")[0]:
            # If it ends without `;` it's a function definition header
            # followed by K&R param declarations — switch state.
            if not s.rstrip().endswith(";") and not s.rstrip().endswith(","):
                in_knr_params = True
                # Capture the function name (last identifier before the `(`).
                pre_paren = s.split("(")[0]
                ids = re.findall(r"[A-Za-z_]\w*", pre_paren)
                if ids:
                    pending_function = ids[-1]
            continue

        # Skip typedef/struct/union/enum/const-only declarations
        first_word = s.split()[0] if s.split() else ""
        if first_word in ("typedef",):
            continue
        if first_word in ("struct", "union", "enum") and not s.rstrip().endswith(";"):
            # likely a struct/union/enum DEFINITION (multi-line). Skip until
            # we leave the depth>0 territory.
            continue

        # Run our regex
        m = DECL_RE.match(s)
        if not m:
            continue
        head = m.group("head") or ""
        name = m.group("name") or ""

        if name in KEYWORDS_NOT_VARS:
            continue

        # Split head into tokens to classify
        head_tokens = re.findall(r"[A-Za-z_]\w*", head)
        if not head_tokens:
            continue
        # The last token of head is part of the type (e.g. `int` in `static int`)
        # The earlier tokens may be storage classes.
        storage_set = {t for t in head_tokens if t in STORAGE_TOKENS}
        type_tokens = [t for t in head_tokens if t not in STORAGE_TOKENS]

        # Must have at least one type token
        if not type_tokens:
            continue

        # Skip pure const definitions
        if "const" in storage_set or "const" in type_tokens:
            continue

        typ = " ".join(type_tokens)

        # Classify
        kind = "extern"
        if "static" in storage_set or "STATIC_VAR" in storage_set or "STATIC_DCL" in storage_set:
            kind = "static"
        if "__thread" in storage_set:
            kind = "thread"
        if "NEARDATA" in storage_set and kind == "extern":
            kind = "extern+NEAR"
        if "E" in storage_set:
            kind = "E-decl"

        # Filter out things that are clearly not data:
        # - if name starts with capital and there are parens later, it's a func ptr decl
        # - declared as a function-typedef-style is ambiguous; we let it
        # through and let the user check

        findings.append({
            "file": path,
            "line": lineno,
            "kind": kind,
            "type": typ.strip(),
            "name": name,
            "rest": m.group("rest").strip(),
        })

    return findings


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--summary", action="store_true",
                    help="Only print per-file counts.")
    ap.add_argument("--exclude-thread", action="store_true",
                    help="Don't report __thread vars (already per-thread).")
    ap.add_argument("--csv", action="store_true",
                    help="Emit machine-readable CSV.")
    ap.add_argument("--func-statics", action="store_true",
                    help="Also report `static T x;` inside function bodies. "
                         "These persist across calls and yields, same hazard "
                         "as file-scope globals under multi-env vecenv.")
    args = ap.parse_args(argv)

    counts: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    all_findings: list[dict] = []

    for path in args.files:
        if not os.path.isfile(path):
            continue
        finds = find_globals(path, include_func_statics=args.func_statics)
        for f in finds:
            if args.exclude_thread and "thread" in f["kind"]:
                continue
            counts[path][f["kind"]] += 1
            all_findings.append(f)

    if args.csv:
        print("file,line,kind,type,name,rest,function")
        for f in all_findings:
            r = f["rest"].replace(",", ";").replace("\n", " ")
            fn = f.get("function", "")
            print(f"{f['file']},{f['line']},{f['kind']},{f['type']},"
                  f"{f['name']},{r},{fn}")
        return 0

    if args.summary:
        # Print one line per file with counts
        print(f"{'FILE':50s} {'static':>7s} {'extern':>7s} "
              f"{'thread':>7s} {'E-decl':>7s} {'func':>7s} {'total':>7s}")
        print("-" * 95)
        files_sorted = sorted(counts.keys(),
                              key=lambda p: -sum(counts[p].values()))
        for p in files_sorted:
            c = counts[p]
            tot = sum(c.values())
            print(f"{p:50s} {c.get('static',0):>7d} "
                  f"{c.get('extern',0):>7d} {c.get('thread',0):>7d} "
                  f"{c.get('E-decl',0):>7d} {c.get('func-static',0):>7d} "
                  f"{tot:>7d}")
        return 0

    # Default: verbose per-finding output, grouped by file
    for path in sorted({f["file"] for f in all_findings}):
        finds = [f for f in all_findings if f["file"] == path]
        if not finds:
            continue
        print(f"\n=== {path} === ({len(finds)} potential globals)")
        for f in finds:
            kind = f["kind"]
            fn_suffix = f"  in {f['function']}()" if f.get("function") else ""
            print(f"  {path}:{f['line']:5d} [{kind:14s}] "
                  f"{f['type']:25s} {f['name']}"
                  f"{(' ' + f['rest']) if f['rest'] else ''}{fn_suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

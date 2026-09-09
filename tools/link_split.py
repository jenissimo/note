#!/usr/bin/env python3
"""Publish the functions the split turned into cross-module references.

Cutting note_win32.c into modules left every function `static`, so anything
called from another file is now undeclared.  Rather than guessing which ones
those are, this reads the compiler's own complaints, finds each name's
definition, drops its `static`, and writes a prototype into note_win32.h.

It is idempotent: run, build, run again until the build is clean.

    python tools/link_split.py errors.txt
"""

import io
import os
import re
import sys

DIR = "src/platform/win32"
HDR = os.path.join(DIR, "note_win32.h")
MARK = "/* ---- published by the split ---- */"

# 'name': undeclared identifier   /   'name': undefined; assuming extern
NAME_RE = re.compile(r"(?:C2065|C4013|C2371|C2146).*?[\"']([A-Za-z_][A-Za-z0-9_]*)[\"']")
# The Russian toolchain reports the same codes with translated text, so match
# on the code and pull the quoted identifier that follows it.
ALT_RE = re.compile(r"(?:error|warning) (?:C2065|C4013)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)")


def sources():
    return [os.path.join(DIR, f) for f in sorted(os.listdir(DIR))
            if f.startswith("win32_") and f.endswith(".c")]


def find_definition(name):
    """The file, line index and full signature of a static definition."""
    for path in sources():
        lines = io.open(path, encoding="utf-8", newline="").read().split("\n")
        for i, line in enumerate(lines):
            if not line.startswith("static "):
                continue
            # A definition, not a forward declaration or a variable.
            if re.search(r"\b%s\s*\(" % re.escape(name), line) is None:
                continue
            if line.rstrip().endswith(";"):
                continue
            # Gather the signature, which may wrap across lines.
            sig, j = line, i
            while "{" not in sig and not sig.rstrip().endswith(")") and j + 1 < len(lines):
                j += 1
                sig += " " + lines[j].strip()
            return path, i, j, sig
    return None


def publish(names):
    header = io.open(HDR, encoding="utf-8", newline="").read()
    if MARK not in header:
        header = header.replace(
            "#endif /* NOTE_WIN32_H */",
            MARK + "\n\n#endif /* NOTE_WIN32_H */")

    added = []
    for name in sorted(names):
        if re.search(r"\b%s\s*\(" % re.escape(name), header):
            continue                      # already declared
        found = find_definition(name)
        if not found:
            print("  ? no definition found for %s" % name)
            continue

        path, i, j, sig = found
        lines = io.open(path, encoding="utf-8", newline="").read().split("\n")
        lines[i] = lines[i][len("static "):]
        io.open(path, "w", encoding="utf-8", newline="").write("\n".join(lines))

        proto = sig[len("static "):]
        proto = proto.split("{")[0].strip().rstrip()
        if not proto.endswith(")"):
            proto = proto.rstrip(",")
        added.append(proto + ";")
        print("  + %-28s from %s" % (name, os.path.basename(path)))

    if added:
        header = header.replace(MARK, MARK + "\n" + "\n".join(added))
        io.open(HDR, "w", encoding="utf-8", newline="").write(header)
    return len(added)


def main():
    text = io.open(sys.argv[1], encoding="utf-8", errors="replace").read()
    names = set(NAME_RE.findall(text)) | set(ALT_RE.findall(text))
    names = {n for n in names if not n.isupper()}      # skip macros and types
    print("%d distinct names reported" % len(names))
    n = publish(names)
    print("published %d" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())

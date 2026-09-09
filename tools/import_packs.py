#!/usr/bin/env python3
"""Convert open theme and syntax packs into note's own definition format.

Sources (both MIT licensed, whole-repository):

  themes  tinted-theming/schemes   base16 palettes  -> assets/themes/*.theme
  syntax  zyedidia/micro           runtime/syntax   -> assets/syntax/*.syntax

The theme conversion is faithful: base16 is a 16-colour palette with settled
meanings, and note's theme keys map onto it one for one.

The syntax conversion compiles micro's regex grammars down to note's format.
An alternation that is nothing but words becomes a word list -- a lookup is
cheaper than an automaton, and two hundred keywords cost a few hundred bytes
rather than a program too large to compile.  Everything else is kept as a
rule, a regular expression the lexer runs directly, with POSIX bracket
expressions rewritten as ordinary classes.

What is still dropped: patterns using constructs note's engine does not have
(lookaround, backreferences), nested rules inside a region, and embedded
sublanguages such as JavaScript inside HTML.

Usage:
    python tools/import_packs.py --schemes DIR --micro DIR [--out assets]
    python tools/import_packs.py --fetch          # sparse-clone into a tempdir
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

import yaml

# --------------------------------------------------------------------------
# Themes
# --------------------------------------------------------------------------

# base16 assigns each slot a settled role; these are the conventional
# mappings that terminal and editor ports have used for years.
THEME_KEYS = [
    ("background", "base00"),
    ("foreground", "base05"),
    ("gutter_bg",  "base00"),
    ("gutter_fg",  "base03"),
    ("selection",  "base02"),
    ("ui_bg",      "base01"),
    ("ui_fg",      "base04"),
    ("keyword",    "base0E"),
    ("type",       "base0A"),
    ("comment",    "base03"),
    ("string",     "base0B"),
    ("number",     "base09"),
    ("preproc",    "base0C"),
]


def convert_themes(src):
    docs = []
    names = set()

    for fn in sorted(os.listdir(src)):
        if not fn.endswith((".yaml", ".yml")):
            continue
        with open(os.path.join(src, fn), encoding="utf-8") as f:
            doc = yaml.safe_load(f)
        if not isinstance(doc, dict):
            continue

        pal = doc.get("palette") or {}
        if "base00" not in pal:
            continue

        name = str(doc.get("name") or os.path.splitext(fn)[0]).strip()
        # Names are the registry key, so they have to be unique.
        base, n = name, 2
        while name in names:
            name = "%s %d" % (base, n)
            n += 1
        names.add(name)

        dark = str(doc.get("variant", "dark")).lower() != "light"

        lines = ["# Converted from the base16 scheme %r." % fn,
                 "# Upstream: tinted-theming/schemes (MIT). See NOTICE.md.",
                 ""]
        lines.append("name = %s" % name)
        lines.append("dark = %s" % ("yes" if dark else "no"))
        if doc.get("author"):
            lines.append("# author = %s" % str(doc["author"]).replace("\n", " "))
        for key, slot in THEME_KEYS:
            colour = pal.get(slot)
            if colour:
                lines.append("%-11s= %s" % (key, str(colour).strip()))

        docs.append("\n".join(lines))

    return docs


# --------------------------------------------------------------------------
# Syntax
# --------------------------------------------------------------------------

# note_config.h caps a language at this many rules.
MAX_RULES = 24

PRETTY = {
    "c": "C", "c++": "C++", "cpp": "C++", "csharp": "C#", "objc": "Objective-C",
    "html": "HTML", "css": "CSS", "xml": "XML", "json": "JSON", "yaml": "YAML",
    "toml": "TOML", "ini": "INI", "sql": "SQL", "php": "PHP", "js": "JavaScript",
    "javascript": "JavaScript", "typescript": "TypeScript", "ts": "TypeScript",
    "go": "Go", "rust": "Rust", "python": "Python", "ruby": "Ruby",
    "java": "Java", "kotlin": "Kotlin", "swift": "Swift", "lua": "Lua",
    "perl": "Perl", "shell": "Shell", "bash": "Shell", "zsh": "Zsh",
    "make": "Makefile", "cmake": "CMake", "dockerfile": "Dockerfile",
    "markdown": "Markdown", "tex": "TeX", "haskell": "Haskell", "asm": "Assembly",
    "nasm": "Assembly (NASM)", "vhdl": "VHDL", "verilog": "Verilog",
    "powershell": "PowerShell", "batch": "Batch", "vim": "Vim script",
}

WORD = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# \b( a | b | c )\b  -- the shape micro uses for a keyword list
ALTERNATION = re.compile(r"\\b\(([^()]*)\)\\b")
# The same shape, but as the entire pattern: then it really is just a list.
WHOLE_LIST = re.compile(r"^\(?\\b\((.*)\)\\b\)?$")

# note's engine has no POSIX bracket expressions, so spell them out.  They
# appear inside a class, [[:space:]], so the replacement drops the brackets.
POSIX = {
    "[:space:]":  " \\t\\r\\n\\f\\v",
    "[:blank:]":  " \\t",
    "[:digit:]":  "0-9",
    "[:xdigit:]": "0-9A-Fa-f",
    "[:alpha:]":  "A-Za-z",
    "[:alnum:]":  "0-9A-Za-z",
    "[:upper:]":  "A-Z",
    "[:lower:]":  "a-z",
    "[:word:]":   "0-9A-Za-z_",
    "[:punct:]":  "!-/:-@\\[-`{-~",
    "[:cntrl:]":  "\\x00-\\x1f",
    "[:print:]":  " -~",
    "[:graph:]":  "!-~",
}

# Constructs the engine does not implement.  A pattern using one is dropped
# rather than converted into something that quietly means something else.
UNSUPPORTED = re.compile(r"\(\?(?!:)|\\[1-9]|\\p\{|\\P\{|\\[GKzZAQE]|\(\?#")


def portable_pattern(pat):
    """A micro regex rewritten for note's engine, or None if it cannot be."""
    for name, body in POSIX.items():
        pat = pat.replace(name, body)
    if "[:" in pat and ":]" in pat:
        return None                      # a class we do not know
    if UNSUPPORTED.search(pat):
        return None
    if "\n" in pat or "\r" in pat:
        return None                      # values are one line
    return pat.strip() or None


def whole_word_list(pattern):
    """Split a bare alternation into plain words and whatever is left.

    Returns (words, leftover_pattern) or None if the pattern is not of that
    shape at all.  Splitting matters: a list of two hundred shell builtins is
    a word list except for the one alternative that is "\\[", and keeping the
    whole thing as a regex would both exceed the engine's program size and
    cost far more to match than a lookup.
    """
    m = WHOLE_LIST.match(pattern.strip())
    if not m:
        return None

    # Split on the top-level bars only: an alternative may itself contain a
    # group, as in "foo(bar)?", and that is one alternative, not two.
    parts, depth, cur = [], 0, []
    for ch in m.group(1):
        if ch == "\\":
            cur.append(ch)
            continue
        if cur and cur[-1] == "\\":
            cur.append(ch)
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "|" and depth == 0:
            parts.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    parts.append("".join(cur))
    parts = [p for p in parts if p]
    if not parts:
        return None

    words = [p for p in parts if WORD.match(p)]
    rest = [p for p in parts if not WORD.match(p)]
    if not words:
        return None

    leftover = r"\b(%s)\b" % "|".join(rest) if rest else None
    return words, leftover


# micro's rule names, mapped onto the colours note has.
RULE_KIND = {
    "statement":       "keyword",
    "identifier":      "type",
    "type":            "type",
    "preproc":         "preproc",
    "symbol":          "operator",
    "constant":        "number",
    "special":         "operator",
}


# Characters that make a pattern more than a literal.  Braces are left out:
# they are only special in a {m,n} quantifier, and several languages really do
# open a comment with "{#" or "{-".
REGEX_META = set("^$|[]()*+?.")


def as_literal(pat):
    """A regex fragment reduced to the literal it denotes, or None.

    micro's delimiters are regexes, and most are plain text with a few
    backslash escapes -- "/\\*" is just "/*".  Some are not: a rule like
    "(^|s)#" or "^>.*" describes a position, not a delimiter, and note's lexer
    has no way to honour it.  Returning None for those keeps regex fragments
    from being written out as if they were literal text.
    """
    out, i = [], 0
    while i < len(pat):
        c = pat[i]
        if c == "\\":
            if i + 1 >= len(pat):
                return None                 # a trailing backslash means nothing
            out.append(pat[i + 1])
            i += 2
        elif c in REGEX_META:
            return None
        else:
            out.append(c)
            i += 1
    return "".join(out) or None


def words_from(pattern):
    """Pull a plain keyword list out of a regex, or return nothing."""
    found = []
    for group in ALTERNATION.findall(pattern):
        parts = group.split("|")
        if not parts:
            continue
        # Only accept a group that is entirely plain words: anything with
        # nested syntax means the pattern is doing more than listing words.
        if all(WORD.match(p) for p in parts if p):
            found.extend(p for p in parts if p)
    return found


def expand_pattern(tok, limit=64):
    """Expand a small regex like '(c|C)' or 'ii?' into literal strings."""
    results = [""]

    i = 0
    while i < len(tok):
        c = tok[i]

        if c == "(":
            depth, j = 1, i + 1
            while j < len(tok) and depth:
                if tok[j] == "(":
                    depth += 1
                elif tok[j] == ")":
                    depth -= 1
                j += 1
            if depth:
                return []
            inner = tok[i + 1:j - 1]
            alts = []
            for part in inner.split("|"):
                alts.extend(expand_pattern(part, limit))
            if not alts:
                return []
            optional = j < len(tok) and tok[j] == "?"
            if optional:
                alts = alts + [""]
                j += 1
            results = [a + b for a in results for b in alts]
            i = j

        elif c.isalnum():
            if i + 1 < len(tok) and tok[i + 1] == "?":
                results = [r + c for r in results] + list(results)
                i += 2
            else:
                results = [r + c for r in results]
                i += 1

        else:
            return []                       # anything else: give up cleanly

        if len(results) > limit:
            return []

    return results


EXT_IN_PATTERN = re.compile(r"\\\.([A-Za-z0-9_()|?]+)\$")


def extensions_from(detect, filetype):
    pattern = ""
    if isinstance(detect, dict):
        pattern = str(detect.get("filename", "") or "")

    exts = []
    for tok in EXT_IN_PATTERN.findall(pattern):
        for e in expand_pattern(tok):
            if e and e.lower() not in [x.lower() for x in exts]:
                exts.append(e)

    if not exts and WORD.match(filetype or ""):
        exts = [filetype]
    return exts


def convert_syntax(src):
    docs, skipped = [], []
    names = set()

    for fn in sorted(os.listdir(src)):
        if not fn.endswith((".yaml", ".yml")):
            continue
        with open(os.path.join(src, fn), encoding="utf-8") as f:
            try:
                doc = yaml.safe_load(f)
            except yaml.YAMLError:
                skipped.append((fn, "unparsable"))
                continue
        if not isinstance(doc, dict):
            continue

        filetype = str(doc.get("filetype") or os.path.splitext(fn)[0])
        rules = doc.get("rules") or []

        keywords, types, quotes = [], [], []
        # Named apart from `rules` above, which is micro's own list.
        pattern_rules = []
        line_comment = block = None
        preproc = False

        for rule in rules:
            if not isinstance(rule, dict):
                continue
            for key, val in rule.items():
                base = key.split(".")[0]

                if isinstance(val, str):
                    # A pattern that is nothing but a list of words becomes a
                    # word list: cheaper to match and far smaller than an NFA.
                    # Anything else -- operators, number formats, "an
                    # identifier in capitals" -- is kept as a rule, which is
                    # what the lexer's regex support is for.
                    split = whole_word_list(val)
                    if split:
                        words, leftover = split
                        if base == "type":
                            types.extend(words)
                        else:
                            keywords.extend(words)
                        if not leftover:
                            continue
                        val = leftover      # the alternatives that are not words

                    if base == "preproc":
                        preproc = True

                    kind = RULE_KIND.get(base)
                    if not kind:
                        continue
                    if key.endswith(".bool"):
                        kind = "keyword"
                    pat = portable_pattern(val)
                    if pat and len(pattern_rules) < MAX_RULES:
                        pattern_rules.append((kind, pat))

                elif isinstance(val, dict):
                    raw_end = str(val.get("end", ""))
                    start = as_literal(str(val.get("start", "")))
                    end = as_literal(raw_end)
                    if base == "comment":
                        # "$" as the end means the comment runs to the line
                        # break, which is what a line comment is.
                        if raw_end == "$" and start and not line_comment:
                            line_comment = start
                        elif start and end and not block:
                            block = (start, end)
                    elif base == "constant" and key.endswith(".string"):
                        if start and len(start) == 1 and start not in quotes:
                            quotes.append(start)

        exts = extensions_from(doc.get("detect"), filetype)
        if not exts:
            skipped.append((fn, "no extensions"))
            continue
        if not (keywords or types or line_comment or block or quotes
                or pattern_rules):
            skipped.append((fn, "nothing note can express"))
            continue

        # Preserve first-seen order but drop duplicates.
        def uniq(seq):
            seen, res = set(), []
            for x in seq:
                if x not in seen:
                    seen.add(x)
                    res.append(x)
            return res

        keywords, types = uniq(keywords), uniq(types)

        name = PRETTY.get(filetype.lower(), filetype.replace("_", " ").title())
        base_name, n = name, 2
        while name in names:
            name = "%s %d" % (base_name, n)
            n += 1
        names.add(name)

        lines = ["# Converted from micro's %r syntax file." % fn,
                 "# Upstream: zyedidia/micro (MIT). See NOTICE.md.",
                 "# Lossy on purpose: note's lexer keeps word lists and",
                 "# delimiters, not micro's regular expressions.",
                 ""]
        lines.append("name = %s" % name)
        lines.append("extensions = %s" % " ".join(exts))
        if line_comment:
            lines.append("line_comment = %s" % line_comment)
        if block:
            lines.append("block_comment = %s %s" % block)
        if quotes:
            lines.append("quotes = %s" % "".join(quotes))
        if preproc:
            lines.append("preproc = yes")
        if keywords:
            lines.append("keywords = %s" % " ".join(keywords))
        if types:
            lines.append("types = %s" % " ".join(types))
        for kind, pat in pattern_rules:
            lines.append("rule = %s %s" % (kind, pat))

        docs.append((name, exts, lines))

    resolved, lost = resolve_conflicts(docs)
    for name, exts in lost:
        skipped.append((name, "every extension went to another language: "
                              + " ".join(exts)))
    return resolved, skipped


# --------------------------------------------------------------------------

# When several languages claim an extension, note's registry lets the last one
# loaded win, which would otherwise come down to alphabetical order of the
# source files -- and hand every .h file to Objective-C.  These say who keeps
# it; the extension is removed from the others.
PREFER = {
    "h":      "C",
    "def":    "C",
    "i":      "C",
    "ii":     "C",
    "hh":     "C++",
    "m":      "Objective-C",
    "v":      "Verilog",
    "fs":     "Fsharp",
    "conf":   "INI",
    "ebuild": "Shell",
}


def resolve_conflicts(docs):
    """Strip an extension from every language but its preferred owner."""
    claims = {}
    for name, exts, _ in docs:
        for e in exts:
            claims.setdefault(e.lower(), []).append(name)

    resolved, lost = [], []
    for name, exts, lines in docs:
        keep = []
        for e in exts:
            owners = claims[e.lower()]
            if len(owners) == 1:
                keep.append(e)
                continue
            # An explicit preference, else the first by name so the result is
            # at least deterministic rather than depending on file order.
            winner = PREFER.get(e.lower(), sorted(owners)[0])
            if winner == name:
                keep.append(e)
        if not keep:
            lost.append((name, exts))     # everything it claimed went elsewhere
            continue
        out = []
        for line in lines:
            if line.startswith("extensions = "):
                out.append("extensions = %s" % " ".join(keep))
            else:
                out.append(line)
        resolved.append("\n".join(out))
    return resolved, lost


NOTICE = """# Third-party definitions

note ships converted copies of two open packs.  Both are MIT licensed across
the whole repository, so every file below carries that licence.

## Themes -- `assets/themes/*.theme`

Converted from the base16 schemes collected by **tinted-theming/schemes**.
<https://github.com/tinted-theming/schemes> -- MIT.
Individual palettes name their own authors in a comment at the top of each
converted file.

## Syntax -- `assets/syntax/*.syntax`

Converted from the syntax files of **micro**, by Zachary Yedidia and
contributors.  <https://github.com/zyedidia/micro> -- MIT.

The conversion is lossy by design.  micro describes languages with regular
expressions; note's lexer has no regex engine and works from keyword lists and
delimiters, so the converter keeps the word lists, comment and string
delimiters and the preprocessor flag, and drops operator patterns, number
formats, nested rules and embedded sublanguages.

Regenerate both with:

    python tools/import_packs.py --fetch
"""


# The set compiled into the executable, so that note.exe on its own is a
# complete editor.  Everything else stays in the full packs beside it, which
# override these by name when present.  Chosen by what a Notepad replacement
# is actually pointed at, not by what exists.
CORE_LANGS = [
    "C", "C++", "C#", "Python", "JavaScript", "TypeScript", "Java", "Go",
    "Rust", "PHP", "Ruby", "Lua", "Perl", "Shell", "PowerShell", "Batch",
    "SQL", "HTML", "XML", "CSS", "JSON", "YAML", "TOML", "INI", "Markdown",
    "Makefile", "CMake", "Dockerfile", "Assembly",
]

# Both a light and a dark of each family, so switching theme is a real choice
# without the executable carrying three hundred palettes.
CORE_THEMES = [
    "Default Dark", "Default Light",
    "Github", "Github Dark",
    "Gruvbox dark, medium", "Gruvbox light, medium",
    "Solarized Dark", "Solarized Light",
    "Nord", "Nord Light",
    "Dracula", "Monokai", "OneDark", "Tomorrow Night",
]


def pick(docs, wanted):
    """The named documents, in the order `wanted` gives, plus what was missed."""
    by_name = {}
    for doc in docs:
        for line in doc.split("\n"):
            if line.startswith("name = "):
                by_name[line[7:].strip()] = doc
                break
    kept = [by_name[n] for n in wanted if n in by_name]
    missing = [n for n in wanted if n not in by_name]
    return kept, missing


def write_pack(path, docs):
    """A pack is just our definition format, several documents to a file,
    separated by a line of three dashes."""
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("# Generated by tools/import_packs.py -- do not edit.\n")
        f.write("# To change one of these, copy it into a .syntax or .theme\n")
        f.write("# file beside this pack; a definition loaded later wins.\n")
        for doc in docs:
            f.write("---\n")
            f.write(doc)
            f.write("\n")


def arena_chars(docs):
    """How much of note's string arena these documents will occupy: every
    value the core keeps as a string, plus a terminator each."""
    total = 0
    keep = ("name", "extensions", "keywords", "types",
            "line_comment", "block_comment", "quotes")
    for doc in docs:
        for line in doc.split("\n"):
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            key, val = line.split("=", 1)
            if key.strip() in keep:
                total += len(val.strip()) + 1
    return total


def fetch(dest):
    """Sparse-clone just the directories we read, not the whole histories."""
    repos = [
        ("https://github.com/tinted-theming/schemes.git", "schemes", "base16"),
        ("https://github.com/zyedidia/micro.git", "micro", "runtime/syntax"),
    ]
    for url, name, sub in repos:
        path = os.path.join(dest, name)
        if os.path.isdir(path):
            continue
        subprocess.check_call(
            ["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
             "-q", url, path])
        subprocess.check_call(["git", "sparse-checkout", "set", sub], cwd=path)
    return (os.path.join(dest, "schemes", "base16"),
            os.path.join(dest, "micro", "runtime", "syntax"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schemes", help="path to tinted-theming/schemes/base16")
    ap.add_argument("--micro", help="path to micro/runtime/syntax")
    ap.add_argument("--out", default="assets", help="output directory")
    ap.add_argument("--fetch", action="store_true",
                    help="clone the sources into a temporary directory first")
    args = ap.parse_args()

    tmp = None
    if args.fetch:
        tmp = tempfile.mkdtemp(prefix="note-packs-")
        args.schemes, args.micro = fetch(tmp)

    if not args.schemes or not args.micro:
        ap.error("give --schemes and --micro, or --fetch")

    try:
        os.makedirs(args.out, exist_ok=True)

        themes = convert_themes(args.schemes)
        langs, skipped = convert_syntax(args.micro)

        # One file each rather than several hundred: note reads these at every
        # launch, and hundreds of file opens is a visible delay for an editor
        # that is supposed to start instantly.  Individual .syntax/.theme files
        # dropped beside them still load afterwards and override by name.
        write_pack(os.path.join(args.out, "syntax.pack"), langs)
        write_pack(os.path.join(args.out, "themes.pack"), themes)

        # The curated subset that gets compiled into the executable.
        core_langs, miss_l = pick(langs, CORE_LANGS)
        core_themes, miss_t = pick(themes, CORE_THEMES)
        write_pack(os.path.join(args.out, "core.syntax.pack"), core_langs)
        write_pack(os.path.join(args.out, "core.themes.pack"), core_themes)
        print("core:      %d languages, %d themes" % (len(core_langs), len(core_themes)))
        for n in miss_l + miss_t:
            print("    core wanted %r but the pack has no such name" % n)

        with open(os.path.join(args.out, "NOTICE.md"), "w",
                  encoding="utf-8", newline="\r\n") as f:
            f.write(NOTICE)

        print("themes:    %d" % len(themes))
        print("languages: %d  (skipped %d)" % (len(langs), len(skipped)))
        print("arena:     %d chars needed (NOTE_ARENA_CHARS)"
              % (arena_chars(langs) + arena_chars(themes)))
        for fn, why in skipped:
            print("    skipped %-28s %s" % (fn, why))
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())

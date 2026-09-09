# Third-party definitions

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

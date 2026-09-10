# note/gb — a text editor and BASIC for the Game Boy

A design note, written before the code, about the two things that decide whether
this is usable at all: getting text in through eight buttons, and getting more
than twenty characters onto a 160×144 screen.

Everything below is for the DMG. Colour is a later concern; if it works in four
greys it works everywhere.

## The hardware, as it constrains this

| | |
|---|---|
| screen | 160×144 px = 20×18 tiles of 8×8 |
| VRAM | 384 tiles, but the background can address only **256 at a time** |
| WRAM | 8 KB, shared with the stack and everything GBDK needs |
| save | SRAM on the cartridge, 8 KB with a battery |
| input | 8 buttons: 4 directions, A, B, Start, Select |

The 256-tile limit is the one that shapes the screen, and it is worth being
precise about. LCDC bit 4 selects which half of VRAM the background reads tiles
from: `$8000` unsigned gives tiles 0–255, `$8800` signed gives −128…127. Both
windows are 256 tiles wide, and only one is live at a time.

## 40 columns, not 20

An 8×8 font gives 20 columns. That is not an editor, it is a receipt printer.

The way out is to stop treating tiles as a font. Instead of a tilemap that
points 360 cells at a shared alphabet, point each cell at **a tile of its own**
and rewrite the tile's pixels as the text changes. The tilemap then never
changes: cell *(r, c)* always shows tile *r × 20 + c*.

With a 4-pixel-wide font, one 8×8 tile holds two characters side by side, so
20 tile columns become **40 text columns**. Drawing one character is a
read-modify-write of one nibble column in eight tile rows — and since both
characters of a cell are known from the text buffer, nothing has to be read back
from VRAM at all:

```
tile_row[i] = (glyph_left[i] & 0xF0) | (glyph_right[i] >> 4)
```

The cost is one tile per cell, and tiles are the scarce resource:

```
20 × rows_of_text  +  shared_glyphs  ≤  256
```

| text rows | unique tiles | left for shared glyphs |
|---|---|---|
| 9 | 180 | 76 |
| **10** | **200** | **56** |
| 11 | 220 | 36 |
| 12 | 240 | 16 |

Ten rows is the sweet spot: **40 × 10 = 400 characters visible**, twenty times
what a naive 8×8 layout shows, with 56 tiles left over — enough for an on-screen
keyboard drawn in a normal shared 8×8 alphabet, where legibility matters more
than density.

```
 row  0 ┌────────────────────────────────────────┐
        │ 10 REM NOTE/GB                         │  text, 4x8 font
        │ 20 FOR I=1 TO 10                       │  40 columns
     …  │ 30 PRINT I*I                           │  200 unique tiles
        │ 40 NEXT I                              │
 row  9 ├────────────────────────────────────────┤
 row 10 │ L4 C7  *  PRINT?                       │  status + suggestion
 row 11 ├────────────────────────────────────────┤
 row 12 │  A B C D E F G H I J                   │  on-screen keyboard
     …  │  K L M N O P Q R S T                   │  8x8 shared glyphs
 row 17 │  U V W X Y Z 0 1 2 3 …                 │
```

**Later, for twelve rows and a keyboard with its own tileset:** flip LCDC bit 4
mid-frame from a STAT interrupt at the scanline where the keyboard starts. The
text area then reads `$8000` (256 unique cells) and the keyboard reads `$8800`
signed indices 0…127, which land at `$9000-$97FF` and overlap nothing. It is a
known trick and it doubles the tile budget; it is also the kind of timing code
that fails on one revision of hardware and not another, so it is not where this
starts.

## Eight buttons

Two problems, not one: choosing a character, and moving around the text. Most
handheld on-screen keyboards solve the first and forget the second, which is why
editing anything on a console feels like punishment.

The split here is **the D-pad drives the keyboard; B held turns the D-pad into
the text caret**. Nothing is modal, nothing has to be toggled, and the most
frequent two actions — pick a letter, move the caret — never contend.

| | |
|---|---|
| D-pad | move the keyboard selection; wraps at every edge |
| A | insert what is selected; hold to repeat |
| B tap | Backspace |
| B hold + D-pad | move the caret: ←→ by character, ↑↓ by line |
| B hold + A | accept the suggestion |
| Select tap | next keyboard layer |
| Select hold | delete to end of line |
| Start | menu: Run, New, Load, Save |

**Repeat with acceleration**, on both the selection and the caret: 16 frames
before the first repeat, then every 6, then every 2 after half a second. Holding
Right should cross the keyboard in under a second and cross a line of text about
as fast — without acceleration, an on-screen keyboard is unusable and everyone
who has shipped one has learned this the same way.

**Three layers**, cycled by Select, because a flat grid of 64 symbols is four
D-pad presses deep in every direction:

1. **letters** — A–Z, 0–9, space
2. **symbols** — `= + - * / ( ) < > , ; : " . $ #` and the rest
3. **keywords** — whole BASIC words

The third layer is the interesting one, and it is not novel: it is what
Sinclair did in 1980. **A keyword is one selection, not five.** `PRINT` costs one
press on the keyword layer instead of five characters hunted across a grid, and
it lands in the buffer as a single token byte. The thing that made the ZX81
bearable is exactly the thing that makes this bearable, for exactly the same
reason.

**Frequency, not alphabet.** The letter layer is laid out by how often letters
appear in BASIC source rather than A-to-Z, with the common ones clustered at the
resting position of the selection. Alphabetical order is a convention for
*finding* letters when you already know the alphabet; on a grid you navigate by
distance, and distance should be paid where it is cheapest.

**Prediction, sized to the machine.** T9 over English needs a dictionary the
cartridge would rather spend on the interpreter. But the useful vocabulary here
is not English — it is about forty BASIC keywords plus the variable names
already used in the program. That table is a few hundred bytes, it is exact
rather than probabilistic, and after one or two letters there is usually one
candidate. It shows in the status row; `B + A` accepts it. Pressing Up from the
top keyboard row lands on it too, so it is also reachable without a chord.

## The BASIC

**Integer only**, 16-bit signed. Floating point on a Z80-alike costs kilobytes of
ROM and milliseconds per operation, and buys nothing for a program that draws
tiles and counts loops. `PRINT 7/2` gives `3`, and the manual says so.

**Line-numbered**, stored **tokenised**: keywords are single bytes ≥ 128. Both
choices fall out of the constraints rather than nostalgia — tokens make the
keyword layer a one-byte insert, make the program a third smaller, and make the
interpreter a jump table instead of a string comparison. Line numbers avoid
block structure, which avoids an indenting editor, on a screen where indentation
costs columns that do not exist.

The subset:

```
LET  PRINT  INPUT  IF…THEN  GOTO  GOSUB  RETURN
FOR…TO…STEP  NEXT  REM  END  CLS  PAUSE
```

Variables: `A`–`Z` integer, `A$`–`Z$` string, one `DIM A(n)` array.
Operators: `+ - * / MOD`, comparisons, `AND OR NOT`.

## What fits

| | |
|---|---|
| program text, tokenised | ~3 KB |
| variables and strings | ~1 KB |
| interpreter and editor state | ~1 KB |
| GBDK, stack, screen bookkeeping | the rest of the 8 KB |

Three kilobytes of tokens is roughly **6–8 KB of typed source**, call it 250–400
lines of BASIC — more than anyone will type on a D-pad in one sitting, which is
the right place for that limit to sit. SRAM holds one program of that size with
room for a couple of save slots; MBC5 with a battery, so it survives the power
switch.

## What the screen turned out to be

The note above was written before the code. Two things moved.

**Eight rows, not ten.** Eight divides 32, the height of the hardware tilemap,
so the map can be filled once with a repeating pattern of the eight buffer rows
and scrolling becomes a single write to SCY -- no redraw, no tearing, and only
the row coming into view is ever painted. Ten rows would have bought two more
lines of text and cost a scrolling redraw of the whole screen. 160 tiles for the
text area leaves 96, which is where everything below is paid for.

**Three greys are a highlighter.** A cell's tile is composed from the buffer on
every redraw anyway, and `set_bkg_data` moves a whole tile whether one bitplane
is written or two. So writing both costs nothing and buys the DMG's three inks:

| shade | what it is |
|---|---|
| light grey | comments and the line number -- what you skim past |
| dark grey | names, operators, numbers |
| black | keywords and quoted text -- what you look for |

Not the core's `note_syntax`: that wants a language registry, a span list and a
token enum, which is a kilobyte of ROM to say that REM starts a comment. BASIC
is lexed a line at a time, in place, into a shade per column.

**The cursor is the cell, inverted.** A bar between two characters is a
mouse-and-proportional-font idea; on a fixed grid the honest cursor is the cell
itself turned inside out. It is the same tile write the cell was going to get,
so the sprite the caret used to be is gone -- and with it the question of what
happens when it lands on a character.

**Reverse video is the interface's one idea.** The same inversion draws the
status band at the top of the keyboard panel and the cap under the selected key.
That is what separates the document from the machinery: before it, loose letters
sat under loose letters and the eye had nothing to tell it where one stopped.

A reversed glyph is a different tile from an upright one, and a second alphabet
is 53 tiles for a bar that shows twenty characters. So the bands and the cap own
tiles that are rewritten as their content changes -- the same trick the text area
is built on -- and the status band only rewrites the columns whose character
actually changed, because twenty tile writes will not fit in a VBlank and one or
two always will.

```
tiles  0..159   text area, one per cell
     160..212   the shared alphabet: 26 letters, 10 digits, 17 symbols
          213   blank
     214..233   the status band, one per column
     234..241   the cap under the selected key
     242..254   the legend on the bottom band
          255   a reversed space: the bands' own fill
```

Which leaves nothing spare, and no sprites turned on.

## Order of work

1. The 4×8 unique-tile screen. Everything else is unreadable without it.
2. Text buffer, caret, insert and delete. `note_buffer.c` from the core already
   does this without allocating and without assuming `int` is wider than 16 bits,
   which is exactly the machine this was written against.
3. The keyboard: layers, wraparound, accelerating repeat.
4. Tokeniser and the keyword layer.
5. The interpreter.
6. SRAM save and load.

Steps 1–3 are the prototype: if typing a line of BASIC does not feel decent, the
interpreter behind it does not matter.

# Exact Minesweeper chord-set optimizer

`minesweeper_chord_dp.py` finds a shortest clearing sequence for a completely
known Minesweeper board under the usual click-count model:

- placing a flag costs one click;
- an ordinary reveal costs one click;
- a chord costs one click;
- flags can be placed before their neighboring numbers are opened.

It implements a frontier dynamic program over possible chord sets. This is an
exact exponential algorithm parameterized by the frontier width, not a
polynomial-time solution for arbitrary boards.

## Input and basic use

The input format is detected automatically. You can supply any of the
following.

### Text grid

Create a rectangular text file using `*` for a mine and `.` for a safe square:

```text
*...
....
..*.
....
```

`M` is also accepted as a mine. Digits `0` through `8` are accepted as safe
squares, but are ignored and recalculated from the mine positions.

Run:

```console
python minesweeper_chord_dp.py board.txt
```

### LlamaSweeper board-editor URL

Pass the complete URL in quotes. Quoting is especially important in Windows
Command Prompt because the URL contains `&`:

```console
python minesweeper_chord_dp.py "https://llamasweeper.com/#/game/board-editor?b=2&m=0000000000000000220c8014k0pg0i80h30cc01140oo0ii0hh00"
```

The parser supports standard board codes `b=1`, `b=2`, and `b=3`, as well as
custom dimensions. The `m=` value is a row-major bitfield packed five cells per
character using the alphabet `0-9a-v`. Any final unused bits must be zero.

You can also pass only the parameter portion:

```console
python minesweeper_chord_dp.py "b=43&m=g20"
```

### MBF hexadecimal or file

Pass hexadecimal in quotes:

```console
python minesweeper_chord_dp.py "1e 10 00 08 10 03 0a 05 15 06 11 08 0a 09 19 0a 15 0c 1d 0e"
```

Binary `.mbf` files are also accepted directly:

```console
python minesweeper_chord_dp.py example.mbf
```

The MBF header is width, height, and a two-byte big-endian mine count. It is
followed by one `(x, y)` byte pair per mine.

If automatic detection is ever inconvenient, use `--format grid`,
`--format llamasweeper`, or `--format mbf`.

The output gives the optimal count, its breakdown, DP statistics, and a valid
action sequence. Coordinates are 1-based. Use `--zero-based` to change that.

Useful options:

```console
python minesweeper_chord_dp.py board.txt --json
python minesweeper_chord_dp.py board.txt --verify
python minesweeper_chord_dp.py board.txt --order rows
python minesweeper_chord_dp.py board.txt --order columns
python minesweeper_chord_dp.py board.txt --max-states 5000000
python minesweeper_chord_dp.py board.txt --method bruteforce
```

`--verify` compares the DP answer with exhaustive enumeration when the board
has at most 25 possible chord squares. This is useful while modifying the
solver.

## Objective optimized by the DP

Let `S` be the set of numbered squares that will be chorded. Its cost is

```text
|S|
+ number of distinct mines adjacent to S
+ number of chord-propagation components in S
+ number of 3BV units not opened by S.
```

The four terms are chord clicks, flag clicks, initial left clicks, and remaining
ordinary left clicks.

Two selected chord squares belong to the same propagation component when one
can eventually reveal the other. This includes:

1. directly adjacent chord squares; and
2. chord squares on the boundary of the same connected zero region.

The second case matters because chording either square opens a zero, whose
flood fill reveals the other square. One seed click therefore suffices for the
whole connected component.

The 3BV units are the standard ones: one per connected zero region, plus one
for each nonzero square not adjacent to a zero. A zero-region unit is opened by
any chord touching one of its zeroes. An isolated-number unit is opened by a
chord on that square or an adjacent square.

## Frontier state

Candidates are processed in row or column order. At each cut the state stores:

- which processed chord candidates still touch an unprocessed candidate;
- a canonical partition saying which of those boundary candidates are already
  connected through selected chords in the processed region; and
- one bit for every mine/3BV OR-factor whose candidate scope crosses the cut,
  recording whether a selected chord has already hit it.

When the last boundary vertex of a selected component disappears, the DP adds
one seed click. When the last variable of a factor is processed, it adds either
the required flag cost or the uncovered-3BV cost. Equivalent boundary states
are merged, retaining only the cheapest partial chord set.

The implementation tries both row and column layouts cheaply and chooses the
one with the smaller estimated frontier. Runtime is still exponential in the
frontier size, so unusually wide or highly connected boards can exceed the
state limit. In that case, try the other order or raise `--max-states` if memory
allows.

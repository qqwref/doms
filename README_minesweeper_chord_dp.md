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

To print only the executable clicks as `(click_type, x, y)` tuples, use:

```console
python minesweeper_chord_dp.py board.txt --click-tuples
```

The click types are `left`, `right`, and `chord`; `right` places a flag. The
same list is included as the `clicks` field in `--json` output. In JSON, each
tuple is naturally represented as an array.

Useful options:

```console
python minesweeper_chord_dp.py board.txt --json
python minesweeper_chord_dp.py board.txt --click-tuples
python minesweeper_chord_dp.py board.txt --verify
python minesweeper_chord_dp.py board.txt --order rows
python minesweeper_chord_dp.py board.txt --order columns
python minesweeper_chord_dp.py board.txt --band-size 2
python minesweeper_chord_dp.py board.txt --progress
python minesweeper_chord_dp.py board.txt --progress --progress-every 5
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

Candidates are processed in a spatial sweep. At each cut the state stores:

- which processed chord candidates still touch an unprocessed candidate, plus
  one connector for each zero opening that crosses the boundary;
- a canonical partition saying which of those boundary candidates are already
  connected through selected chords in the processed region; and
- one bit for every mine/3BV OR-factor whose candidate scope crosses the cut,
  recording whether a selected chord has already hit it.

The zero connector is important: it represents the propagation hyperedge
without materializing a clique between every pair of chord squares around a
large opening. When the last boundary item of a selected component disappears, the DP adds
one seed click. When the last variable of a factor is processed, it adds either
the required flag cost or the uncovered-3BV cost. Equivalent boundary states
are merged, retaining only the cheapest partial chord set.

All 3BV units, direct chord adjacency, zero-opening membership, and mine/3BV
factor membership are precomputed. Hot transitions use integer bitsets and
`bit_count()` rather than allocating factor objects or dictionaries. A cached
connectivity transition is shared by all states having the same boundary
partition.

The DP also performs exact Pareto pruning. For equal connectivity, a state can
be discarded when another state has a superset of future-useful hits at a
provably sufficient cost advantage. In particular, flags that have already
been paid for are useful resources for later chords. Covered 3BV units require
an additional potential-cost check because their saving has already been
credited. `--dominance-comparisons 0` disables this pruning; the default caps
its work per layer so pruning itself cannot grow without bound.

The implementation estimates both row and column layouts. It also tests wider
bands when the width estimate shows a clear improvement; `--band-size N`
forces both orientations to be compared at a requested band size. A band of
one eagerly merges states after each tile and is normally fastest, while some
boards benefit dramatically from a wider band. `--progress` prints the swept
tile count, processed chord candidates, valid boundary states, live boundary
size, and cumulative dominance pruning to standard error.

Runtime remains exponential in frontier size, so unusually wide or highly
connected boards can exceed the state limit. In that case, try a specific
`--order`, experiment with `--band-size`, or raise `--max-states` if memory
allows.

More precisely, let `N = n*m`, let `q <= N-k` be the number of possible chord
squares, `b` the maximum number of live chord/zero-connector items at a cut,
and `f` the maximum number of live mine/3BV factors. If `B(i)` is the `i`th Bell
number, the number of retained states is bounded by

```text
S <= min(2^q, B(b + 1) * 2^f).
```

Ignoring the explicitly capped dominance checks, the optimized implementation
takes approximately `O(q*S*b)` time and `O(S*(b+q))` memory; cached connectivity
often makes the observed transition cost much smaller. Precomputation is
polynomial (`O(N + qF + q(q+z))` in this implementation, where `F` is the total
factor count and `z` the number of zero regions).

On a narrow grid with local interactions, `b,f = O(min(n,m))`, yielding the
usual frontier-DP form
`N * 2^O(min(n,m) log(min(n,m)))`. The strict worst case is still exponential:
since `q <= N-k`, a simple board-size bound is
`O(poly(N) * 2^(N-k))` time and exponential memory. The default two-million
state limit stops the solver before that worst case consumes unbounded memory.

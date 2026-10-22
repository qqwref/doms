# Deterministically Optimal Minesweeper Solver (DOMS)

DOMS is based on an idea by **qqwref** and was implemented and optimized by
**OpenAI Codex**.

The standalone C++17 implementation in `minesweeper_chord_dp.cpp` finds a
shortest sequence of clicks that clears a
Minesweeper board when every mine position is known in advance.

The program optimizes the complete click sequence, including:

- right-clicks used to flag mines;
- left-clicks used to open the first square of a chord component;
- chord clicks; and
- ordinary left-clicks needed for 3BV openings not reached by a chord.

By default, a single-board solve outputs both the optimal number of clicks and
a valid sequence of actions that achieves it. `--count-only` omits the replay
and is the default for bulk runs. Both modes are exact.

The solver is a standalone C++17 program with no third-party libraries.
Importing a World of Minesweeper game URL requires an installed Chrome,
Edge, or Chromium browser. PTTACG strings, LlamaSweeper URLs, MBF input, and
random generation work offline without a browser. No Python script is needed
to compile or run the current C++ solver, including its game-URL importer.

## Building

### GCC or Clang

```console
g++ -std=c++17 -O3 -DNDEBUG -pthread minesweeper_chord_dp.cpp -o doms
```

For the machine on which the binary is compiled and run, native CPU tuning may
improve performance:

```console
g++ -std=c++17 -O3 -DNDEBUG -march=native -pthread minesweeper_chord_dp.cpp -o doms
```

With Clang, replace `g++` with `clang++`.

### Windows with MinGW-w64

Use a MinGW-w64 GCC toolchain, such as the MSYS2 UCRT64 environment:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
g++ -std=c++17 -O3 -DNDEBUG -pthread .\minesweeper_chord_dp.cpp -o .\doms.exe
```

Run these commands from the repository root. The PATH setting selects MSYS2's
UCRT64 compiler instead of another installed `g++`.

This Windows command uses a portable x86-64 instruction target. If the program
crashes with a CPU-specific build, first rebuild without `-march=native` and
run the same board again. `-mtune=native` can tune scheduling without enabling
new instructions, but it is optional. If the crash persists, record the
compiler name/version, the exact command and board, and the Windows exception
code (Event Viewer → Windows Logs → Application). An illegal-instruction
exception points toward an instruction/CPU mismatch; an access violation
points toward memory access and needs a debugger backtrace to locate it.
Windows may close a crashing console program without printing an exception;
`echo %ERRORLEVEL%` immediately after the command or the Application event
can reveal the exit code. With MinGW GDB, `gdb --args doms.exe BOARD` followed
by `run` and `bt` at the GDB prompt captures the faulting call stack.

The source currently uses GCC/Clang bit-count intrinsics; the Microsoft
`cl` compiler is not supported as written. The resulting executable does not
require Python.

## Quick start

Pass a board as the first argument. For example, to solve a PTTACG-encoded board
from a LlamaSweeper URL and print the exact clicks as `(click_type, x, y)`
tuples:

```console
./doms "https://llamasweeper.com/#/game/board-editor?b=2&m=000g14s0010421g080414h4540a0g8880kg200800080o5400c00" --click-tuples
```

On Windows:

```console
doms.exe "https://llamasweeper.com/#/game/board-editor?b=2&m=000g14s0010421g080414h4540a0g8880kg200800080o5400c00" --click-tuples
```

Always quote LlamaSweeper URLs. In particular, shells treat the `&` in the URL
as a special character when it is not quoted.

For ordinary human-readable output:

```console
./doms example.mbf
```

For machine-readable output:

```console
./doms example.mbf --json > solution.json
```

For single-board solves, the solution is written to standard output. Timing,
progress, and generated board information are written to standard error, so
they do not corrupt JSON or click-tuple output.

## Input formats

The program automatically recognizes PTTACG strings, compatible LlamaSweeper
URLs, World of Minesweeper game URLs, and MBF input. Text-grid input is not
supported.

### PTTACG string or compatible LlamaSweeper URL

Complete board-editor URLs and bare `b=...&m=...` parameter strings are
accepted:

```console
./doms "https://llamasweeper.com/#/game/board-editor?b=2&m=0000000000000000220c8014k0pg0i80h30cc01140oo0ii0hh00"
```

This board-string format was created by
[PTTACGfans](https://github.com/PTTACGfans) for the
[Minesweeper ZiNi Calculator](https://pttacgfans.github.io/Minesweeper-ZiNi-Calculator/)
([source code](https://github.com/PTTACGfans/Minesweeper-ZiNi-Calculator)).
LlamaSweeper supports the format but did not originate it. This README refers
to these encodings as **PTTACG strings**.

The standard board codes are:

| Code | Dimensions |
| --- | --- |
| `b=1` | 9 x 9 |
| `b=2` | 16 x 16 |
| `b=3` | 30 x 16 |

Unambiguous custom-dimension codes are also supported, such as `b=3020` for
30 x 20 and `b=3030` for 30 x 30. A leading `?` is optional. The `m=` value is
the row-major mine bitfield, packed five cells per character with the alphabet
`0-9a-v`.

### World of Minesweeper game URL

For a **completed, won** game, DOMS can read the rendered board from
Minesweeper Online (formerly World of Minesweeper):

```console
./doms "https://minesweeper.online/game/2899477113" --count-only
```

On Windows, use `doms.exe` instead of `./doms`. DOMS launches an installed
Chrome, Edge, or Chromium browser in headless mode and reads the rendered cells
from its DOM. It looks for a browser in common installation locations and on
`PATH`. If yours is elsewhere, set `DOMS_BROWSER` to the executable path:

```bat
set "DOMS_BROWSER=C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
doms.exe "https://minesweeper.online/game/2899477113" --count-only
```

DOMS prints `Imported board:` followed by a compatible LlamaSweeper
board-editor URL to standard error before solving. With `--json`, the same URL
is also in `imported_board_url`. This import needs internet access and works
for completed, won games: once every safe cell is open, every closed cell is a
mine. It cannot infer all mines from an unfinished or lost game's visible
cells. The game URL contains an ID, not the board data.

If no suitable browser is found, this input produces a descriptive error and
exits with status `2`. Browser detection runs only for World of Minesweeper
game URLs; it is not needed for `.mbf` files or LlamaSweeper URLs. The importer
depends on the site's rendered cell markup and can fail if the game does not
load or that markup changes.

### MBF file or hexadecimal

Binary `.mbf` files can be passed directly:

```console
./doms example.mbf
```

MBF data can also be supplied as quoted hexadecimal bytes:

```console
./doms "1e 10 00 08 10 03 0a 05 15 06 11 08 0a 09 19 0a 15 0c 1d 0e"
```

The four-byte MBF header contains the width, height, and a two-byte big-endian
mine count. It is followed by one `(x, y)` byte pair per mine.

### Random standard board

The program can generate and immediately solve a standard Beginner,
Intermediate, or Expert board:

```console
./doms --generate beginner
./doms --generate intermediate
./doms --generate expert --progress
```

Beginner boards are 9 x 9 with 10 mines. Intermediate boards are 16 x 16 with
40 mines. Expert boards are 30 x 16 with 99 mines. The generated PTTACG string,
embedded in a compatible LlamaSweeper board-editor URL, and the random seed are
printed before the solve.

Use `--seed` for a reproducible board:

```console
./doms --generate expert --seed 8675309
```

Seeded generation is reproducible across C++ standard-library implementations.
The printed PTTACG board URL is the most portable way to preserve a generated
board.

### Bulk CSV generation

Use `--bulk` with a difficulty or custom size and a board count to generate and
optimally solve many boards:

```console
./doms --bulk beginner 100 > beginner-results.csv
./doms --bulk intermediate 25 > intermediate-results.csv
./doms --bulk expert 10 > expert-results.csv
./doms --bulk evil 10 > evil-results.csv
./doms --bulk 35x35/250 10 > custom-results.csv
```

`evil` means 30 columns by 20 rows with 130 mines, equivalent to `30x20/130`.
Custom sizes use `NxM/K`: **width first, then height**, matching PTTACG, followed
by the mine count. Dimensions must be positive integers; the mine count can
range from zero to the number of cells. As with other boards, the dimensions
must be representable unambiguously in PTTACG. Both `x` and `X` are accepted.

The CSV contains these columns:

```csv
board,3bv,optimal_clicks,solve_seconds
"b=1&m=080212g040002k00g",16,12,0.004321
```

`board` is the bare PTTACG string. The CSV is written to standard output; the
RNG seed, progress, and total elapsed time are written to standard error, so
redirecting standard output produces a clean CSV file.
Each row's `solve_seconds` covers model construction and solving that board.
After the run, standard error reports the median, p5, and p95 per-board solve
times. Percentiles use linear interpolation between neighboring sorted values.

Bulk generation uses `std::mt19937_64` and unbiased rejection sampling. Supply
`--seed` to reproduce the complete sequence:

```console
./doms --bulk expert 10 --seed 8675309 > expert-results.csv
```

Bulk runs use count-only DP by default: states store the cost but no chord set,
and the solver does not reconstruct moves. By default, the solver starts one
worker per hardware thread reported by `std::thread::hardware_concurrency()`
(falling back to one, and using no more workers than boards). Use `--threads N`
to set the number of worker threads, including `--threads 1` for sequential
execution. Board generation uses a single RNG on the main thread, so a seed
produces the same boards and CSV row order at every thread count.

For a single board, replay remains the default. Use `--count-only` to print
only the optimal click count and DP statistics. `--no-count-only` enables
reconstruction and validation during bulk runs too. Count-only JSON omits move
and breakdown fields.
`--click-tuples` requires replay. Each worker solves one board at a time; use
fewer threads if simultaneous difficult boards consume too much memory.

### Fixed single-thread benchmark

Run from the repository root so the program can load
`benchmarks/expert_benchmark_reference.csv`. On Windows PowerShell, use
`.\doms.exe` instead of `./doms`.

```console
./doms --benchmark
./doms --benchmark --benchmark-warmup 0 --benchmark-repeat 1
```

The benchmark solves three fixed sets of 1,000 Expert boards (30 x 16, 99
mines), sequentially on one thread in count-only mode, using these seeds:

1. `15415727658250109428`
2. `13781498123793209364`
3. `1443274517951773430`

By default it requests high priority, warms up on the first 100 boards of each
set once, then repeats the three sets three times in the order above: 9,000
timed solves plus 300 warm-up solves. Priority elevation is best effort and
prints a warning on failure. `--benchmark-warmup N` changes the warm-up count
per set (capped at 1,000; `0` disables it). `--benchmark-repeat N` changes the
number of timed repeats. The second command above runs exactly 3,000 solves.
`--benchmark-cpu N` optionally pins the thread to zero-based logical CPU `N`
on Windows or Linux; failure to pin exits with status `2`.

Every board's encoding, 3BV, and optimal click count must match the committed
references, including during warm-up. A mismatch stops with exit status `2`
and identifies the seed, board index, and expected/actual values. Each timing
row prints `PASS` only after all 1,000 boards match. The benchmark never updates
the references.

Each timed set reports wall time (including generation and validation), summed
solve time, thread CPU solve time, and median/p5/p95 per-board solve times.
Solve intervals cover model construction and solving. CPU time excludes time
the thread is not running; unsupported platforms report zero. Percentiles use
linear interpolation. An aggregate section reports median/minimum/maximum
totals per set across repeats and `cv_solve_percent` (population standard
deviation divided by mean solve time, as a percentage). Standard output mixes
text with timing and aggregate tables; it is not a single CSV table.

`--progress`, `--order`, `--band-size`, and explicit `--count-only` are supported.
Other input modes, `--seed`, `--threads`, `--no-count-only`, `--profile`, `--json`,
and `--click-tuples` cannot be combined with `--benchmark`.

### Fixed multithread benchmark

```console
./doms --benchmark-multi
./doms --benchmark-multi --threads 4
```

This runs the same three fixed sets once each in count-only mode and validates
against the same references. It uses one worker per available hardware thread
(falling back to one); `--threads N` overrides the worker count. Generation
happens before parallel solving, so every worker count receives the same boards.
Each set reports total elapsed time and median/p5/p95 per-board solve times.
Total time includes generation and validation; per-board times cover model
construction and solving. Concurrent boards can use substantially more memory.

This mode does not warm up, raise priority, or report repeat aggregates. It
rejects `--benchmark-warmup`, `--benchmark-repeat`, and `--benchmark-cpu`.

## Output formats and coordinates

All output coordinates are 1-based.

The normal action listing uses `(row, column)`. JSON coordinate arrays and the
named fields in `actions` also use row before column. Click tuples use
`(click_type, x, y)`, where `x` is the column and `y` is the row.

### Human-readable output

The default output includes:

- the optimal click count and unchorded 3BV;
- a breakdown into flags, component-seeding left-clicks, chords, and remaining
  3BV clicks;
- the number of chord candidates before and after proven preprocessing;
- frontier-DP statistics; and
- the complete ordered action sequence.

Elapsed solve time is always printed to standard error. For a single board it
covers model construction, preprocessing, ordering, DP, and any reconstruction
and validation. It excludes input loading, browser startup, and output printing.

### Click tuples

```console
./doms example.mbf --click-tuples
```

This prints a list of:

```text
('left', x, y)
('right', x, y)
('chord', x, y)
```

`right` means place a flag.

The replay places each flag immediately before the first chord that needs that
mine. Component-opening left clicks and the chord order remain the same, so
the replay still uses the optimal number of clicks.

### JSON

```console
./doms example.mbf --json
```

JSON output includes:

- `optimal_clicks` and `three_bv`;
- the selected chord squares and required flags;
- chord components and remaining base clicks;
- an ordered `actions` list using named row/column fields;
- an ordered `clicks` list using `[type, x, y]`; and
- search statistics, including the two candidate-reduction counts,
  `opening_chain_absorptions`, `candidate_reduction_seconds`,
  `peak_dp_states`, and `solve_seconds`.

`--json` and `--click-tuples` are mutually exclusive.

## Command-line reference

```text
Usage: doms [BOARD] [options]

BOARD may be a PTTACG string, compatible LlamaSweeper URL,
World of Minesweeper game URL, MBF filename, or quoted MBF hexadecimal.
```

| Option | Meaning |
| --- | --- |
| `--benchmark` | Validate three fixed 1,000-board Expert sets on one thread; default: 100 warm-up boards/set, high priority, three repeats, timing rows and aggregate statistics. |
| `--benchmark-multi` | Run the same fixed benchmark using available CPU threads; accepts `--threads N` to set the worker count. |
| `--benchmark-warmup N` | Warm up on `N` boards per fixed set before timing; requires `--benchmark`; default `100`, and `0` disables it. |
| `--benchmark-repeat N` | Run each fixed set `N` times and report aggregate median/min/max/CV statistics; requires `--benchmark`, default `3`. |
| `--benchmark-cpu N` | Pin the single benchmark thread to logical CPU `N`; requires `--benchmark`. |
| `--generate beginner\|intermediate\|expert` | Generate, print, and solve one random standard board. Do not also supply `BOARD`. |
| `--bulk beginner\|intermediate\|expert\|evil\|NxM/K COUNT` | Generate and optimally solve `COUNT` boards. `evil` is `30x20/130`; custom sizes are width x height / mines, matching PTTACG. Writes per-board solve times in the CSV and median, p5, and p95 solve times to standard error. |
| `--seed N` | Reproduce a generated board or complete bulk sequence. Requires `--generate` or `--bulk`. |
| `--threads N` | Set the worker count for `--bulk` or `--benchmark-multi`; default uses `hardware_concurrency()`, with a fallback of one. |
| `--count-only` | Store only state costs and omit move reconstruction; default for bulk runs. |
| `--no-count-only` | Reconstruct and validate moves; default for single boards. Bulk output remains CSV. |
| `--order NAME` | Choose `auto` (the default) or any sweep order printed on the `DP:` line. Examples include `columns-reverse-smart`, `rows-reverse-band-2`, and `columns-reverse-dynamic`. See below for the naming rules. |
| `--band-size N` | Force a fixed band width of `N` rows or columns. With `--order auto`, compare both orientations; with a plain row or column order, use that direction. |
| `--progress` | Print frontier size and pruning progress for one board; print completed boards during bulk runs, or validation progress every 100 boards during benchmarks. Multithread benchmark progress is reported after each set finishes solving. |
| `--profile` | Print phase timings, dominance details, and state/branch counts to standard error. Single boards only, including `--generate`; cannot be combined with `--bulk`. |
| `--json` | Print machine-readable JSON. |
| `--click-tuples` | Print only `(click_type, x, y)` tuples. |
| `-h`, `--help` | Display command-line help. |

The program exits with status `0` on success and `2` after a caught input,
resource, or validation error. There is no configurable state-count ceiling
or dominance-comparison budget.

## Useful examples

Show live frontier statistics while solving:

```console
./doms example.mbf --progress
```

Force a particular sweep direction:

```console
./doms example.mbf --order rows
./doms example.mbf --order columns
./doms example.mbf --order columns-reverse-smart
```

Try wider frontier bands:

```console
./doms example.mbf --band-size 2
./doms example.mbf --order columns-reverse-band-2
./doms example.mbf --order columns-reverse-dynamic
```

Find where one solve spends its time (including dominance subphases):

```console
./doms example.mbf --profile
./doms example.mbf --count-only --profile
```

`--profile` reports model construction, candidate reduction, ordering, factor
planning, DP setup, transitions, dominance, and move reconstruction. Dominance
is further split into setup, same-connectivity checks, forward and compensated
cross-connectivity checks, erasure, and cleanup. The final counters report:

- states processed and distinct successor states generated, after cheap
  conditional pruning and duplicate merging but before full dominance;
- branches skipped by conditional dominance;
- full dominance passes and intermediate cuts where those passes were deferred;
- peak generated states; and
- states removed by exact, forward, and reverse dominance.

Here, `exact` means identical-connectivity pruning, and `reverse` includes the
cost-compensated connectivity checks. All three pruning rules are exact.
`peak_dp_states` (also printed on the `DP:` line) is the largest table retained
between candidate steps, including intermediate steps where full pruning is
deferred. It can differ from the pre-pruning peak reported by `--profile`.
Timing and profile text stay on standard error even with `--json`.

## What is being optimized?

Let `S` be the set of numbered safe squares that will be chorded. The click
cost of `S` is:

```text
number of selected chord squares
+ number of distinct mines that must be flagged
+ number of chord-propagation components
+ number of 3BV units not opened by a chord
```

The four terms are respectively chord clicks, right-clicks, initial left-clicks,
and remaining ordinary left-clicks.

Two selected chord squares are in the same propagation component when opening
and chording one can eventually reveal the other. This includes direct chord
adjacency and connections through a shared zero opening. One initial left-click
is sufficient for each such component.

The program precomputes standard 3BV units: one for every connected zero region
and one for every nonzero safe square not adjacent to a zero.

## Algorithm overview

Before choosing a sweep order, DOMS repeatedly removes chord candidates using
two exact certificates:

- **Swap domination.** Candidate `v` can be removed when an adjacent candidate
  `u` covers every 3BV unit covered by `v`, requires a subset of `v`'s mines,
  and preserves every propagation connection of `v`. Any solution using `v`
  can use `u` instead; if it already uses `u`, it can simply delete `v`.
- **Left-click domination.** DOMS bounds the effect of deleting a chord for
  every possible arrangement of its selected propagation neighbors. The worst
  possible component split is represented by an independent set of those
  neighbors. A small branch-and-bound proof search compares that split and any
  lost 3BV coverage with the saved chord click, seed click, and mines usable by
  no other remaining chord. The candidate is removed only when the bound is
  nonpositive for every independent set.

The reductions are applied iteratively. Each step proves that every solution
using the removed candidate has a no-worse solution without it, so composing
the steps preserves the global optimum. `--progress` prints the before/after
candidate counts and the time spent proving the reductions.

The primary solver is a frontier dynamic program over possible chord sets.
Candidates are processed in a spatial sweep. A boundary state records:

- the future reachability of each unfinished selected-chord component; and
- which active mine-flag and 3BV-coverage factors have already been hit.

Each candidate has a skip branch and a chord branch. Before constructing both,
DOMS uses cached connectivity relations and a cheap cost inequality to discard
a branch when its sibling provably dominates it. It then normalizes and merges
duplicate states. These inexpensive operations run after every candidate;
the full dominance passes run after **two candidates**, and after the final
candidate if it is an unpaired tail. This gives both candidates' effects a
chance to combine before the more expensive comparisons.

Equivalent histories are merged. Connectivity signatures are interned per
layer. Their words share one contiguous arena, with offsets, lengths, and
hashes stored in a separate metadata array. This replaces per-signature word
allocations and pointer indirection while retaining the same signature IDs.
DP states are held in a dense open-addressed table. Once a layer's
duplicate merging is complete, lookup indexes of at least 64 KiB are released:
dominance removes
states directly by dense-entry index, and the next transition iterates the
survivors. This avoids hashing deleted states and reduces memory held during
dominance pruning. Smaller layers keep their index for allocator reuse.
Mine/3BV factors
receive reusable slots for only the portion of the sweep in which they are
live. When at most 64 slots are required, the complete factor state is stored
and processed as one `uint64_t`; wider custom boards use a dynamically sized
bitset fallback. Future connectivity contacts likewise receive stable reusable
boundary slots. Standard boards therefore store each unfinished component in
one or two 64-bit words rather than a bitset spanning every chord candidate;
boundaries requiring more than 128 slots use the general representation.
Connectivity transitions are cached per signature within each candidate step.
Their state-hash prefixes are cached too: successors sharing connectivity only
need to hash their factor bits. Opening absorption recomputes the prefix if it
changes the successor's connectivity ID. The resulting hashes are unchanged.
For one-word components, transitions sort the component words directly to
canonicalize the signature, avoiding an offset sort and a separate copy.
Count-only mode uses a separate template specialization that omits the chosen
chord history from every state and skips move reconstruction and validation.

DOMS also applies **opening-chain absorption**, an exact state equivalence. If
an unfinished chord component can reach exactly the undecided border of one
zero opening, and that opening is already covered, the state is normalized by
removing the component, marking the opening uncovered, and adding one click.
One-word component signatures are sorted, so matching opening borders use
binary search; wider signatures keep the full component comparison.
If no later border chord is selected, this pays the component's eventual seed
click; if one is selected, its new seed click and newly saved 3BV click cancel.
The normalized state therefore has exactly the same cost under every future
decision, but can merge with many more DP histories.

The solver also applies exact dominance pruning:

- Within one connectivity signature, a state is discarded when another state's
  existing cost advantage covers every possible future flag liability and 3BV
  saving.
- Between compatible connectivity signatures, a coarser partition may dominate
  a finer partition when it can never require more future component-seeding
  clicks.
- A cheaper state with finer connectivity can also dominate a coarser state
  when its saving covers a proven upper bound on the extra seed clicks.
  A common-merge bound extends this cost-compensated rule to some incomparable
  connectivity signatures as well.

The pruning passes run in that order: identical connectivity, coarsening, then
cost-compensated connectivity. They share buckets grouped by total future
reachability, cached factor counts, and sorted cost keys. Safe cost and
component-size filters reject impossible comparisons before the full
connectivity tests. Sorted cost cutoffs stop scans once later witnesses cannot
afford to dominate the target; the compensated pass skips targets already
removed by coarsening dominance. During that pass it keeps a compact list of
remaining targets, refreshes the cost bounds when targets are removed, and
stops comparing a target signature once its list is empty. States removed by
cross-connectivity dominance remain available as witnesses until all passes
finish; identical-connectivity dominance retains only its survivors.

Dominance setup's cached records point directly to stable DP table entries
and derive the final sort field from their cached hit counts. On 64-bit builds
this reduces each record from 48 to 32 bytes and reduces the data moved during
sorting. Count-only mode also stores these records in one shared array, with
a slice for each reachability bucket. Replay mode retains per-bucket buffers,
which used less peak RSS with heap-backed chord histories in testing. Bucket
sizes are accumulated while grouping signatures, avoiding a separate scan.
Each bucket's connectivity-signature IDs also occupy a slice of one shared
array, avoiding per-bucket allocations while preserving signature and witness
order.

Dominance setup's cached records point directly to stable DP table entries
and derive the final sort field from their cached hit counts. On 64-bit builds
this reduces each record from 48 to 32 bytes and reduces the data moved during
sorting. Count-only mode also stores these records in one shared array, with
a slice for each reachability bucket. Replay mode retains per-bucket buffers,
which used less peak RSS with heap-backed chord histories in testing. Bucket
sizes are accumulated while grouping signatures, avoiding a separate scan.
See the [transition and setup benchmark notes](benchmarks/transition_setup_results.md)
for measured timings, memory usage, test boards, and validation details.

The common connectivity-matching path builds eligibility one fine component
at a time. If a fine component has no containing coarse component, it rejects
the comparison immediately. Otherwise it uses the same eligibility graph and
complete matching test, preserving every dominance decision.

Survivor lists for all connectivity signatures share one contiguous pointer
buffer per dominance pass. Each signature receives a disjoint slice sized by
its incoming state count, which bounds its maximum possible survivors. This
avoids a separate allocation for every signature while preserving witness
order, stable pointers, and all dominance comparisons.

Sorted component sizes also share one contiguous buffer, with prefix offsets
assigning each live signature its own slice. This removes per-signature
vector allocations while preserving every component-size filter result.

**Dominance pruning has no comparison budget.** Every scheduled pass runs
without a numerical comparison limit, retaining the safe filters and early
exits above. Budget counters, exhaustion checks, and exhaustion reporting have
also been removed. Removing the cap can reduce total runtime and memory by
preventing avoidable states from accumulating in later layers. It does not
guarantee a faster solve on every board, and it does not change exactness.

For `--order auto`, the program evaluates row, column, and promising banded
orders in all four sweep directions: left-to-right, right-to-left,
top-to-bottom, and bottom-to-top. The final selection minimizes **estimated
peak total width plus estimated peak connectivity width**, then peak total
width, then estimated total work; the order name breaks remaining ties.
This gives extra weight to connectivity because component relationships are
more expensive than independent factor bits. Ordering is a performance
heuristic, not a guarantee of the fastest possible solve. Direction matters
especially on rectangular Expert boards.

Width estimation accumulates graph and factor interval endpoints in difference
arrays, then sweeps the boundaries once. This avoids scanning every interval
at every boundary while preserving exactly the same estimates and order choices.

Smart line ordering combines current-line scopes into the before/after subset
histograms, using two transforms instead of three for each crossing-cost
calculation. Every crossing score and ordering tie break remains identical.

Automatic ordering also tries bands whose widths change across the board. It
computes the frontier profile for each contiguous band of one through eight
rows or columns, then uses a short dynamic program to choose band boundaries.
The three optimization passes minimize peak estimated width, peak connectivity
width, and accumulated estimated work, in that order. A selected order such as
`columns-dynamic-7.4.1.1.8.1.6.2` lists its consecutive band widths. The
search still evaluates the exact click objective regardless of which ordering
is chosen. Supplying `--band-size` continues to force the requested fixed
width.

`--order` accepts `rows` or `columns`, optionally followed by `-reverse`,
then one of `-smart`, `-band-N`, or `-dynamic`. The `-dynamic` form computes
variable widths for that direction. You can also pass the full order name
printed on the `DP:` line, such as `columns-dynamic-7.4.1.1.8.1.6.2`, to
reuse those exact widths on the same board. The widths must sum to the number
of rows or columns being swept. `--band-size` combines only with `auto` or a
plain row or column direction (with optional `-reverse`).

## Complexity and limitations

The algorithm is exact but exponential. Its practical cost depends strongly on
frontier width, connectivity, and the number of states that survive merging
and pruning; it is not polynomial in board area.

For an `n x m` board with `k` mines, there are at most `nm - k` original chord
candidates. Let `q` be the number remaining after proven reduction. There are
at most `2^q` chord subsets, but many partial subsets have the same boundary
state or are dominated. Future-reach masks of different unfinished components
can overlap, so they should not be counted as an ordinary partition of future
boundary positions.

For the main DP storage, let:

- `S` be the largest number of distinct states in a layer, including states
  awaiting full dominance pruning;
- `R` be the largest number of interned connectivity signatures;
- `c` be the largest number of unfinished components in a signature;
- `F` be the number of 64-bit words stored for a factor mask, at least one;
- `B` be the number of 64-bit words stored per component's future-reach mask;
  this is one or two on the compact path; and
- `H = ceil(q / 64)` be the chosen-chord history size in replay mode.

Ignoring preprocessing data and auxiliary pruning indexes, the main DP tables
and connectivity pools use approximately:

```text
Count-only: O(S * (1 + F)     + R * c * B)
Replay:     O(S * (1 + F + H) + R * c * B)
```

The old and new tables coexist during a transition. Each bulk worker has its
own DP tables, so parallel solves add to peak memory use.

Without a comparison budget, a full dominance pass can examine `O(S^2)`
state pairs in the worst case, with `O(F)` work for each factor comparison.
Sorting costs `O(S log S)`. Structural connectivity checks can additionally
examine `O(R^2)` signature pairs, with a conservative bound of
`O(c^2 * B + c^3)` per pair for bitset and matching work. There are
`ceil(q / 2)` full pruning passes. These bounds are deliberately pessimistic:
shared buckets, cached relations, scalar filters, cost cutoffs, and early
successes avoid most potential comparisons on typical boards. Transition,
ordering, and preprocessing work are additional costs.

Candidate preprocessing performs polynomial subset and connectivity checks for
swap domination. Its local independent-set certificate is exponential in the
largest examined propagation neighborhood size `d` in the worst case, although
dense opening boundaries prune strongly; a neighborhood larger than 63 is
conservatively left unreduced by this certificate. This does not affect
exactness. On intended boards, reducing candidate count and frontier width
generally saves much more time than preprocessing consumes.

Some unusually connected boards can still use a large amount of memory or take
a long time. Useful responses are:

1. Enable `--profile` for a timing breakdown and `--progress` to locate growth.
2. Use `--count-only` if you do not need the actual moves.
3. Compare explicit directions or dynamic-band orders using `--order`.
4. Reduce `--threads` for bulk runs if concurrent boards use too much memory.

The solver assumes that all mine locations are already known and that flags may
be placed before their neighboring numbered squares are opened. It is not a
solver for discovering an unknown board without guessing.

## Development checks

Run `./doms --benchmark` for repeated, validated optimization timing. Use
`./doms --benchmark --benchmark-warmup 0 --benchmark-repeat 1` for a
3,000-board integration check. Run benchmark commands from the repository root.

Compile with warnings enabled:

```console
g++ -std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic minesweeper_chord_dp.cpp -o doms
```

Compile with AddressSanitizer and UndefinedBehaviorSanitizer:

```console
g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer minesweeper_chord_dp.cpp -o doms_san
```

Run representative PTTACG and MBF boards through both builds and compare their
optimal click counts. Replay mode automatically checks that its action count
matches the DP result and simulates the moves to confirm that all safe cells
are opened legally. Compare count-only and replay results as well. Tied
optimal solutions may use different action sequences.

# Optimization history

## Experiment: contiguous reachability-bucket signature IDs

Replace each dominance bucket's independently allocated signature-ID vector
with a slice of one shared array. Count IDs while assigning buckets, assign
disjoint slices, then fill them in the original ascending signature order.
Keep pair postings, witness selection, pruning certificates, search limits,
and all comparisons unchanged. Include the existing workspace edits in the
baseline and preserve them regardless of this experiment's outcome.

Build both versions with MSYS2 UCRT64 and identical C++17 optimization flags.
Compare full solver/replay results and pruning counters, then run sequential
baseline/changed/changed/baseline plain `--benchmark` invocations. Keep only
a measurable overall improvement; otherwise revert this experiment's solver
change and stop. Temporary snapshots, executables, tests, and logs live in
`%TEMP%\doms-bucket-ids-20261021`, outside version control.

### Results: retained

Both builds used MSYS2 UCRT64 GCC and
`-std=c++17 -O3 -DNDEBUG -pthread`. The changed build additionally enabled
`-Wall -Wextra -Wpedantic` and compiled without diagnostics. Four plain
`--benchmark` invocations ran sequentially in A/B/B/A order, without other
compilation or solver jobs from this session overlapping measurement. Each
used one thread, a successful high-priority request, 100 warm-up boards per
fixed set, and three timed repeats. Cells sum each set's three repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 47.484813 | 46.236706 | 45.988799 | 47.829965 |
| 2 | 46.136814 | 44.831986 | 44.628910 | 46.057485 |
| 3 | 45.295159 | 43.225148 | 43.498751 | 44.887431 |
| Total solve seconds | 138.916786 | 134.293840 | 134.116460 | 138.774881 |

Mean totals decreased from **138.845834 s to 134.205150 s (3.34%)**.
Both adjacent pairs improved (3.33% and 3.36%), as did all six corresponding
set totals. Mean CPU time decreased from 137.101563 s to 132.953125 s (3.03%).
These are local measurements from two invocations per build; they support
retention on this workload without guaranteeing improvement on every board.
Peak memory was not measured.

- All four benchmarks passed unchanged board, 3BV, and optimal-click
  references: 36,000 timed solves and 1,200 warm-ups on 3,000 distinct boards.
- All non-timing JSON fields, exact replay actions, and four state/pruning
  counter lines matched in 36 baseline/changed comparisons: three standard
  difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309, and both solve
  modes. All 18 changed replays passed built-in legal-action validation.
- An assertion-enabled differential harness compared the actual baseline
  and changed pruning functions on 2,000 seeded tables across compact/wide
  factor masks, count-only/replay records, and one through three connectivity
  words. Survivors, costs, replay histories, and all pruning counters matched.
  Cases included empty signatures, overlapping reaches, bit 63, common-union
  buckets large enough to use pair postings, and heap-backed replay histories.
- The shared array preserves ascending signature IDs within every bucket.
  Posting selection and witness order are unchanged. No objective, pruning
  certificate, search limit, or benchmark reference changed.

Source snapshots, measured executables, the temporary differential harness,
comparison script, solver JSON/profile outputs, raw benchmark logs, and
`results.json` are retained in the artifact directory above. Existing
workspace edits were preserved. `doms.exe` is the measured changed build,
verified by SHA-256 and an Expert replay smoke check. `git diff --check` passed.

## Experiment: free anchors before augmenting connectivity matches

In the inline (at most 64 components) connectivity-coarsening matcher, try
an unmatched eligible fine component before recursively displacing existing
anchors. Track matched fine components with a word mask, initializing matching
entries only when assigned. Eligibility entries are also assigned before use,
so their full-array zero initialization is unnecessary. Retain the original
augmenting-path fallback and the general wide-component implementation.

This computes the same exact containment-and-matching predicate, changing only
how a matching is found. No dominance certificate, objective, search limit,
ordering choice, or replay tie break changes.

Build baseline and changed sources with MSYS2 UCRT64 and identical C++17
optimization flags. Compare the actual original and changed predicates on
randomized and boundary cases, then compare complete count-only/replay results.
Run sequential baseline/changed/changed/baseline fixed benchmarks, retaining
the default warm-up and three timed repeats. Keep only a measurable overall
improvement; otherwise revert the solver change and stop. Temporary artifacts
are outside version control in `%TEMP%\doms-free-match-20261021`.

### Results: reverted; no measurable improvement

Both builds used MSYS2 UCRT64 GCC and `-std=c++17 -O3 -DNDEBUG -pthread`.
The changed build additionally enabled `-Wall -Wextra -Wpedantic` and had no
diagnostics. Runs were sequential A/B/B/A with no overlapping compilation
or solver jobs. Each used plain `--benchmark`: one thread, a high-priority
request (successful), 100 warm-up boards per fixed set, and three timed
repeats of all three 1,000-board Expert sets.

| Invocation | Total solve seconds (all nine timed sets) |
| --- | ---: |
| Baseline 1 | 138.811153 |
| Changed 1 | 138.425759 |
| Changed 2 | 134.783628 |
| Baseline 2 | 134.298911 |

Mean totals were **136.555032 s baseline** and **136.604694 s changed**:
the change was 0.036% slower. The first adjacent pair favored the change by
0.28%, while the second favored baseline by 0.36%. CPU-time totals likewise
showed no meaningful separation (means 135.210938 s and 135.109375 s).
Visible timing drift exceeds the differences between builds. There is no
demonstrated overall speed improvement, so the entire solver change was
reverted and no follow-on optimization was attempted.

- All four default benchmark invocations passed unchanged board, 3BV, and
  optimal-click references: 36,000 timed solves and 1,200 warm-up solves over
  3,000 distinct Expert boards.
- An assertion-enabled differential harness compared the actual original
  and changed matching functions on 300,000 seeded cases, using one through
  three signature words. It included empty signatures, overlapping reaches,
  constructed containment cases, and 64-, 65-, and 66-component boundaries.
  Every result matched, including the unchanged general fallback.
- All semantic JSON fields, replay actions, and profiling state/pruning
  counters matched across 36 full-solver comparisons: three standard
  difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309, and both modes.
  All 18 changed replays passed built-in legal-action validation.

Baseline/changed source snapshots, measured executables, the temporary
matching harness, comparison/benchmark script, raw logs and machine-readable
timings are retained in the artifact directory above. No benchmark reference
data changed. `doms.exe` was rebuilt from the restored baseline source.

## Experiment: contiguous cached dominance entries

Replace the separate cached-state allocation for each reachability bucket
with one table-sized buffer and disjoint slices assigned from the existing
bucket state counts. Populate and sort each slice in the same order as before.
The buffer stays fixed during pruning, preserving survivor pointer validity.
This changes allocation only: objectives, comparisons, witness ordering,
pruning certificates, and search limits remain unchanged. Both factor-mask
representations and replay/count-only specializations use the same code.

Include all pre-existing workspace edits in the baseline. Build both versions
with MSYS2 UCRT64 and identical C++17 optimization flags. Run full sequential
baseline/changed/changed/baseline `--benchmark` invocations and differential
solver/replay checks. Keep the experiment only if it establishes a measurable
overall improvement; otherwise restore the source snapshot and stop.

Artifacts are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-bucket-storage-20261021`.

### Results: retained

Both builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
also enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
Four complete benchmark invocations ran sequentially, with no compilation
or other solver jobs from this session overlapping the timing runs.
Each set contains 1,000 fixed Expert boards, one thread, count-only:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 17.613099 | 17.319839 | 17.562920 | 17.689766 |
| 2 | 16.869075 | 16.614381 | 16.865247 | 17.162854 |
| 3 | 16.410030 | 16.110726 | 16.438888 | 16.639500 |
| Total | 50.892204 | 50.044946 | 50.867055 | 51.492120 |

Mean total time decreased from **51.192162 s to 50.456001 s (1.44%)**.
Both adjacent pairs improved overall (1.66% and 1.21%), as did all six
corresponding set totals. This is a modest local gain from two repetitions
per version, with timing variation; it does not establish a gain on every
board. Peak memory was not measured. The new buffer default-initializes
entries before population, trading that work for fewer bucket allocations.

- All four complete `--benchmark` invocations passed unchanged board, 3BV,
  and optimal-click references: 12,000 solves over 3,000 distinct boards.
- All non-timing JSON fields and state/pruning counters matched across 36
  full solver comparisons: three difficulties, seeds 1, 42, 100, 2026,
  123456, and 8675309, and both solve modes. All 18 changed replays passed
  built-in legal-action validation.
- A temporary differential harness compared the actual baseline and changed
  pruning functions on 2,000 seeded tables across compact/wide factor masks,
  count-only/replay records, and one through three connectivity words.
  Survivors, costs, chosen histories, and all pruning counters matched.
  Cases included empty signatures, overlapping reaches, boundary bit 63,
  large common-union buckets, and heap-backed replay histories.

Source snapshots, measured builds, temporary differential harnesses,
comparison outputs, and raw benchmark logs/results are in the artifact
directory above. Existing workspace changes were preserved. `doms.exe` is
the measured retained build, verified by SHA-256 and an Expert replay smoke
check. `git diff --check` passed.

## Smart ordering: count closed vertices with the existing subset transform

### Plan and implementation

The smart-line scorer previously scanned every selected current-line vertex
at every subset cut to count vertices with an unprocessed neighbor. A vertex
without a later-line neighbor closes exactly when its closed neighborhood
(itself and its same-line neighbors) is contained in the selected mask.
Add one histogram entry for that closed neighborhood to the existing
old-vertex histogram, before its subset-zeta transform. The transformed value
then counts both closed old vertices and closed current-line vertices.
The live-vertex score is `old_always + old_total + popcount(mask) - closed`.

This removes the selected-vertex loop at every cut and two small per-line
arrays without adding another transform or subset-sized buffer. The final
vertex scoring loop is O(2^p) instead of O(p * 2^p); the existing transform
and path DP still take O(p * 2^p). Every graph score, factor score, tie break,
and complete order is preserved. No search, pruning certificate, objective,
or search limit changes.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
  also enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
- An assertion-enabled temporary harness compared the actual original and
  changed smart-order functions. All 4,400 complete orders matched: 300
  seeded standard boards before and after reduction, plus 500 synthetic
  graph/scope models, each in both orientations and both sweep directions.
- The harness also compared 2,097,151 cut counts with direct selected-vertex
  scans, covering every mask at each line size from zero through 20.
- All non-timing JSON fields and state/pruning counters matched in 36
  baseline/changed solver comparisons: three difficulties, seeds 1, 42, 100,
  2026, 123456, and 8675309, and both count-only and replay modes. All 18
  replays passed the built-in legal-action checks.
- Four complete sequential `--benchmark` invocations passed all unchanged
  board, 3BV, and optimal-click references: 12,000 solves over 3,000 distinct
  Expert boards. No compilation or other solver jobs from this session
  overlapped the timing runs.
- `git diff --check` passed. `doms.exe` is the measured changed build.

### Measurements

Runs were baseline/changed/changed/baseline. Each cell is total seconds for
one fixed 1,000-board Expert set, on one thread in count-only mode:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.325184 | 17.795119 | 18.472514 | 19.200669 |
| 2 | 17.636364 | 16.986800 | 17.755737 | 18.270290 |
| 3 | 17.372248 | 16.487635 | 17.787841 | 17.382446 |
| Total | 53.333796 | 51.269554 | 54.016092 | 54.853405 |

Mean total runtime decreased from **54.093601 s to 52.642823 s (2.7%)**.
Both adjacent pairs improved overall (3.9% and 1.5%), but the third set in
the second pair regressed. These are local measurements from two repetitions
per build, with visible timing variation; gains remain workload-dependent.
Memory use was not measured.

Baseline source, original executable, measured builds, temporary differential
harness, comparison script/outputs, and raw benchmark logs/results are saved
outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-live-vertices-20261021`.

## Next optimization: combine crossing-count subset transforms

Smart line ordering subtracts three transformed scope histograms when scoring
each partial-line cut. A scope confined to the current line contributes to
both the selected-mask and complement-mask sums. Insert that scope into both
the before and after histograms before transforming them. By linearity, two
transforms now give exactly the same crossing counts as the previous three.
The two crossing-count calls per eligible line use four transforms instead of
six; the separate old-neighbor transform is unchanged. One temporary integer
array per crossing-count call is also eliminated.

Every crossing score, ordering tie break, chosen order, and subsequent search
is preserved. The objective, pruning certificates, and search limits are
unchanged. Existing workspace changes were included in the baseline snapshot
and preserved.

### Validation

Both builds used MSYS2 UCRT64 GCC 16.1.0 with identical
`-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic` flags.
The changed build compiled without diagnostics.

- A temporary harness extracted the actual baseline smart-order function and
  compared it with the changed function on 300 seeded boards (100 per standard
  difficulty), before and after candidate reduction. All 2,400 complete orders
  matched across row/column orientations and both sweep directions. This also
  exercised unreduced lines near the 20-candidate subset limit.
- All non-timing JSON fields matched in 36 baseline/changed comparisons:
  three difficulties; seeds 1, 42, 100, 2026, 123456, and 8675309; both solve
  modes. All 18 changed replays passed built-in legal-action simulation.
- Four complete `--benchmark` invocations validated 12,000 solves over the
  same 3,000 distinct boards against unchanged board, 3BV, and optimal-click
  references. Every run passed.

### Benchmark results

Runs were sequential in baseline/changed/changed/baseline order. Compilation
and other solver jobs from this session did not overlap these timing runs.
Each invocation ran three fixed 1,000-board Expert sets on one thread in
count-only mode. Each cell below is total seconds for one fixed set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 20.591916 | 19.980619 | 20.003988 | 20.132177 |
| 2 | 19.418958 | 19.350945 | 18.986695 | 19.440555 |
| 3 | 18.780016 | 18.733552 | 18.125754 | 18.936448 |
| Total | 58.790890 | 58.065116 | 57.116437 | 58.509180 |

Mean total time decreased from **58.650035 s to 57.590777 s**, a **1.8%**
reduction. Both pairs improved (1.2% and 2.4%), as did all six corresponding
set totals. This is a modest local gain from two repetitions on one machine;
some individual differences are small and machine activity affects timings.
Memory consumption was not measured. Raw total/median/p5/p95 measurements
are recorded in `benchmarks/crossing_transform_benchmark.json`.

Source snapshots, binaries, the full-order differential harness, comparison
scripts, solver JSON outputs, and raw benchmark logs are saved under
`C:\Users\happy\AppData\Local\Temp\doms-crossing-20261021`.
`doms.exe` was updated from the measured changed build; its previous version
is saved there as `original-doms.exe`. The installed executable matched the
measured build by SHA-256 and passed an Expert replay smoke check.
`git diff --check` passed.

## Goal and evidence

Reduce solve time while preserving the exact optimal click count and valid
optimal replays. Keep all existing pruning certificates and the DP search.

The C++ solver already merges equivalent states, reduces candidates, caches
connectivity transitions, and applies three exact dominance passes. Baseline
profiles of Expert seeds 1, 42, 100, 2026, 123456, and 8675309 show dominance
taking approximately as much time as transitions. In the compensated
connectivity pass, a signature's target list still includes states removed by
the forward pass or earlier compensated comparisons. Signature feasibility
checks also use maxima that include those removed targets.

## Implementation

1. Build a sorted list of still-undominated targets for each target signature.
2. Compute maximum target cost, quasi score, and primary key from that list.
   Use the existing necessary inequalities with these tighter maxima before
   computing connectivity relations.
3. After a successful comparison, compact the target list and refresh its
   maxima. Stop scanning candidate signatures when the list becomes empty.
4. Preserve the complete witness lists until the existing deferred erasure.
   Apply the same implementation to count-only/replay and compact/wide masks.

## Why optimality is preserved

Already-dominated states need no additional deletion proof. Filtering them from
the target list changes neither the available witnesses nor any dominance
certificate. Every remaining target is bounded by the refreshed maxima, so
failure of a necessary inequality still proves that the candidate signature
cannot dominate any remaining target. The order of all actual witness checks
and all final pruning decisions stays the same.

## Validation and acceptance

- Compile baseline and changed sources with the same C++17 optimization flags.
- Compare deterministic bulk Expert outputs and representative profiles,
  including processed states and each pruning counter.
- Compare replay and count-only results, using replay's built-in simulation.
- Exercise the wide-mask specialization and exhausted/partly exhausted target
  cases in a focused differential check against the original pruning code.
- Measure repeated sequential bulk runs; record results and limitations here.
  Keep the change only if the measured workloads support a speed benefit.

## Results

Implemented in `prune_dominated` and documented in the README. The maximum-cost
array that was used only by the old compensated pass has also been removed.

Both sources were compiled with MSYS2 UCRT64 GCC 16.1.0 using
`-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic`.
The compiler reports an existing C++20-extension warning about capturing a
structured binding in the transition code; both baseline and changed builds
have that warning.

Four sequential runs per version, alternating execution order, gave these
median solve times on this machine:

| Workload | Baseline | Changed | Time reduction |
| --- | ---: | ---: | ---: |
| 200 Expert boards, seed 8675309, auto order, one thread | 6.0305 s | 5.7435 s | 4.8% |
| Expert seed 100, explicit rows order, count-only | 1.3615 s | 1.2450 s | 8.6% |

The second workload deliberately exercises a larger frontier: 78,659 retained
states at peak. Its compensated-pass median fell from 0.226 s to 0.128 s
(43.4%). These are local measurements with some run-to-run variation, not a
guarantee for every board. The algorithm remains exponential and peak state
counts are unchanged.

Validation completed successfully:

- All 200 bulk board encodings, 3BV values, and optimal click counts matched
  exactly across versions and repeated runs.
- 36 complete solver comparisons covered Beginner, Intermediate, and Expert,
  seeds 1, 42, 100, 2026, 123456, and 8675309, in both count-only and replay
  modes. All non-timing JSON fields and pruning counters matched. The 18 replay
  cases also passed the built-in legal-action simulation.
- A temporary differential harness compared the original and changed pruning
  functions on 2,000 seeded randomized tables. It covered all four combinations
  of compact/wide factor masks and count-only/replay records, plus one through
  three connectivity words, including overlapping reaches. Surviving states,
  costs, chosen histories, and all three pruning counters matched exactly.
- `git diff --check` passed.

Baseline snapshots, validation harnesses, and compiled comparison executables
were kept in the system temporary directory. The existing workspace
`doms.exe`, `compiler_check.cpp`, and `expert-results.csv` were left untouched.
Rebuild the executable from the changed source to use the optimization.

## Next optimization: release completed layer lookup indexes

A fresh profile of Expert seed 100 with `--order rows --count-only` spent
0.704 s in transitions, 0.754 s in dominance, including 0.029 s erasing states.
Each completed layer retained its hash index even though duplicate merging was
finished. Dominance erased by iterator but still rehashed each deleted state
and searched for its hash slot. Later transitions only iterate that layer.

Release indexes of at least 64 KiB after transitions have finished constructing
the layer, before dominance scratch allocations. Erase directly using the iterator's
dense-entry index when the lookup index is absent. Preserve the original
indexed erase path, linear find fallback, alive flags, and wide-storage
cleanup. Rebuild an appropriately sized index if a caller inserts again.
Keep smaller indexes: an initial version releasing every index improved the
large-frontier median by 4.0% but regressed the bulk median by 1.4%, consistent
with allocator reuse mattering more on small layers. The size threshold is
only a storage choice; it places no limit on states or dominance comparisons.

The state entries and iterator order stay identical. This removes only unused
lookup storage and redundant hash work; it changes no cost, connectivity,
branching, merging, dominance certificate, pruning schedule, or search limit.
On the profiled 112,112-state layer the released 262,144-slot index saves
1 MiB during dominance pruning. Other layer sizes have different savings.

Two connectivity-interning alternatives were evaluated first and discarded.
Using try_emplace alone regressed the bulk median by about 1.3%. A compact
connectivity pool showed an initial gain that did not reproduce in a separate
timing pass. Neither alternative remains in the source.

Validation of the retained index-lifecycle optimization:

- 36 baseline/changed full-solver comparisons across three standard
  difficulties, six seeds, and both solve modes matched every non-timing JSON
  field. All 18 replay cases passed the solver's legal-action simulation.
- 100 additional seeded random boards of size 2 through 4 in each dimension
  matched independent Python exhaustive search in both solve modes.
- A focused C++ harness with standard-library assertions checked all four
  combinations of compact/wide masks and count-only/replay records. It covered
  indexed and unindexed erasure, repeated discard and erase, linear find,
  iteration, reinsertion with index rebuilding, complete erasure, and overflow
  storage in six-word masks and seven-word chosen histories. The final harness
  used 12,000 states per specialization to exceed the release threshold.
- All 15 existing Python tests passed. Both C++ versions were compiled with
  GCC 16.1.0 and identical `-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra
  -Wpedantic` flags. The existing structured-binding capture warning remains.

Benchmark and validation artifacts are in the system temporary directory under
`doms-opt-20261021`; the baseline snapshot includes all earlier working-tree
optimizations. Timing results below compare against that snapshot.

Four final sequential runs per version, with alternating execution order:

| Workload | Baseline median | Changed median |
| --- | ---: | ---: |
| 200 Expert boards, seed 8675309, auto order, one thread | 5.8586 s | 5.8377 s |
| Expert seed 100, explicit rows order, count-only | 1.3765 s | 1.3733 s |

These end-to-end differences (0.4% and 0.2%) are within observed timing noise;
there is no demonstrated material overall speedup on these workloads. A final
paired profile reduced erasure time from 0.025 s to 0.009 s (64%), with exactly
matching processed/generated states and all pruning counters. The retained
change removes redundant erasure work and lookup memory on large layers, but
dominance comparisons and transitions still dominate total runtime. Larger
overall gains require optimizing those phases. All bulk CSV rows matched.

**Current status: excluded.** The completed-layer index release had no
demonstrated material overall speedup and was removed in the combined
weak-evidence ablation.

`doms.exe` was rebuilt from the final source and smoke-tested. Its previous
version is saved as `original-doms.exe` in the temporary artifact directory;
`compiler_check.cpp` and `expert-results.csv` remain untouched.

## Next optimization: linear interval width estimation

### Plan and implementation

Automatic ordering repeatedly calls `width_estimate`, including for partial
sweeps when selecting dynamic bands. The old estimator materialized graph and
factor intervals, then scanned every interval again at every requested cut.
An Expert seed 100 profile attributed about 0.010 s of a roughly 0.038 s solve
to ordering, making this a useful target independent of the DP state search.

Replace interval lists with graph and factor difference arrays. An interval
covering cuts `first` through `last - 1` increments its array at `first` and
decrements it at `last`. Prefix sums give the exact active counts at each cut.
For partial sweeps, accumulate from cut zero but score only the requested cuts.
Keep all maximum calculations, saturated work accumulation, candidate orders,
tie-breaking, pruning rules, and DP behavior unchanged.

For `q` candidates and `I` intervals, the boundary-scoring work changes from
O(q * I) to O(q + I); constructing the interval endpoints still requires the
same graph and scope traversal. Temporary endpoint storage is O(q).

Three earlier experiments in this session were discarded: caching dominance
comparison data, bypassing scalar dominance count filters, and inlining
portable population counts. None showed a consistent overall improvement.
Caching opening-absorption matches helped one large-frontier workload but had
no demonstrated bulk benefit and required extra per-signature storage, so it
was also discarded. Only the interval-estimation change remains.

### Exactness and validation

Each interval contributes one to exactly the same cuts under the old scan and
the difference-array prefix sum. Thus every returned maximum and work estimate
is identical, including partial ranges and saturated work sums. This preserves
the chosen order as well as the exact optimization objective.

- Added `tests/width_estimate.cpp`, which compares 19,000 estimates with the
  original scanning function on 1,000 deterministic randomized models. Cases
  include empty orders/scopes, singleton orders, arbitrary partial ranges,
  dense graphs, and work saturation.
- All non-timing JSON fields matched the session baseline in 36 full-solver
  comparisons: three difficulties, six seeds, and both solve modes. The 18
  replay cases passed the built-in legal-action simulation.
- 100 small random boards matched independent Python exhaustive search in
  both count-only and replay modes.
- All 15 existing Python tests and `git diff --check` passed.
- Both executables used GCC 16.1.0 with identical
  `-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic` flags. The existing
  structured-binding capture extension warning remains.

### Measurements

Four sequential runs per version, alternating execution order, against a
snapshot containing all prior working-tree optimizations:

| Workload | Baseline median | Changed median | Change |
| --- | ---: | ---: | ---: |
| 200 Expert boards, seed 8675309, auto order, one thread | 5.9652 s | 5.2690 s | 11.7% faster |
| Expert seed 100, explicit rows, count-only | 1.4311 s | 1.4535 s | 1.6% slower |

All four bulk runs were faster with the change, and every CSV row matched.
The explicit plain-row control bypasses width estimation altogether; its
small timing difference reflects run-to-run variation in unchanged solve
code. This optimization benefits ordering and does not accelerate DP for an
already specified plain order. Performance remains board-dependent.

Artifacts and baseline snapshots are in the system temporary directory under
`doms-opt-next-20261021`. `doms.exe` was updated from the validated build and
smoke-tested with an Expert replay; its previous version was saved there as
`original-doms.exe`. Existing `compiler_check.cpp` and `expert-results.csv`
were preserved.

## Next optimization: defer DP hash table growth until insertion

`FactorTable::find_or_emplace` previously ensured insertion capacity before every
lookup. In duplicate-heavy layers, that could rehash the table even when the
successor was already present. The lookup now probes first and grows only after
it finds an empty slot for a new state. If a tombstone is available, insertion
reuses it without increasing occupied-slot count or growing the table. This
changes only index capacity and probing; state equality, record updates, and
all DP and dominance decisions are unchanged.

Validation and measurements:

- GCC 16.1.0 rebuilt the C++17 solver with `-O3 -DNDEBUG -pthread`; only the
  existing structured-binding capture extension warning appeared.
- Full replay JSON for Expert seed 100 with row order matched the prior build
  in every non-timing field, including the 121-click optimum.
- All 200 Expert board encodings, 3BV values, and optimal click counts matched
  on seed 8675309.
- A larger sequential bulk comparison on 1,000 Expert boards, seed 20261021,
  took 26.581 s before and 25.582 s after (3.8% faster). Five 200-board paired
  runs had medians of 5.431 s and 5.332 s (1.8% faster); per-run timings varied
  with machine load.
- `git diff --check` passed.

**Current status: excluded.** Separate results favored deferred growth, but
the latest controlled review had contradictory rounds. It was removed in the
combined weak-evidence ablation.

## Next optimization: sort one-word connectivity components directly

### Plan and implementation

Connectivity transitions previously sorted component offsets with an indirect
lexicographic comparator, then copied those components into a canonical word
buffer before interning it. When a component occupies one word, the outgoing
word buffer already contains exactly those components contiguously.

Sort that buffer directly and intern it, skipping the offset sort and canonical
copy. Keep the existing path for signatures with multiple words. This change
applies to both count-only and replay modes and both factor-mask types.

Every retained component contributes exactly one word to the buffer; empty
components have already been removed and charged. Sorting those words produces
the same ordered multiset as sorting offsets by their referenced words. Equal
components retain their multiplicity. Thus the interned signatures, component
closure charges, state insertion order, costs, and all pruning decisions are
identical. No heuristic or search limit is introduced.

A greedy matching shortcut in connectivity dominance was evaluated first and
discarded because initial timings did not support a benefit. It is absent from
the final source. The final timing runs below ran sequentially without other
compilation or validation jobs from this session running concurrently.

### Validation and measurements

Both versions were compiled with MSYS2 UCRT64 GCC 16.1.0 and identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. The existing structured-binding capture
extension warning remains.

Four runs per version, alternating execution order, against a snapshot of the
source at the start of this session, including all preceding optimizations:

| Workload | Baseline median | Changed median | Time reduction |
| --- | ---: | ---: | ---: |
| 200 Expert boards, seed 8675309, auto order, one thread | 5.4155 s | 5.1850 s | 4.3% |
| Expert seed 100, explicit rows, count-only | 1.4460 s | 1.3340 s | 7.7% |

A separate single-thread run of 500 Expert boards with seed 20261021 took
13.229 s before and 12.668 s after (4.2% faster); all 500 board encodings,
3BV values, and optimal click counts matched.

All four paired runs improved on both workloads. These are local measurements;
performance depends on the board and machine. The optimization saves work for
one-word component signatures; wider signatures retain the original algorithm.

Validation passed:

- All non-timing JSON fields and processed/generated state and pruning counters
  matched in 36 baseline/changed comparisons across three difficulties, six
  seeds, and both modes. The 18 replay cases passed built-in legal-action
  simulation.
- All four 200-board bulk comparisons matched board encodings, 3BV values, and
  optimal click counts. Row-order JSON and pruning counters also matched in
  every timing run.
- 100 small random boards matched the independent Python exhaustive solver in
  both count-only and replay modes.
- A temporary differential C++ harness compared the actual old/new transition
  bodies on 20,000 randomized cases, checking outgoing signatures and closure
  charges for both branches. It covered empty signatures, duplicate and
  overlapping components, slot removal, zero future reach, and one-, two-,
  three-, and seven-word components. Every comparison matched.
- `git diff --check` passed.

Session snapshots, comparison scripts, focused harnesses, and executables are
in the system temporary directory under `doms-matching-20261021`. The initial
executable is saved there as `original-doms.exe`. Existing unrelated workspace
changes were preserved. `doms.exe` was updated from the validated build and
smoke-tested with an Expert replay.

## Next optimization: binary-search one-word opening absorption matches

### Plan and implementation

Opening-chain absorption compares each active opening pattern against every
component in a successor connectivity signature. One-word signatures are
already sorted, so replace that linear component scan with `lower_bound` and
skip any already-removed duplicates at the matching value. Keep the existing
word-by-word linear scan for wider signatures.

The signature is a sorted multiset, and absorption requires exact equality
with the opening's remaining border. Binary search finds the same first equal
component the old scan found; advancing across removed equal entries preserves
the old behavior when signatures or opening borders contain duplicates. Factor
checks, component removals, cost changes, and subsequent interning are
unchanged. This changes transition work only and adds no search limit or
heuristic.

### Validation and measurements

Both versions were compiled with MSYS2 UCRT64 GCC 16.1.0 using
`-std=c++17 -O3 -DNDEBUG -pthread`. The compiler runtime directory must be on
`PATH` when compiling and running these builds.

Four sequential runs, alternating versions, solved the same 200 Expert boards
(seed 8675309, one thread). The median wall times were 5.185 s with binary
search and 5.300 s with the prior scan, a 2.3% reduction. All board encodings,
3BV values, and optimal click counts matched in both paired comparisons. These
measurements show a modest gain on this workload; the effect may vary with
opening absorption frequency and component counts. Wider signatures retain
the original linear scan.

The binaries, isolated prior-scan source, and raw CSV/log files are in the
system temporary directory under `doms-absorption-20261021`. The existing
workspace executable and unrelated files were left untouched.

## Avoid clearing opening-absorption component markers

Each successor that entered opening-chain absorption zero-filled a byte for
every component in its connectivity signature. This happened even when no
opening matched or no component was removed. Replaced that clear with reusable
generation stamps: each transition advances a generation, and a component is
considered removed only when its stamp matches that generation. Matching,
duplicate handling, component removal, and connectivity interning remain the
same. This is scratch bookkeeping only; it does not change any DP state,
cost, pruning certificate, or search limit.

The prior and changed sources were built with MSYS2 UCRT64 GCC 16.1.0 and
identical `-std=c++17 -O3 -DNDEBUG -pthread` flags. Two sequential paired runs
of 100 Expert boards (seed 20261021, one thread) produced identical board
encodings, 3BV values, and optimal click counts in every row. Their total solve
times were 2.609 s before and 2.513 s after in the first pair, then 2.514 s
before and 2.556 s after in the reverse-order pair. The medians were 2.562 s
and 2.535 s respectively, a 1.0% reduction; this is a small gain within likely
run-to-run variation, so treat the speed result as provisional. The changed
build also completed an Expert replay smoke check. `doms.exe` was left
untouched. Build variants and CSV/logs are in the system temporary directory
under `doms-before-stamps` and `stamps-*.csv` / `stamps-*.log`.

**Current status: excluded.** The 1.0% result was within likely run-to-run
variation. Generation stamps were removed in the combined weak-evidence
ablation; the independently measured one-word binary-search optimization
remains enabled.

## Next optimization: direct one-word connectivity transitions

### Plan and implementation

For connectivity signatures with one 64-bit word per component, construct
successors directly from those words. Previously, even this common case used
the general projection buffer, populated an offset list, and ran per-word
copying and nonempty checks before sorting the resulting words.

The specialized path clears the current candidate's bit, merges touching
components on the selected branch, counts closed components, and sorts and
interns the same component multiset. It preserves duplicate components and
the seed charge when the selected component has no future reach. With a
one-word signature, every assigned connectivity slot is below 64; an absent
slot uses a zero mask. Wider signatures retain the general transition path.
No objective, state representation, pruning rule, order, or search limit changes.

### Validation and measurements

Baseline and changed sources were built with MSYS2 UCRT64 GCC 16.1.0 and
identical `-std=c++17 -O3 -DNDEBUG -pthread` flags. The baseline snapshot
includes all working-tree optimizations present before this change.

- All non-timing JSON fields matched in 36 comparisons across Beginner,
  Intermediate, and Expert boards, six seeds, and both solve modes. The 18
  replay cases completed built-in legal-action validation.
- A temporary C++ harness extracted the actual baseline and changed
  transition bodies and compared both branches on 50,000 randomized cases.
  Cases included duplicate components, empty signatures, no future neighbors,
  absent slots, and slots 0 through 63. All signatures and closure charges
  matched. The harness used standard-library assertions; this toolchain lacks
  the UBSan runtime, so no sanitizer result is claimed.
- Expert seed 100 with explicit rows order and count-only profiling produced
  identical processed/generated state counts and every pruning counter.
  Transition time was 0.546 s before and 0.520 s after in this single pair;
  this phase measurement is illustrative rather than a repeated benchmark.
- Four alternating baseline/changed pairs each solved 500 Expert boards with
  seed 20261021 and one worker. Every board encoding, 3BV value, and optimal
  click count matched in all eight CSVs.

| Pair | Baseline wall time | Changed wall time |
| --- | ---: | ---: |
| 1 | 11.6379 s | 11.4198 s |
| 2 | 11.7907 s | 11.6902 s |
| 3 | 11.9170 s | 11.2489 s |
| 4 | 11.8631 s | 11.6902 s |
| Median | 11.8269 s | 11.5550 s |

The median decreased by 2.3%. These are local measurements on one workload;
ancillary profiling and harness compilation overlapped portions of the run,
so the exact speed gain needs confirmation under controlled machine load.
The change is retained for removing demonstrably redundant transition work
while preserving every compared state and result.

Sources, executables, comparison scripts, the randomized harness, JSON output,
and raw CSV/log files are in the system temporary directory under
`doms-word-transitions-20261021`. `doms.exe` was updated from the changed build
and completed an Expert replay smoke check. Its previous version is saved as
`original-doms.exe` in that directory. Existing unrelated changes were preserved.

## Next optimization: compact connectivity-signature lookup

### Plan and implementation

Replace the connectivity pool's node-based unordered-map index with a compact,
open-addressed array of 32-bit IDs. Store signatures and their cached hashes
in a deque, with a contiguous pointer array for fast ID lookup. This avoids
separate hash-node allocation and repeated hashing during index growth while
preserving references across insertion and rehashing. Grow only for new
signatures, keep the index at most half full, and reset its logical size at
each layer. The index has no tombstones because signatures are never erased
within a layer.

Full vector equality still decides identity, including under hash collisions.
Insertion order still assigns IDs, the empty signature remains ID zero, and
component multiplicities and every signature word are preserved. The state
representation, objective, candidate order, pruning rules, and search remain
unchanged. The ID-capacity check reports resource exhaustion rather than
silently overflowing into the reserved empty-slot value.

### Validation and measurements

Baseline and changed binaries used MSYS2 UCRT64 GCC 16.1.0 with identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. The baseline snapshot includes every
working-tree optimization present at the beginning of this session. Timing
runs were sequential, alternated execution order, and did not overlap other
compilation or solver jobs from this optimization session.

Four paired runs each solved the same 500 Expert boards, seed 20261021,
automatic order, count-only, one worker:

| Pair | Baseline wall time | Changed wall time |
| --- | ---: | ---: |
| 1 | 11.4183 s | 11.0245 s |
| 2 | 12.5869 s | 11.9345 s |
| 3 | 12.4465 s | 11.4440 s |
| 4 | 12.4614 s | 11.7106 s |
| Median | 12.4539 s | 11.5773 s |

The median time decreased by **7.0%**, with improvement in all four pairs.
Four alternating pairs on Expert seed 100, explicit rows order, count-only
with profiling, had medians of 1.1121 s before and 1.0537 s after, a **5.3%**
reduction. These are local workload measurements; machine activity and board
structure affect the gain. Memory consumption was not measured.

Validation passed:

- 100,000 randomized differential cases against the actual prior pool design,
  including duplicate and wide signatures, reserve, reset, and swapping pools.
- Forced hash collisions, repeated index growth, empty ID zero, and stable
  signature references across insertion and reserve.
- 100 small random boards matched the independent exhaustive Python solver in
  both modes, including 100 built-in legal-action replay validations.
- All non-timing JSON fields matched in 36 baseline/changed comparisons across
  three difficulties, six seeds, and both modes, including 18 replay checks.
- All non-timing JSON fields and every processed/generated state and pruning
  counter matched across eight explicit-row profile runs.
- Board encodings, 3BV values, and optimal counts matched in all eight 500-board
  timing CSVs and in a separate 1,000-board comparison using seed 8675309 and
  12 workers.

The regression test is `benchmarks/test_connectivity_pool.cpp`:

```console
g++ -std=c++17 -O2 -pthread benchmarks/test_connectivity_pool.cpp -o connectivity_pool_test
./connectivity_pool_test
```

Keep assertions enabled for this test. Raw timing values and validation counts
are recorded in `benchmarks/connectivity_pool_results.json`. Source snapshots,
executables, comparison scripts, and raw logs are in the system temporary
directory under `doms-opening-cache-20261021`. Earlier opening-match caching
and single-lookup experiments were discarded; their source snapshots remain
there for reference. Concurrent benchmark-CLI edits in the workspace were
preserved and are included in the final build.

`doms.exe` was rebuilt from the final workspace source, compared with the
measured build on an Expert replay, and smoke-tested after installation.
`git diff --check` passed. The previous executable is saved as
`original-doms.exe` in the session artifact directory.

## Next optimization: contiguous dominance survivor storage

### Plan and implementation

Every dominance pass previously reserved a separate pointer vector for each
connectivity signature. Replace these allocations with one contiguous pointer
buffer and a small list view per signature. The already computed incoming
state counts assign disjoint slices of the buffer. A signature can retain no
more survivors than its incoming states, so its slice cannot overflow.

Appending witnesses preserves the original sorted order. The buffer never
grows during pruning, so pointers remain stable. Exact, coarsening, and
compensated dominance still receive the same witness lists and retain the
same deferred deletion behavior. This changes storage allocation only, with
no change to the objective, search, pruning rules, or optimality guarantees.
The implementation applies to compact and wide masks and both solve modes.

### Benchmark results

Both binaries were built with MSYS2 UCRT64 GCC 16.1.0 using
`-std=c++17 -O3 -DNDEBUG -pthread`, without native CPU tuning. The changed
build also enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
The baseline is a snapshot of the source immediately before this change.

Four complete `--benchmark` invocations ran sequentially in the order
baseline, changed, changed, baseline. No other compilation or solver work
from this session overlapped the timing runs. Every invocation solved the
same three fixed sets of 1,000 Expert boards on one thread in count-only mode.
Every board encoding, 3BV, and optimal count matched the committed references.
These are 12,000 solves over 3,000 distinct boards.

Each cell below is total seconds for one 1,000-board set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 21.298796 | 20.572758 | 19.968219 | 22.358321 |
| 2 | 20.609504 | 20.071294 | 19.347290 | 20.583918 |
| 3 | 19.991003 | 19.957048 | 19.406504 | 19.805018 |
| Total | 61.899303 | 60.601100 | 58.722013 | 62.747257 |

Mean total benchmark time decreased from **62.323280 s to 59.661557 s**,
a **4.3%** reduction. The corresponding total reductions in the two pairs
were 2.1% and 6.4%. All six corresponding set totals improved, although one
difference was very small. Two repetitions on one machine support retaining
the change but do not establish a universal or statistically precise gain.
Memory use was not measured.

### Additional validation

- A temporary differential harness compared the actual baseline pruning
  function with the changed function on 4,000 deterministic randomized tables.
  Retained states in iteration order, costs, replay histories, removed counts,
  and all three dominance counters matched. Cases covered compact and
  eight-word factor masks, count-only and replay records with eight-word
  histories, empty/singleton tables, unused signature IDs, and one through
  three connectivity words.
- All non-timing JSON fields matched in 36 complete baseline/changed solver
  comparisons across Beginner, Intermediate, and Expert; seeds 1, 42, 100,
  2026, 123456, and 8675309; and both solve modes. All 18 changed replay
  cases passed built-in legal-action simulation, and count/replay optima
  matched for every board.
- `git diff --check` passed.

Source snapshots, binaries, raw benchmark logs including median/p5/p95,
machine-readable timing results, the differential harness, and full-solver
comparison outputs are saved under
`C:\Users\happy\AppData\Local\Temp\doms-survivors-20261021`.
`doms.exe` was updated from the measured build; its previous version is saved
there as `original-doms.exe`.

## Next optimization: contiguous component-size storage

### Plan and implementation

Dominance checking builds sorted component-size lists for live connectivity
signatures. Replace the separately growing vectors with one integer buffer.
Prefix offsets assign each signature exactly as many entries as it has
components; signatures without surviving states receive an empty slice.
Population counting, per-signature sorting, and indexed comparisons use the
same values in the same order. Minimum and maximum population filters are
unchanged. This removes repeated allocation and growth without changing any
dominance rule, candidate order, objective, or search limit. It applies to
compact and wide masks and both solve modes.

### Validation

- Baseline and changed sources compiled without diagnostics using MSYS2 UCRT64
  GCC 16.1.0 and identical `-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra
  -Wpedantic` flags. The baseline includes all working-tree changes present
  before this optimization, including contiguous survivor storage.
- A temporary differential harness extracted the actual baseline pruning
  function and compared it with the changed function on 4,000 deterministic
  randomized tables. Retained states in iteration order, costs, replay
  histories, removed counts, and all three pruning counters matched. Cases
  covered compact and eight-word factor masks, both solve modes, unused IDs,
  empty signatures/tables, and one through three connectivity words with
  nonzero high words.
- All non-timing JSON fields matched in 36 baseline/changed comparisons:
  Beginner, Intermediate, and Expert boards; seeds 1, 42, 100, 2026, 123456,
  and 8675309; and both solve modes. All 18 changed replay cases passed
  built-in legal-action validation, and replay/count-only optima matched.

### Benchmark results

Four complete `--benchmark` invocations ran sequentially in the order
baseline, changed, changed, baseline. Each invocation solved the same three
fixed sets of 1,000 Expert boards on one thread in count-only mode. No other
compilation or solver work from this session overlapped these timing runs.
All 12,000 solves matched the committed board, 3BV, and optimal-click
references. References were unchanged.

Each cell below is total seconds for one 1,000-board set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 23.839667 | 22.085089 | 21.446997 | 22.279099 |
| 2 | 22.403302 | 20.549677 | 20.477444 | 21.729241 |
| 3 | 21.272249 | 20.169525 | 20.048479 | 20.756029 |
| Total | 67.515218 | 62.804291 | 61.972920 | 64.764369 |

Mean total benchmark time decreased from **66.139794 s to 62.388606 s**,
a **5.7%** reduction. All six corresponding set totals improved. These are
two repetitions on one machine; the gain depends on workload and machine
activity. Memory consumption was not measured.

Source snapshots, binaries, the differential harness, full-solver comparison
outputs, raw benchmark logs including median/p5/p95, and `results.json` are in
`C:\Users\happy\AppData\Local\Temp\doms-populations-20261021`.
`doms.exe` was updated from the measured changed build, with its previous
version saved there as `original-doms.exe`. An installed-build Expert replay
check and `git diff --check` passed.


## Next optimization: contiguous subset-zeta updates

### Plan and implementation

Smart row/column ordering computes subset sums seven times per eligible line.
The original transform scanned every mask for each bit and branched on whether
that bit was set. Replace it with contiguous blocks: add each block's lower
half into its upper half. This visits precisely the original destination masks
in the same ascending order, with the same source and addition for every bit.
It eliminates the mask branch and enables inner-loop vectorization.

Both crossing-scope counts and old-neighbor counts use `subset_zeta`.
All ordering scores, tie breaks, chosen orders, search states, pruning rules,
and optimality guarantees are preserved. Explicit orders that do not use
smart ordering are unaffected. Existing workspace edits were preserved.

### Validation

- Both builds use MSYS2 UCRT64 GCC 16.1.0 and identical
  `-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic` flags;
  the changed build compiled without diagnostics.
- `benchmarks/subset_zeta_regression.cpp` compares against the original
  transform for every size from zero through 20 bits and 500 randomized
  arrays, including signed entries. Independent direct submask sums cover
  all masks through 10 bits, and closed-form sums cover all sizes through
  the supported 20-bit maximum. Empty input is also checked.
- All non-timing JSON fields matched in 36 complete baseline/changed solver
  comparisons over three difficulties, seeds 1, 42, 100, 2026, 123456, and
  8675309, and both solve modes. All 18 changed replays passed built-in
  legal-action validation; replay/count-only optima matched.
- Four complete `--benchmark` invocations validated 12,000 solves over the
  same 3,000 distinct boards against the unchanged committed board, 3BV,
  and optimal-click references. Every run passed.

Build the regression test with assertions enabled:

```console
g++ -std=c++17 -O2 -pthread benchmarks/subset_zeta_regression.cpp -o subset_zeta_test
./subset_zeta_test
```

### Benchmark results

Runs were sequential in baseline/changed/changed/baseline order, with no
compilation or other solver jobs from this session overlapping the timings.
Each invocation runs the three fixed 1,000-board Expert sets on one thread
in count-only mode. Each cell is total seconds for one fixed set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 21.304849 | 20.281375 | 19.679126 | 20.930389 |
| 2 | 19.811962 | 19.066739 | 19.099255 | 20.373338 |
| 3 | 19.159131 | 18.407553 | 18.572140 | 19.826737 |
| Total | 60.275942 | 57.755667 | 57.350521 | 61.130464 |

Mean total time decreased from **60.703203 s to 57.553094 s**, a
**5.2%** reduction. Both pairs improved (4.2% and 6.2%), as did all six
corresponding set totals. Two repetitions on one machine support retaining
this change; they do not establish a universal or statistically precise gain.
Memory consumption was not measured. Raw total/median/p5/p95 measurements
are recorded in `benchmarks/subset_zeta_benchmark.json`.

Two earlier experiments were discarded after full paired benchmarks:
contiguous cached-bucket entries improved mean total time by only 0.9%, with
one pair regressing; scalar one-word union keys regressed mean time by 0.8%.
Both passed differential and reference checks, but neither justified retention.
Their artifacts are under the system temporary directory in
`doms-buckets-20261021` and `doms-union-keys-20261021` respectively.

The retained optimization's source snapshots, binaries, comparison script,
JSON outputs, and raw logs are under `C:\Users\happy\AppData\Local\Temp\doms-zeta-20261021`.
`doms.exe` was updated from the measured changed build; the previous executable
is saved there as `original-doms.exe`.

The installed executable matched the measured build byte-for-byte and passed
an Expert replay smoke check. `git diff --check` passed.


## Next optimization: preclassify scope sweep extents

### Plan and implementation

Smart row/column ordering used to scan every scope's candidates for every
sweep line. Cache each candidate's sweep-line number, then compute each
scope's first and last sweep lines once per ordering invocation.

Scopes strictly outside the current line contribute zero at every internal
cut and can be skipped. Scopes extending to both earlier and later lines
contribute one at every cut, so increment the always-crossing count directly.
Only scopes touching their first or last line need the original detailed scan.
Empty scopes remain ignored. The subset transforms and cut-score DP are
unchanged. Every score, tie break, chosen order, solver state, and pruning rule
is preserved. Both sweep directions and both orientations use the cache;
explicit orders without smart ordering are unaffected. Additional memory is
one integer per candidate and two integers per scope, local to each invocation.
Existing workspace changes, including the CSV reference loader, are preserved.

Three earlier experiments were discarded after four full `--benchmark`
invocations each: combined closed-vertex counts averaged 2.3% slower,
per-cut score caching averaged 2.1% slower, and candidate-line caching alone
averaged 1.2% slower. All 36,000 reference-validated solves passed. Another
`doms.exe` process overlapped part of the score-cache timing runs, so those
measurements do not isolate its performance effect. None was retained alone.
Their source snapshots, tests, and raw logs remain outside the repository
under `doms-live-counts-20261021`, `doms-order-scores-20261021`, and
`doms-cached-lines-20261021` in the system temporary directory.

### Handoff status ? stopped at user request, 2026-10-21

**This is an unfinished experiment, not a measured speed improvement.** The
scope-extents implementation remains in `minesweeper_chord_dp.cpp`, in
`smart_line_order_indices` (candidate `sweep_lines`, `scope_extents`, and the
`crossing_counts` fast classifications). Neither closed-vertex counting nor
per-cut score caching remains in the source. Candidate-line caching remains
only as part of this broader scope-extents experiment.

Completed validation for the current implementation:

- Compiled without diagnostics using MSYS2 UCRT64 GCC 16.1.0 and
  `-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic`.
- 240 complete smart orders matched the unmodified baseline, covering three
  difficulties, ten generated boards each, original and reduced models,
  row and column orientations, and both sweep directions.
- All non-timing JSON fields matched in 36 baseline/changed solver comparisons:
  Beginner, Intermediate, Expert; seeds 1, 42, 100, 2026, 123456, 8675309;
  count-only and replay. All 18 changed replays passed built-in legal-action
  validation.
- The first baseline `--benchmark` had just started when the user requested
  this handoff. It was interrupted. There are **no completed benchmark timing
  results for the scope-extents implementation**. No solver/test process from
  this session remains running.
- This session did not replace the repository's `doms.exe` with an experiment
  build. Do not assume that executable contains the scope-extents change.

Artifacts for the current experiment are in
`C:\Users\happy\AppData\Local\Temp\doms-scope-extents-20261021`:
`baseline.cpp`, `baseline.exe`, `changed.cpp`, `changed.exe`,
`order_differential.cpp`, `order_differential.exe`, `measure.py`, and
`summarize.py`. The current source matched `changed.cpp` byte-for-byte at
handoff. The baseline includes the existing solver optimizations and the
CSV benchmark-reference loader, but excludes candidate-line caching and
scope-extents classification. Both executables use identical compiler flags.

`measure.py` reruns the 36 full-solver comparisons, then executes four complete
`--benchmark` invocations sequentially in baseline/changed/changed/baseline
order, saving logs and `raw_results.json` in that temporary directory.
`summarize.py` writes `results.json` there. Run from the repository root so
`benchmarks/expert_benchmark_reference.csv` can be opened:

```powershell
python "$env:TEMP/doms-scope-extents-20261021/measure.py"
python "$env:TEMP/doms-scope-extents-20261021/summarize.py"
```

Before resuming, inspect `AGENTS.md`, the working tree, and source snapshots:
other work was occurring in this repository during the session. In particular,
the CSV reference migration, README changes, `.gitignore`, `AGENTS.md`, tests,
and existing solver optimizations were not introduced by this experiment and
must be preserved. Do not regenerate reference answers. Keep temporary
scripts, binaries, logs, and result JSON outside version control per `AGENTS.md`.
Avoid overlapping timing runs with other solver or compilation jobs.

Retain the scope-extents change only if further measurements support it.
If rejecting it, revert only the sweep-line cache, extent construction, and
`crossing_counts` changes introduced here; use the temporary baseline for a
focused comparison rather than overwriting concurrent edits. If retaining it,
rebuild `doms.exe` from the final source, run an Expert replay smoke check,
check `git diff --check`, and document the actual benchmark measurements.

For context, the completed rejected experiments had these mean sums of the
three fixed 1,000-board benchmark set totals (two invocations per variant):

| Experiment | Baseline mean seconds | Changed mean seconds | Outcome |
| --- | ---: | ---: | --- |
| Combined closed-vertex counts | 55.065695 | 56.323052 | Discarded; 2.3% slower |
| Cached per-cut scores | 57.976058 | 59.200909 | Discarded; 2.1% slower; concurrent solver activity |
| Cached candidate lines alone | 56.132824 | 56.825385 | Discarded alone; 1.2% slower |

All four full `--benchmark` invocations passed for each experiment: 36,000
validated solves over the same 3,000 distinct Expert boards across the three
experiments. These timings do not measure the current scope-extents change.
The first two experiments used an older baseline executable with compiled-in
references; their changed score-cache build included the concurrent CSV-loader
migration. The candidate-line and scope-extents experiments use the same
CSV-loader code in both variants. This further limits interpretation of the
earlier small timing differences.

### Completed scope-extents benchmark

The scope-extents experiment was resumed and measured sequentially in
baseline/changed/changed/baseline order using the archived binaries and the
CSV reference data. Four complete `--benchmark` invocations passed, validating
12,000 solves across the same 3,000 boards. Mean total time was 55.981259 s
for baseline and 54.697802 s for changed, a 2.3% reduction. Set totals were
faster in all six changed-versus-baseline comparisons. This is a modest local
gain from two repetitions; machine activity can affect timings.

The 36 complete JSON comparisons also passed, including 18 legal replay
simulations, and 240 smart-order comparisons matched the original orders. The
working source matched the measured changed snapshot by SHA-256. Raw runs and
logs are in `C:\Users\happy\AppData\Local\Temp\doms-scope-extents-20261021`.

## Connectivity matching: reject unrepresented fine components early

The inline (up to 64 components) path of `connectivity_coarsens` previously
built the entire eligibility graph before checking whether every fine
component had at least one containing coarse component. Build one fine
component's eligibility column at a time and reject as soon as its column is
empty, matching the existing general path's early rejection strategy.

An empty column makes matching impossible. For comparisons that pass this
check, every eligibility bit is identical to the original graph, and the
matching code and traversal order are unchanged. No pruning certificate,
search limit, state order, objective, or replay choice changes. Earlier working
tree modifications and the benchmark reference data were preserved.

### Validation

- MSYS2 UCRT64 builds used identical `-std=c++17 -O3 -DNDEBUG -pthread
  -Wall -Wextra -Wpedantic` flags and compiled without diagnostics.
- A temporary differential harness compared the actual original function with
  the changed function on 90,000 deterministic randomized signature pairs,
  including 13,334 accepted relationships. It covered one through three words,
  empty signatures, overlapping reaches, constructed containments, and 64- and
  65-component cases. Assertions were enabled; every result matched.
- All non-timing JSON fields matched in 36 full-solver comparisons across
  Beginner, Intermediate, and Expert; seeds 1, 42, 100, 2026, 123456, and
  8675309; count-only and replay modes. All 18 replays passed built-in
  legal-action simulation.
- Four sequential full `--benchmark` invocations passed all fixed board, 3BV,
  and optimal-click references: 12,000 solves over 3,000 distinct Expert boards.

### Measurements

Runs were sequential in baseline/changed/changed/baseline order, without other
solver or compilation jobs from this session overlapping the measurements.
Each cell is the total seconds for one fixed 1,000-board Expert set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.004070 | 18.404164 | 18.597040 | 18.286924 |
| 2 | 18.462028 | 17.573956 | 17.865946 | 17.630166 |
| 3 | 17.548591 | 16.974183 | 17.304989 | 17.364964 |
| Total | 54.014689 | 52.952303 | 53.767975 | 53.282054 |

Mean total time decreased from **53.648372 s to 53.360139 s (0.5%)**.
One pair improved and the other regressed; this is within observed timing
variation and does **not demonstrate a reliable end-to-end speedup**. The
retained change skips unnecessary graph construction for impossible matches;
larger overall speed gains remain unproven. Memory consumption was not measured.

Source snapshots, binaries, the differential harness, all CLI comparison
outputs, raw results, and benchmark logs are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-coarsens-early-20261021`.
The measured changed executable was installed as `doms.exe`, checked by SHA-256,
and passed an Expert replay smoke check. Its previous version is archived as
`original-doms.exe`. `git diff --check` passed.

**Current status: excluded.** The 0.5% result did not demonstrate a reliable
end-to-end speedup. This early rejection was removed in the combined
weak-evidence ablation.

## Candidate reduction: enumerate eligible replacement neighbors

Swap reduction previously scanned every candidate as a possible replacement,
then immediately rejected non-neighbors. Enumerate the victim's propagation
bitset instead, visiting words and set bits in ascending candidate order.
This preserves every eligible comparison, its order, the first replacement,
and all sequential deletion choices. The certificate, exact objective, DP,
and search limits are unchanged. The enumeration overhead per victim changes
from O(q) to O(ceil(q / 64) + degree); certificate checks are unchanged.

An earlier experiment assigning free anchors before augmenting-path matching
was discarded because full benchmarks did not establish a clear overall gain.
That experiment is absent from the retained source.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build also passed
  `-Wall -Wextra -Wpedantic` without diagnostics.
- An assertion-enabled temporary differential harness compared the actual old
  and new reduction functions on 3,000 deterministic boards, 1,000 per standard
  difficulty. Every reduced model field and both deletion counts matched.
- All non-timing JSON fields matched in 36 full-solver comparisons across
  three difficulties, six seeds, and count-only/replay modes. All 18 replays
  passed built-in legal-action simulation.
- Four sequential `--benchmark` runs in baseline/changed/changed/baseline
  order passed all fixed board, 3BV, and optimal-click references: 12,000
  solves across the same 3,000 distinct Expert boards. No compilation or
  other solver jobs from this session overlapped these four timing runs.
- `git diff --check` passed. `doms.exe` is the measured changed build.

### Measurements

Each cell is total seconds for a fixed 1,000-board Expert set on one thread:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 19.080375 | 18.156901 | 18.080949 | 18.818463 |
| 2 | 18.431692 | 17.792469 | 17.389081 | 18.673921 |
| 3 | 17.537436 | 17.012640 | 16.979660 | 17.611917 |
| Total | 55.049503 | 52.962010 | 52.449690 | 55.104301 |

Mean total runtime decreased from **55.076902 s to 52.705850 s (4.3%)**.
Both changed runs and all corresponding set totals improved. These are local
measurements from two repetitions on one machine; gains remain workload-
dependent and memory use was not measured.

Baseline source and executable, discarded matching experiment, differential
harnesses, comparison scripts, and raw logs/results are outside version
control under `C:\Users\happy\AppData\Local\Temp\doms-matching-20261021`.
The retained experiment's logs have the `neighbor-` prefix.

**Current status: excluded.** The 0.7% difference was within timing variation
and did not establish a material speedup. Candidate reduction again scans
replacement candidates in their original order.

## Dominance setup: scalar keys for one-word connectivity unions

### Plan and implementation

Dominance groups connectivity signatures by their total future reachability.
For one-word signatures, the previous code allocated a one-element vector
for every live signature and used vector keys in its union hash map. Use a
`uint64_t` union and scalar-key map for this case, avoiding those temporary
allocations and the vector stored in each map entry. Keep the existing vector
key path for multiword signatures.

Scalar equality is exactly one-element-vector equality. Both paths assign
bucket IDs on first occurrence while scanning signature IDs in ascending order;
neither iterates the union map. Thus bucket IDs, entry order, sorting, witness
order, pruning certificates, and retained states are unchanged. This introduces
no heuristic or search limit. The baseline includes and preserves the existing
working-tree candidate-neighbor enumeration optimization.

### Validation

- Both executables used MSYS2 UCRT64 GCC 16.1.0 and identical
  `-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
  additionally enabled `-Wall -Wextra -Wpedantic` and emitted no diagnostics.
- All non-timing JSON fields and profile state/pruning counters matched in
  36 baseline/changed comparisons across three standard difficulties, seeds
  1, 42, 100, 2026, 123456, and 8675309, and count-only/replay modes. All 18
  replay cases passed built-in legal-action simulation.
- An assertion-enabled temporary harness compared the actual baseline and
  changed pruning functions on 2,400 seeded randomized tables. All four
  compact/wide-mask and count-only/replay specializations were exercised with
  one through three connectivity words. Cases included common-union buckets
  large enough to exercise pair postings, overlapping component reaches, empty
  signatures, and seven-word factor masks and chosen histories. Survivor order,
  states, costs, histories, and all three pruning counters matched exactly.
- Four sequential full `--benchmark` invocations validated 12,000 solves over
  the same 3,000 distinct Expert boards against unchanged board, 3BV, and
  optimal-click references. Every invocation passed.
- `git diff --check` passed. The measured changed build is installed as
  `doms.exe`.

### Measurements

Measured runs were sequential in baseline/changed/changed/baseline order,
without overlapping compilation or other solver jobs from this session.
Preliminary and interrupted runs were excluded. Each cell below is total
seconds for one fixed 1,000-board Expert set on one thread:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.450707 | 17.729635 | 17.373478 | 19.069482 |
| 2 | 17.775356 | 16.898258 | 17.047473 | 18.734084 |
| 3 | 16.879637 | 16.212125 | 17.062855 | 17.321418 |
| Total | 53.105700 | 50.840018 | 51.483806 | 55.124984 |

Mean total runtime decreased from **54.115342 s to 51.161912 s (5.5%)**.
Both changed totals were below both baseline totals. These are local results
from two repetitions per build; machine activity affects timing, and gains
remain workload-dependent. Memory use was not measured.

Baseline source, original executable, measured builds, temporary harness and
validation scripts, comparison outputs, and raw logs/results are outside
version control in
`C:\Users\happy\AppData\Local\Temp\doms-scalar-union-20261021`.

## Connectivity matching: initialize only live scratch entries

### Plan and implementation

The common connectivity-coarsening matcher uses two stack arrays with capacity
for 64 components. Initialize only `coarse_count` eligibility entries and
`fine_count` matching entries, replacing full-capacity clearing. Eligibility
is indexed only by coarse components; the augmenting-path search indexes
matching entries only by bits set for fine components. Thus every read still
has its original initialized value. Matching order, eligibility, dominance
certificates, and search limits are unchanged. The general path for more than
64 components is unchanged. Existing working-tree changes were preserved in
the baseline.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build additionally
  enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
- A temporary differential harness compared the actual old and new matching
  functions on 100,000 deterministic cases, with one through three signature
  words and zero through 66 components. All results matched, including the
  boundary between stack and general storage.
- All non-timing JSON fields matched in 36 complete solver comparisons:
  three standard difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309,
  and count-only/replay modes. All 18 replays passed built-in simulation.
- Four sequential full `--benchmark` invocations passed all unchanged board,
  3BV, and optimal-click references: 12,000 solves over 3,000 distinct boards.
  Compilation and other solver jobs from this session did not overlap them.
- `git diff --check` passed. `doms.exe` is the measured changed build.

### Measurements

Runs were baseline/changed/changed/baseline; seconds per fixed 1,000-board set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.755798 | 17.758627 | 18.405119 | 18.006809 |
| 2 | 17.724715 | 17.080053 | 17.655041 | 17.265003 |
| 3 | 17.040404 | 16.808487 | 17.159585 | 16.807114 |
| Total | 53.520917 | 51.647167 | 53.219745 | 52.078926 |

Mean total runtime decreased from **52.799922 s to 52.433456 s (0.7%)**.
The first pair improved and the second regressed; this difference is within
observed timing variation. The change eliminates redundant initialization,
but these measurements do not establish a material overall speedup. Memory
use was not measured. Source snapshot, executables, temporary harness,
comparison script, and raw logs are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-matching-init-20261021`.

**Current status: excluded.** The 0.7% gain was inconsistent and not material
end-to-end. Full matcher scratch-array initialization is restored.


## Smart ordering: keep the subset winner local

### Plan and implementation

The smart-line subset DP previously updated its score and predecessor arrays
whenever it found a better predecessor. Keep the winning score and bit in local
variables, then write each array once per subset. Compute the subset's graph,
factor, total, and estimated-work costs once outside its predecessor loop.
Predecessors are still visited in the same ascending-bit order with the same
strict comparison, including saturated work and ties. Every score and complete
order is preserved. No objective, dominance certificate, or search limit changes.
Existing workspace changes were preserved in the baseline snapshot.

An equal-cost factor-dominance bit-test experiment was evaluated first and
removed: its two changed benchmark runs averaged about 1.4% slower than baseline.
It is not part of the retained implementation.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build also enabled
  `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
- An assertion-enabled temporary harness compared the actual original and
  changed smart-order functions. All 1,920 complete orders matched: 180 seeded
  standard boards (60 per difficulty), before and after reduction, plus 120
  synthetic scope/graph models, each in both orientations and sweep directions.
  Synthetic cases exercised ties, saturated work, empty/singleton lines, the
  20-candidate exact subset limit, and the wider-line fallback.
- All non-timing JSON fields matched in 36 full-solver comparisons across three
  difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309, and both solve modes.
  All 18 changed replays passed built-in legal-action simulation.
- Four sequential full `--benchmark` invocations passed all unchanged board,
  3BV, and optimal-click references: 12,000 solves over 3,000 distinct boards.
  Compilation and other solver jobs from this session did not overlap timing.

### Measurements

Runs were baseline/changed/changed/baseline; seconds per fixed 1,000-board set:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 17.170677 | 17.315589 | 16.229758 | 17.030711 |
| 2 | 16.660881 | 16.046551 | 15.464903 | 16.505660 |
| 3 | 15.986088 | 16.168430 | 15.129523 | 15.961244 |
| Total | 49.817646 | 49.530570 | 46.824184 | 49.497615 |

Mean total runtime decreased from **49.657630 s to 48.177377 s (3.0%)**.
Both changed totals were below both baseline totals, but the first pair improved
only slightly and the second pair improved much more. These are local results
from two repetitions per build with visible timing variation; gains remain
workload-dependent. Memory use was not measured.

Baseline source and executable, original `doms.exe`, the discarded experiment,
temporary differential harness and validation script, comparison outputs, and
raw logs/results are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-zero-penalty-20261021`.
The retained experiment's benchmark logs have the `order-` prefix.
`doms.exe` is the measured changed build (verified by SHA-256) and passed an Expert seed-100 replay smoke check. `git diff --check` passed.


## Candidate reduction: decide the deletion threshold directly

### Plan and implementation

The left-click dominance proof previously computed the maximum independent-set
score even when a branch's upper bound was already below the deletion threshold.
Its caller needs only to decide whether any score exceeds that threshold.
Prune against the threshold directly and remove the running maximum. Also count
base factors before building the local proof graph: a negative threshold has
an empty-set counterexample, and a neighborhood no larger than the threshold
cannot exceed it. Both cases now avoid unnecessary local graph construction.
The existing conservative 63-neighbor and 63-base limits are preserved.

For a search node, any completion has score at most
`chosen + popcount(available) - popcount(covered)`: it cannot select more than
all available neighbors, and existing coverage cannot decrease. Therefore an
upper bound at or below the threshold proves that branch safe. Every returned
certificate is identical to the original function. Candidate deletion order,
DP states, objective, dominance certificates, search limits, and replay behavior
are unchanged. Existing workspace edits were preserved in the baseline.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build also enabled
  `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
- An assertion-enabled temporary harness compared the actual baseline and
  changed proof functions in 179,246 decisions. It included 20,000 synthetic
  graphs with independently exhaustive independent-set maxima, each at five
  private-mine counts; all candidates in 300 seeded standard boards before
  and after reduction; and explicit degree/base-count cases around 63/64,
  including multiple storage words. All decisions matched.
- All non-timing JSON fields matched in 36 complete solver comparisons across
  three difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309, and both
  count-only and replay modes. All 18 replays passed built-in simulation.
- Four final sequential full `--benchmark` invocations validated 12,000 solves
  over the same 3,000 distinct Expert boards against unchanged board, 3BV,
  and optimal-click references. No compilation or other solver jobs from this
  session overlapped these timing runs.
- Four earlier benchmark invocations also passed. Those evaluated the initial
  threshold-only change and are excluded from the final measurements below.
- `git diff --check` passed. `doms.exe` contains the measured combined change.

### Measurements

Final runs were baseline/changed/changed/baseline. Seconds per fixed 1,000-board
Expert set, on one thread in count-only mode:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 16.821743 | 16.870489 | 16.593257 | 16.721542 |
| 2 | 16.223792 | 15.916200 | 15.966691 | 16.007069 |
| 3 | 15.545817 | 15.675236 | 15.625386 | 15.503000 |
| Total | 48.591352 | 48.461925 | 48.185334 | 48.231611 |

Mean total time decreased from **48.411482 s to 48.323630 s (0.18%)**.
Both adjacent pairs improved slightly, but individual sets were mixed. This
is within observed timing variation and does **not** establish a material
end-to-end speedup. In the instrumented standard-board differential cases,
proof-search node visits fell from 1,196,950 to 1,180,948 (1.3%); on synthetic
cases they fell from 1,482,305 to 377,993 (74.5%). These counts describe the
focused harness, not the fixed Expert benchmark. Memory use was not measured.
The retained change avoids redundant proof work, with limited measured impact
on total runtime.

Baseline source, original executable, measured baseline, temporary harnesses,
comparison scripts, and raw logs/results are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-threshold-20261021`.
Final timing logs use the `baseline-3`, `changed-3`, `changed-4`, and `baseline-4`
prefixes. Earlier `-1` and `-2` logs cover the threshold-only experiment.

**Current status: excluded.** The 0.18% overall difference did not establish a
material speedup. The original maximum-score proof search is restored.


## Smart ordering: fold vertex closures into the zero-scope transform

### Plan and implementation

The zero-scope crossing score subtracts a subset sum at the selected mask.
The live-vertex score also subtracts a subset sum at the same mask: old
vertices close when all their remaining neighbors have been processed, and
current-line vertices close when their closed neighborhood has been processed
and they have no later-line neighbors. Insert both kinds of vertex closures
into the zero-scope before histogram, before its subset transform. Add the
old-vertex constant and selected-vertex population when producing graph scores.

By linearity, transforming the combined histogram subtracts exactly the same
scope and vertex counts as the two separate transforms. This removes one of
five transforms per eligible line, its zero-filled integer buffer, and the
separate graph-score update pass. The subset DP, all cut scores, tie breaks,
complete orders, candidate reductions, exact search, pruning certificates, and
search limits are preserved. Existing workspace edits were included in the
baseline snapshot and preserved.

### Validation

- Baseline and changed builds used MSYS2 UCRT64 GCC with identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build additionally
  enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
- An assertion-enabled temporary harness compared the actual baseline and
  changed smart-order functions: all 1,936 complete orders and 1,277,387,968
  graph/factor cut-score entries matched. Coverage included 180 seeded
  standard boards before and after reduction, 120 synthetic graph/scope
  models, both orientations and sweep directions, empty and singleton lines,
  the 20-candidate exact-subset limit, and the 21-candidate fallback.
- All non-timing JSON fields matched in 36 baseline/changed full-solver
  comparisons across three standard difficulties, seeds 1, 42, 100, 2026,
  123456, and 8675309, and both solve modes. All 18 changed replays passed
  built-in legal-action simulation.
- Four sequential full `--benchmark` invocations passed all unchanged board,
  3BV, and optimal-click references: 12,000 solves over 3,000 distinct Expert
  boards. No compilation or other solver jobs from this session overlapped
  the timing runs.
- `git diff --check` passed. `doms.exe` is the measured changed build.

### Measurements

Runs were baseline/changed/changed/baseline. Seconds per fixed 1,000-board
Expert set, on one thread in count-only mode:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 17.389895 | 17.418298 | 17.932518 | 18.243635 |
| 2 | 16.884031 | 16.973652 | 17.424523 | 17.944907 |
| 3 | 16.647934 | 17.183833 | 17.030785 | 16.956516 |
| Total | 50.921860 | 51.575783 | 52.387826 | 53.145058 |

Mean total runtime decreased from **52.033459 s to 51.981805 s (0.10%)**.
The first pair regressed and the second improved; machine timing drift is
larger than the average difference. These measurements do **not** establish a
material overall speedup. The retained change eliminates redundant transform
work while preserving every ordering score and exact search result. Peak
memory consumption was not measured; eliminating an allocation does not imply
that overall peak storage decreased.

Source snapshots, the previous executable, measured baseline, differential
harness, full-solver comparison script and outputs, and raw benchmark logs are
outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-fused-closures-20261021`.

**Current status: excluded.** The 0.10% result did not establish a material
speedup. Vertex closures are scored separately from the zero-scope transform.

## Combined ablation of weak-evidence optimizations

Removed the optimizations whose individual plan results were provisional or
within timing variation: deferred hash-table growth, completed-layer lookup
index release, opening-absorption generation stamps, early rejection in
connectivity coarsening, propagation-neighbor enumeration during candidate
reduction, live-only initialization in the connectivity matcher, direct
threshold pruning, and folding vertex closures into the zero-scope transform.
The original opening-absorption scan, full matcher initialization, ordinary
candidate scan, indexed layer erasure, and separate closure scoring are
restored. Changes with repeatable measured gains remain enabled.

The all-features source was saved before removal. Both builds used MSYS2
UCRT64 GCC with `-std=c++17 -O3 -DNDEBUG -pthread`. Runs were sequential in
all/removed/removed/all order. Each invocation ran the same three fixed
1,000-board Expert sets in single-thread count-only mode and validated every
board encoding, 3BV value, and optimal click count against the tracked
reference data.

| Version | Run 1 total (s) | Run 2 total (s) | Mean (s) |
| --- | ---: | ---: | ---: |
| All features | 47.682944 | 51.657843 | 49.670394 |
| Weak-evidence features removed | 48.915017 | 50.402885 | 49.658951 |

The reduced version was 0.02% faster by the two-run mean, which is below the
observed run-to-run variation. The paired runs disagree: the full version was
faster in the first pair, and the reduced version was faster in the second.
This combined workload gives no measurable overall speed benefit for keeping
the removed features. All four invocations passed all 3,000 references.

The all-features source snapshot, both executables, and four benchmark logs
are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-weak-ablation-20261021`.

## Dominance setup: flat lookup for one-word reachability unions

### Plan and implementation

A fresh Expert seed 100 profile attributed 0.003 s of a 0.022 s solve to
dominance setup. One-word signatures used a temporary `unordered_map` to
assign buckets by the union of their component reaches. Replace its node
allocations with a flat slot array holding bucket IDs and a dense vector of
full 64-bit union keys. Hash with the existing mixer and resolve collisions
by linear probing and full-key equality. Size the index for at most half
occupancy using the incoming connectivity count, with overflow checking.
The multiword union map remains the same.

Buckets are still assigned in incoming signature order. The lookup index is
never iterated to choose comparison order. Thus bucket IDs, state order,
sorted witnesses, pruning decisions, and replay tie breaks are preserved.
There are no changes to the objective, pruning certificates, schedule, or
search limits. Existing workspace changes were included in the baseline.

### Validation

- Both solver builds used MSYS2 UCRT64 GCC and identical
  `-std=c++17 -O3 -DNDEBUG -pthread` flags.
- Two retained-version and two bracketing baseline `--benchmark` invocations
  each passed all 3,000 fixed board, 3BV, and optimal-click references.
- All non-timing JSON fields and processed/generated-state and pruning
  counters matched in 36 comparisons across three standard difficulties,
  six seeds, and both count-only and replay modes. All 18 changed replays
  passed the solver's built-in action validation.
- A temporary differential harness compared the actual baseline and changed
  pruning functions on 2,000 seeded tables across all four compact/wide-mask
  and count-only/replay specializations, with one through three connectivity
  words. Survivors, costs, chosen histories, and each pruning counter matched.
  Cases included empty signatures, duplicate unions, singleton buckets, and
  deliberately colliding scalar keys. Wide masks included bits above 64 and
  chosen histories used heap storage. The harness compiled with
  `-std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic` without diagnostics.

### Measurements

All benchmark invocations ran sequentially, with no compilation or other
solver jobs from this session overlapping timing. Each cell is seconds for
one fixed 1,000-board Expert set, one thread, count-only:

| Fixed set | Baseline before | Changed 1 | Changed 2 | Baseline after |
| --- | ---: | ---: | ---: | ---: |
| 1 | 17.738838 | 16.803126 | 16.477592 | 18.439407 |
| 2 | 17.112707 | 16.366381 | 17.018925 | 17.626944 |
| 3 | 16.078824 | 16.018189 | 16.694723 | 17.345220 |
| Total | 50.930369 | 49.187696 | 50.191240 | 53.411571 |

Mean total runtime decreased from **52.170970 s to 49.689468 s (4.8%)**.
Both neighboring baseline/changed pairs improved, as did all six corresponding
set totals. These are local measurements from two repetitions per version;
the baseline drift shows machine activity affects the result. Peak memory
was not measured. `doms.exe` is the measured retained build.

Earlier trials of three-candidate pruning, software population counts,
runtime hardware population counts, singleton-bucket skipping, and a
zero-penalty dominance shortcut passed references but did not establish a
speed benefit. Those changes were discarded. An additional initial baseline
took 50.215280 s; it is excluded from the two bracketing-run mean above.

Source snapshots, the baseline executable, temporary differential harness,
comparison script, and raw logs are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-prune-cadence-20261021`.

## Experiment: direct index for dominance pair postings

Replace the hash map for one-word coarsening pair postings with a 4096-slot
integer index and dense posting vectors, allocated only for eligible buckets
(at least 64 signatures). Keys encode two boundary positions as `(a << 6) | b`.
The posting contents, candidate selection order, and all dominance certificates
remain identical. Multiword signatures use the existing path. No objective,
search limit, or pruning schedule changes.

Compare identical optimized MSYS2 UCRT64 builds in sequential
baseline/changed/changed/baseline full `--benchmark` runs. Validate reference
counts, representative complete outputs and replay simulations. Retain only
if measurements establish a speed improvement; otherwise restore the source
snapshot and stop. Existing workspace edits are included in the baseline.

### Results: retained

Both builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build also
used `-Wall -Wextra -Wpedantic` and compiled without diagnostics.

Six sequential full benchmark invocations ran in
baseline/changed/changed/baseline/changed/baseline order. No compilation or
other solver jobs from this session overlapped the timing runs. Each cell
below is seconds for one fixed 1,000-board Expert set, single-thread,
count-only:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 | Changed 3 | Baseline 3 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 18.843277 | 18.271297 | 17.694449 | 18.758690 | 17.470948 | 17.998452 |
| 2 | 17.600979 | 17.583820 | 17.028529 | 17.828358 | 16.859608 | 17.070656 |
| 3 | 16.733105 | 17.062245 | 16.639988 | 16.491319 | 16.180903 | 16.595932 |
| Total | 53.177361 | 52.917362 | 51.362966 | 53.078367 | 50.511459 | 51.665040 |

Mean total time decreased from **52.640256 s to 51.597262 s (1.98%)**.
All three neighboring pairs improved overall (0.49%, 3.23%, and 2.23%).
Seven of nine paired set totals improved; the third set regressed in the
first two pairs. This is a modest local improvement from three repetitions
per build, with visible timing variation, rather than a gain on every board.
Peak memory was not measured; the direct index costs 16 KiB per eligible
bucket in addition to dense posting-vector storage, replacing hash nodes.

Validation completed successfully:

- All six benchmark invocations passed unchanged board, 3BV, and optimal-click
  references: 18,000 solves over the same 3,000 distinct Expert boards.
- A temporary differential harness extracted the actual baseline pruning
  function and compared 2,000 seeded randomized tables across compact/wide
  masks, count-only/replay records, and one through three signature words.
  Large common-union buckets exercised pair postings and boundary bits through
  bit 63. Survivors, costs, chosen histories, and all pruning counters matched.
- All non-timing JSON fields and processed/generated-state and pruning counters
  matched in 36 complete solver comparisons: three standard difficulties,
  seeds 1, 42, 100, 2026, 123456, and 8675309, and both solve modes. All 18
  changed replays passed built-in legal-action validation.

Source snapshots, the previous executable, measured baseline and changed
builds, temporary harnesses, comparison script, and raw timing logs/results
are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-pair-index-20261021`.
`doms.exe` is the measured retained build. `git diff --check` passed.


## Experiment: specialize one-word connectivity comparisons

Compile fixed-one-word versions of connectivity coarsening and common-merge
penalty calculations, dispatching to them when `signature_words == 1`.
Keep the same algorithms and runtime-width fallback for larger signatures.
The fixed width allows removal of inner word loops, runtime divisions, and
index multiplications. Exact quotient already has a scalar path and is
unchanged. Matching order, certificates, pruning schedule, objectives, and
search limits are unchanged. Include all existing workspace edits in baseline.

Compare identical MSYS2 UCRT64 `-std=c++17 -O3 -DNDEBUG -pthread` builds using
sequential baseline/changed/changed/baseline `--benchmark` invocations.
Compare the actual original relation functions on seeded random signatures,
including multiword and greater-than-64-component cases. Compare complete
solver outputs and state/pruning counters across difficulties and solve modes.
Retain only if a measurable overall speed improvement is established;
otherwise restore this experiment's source snapshot, record results, and stop.


### Results: retained

Both builds used MSYS2 UCRT64 GCC with identical optimization flags. The
changed build additionally enabled `-Wall -Wextra -Wpedantic` and compiled
without diagnostics. All four benchmark invocations ran sequentially, with
no compilation or other solver jobs from this session overlapping timing.
Each set contains 1,000 fixed Expert boards, one thread, count-only:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 17.915614 | 17.570839 | 17.408438 | 17.803069 |
| 2 | 17.342171 | 16.890086 | 16.954896 | 17.321863 |
| 3 | 16.713121 | 16.457434 | 16.429458 | 16.647511 |
| Total | 51.970906 | 50.918359 | 50.792792 | 51.772443 |

Mean total time decreased from **51.871674 s to 50.855576 s
(1.96%)**. Both adjacent pairs improved overall,
as did all six corresponding set totals. This is a modest local gain from
two repetitions per build on one machine; timing variation remains and this
is not evidence of improvement on every board. Peak memory was not measured.

Validation:

- All four complete `--benchmark` runs passed unchanged board, 3BV, and
  optimal-click references: 12,000 solves across 3,000 distinct Expert boards.
- A temporary assertion-enabled harness compared the actual original and
  changed coarsening and merge-penalty functions on 60,000 seeded signature
  pairs. It exercised one through three words, empty signatures, overlapping
  reaches, bit 63, containment, and component counts beyond 64 (including
  the merge routine's 128-total-component limit). Both results always matched.
- All non-timing JSON fields and state/pruning counters matched across 36
  complete solver comparisons: three difficulties, seeds 1, 42, 100, 2026,
  123456, and 8675309, and both count-only and replay modes. All 18 changed
  replays passed built-in legal-action validation.

Sources, the original executable, measured builds, temporary relation harness,
comparison script, solver comparison result, and raw benchmark logs/results
are outside version control under
`C:\Users\happy\AppData\Local\Temp\doms-word-specialization-20261021`.
`doms.exe` is the measured retained build. `git diff --check` passed.

## Experiment: short-circuit smart-order predecessor scores

Evaluate predecessor scores in lexicographic field order, rejecting a losing
prefix before calculating later maxima and saturated work. Preserve strict
comparison, ascending predecessor-bit order, saturation, and every chosen
order. No objective, pruning certificate, or search limit changes. Include
all existing workspace edits in the baseline.

Build with identical MSYS2 UCRT64 C++17 optimization flags. Compare complete
smart orders directly, including synthetic models and saturation cases, and
compare full solver outputs in count-only and replay modes. Measure sequential
baseline/changed/changed/baseline full `--benchmark` runs. Retain only on a
measurable overall improvement; otherwise restore this experiment's source
snapshot, record the result, and stop. Temporary artifacts are under
`%TEMP%\doms-score-shortcircuit-20261021`.

### Results: reverted; no reliable speed improvement

Baseline and changed builds used MSYS2 UCRT64 with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
also used `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
All timing runs were sequential; no compilation or other solver jobs from
this session overlapped them. An additional changed/baseline pair followed
the planned four invocations because the initial gain was only about 0.5%.
Each set contains 1,000 fixed Expert boards, one thread, count-only:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 | Changed 3 | Baseline 3 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 17.580575 | 17.411942 | 17.546679 | 17.507573 | 17.597110 | 17.312475 |
| 2 | 16.902536 | 16.791807 | 16.764601 | 17.009913 | 16.840884 | 16.854705 |
| 3 | 16.347988 | 16.356316 | 16.345516 | 16.399495 | 16.399349 | 16.374669 |
| Total | 50.831099 | 50.560065 | 50.656796 | 50.916981 | 50.837343 | 50.541849 |

Mean total runtime was **50.763310 s baseline versus 50.684735 s changed**,
a nominal **0.15%** reduction. The first two neighboring pairs improved by
about 0.53% and 0.51%, but the third regressed by 0.58%. The small aggregate
difference and inconsistent repeated result do not establish a reliable
speed improvement. The optimization was reverted and no follow-on
optimization was attempted, as required by repository guidance.

Correctness validation passed before reverting:

- All six complete `--benchmark` invocations passed unchanged board, 3BV,
  and optimal-click references: 18,000 solves over 3,000 distinct boards.
- All non-timing JSON fields and state/pruning counters matched across 36
  baseline/changed comparisons: three standard difficulties, seeds 1, 42,
  100, 2026, 123456, and 8675309, and both solve modes. All 18 changed
  replays passed the built-in legal-action validation.
- A temporary assertion-enabled harness compared the actual baseline and
  changed smart-order functions on 300 generated boards, before and after
  candidate reduction, in both orientations and sweep directions. All
  2,400 complete orders matched.
- A separate assertion-enabled harness compared 2,002 complete synthetic
  orders, covering sparse/dense graphs, empty/repeated scopes, equal scores,
  high crossing counts, and middle-column work sums that saturate at the
  maximum uint64_t value. Every order matched.

The source was restored byte-for-byte from this experiment's baseline
snapshot, verified by SHA-256. Existing workspace edits were preserved.
`doms.exe` was left unchanged and matches its pre-experiment snapshot by
SHA-256. `git diff --check` passed. Source snapshots, measured binaries,
temporary harnesses/scripts, raw logs, and summaries are outside version
control under `%TEMP%\doms-score-shortcircuit-20261021`.


## Experiment: precompute last-neighbor sweep lines

For smart ordering, precompute the latest sweep line among each vertex's
neighbors. A vertex from an earlier line is always live before that last
neighbor line, and already closed after it. Only on that last line is a
neighbor scan needed to construct its subset mask. Isolated vertices keep
last line -1 and never contribute. Compute this separately for each sweep
orientation and direction. Every cut score, predecessor tie break, complete
order, and subsequent exact DP search remains unchanged.

Include all existing workspace edits in the baseline. Build with identical
MSYS2 UCRT64 C++17 optimization flags, compare actual complete smart orders
on generated boards and synthetic graphs, and compare full solver outputs
in replay and count-only modes. Measure sequential baseline/changed/changed/
baseline full `--benchmark` runs against the unchanged reference CSV. Retain
only if they establish a measurable speed improvement; otherwise restore
this experiment's source snapshot, record the result, and stop. Temporary
artifacts are under `%TEMP%\doms-neighbor-extents-20261021`.


### Results: reverted; no speed improvement

Both builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
also enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
The four full benchmark invocations ran sequentially, without overlapping
compilation or other solver jobs from this session. Each set contains 1,000
fixed Expert boards, on one thread in count-only mode:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 16.529994 | 17.113621 | 16.778322 | 16.489810 |
| 2 | 15.705320 | 16.156075 | 16.177385 | 15.869222 |
| 3 | 16.665725 | 15.616542 | 16.306804 | 16.471484 |
| Total | 48.901039 | 48.886238 | 49.262511 | 48.830516 |

Mean total time was **48.865778 s baseline versus 49.074375 s changed**.
The changed version was **0.43% slower** overall.
The first neighboring pair was effectively tied; the second regressed.
These local measurements do not establish a speed improvement. The change
was reverted and no follow-on optimization was attempted, as required by
repository guidance.

Correctness validation passed before reverting:

- All four complete `--benchmark` runs passed unchanged board, 3BV, and
  optimal-click references: 12,000 solves over 3,000 distinct Expert boards.
- An assertion-enabled temporary harness compared the actual baseline and
  changed smart-order functions on 300 generated standard boards, before
  and after candidate reduction, and 500 synthetic graphs. All 4,400 complete
  orders matched across both orientations and both sweep directions. Cases
  included sparse/dense graphs, isolated vertices, empty scopes, long-range
  edges, and unreduced standard-board lines near the 20-candidate limit.
- All non-timing JSON fields matched across 36 full solver comparisons:
  three standard difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309,
  and both solve modes. All 18 changed replays passed built-in legal-action
  validation.

The solver source was restored byte-for-byte from this experiment's baseline
snapshot, verified by SHA-256. Existing workspace edits were preserved and
`doms.exe` was left untouched. Snapshots, measured binaries, temporary test
harness/script, solver comparison outputs, and raw benchmark logs/results are
outside version control under `%TEMP%\doms-neighbor-extents-20261021`.


## Candidate reduction: reuse left-click proof buffers

`left_click_dominated_candidate` builds temporary adjacency and base-factor
lists for each candidate. Reuse two vectors across candidates in the same
reduction pass, clearing their logical contents while retaining capacity.
Enumeration order, threshold checks, branch-and-bound decisions, candidate
deletion order, and every proof remain unchanged. This removes repeated vector
allocations without changing the exact search.

Both builds used MSYS2 UCRT64 GCC 16.1.0 with identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. The four full `--benchmark`
invocations ran sequentially in baseline/changed/changed/baseline order. Each
invocation validated the same three fixed 1,000-board Expert sets against the
tracked board, 3BV, and optimal-click references:

| Fixed set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.223447 | 17.167035 | 16.534316 | 17.096901 |
| 2 | 16.662144 | 16.453467 | 15.977757 | 16.524882 |
| 3 | 15.829085 | 15.445578 | 15.455024 | 15.867986 |
| Total | 50.714676 | 49.066080 | 47.967097 | 49.489769 |

The two-run mean decreased from **50.102223 s to 48.516589 s (3.17%)**.
Each changed set was faster than the mean of its two bracketing baseline set
times. All four complete benchmark runs passed all 3,000 references. These
measurements establish a local gain on this fixed Expert workload; other
boards and machines may differ. No objective, search limit, or proof rule
changed.


## Candidate reduction: reuse left-click proof masks

The retained proof-list reuse still allocated `local_edges` and
`local_base_hits` for every candidate examined by left-click dominance. This
experiment retained those two vectors across candidates in one reduction
pass, resized them to the current neighborhood, and cleared their active
entries before constructing the same masks. The independent-set proof,
branch order, threshold, and candidate deletion rules were unchanged.

The baseline and changed builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. Runs were sequential and used the
fixed Expert benchmark, one thread, count-only mode:

| Version | Run 1 (s) | Run 2 (s) | Mean (s) |
| --- | ---: | ---: | ---: |
| Baseline | 51.909274 | 49.752073 | 50.830674 |
| Reused masks | 50.601965 | 50.684702 | 50.643334 |

The mean difference was a nominal **0.37%** improvement. The first
baseline/changed pair favored reuse by 2.52%, while the second favored the
baseline by 1.87%. This does not establish a measurable speedup. All four
benchmark invocations passed their 3,000 board, 3BV, and optimal-click
reference checks. The allocation change was reverted, and no follow-on
optimization was attempted, as required by repository guidance.


## Experiment: cache factor-state hashes

The state table recomputes a factor-state hash while erasing each pruned state
and while rebuilding its lookup index. This experiment stored the hash beside
each inserted state and reused it for those operations. State equality,
insertion order, pruning decisions, and the exact search were unchanged.

Both builds used MSYS2 UCRT64 GCC 16.1.0 and identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. Runs used the fixed Expert benchmark,
one thread, count-only mode; all invocations passed all 3,000 board, 3BV, and
optimal-click reference checks:

| Version | Run 1 (s) | Run 2 (s) | Run 3 (s) | Total (s) |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 17.644634 | 16.758513 | 16.299776 | 50.702923 |
| Baseline 2 | 17.669821 | 16.691577 | 16.190764 | 50.552162 |
| Cached hashes | 17.625194 | 17.309432 | 17.207105 | 52.141731 |

The cached-hash build was **3.0% slower** than the mean of the two baseline
runs. It was reverted, and no follow-on optimization was attempted, as
required by repository guidance.


## Experiment: defer DP table growth until a new state is found

`FactorTable::find_or_emplace` grew its lookup index before probing. This
experiment moved the capacity check to the empty-slot insertion path, allowing
duplicate successors to return without triggering a rehash and allowing an
available tombstone to be reused. State equality, record creation, insertion
order, and all solver decisions were unchanged.

The current workspace snapshot was compiled as the baseline, then the changed
source was compiled with MSYS2 UCRT64 GCC using the same
`-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build additionally used
`-Wall -Wextra -Wpedantic`. Runs were sequential and each full
`--benchmark` invocation validated all 3,000 fixed board, 3BV, and
optimal-click references:

| Version | Run 1 (s) | Run 2 (s) | Mean (s) |
| --- | ---: | ---: | ---: |
| Baseline | 55.847557 | 54.478325 | 55.162941 |
| Deferred growth | 56.014413 | 55.035980 | 55.525197 |

Deferred growth was **0.66% slower** overall. The first changed/baseline pair
was close, while the second was about 1.0% slower. This does not establish a
measurable speed improvement, so the table change was reverted and no
follow-on optimization was attempted. The fixed benchmark references passed
for all four invocations. The changed build also matched the baseline on the
Expert seed-100 row-order profile's state totals, pruning counters, and
121-click optimum. Timing artifacts and the baseline source/executable are
outside version control under `%TEMP%\doms-deferred-growth-20261021`.


## Benchmark variance: consolidate the lab format into `--benchmark`

Tested the existing ordinary and lab modes using the current workspace solver,
MSYS2 UCRT64 GCC, and `-std=c++17 -O3 -DNDEBUG -pthread`. One temporary binary
served both modes; its only source change exposed the already-collected solve
and CPU totals in ordinary-mode output. No solver or timer logic changed.
Six pairs ran sequentially, alternating order: plain/lab, lab/plain,
plain/lab, lab/plain, plain/lab, lab/plain. Lab used its default 100-board
warm-up per set and high priority, with `--benchmark-repeat 1` to compare equal
numbers of timed boards. Neither mode used affinity. Priority elevation
succeeded for every lab invocation. No compilation overlapped timing.

| Pair | Ordinary solve total (s) | Lab solve total (s) |
| --- | ---: | ---: |
| 1 | 51.676915 | 48.345400 |
| 2 | 48.258731 | 47.305842 |
| 3 | 48.478502 | 47.746444 |
| 4 | 47.165941 | 47.656021 |
| 5 | 48.766850 | 48.813554 |
| 6 | 50.560232 | 48.031953 |

| Metric across six invocations | Ordinary | Lab |
| --- | ---: | ---: |
| Mean whole-benchmark solve seconds | 49.151195 | 47.983202 |
| Whole-benchmark solve CV | 3.075% | 1.023% |
| Whole-benchmark wall CV | 3.075% | 1.021% |
| Fixed set 1 solve CV | 3.220% | 1.598% |
| Fixed set 2 solve CV | 2.242% | 0.963% |
| Fixed set 3 solve CV | 4.854% | 0.708% |

CV uses population standard deviation / mean. Whole-benchmark solve CV fell
about 67%, corresponding to about 90% lower observed timing variance. Dropping
the first pair still favors lab: solve CV 2.261% versus 1.059%. All 36,000 timed
board checks and 1,800 lab warm-up checks passed unchanged board, 3BV, and
optimal-click references. This is evidence of improved repeatability on this
Windows machine, not a guarantee on other machines. The experiment tests the
warm-up/priority bundle; it does not isolate their individual contributions or
quantify the additional effect of taking medians over three repeats. Solve-only
and total-wall CV were nearly identical, so solve-only timing by itself did not
explain the improvement.

Retained the lab format as `--benchmark`: 100 warm-up boards per set, best-effort
high priority, three timed repeats by default, detailed rows and per-set
median/min/max/CV summaries. Removed `--benchmark-lab`, its option state, and
the duplicate ordinary single-thread output branch. Retained optional warm-up,
repeat, and affinity controls; multithread benchmarking keeps its existing
format and rejects these single-thread controls. No solver optimization or
reference-data change was made. Experiment source, harness, executable, and raw
logs are outside the repository under
`%TEMP%\doms-benchmark-variance-20261021`.

Final validation: rebuilt `doms.exe` with the prescribed UCRT64 command. The
new default passed all 9,000 timed references plus 300 warm-up checks; its
per-set solve CVs were 0.300%, 0.885%, and 0.596%. Independently recomputed all
aggregate medians, minima, maxima, and CVs from printed rows. The one-repeat,
zero-warm-up shortcut passed all 3,000 references. Help and seven invalid-option
checks passed, including removal of the lab flag and rejection of single-thread
controls with multithread mode. `git diff --check` passed. No generated files
or temporary test sources were added to version control.


## Integrate upstream optimizations through `45f1929`

Reviewed the current `qqwref/doms` master via GitHub; its head was
`45f1929830a6dd40b3b0a79200904c3a1115ee85`. The fork's HEAD (`a6f07c8`)
was already included in upstream through merge `a726cfd`. The two missing
optimization commits were:

- [`c511e00`](https://github.com/qqwref/doms/commit/c511e00740b5de2032362cab0ca0c73b371ab42b):
  store connectivity signature words in a shared arena, addressed by offsets.
- [`45f1929`](https://github.com/qqwref/doms/commit/45f1929830a6dd40b3b0a79200904c3a1115ee85):
  cache transition hash prefixes, shrink dominance cache records to direct
  entry pointers plus packed primary keys, and streamline bucket storage.

Integrated these changes into the existing uncommitted solver, preserving its
fixed-word connectivity specializations, scalar union buckets, direct pair
posting indexes, preprocessing/ordering improvements, and benchmark controls.
Replay uses upstream's per-bucket cache buffers; count-only uses its shared
uninitialized cache allocation. Arena views are used only while their pool
is unchanged; callers reacquire views after interning or reserve operations.
The locally reverted lookup-index release and deferred table-growth experiments
were not restored. No command-line features or reference answers were added
or changed.

Built with MSYS2 UCRT64 GCC using
`-std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic`; no diagnostics.
Compared the pre-integration workspace executable against the integrated build
in sequential A/B/B/A order, with no compilation overlapping measurement.
Each invocation used `--benchmark --benchmark-repeat 1`, retaining the default
100 warm-up boards per set and high-priority request. All four invocations
validated 3,000 timed boards and 300 warm-up boards against unchanged references.

| Build | First solve total (s) | Second solve total (s) | Mean (s) |
| --- | ---: | ---: | ---: |
| Baseline | 48.878310 | 50.644570 | 49.761440 |
| Integrated | 45.625173 | 48.233690 | 46.929432 |

The integrated mean was **5.69% lower**.
Both adjacent baseline/integrated pairs favored integration. This is a local
measurement of the two upstream commits together, not an isolated measurement
of each optimization or a guarantee on other workloads.

Additional checks: 52 baseline/integrated JSON comparisons covering generated
Beginner, Intermediate, and Expert boards, replay and count-only modes,
explicit plain/smart/dynamic orders, MBF hexadecimal, and an empty board.
All semantic JSON fields, actions, and search statistics matched after removing
timing fields; replay also performed the solver's built-in legal-action checks.

Based the README text on upstream's current README, retaining its organization
and most wording. Kept necessary corrections for the actual benchmark defaults,
options, working-directory requirement, Windows compiler selection, and reverted
lookup-index behavior. Removed the upstream link to an absent benchmark report.
All documented command-reference options exist in the parser. Detailed local
experiment descriptions remain here instead of expanding the README.

The signature-pool test also passed 100,000 randomized comparisons, reset/swap
checks, forced hash collisions, and arena-growth/ID checks. Its former stable
reference checks were adapted to reacquire views after pool mutations, as the
new arena intentionally exposes temporary views. The test binary uses the
ignored `benchmark_test.exe` name in the repository root.

Temporary snapshots, test sources, baseline executable and raw timing logs
are outside version control at `C:\Users\happy\AppData\Local\Temp\doms-upstream-20261021-i7cscxao`. Existing uncommitted changes were preserved.

Final parallel validation: `--benchmark-multi --threads 4` passed all 3,000
fixed references. In total the timing comparisons and parallel check passed
15,000 timed reference checks plus 1,200 warm-up checks. `git diff --check`
passed. No benchmark reference data or new tracked artifacts were added.

## Experiment: release every completed pruning layer's lookup index

The current solver still hashes and probes every erased state during dominance
pruning. Release the completed layer's duplicate-lookup index before pruning,
and erase using the existing stable dense entry position when the index is
absent. Iteration and all dominance witness ordering remain unchanged. A later
insertion rebuilds the index through the existing reserve path; lookup without
an index retains the existing linear scan. This tests all pruning layers rather
than the earlier review's optional large-index threshold.

Build baseline and changed sources with identical MSYS2 UCRT64 C++17 flags.
Compare count-only and replay JSON and state/pruning counters, exercise table
lookup/erase/reinsertion directly, and run plain --benchmark sequentially in
baseline/changed/changed/baseline order. Retain only a measurable improvement;
otherwise revert this experiment's source changes and stop. Preserve existing
workspace documentation edits. Temporary sources, executables and logs are in
`%TEMP%\doms-prune-index-20261021`.

### Results: retained

Both builds used MSYS2 UCRT64 GCC with `-std=c++17 -O3 -DNDEBUG -pthread`.
The changed build also enabled `-Wall -Wextra -Wpedantic` and compiled without
diagnostics. All four invocations used plain `--benchmark`: one thread, high
priority (successful), 100 warm-up boards per fixed set and three timed repeats.
Runs were sequential A/B/B/A; no compilation or other solver jobs from this
session overlapped timing runs. Cells below sum the three timed repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 46.068860 | 45.271938 | 45.985083 | 48.461022 |
| 2 | 44.051886 | 43.796980 | 45.433218 | 46.465809 |
| 3 | 43.073788 | 42.555784 | 43.923970 | 45.350165 |
| Total solve seconds | 133.194534 | 131.624702 | 135.342271 | 140.276996 |

Mean totals decreased from **136.735765 s to 133.483487 s (2.38%)**.
Both adjacent pairs improved (1.18% and 3.52%), as did all six corresponding
fixed-set totals. Mean CPU time decreased from 135.546875 s to 132.109375 s
(2.54%). This supports retaining the change on this workload, but visible
machine drift and two repetitions per build limit the precision of the gain.
Peak memory was not measured; no claim is made for every board or workload.

- All four benchmarks passed unchanged board, 3BV and optimal-click references:
  36,000 timed solves and 1,200 warm-ups over 3,000 distinct Expert boards.
- All non-timing JSON fields, including exact replay actions, matched in 36
  baseline/changed comparisons: three standard difficulties, seeds 1, 42, 100,
  2026, 123456 and 8675309, and both solve modes. All 18 changed replays passed
  built-in action simulation. All four profile counter lines also matched for
  every comparison, including processed/generated states, branch skips,
  dominance passes and removals.
- An assertion-enabled temporary table harness passed compact/wide factor
  masks and count-only/replay specializations. It exercised empty/repeated
  release, pointer/iterator erasure, repeated erasure, lookup and iteration
  without an index, duplicate merging and index rebuilding on reinsertion,
  erasure of every entry and insertion after exhaustion. Wide factors and
  replay histories used five words to exercise heap storage.
- The change removes only an obsolete lookup index. It changes no objective,
  search limits, pruning certificates, state order or tie breaks.

The experiment directory contains baseline/changed source snapshots, binaries,
the assertion harness, comparison script, 72 solver JSON outputs and profile
logs, raw benchmark logs, and `results.json`. Existing plan edits were preserved.
The installed `doms.exe` is the measured changed binary. Benchmark references
remain unchanged and tracked. `git diff --check` passed.


## Experiment: reject empty connectivity matching rows immediately

When building the inline connectivity containment graph, return false as soon
as a coarse component has no eligible fine component. Every coarse component
requires a distinct fine anchor, so an empty eligibility row makes a complete
matching impossible. This avoids constructing later rows and running the
matcher on that rejected pair. The general wide-component path is unchanged.
No objective, pruning certificate, state ordering, or search limit changes.

Preserve all existing workspace changes in the baseline. Build with MSYS2
UCRT64 and identical C++17 optimization flags. Compare the actual baseline
and changed predicates on randomized and boundary cases, compare complete
count-only/replay results and counters, and run sequential plain --benchmark
invocations in baseline/changed/changed/baseline order. Retain only a measurable
overall improvement; otherwise revert this experiment's solver change and stop.
Artifacts: `%TEMP%\doms-empty-match-20261021`, outside version control.

### Results: reverted; no reliable measurable improvement

Baseline and changed builds used MSYS2 UCRT64 GCC and
`-std=c++17 -O3 -DNDEBUG -pthread`. The changed build additionally enabled
`-Wall -Wextra -Wpedantic` and compiled without diagnostics. Four plain
`--benchmark` invocations ran sequentially A/B/B/A, with no compilation or
other solver jobs from this session overlapping measurements. Every invocation
used one thread, high priority (successful), 100 warm-up boards per fixed set,
and the default three timed repeats. Cells sum each set's three repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 44.333276 | 45.022385 | 44.343812 | 44.549729 |
| 2 | 42.666552 | 42.631306 | 42.591500 | 42.678168 |
| 3 | 41.524326 | 41.255106 | 41.488068 | 41.775085 |
| Total solve seconds | 128.524154 | 128.908797 | 128.423380 | 129.002982 |

Mean solve time was **128.763568 s baseline** versus **128.666089 s changed**,
a nominal 0.076% reduction. The first pair made the change 0.30%
slower; the second made it 0.45% faster. CPU-time means were 127.578125 s
baseline and 127.609375 s changed. These differences do not establish a
reliable measurable speed improvement. The solver change was reverted and no
follow-on optimization was attempted.

- All four benchmarks passed unchanged board, 3BV, and optimal-click references:
  36,000 timed solves and 1,200 warm-up solves over 3,000 distinct Expert boards.
- An assertion-enabled harness compared the actual baseline predicate with
  the changed predicate on 300,000 seeded cases. Coverage included one through
  three signature words, empty signatures, random containment graphs,
  constructed union/containment cases, bit 63, and 63-66 component boundaries.
  Every predicate result matched, including the general fallback.
- All non-timing JSON fields, exact replay actions, and four state/pruning
  counter lines matched in 36 full-solver comparisons: three difficulties,
  seeds 1, 42, 100, 2026, 123456, and 8675309, in both solve modes. All 18
  changed replays passed built-in legal-action validation.

Source snapshots, measured executables, temporary assertion harness and runner,
comparison JSON/profile outputs, raw benchmark logs, and `results.json` are in
the artifact directory above. The restored solver source matches the baseline
snapshot byte-for-byte. Existing workspace edits and benchmark references were
preserved; `doms.exe` was restored to the measured baseline build.

## Experiment: reuse transformed crossing-count buffers

Smart line ordering constructs two transformed scope histograms, then allocates
and initializes a third array for the final crossing scores. Overwrite the
before-only histogram with those scores and return it instead. Each result
reads only the histogram entry at its own index and the separate after-only
array, so all scores, ordering tie breaks, and complete orders are preserved.
No objective, pruning certificate, or search limit changes.

Include existing workspace edits in the baseline. Build with MSYS2 UCRT64 and
identical C++17 optimization flags. Compare the actual smart-order functions
and complete solver/replay results, then run sequential plain `--benchmark`
invocations in baseline/changed/changed/baseline order. Retain only a measurable
improvement; otherwise restore this experiment's solver change and stop.
Temporary artifacts: `%TEMP%\doms-crossing-buffer-20261021`.

### Results: reverted; no measurable speed improvement

Both builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build also
used `-Wall -Wextra -Wpedantic` and compiled without diagnostics. Four plain
`--benchmark` invocations ran sequentially in A/B/B/A order, with no compilation
or other solver jobs from this session overlapping measurement. Each used
one thread, a successful high-priority request, 100 warm-up boards per fixed
set, and three timed repeats. Cells sum solve seconds across three repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 45.147510 | 44.963832 | 45.667321 | 45.710031 |
| 2 | 43.806040 | 44.037965 | 44.254659 | 44.375132 |
| 3 | 42.798461 | 43.120072 | 43.138976 | 43.232738 |
| Total solve seconds | 131.752011 | 132.121869 | 133.060956 | 133.317901 |

Mean solve time was **132.534956 s baseline** versus **132.591413 s changed**,
a nominal **0.043% slowdown**. The first adjacent pair made the change 0.281%
slower; the second made it 0.193% faster. Mean CPU times were 131.242188 s
baseline and 131.140625 s changed. These differences do not establish a
measurable speed improvement. The solver change was reverted and no follow-on
optimization was attempted.

- All four benchmarks passed unchanged board, 3BV, and optimal-click references:
  36,000 timed solves and 1,200 warm-up solves over 3,000 distinct Expert boards.
- An assertion-enabled harness extracted the actual baseline smart-order
  function and compared it with the changed function on 300 seeded boards,
  100 per standard difficulty, before and after candidate reduction. All
  2,400 complete orders matched across both orientations and sweep directions.
- All non-timing JSON fields, exact replay actions, and state/pruning counter
  lines matched across 36 full-solver comparisons: three standard difficulties,
  seeds 1, 42, 100, 2026, 123456, and 8675309, and both solve modes. All
  18 changed replays passed built-in legal-action validation.

Source snapshots, measured executables, temporary differential harness and
runner, solver JSON/profile outputs, raw benchmark logs, and `results.json`
are retained in the artifact directory above. Existing workspace edits and
benchmark references were preserved. The restored solver source matches its
baseline snapshot byte-for-byte; `doms.exe` is the measured baseline binary.

## Experiment: cache opening-absorption component matches per transition

Opening absorption repeatedly searches the same outgoing connectivity signature
for each factor state. Cache each opening's matching component range once per
incoming connectivity and branch, in a flat arena with stable offsets. Keep
opening order, factor-bit checks, duplicate-component consumption, normalization,
and successor insertion order unchanged. Omit openings with no matching component.
Canonical signatures sort equal components contiguously, including wide signatures.
No objective, dominance certificate, search limit, or benchmark reference changes.

Build baseline and changed sources with identical MSYS2 UCRT64 C++17 flags.
Compare component-range absorption against direct scans on randomized and boundary
cases, then compare complete count-only/replay results and pruning counters.
Run sequential plain --benchmark invocations in baseline/changed/changed/baseline
order. Retain only a measurable improvement; otherwise revert this experiment's
solver change and stop. Artifacts are outside version control under
`%TEMP%\doms-union-merge-20261021` (the directory name predates candidate selection).

### Results: reverted; no reliable measurable improvement

Baseline and changed builds used MSYS2 UCRT64 GCC with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build also
used `-Wall -Wextra -Wpedantic` and compiled without diagnostics. All four
invocations used plain `--benchmark`: one thread, high priority (successful),
100 warm-up boards per fixed set, and three timed repeats. Runs were sequential
A/B/B/A, with no compilation or other solver jobs from this session overlapping
measurements. Cells sum solve seconds across the three timed repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 46.573818 | 45.143833 | 44.847931 | 44.689582 |
| 2 | 44.708775 | 43.738616 | 43.759603 | 43.508036 |
| 3 | 43.048748 | 42.822184 | 42.504429 | 41.751167 |
| Total solve seconds | 134.331341 | 131.704633 | 131.111963 | 129.948785 |

Mean solve time was **132.140063 s baseline** versus **131.408298 s changed**,
a nominal **0.554% reduction**. However, the first adjacent pair favored the
change by 1.96%, while the second favored baseline by 0.90%; every fixed set
reversed direction between pairs. Baseline invocation totals drifted by 3.26%,
well above the mean separation. Mean CPU times were 130.929688 s baseline and
130.453125 s changed, also with contradictory adjacent pairs. These results do
not establish a reliable measurable speed improvement. The solver change was
reverted and no follow-on optimization was attempted. Peak memory was not measured.

- All four benchmarks passed unchanged board, 3BV, and optimal-click references:
  36,000 timed solves and 1,200 warm-ups over 3,000 distinct Expert boards.
- An assertion-enabled temporary harness exercised the actual component-range
  helper against independent direct scans on 30,000 seeded canonical signatures,
  with 40 sequential opening operations per signature: 1,200,000 checks. It
  covered one through four words, empty signatures, duplicate components, bit 63,
  63/64/65/129-component boundaries, absent matches, exhausted ranges, and
  repeated factor slots. Matching ranges, consumed components, and factor bits
  agreed throughout.
- All non-timing JSON fields, exact replay actions, and four state/pruning
  counter lines matched across 36 baseline/changed comparisons: three standard
  difficulties, seeds 1, 42, 100, 2026, 123456, and 8675309, in both solve modes.
  All 18 changed replays passed built-in legal-action validation.

Baseline/changed source snapshots, measured executables, temporary harness and
comparison scripts, solver JSON/profile outputs, raw benchmark logs, and
`results.json` are retained in the artifact directory above. The restored source
matches its baseline snapshot byte-for-byte; `doms.exe` is the measured baseline
binary. Benchmark reference data is unchanged. Only this experiment's plan and
results remain as tracked changes.

## Experiment: insert the merged one-word transition component in order

For selected-chord transitions with one-word connectivity, all retained
components are unchanged: components containing the current bit are consumed
by the merge. Their canonical ascending order therefore survives filtering.
Insert the single merged component with lower_bound instead of sorting the
entire outgoing signature. Keep the skip branch and multiword path unchanged.
Duplicates, closed-component counts, interned words, insertion order, replay
ties, pruning certificates, objectives, and search limits remain unchanged.

Build baseline and changed versions with identical MSYS2 UCRT64 C++17 flags.
Compare the actual transition block on randomized/boundary signatures and
compare full replay/count-only results and search counters. Run sequential
plain --benchmark invocations in A/B/B/A order, retaining default warm-ups
and three repeats. Retain only a measurable improvement; otherwise revert
the solver change and stop. Preserve the existing optimization-plan edits.
Temporary artifacts: %TEMP%\doms-sorted-transition-20261021.

### Results: reverted; no measurable speed improvement

Both builds used MSYS2 UCRT64 GCC 16.1.0 with identical
`-std=c++17 -O3 -DNDEBUG -pthread` optimization flags. The changed build
also enabled `-Wall -Wextra -Wpedantic` and compiled without diagnostics.
Four plain `--benchmark` invocations ran sequentially in A/B/B/A order,
with no compilation or other solver jobs from this session overlapping
measurement. Each used one thread, high priority (successful), 100 warm-up
boards per fixed set, and three timed repeats. Cells sum solve seconds
across each fixed set's three repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 43.006921 | 43.168481 | 43.869174 | 42.949016 |
| 2 | 42.013538 | 41.733076 | 43.859144 | 41.935347 |
| 3 | 40.726993 | 40.376488 | 41.462104 | 40.791786 |
| Total solve seconds | 125.747452 | 125.278045 | 129.190422 | 125.676149 |

Mean solve time was **125.711801 s baseline** versus **127.234234 s changed**,
a **1.21% slowdown**. The first adjacent pair favored the change by 0.37%,
while the second favored baseline by 2.80%. Mean CPU times were 124.898438 s
baseline and 126.312500 s changed. These measurements do not establish a
speed improvement. The solver change was reverted and no follow-on
optimization was attempted. Peak memory was not measured.

- All four benchmarks passed unchanged board, 3BV, and optimal-click references:
  36,000 timed solves plus 1,200 warm-up solves over 3,000 distinct Expert boards.
- An assertion-enabled temporary harness extracted the actual baseline and
  changed one-word transition blocks. All 600,000 branch results matched on
  300,000 seeded canonical signatures, comparing outgoing words and closed
  component counts. Cases included empty signatures, duplicate components,
  zero future reach, absent current slots, bit 63, and component counts through
  129, with both compact and direct future-neighbor sources.
- All non-timing JSON fields, exact replay actions, and four search/pruning
  counter lines matched in 36 full-solver comparisons: three difficulties;
  seeds 1, 42, 100, 2026, 123456, and 8675309; count-only and replay modes.
  All 18 changed replays passed the built-in legal-action validation.

Baseline/changed source snapshots and executables, the extracted transition
harness, runner, solver JSON/profile outputs, raw benchmark logs,
`results.json`, and `summary.json` are outside version control under
`%TEMP%\doms-sorted-transition-20261021`. Existing optimization-plan edits
were preserved. The restored solver source matches its baseline snapshot
byte-for-byte; `doms.exe` is the measured baseline binary. Benchmark reference
data is unchanged. Only this experiment's plan and results remain as new
tracked edits.

## Experiment: flatten compact future-neighbor rows

The compact-connectivity path stored one fixed-width future-neighbor bit row
per candidate in `vector<vector<uint64_t>>`, causing one heap allocation per
row. Store the same rows in one flat word vector and address each row by its
fixed-width offset. Bit construction and transition order remain unchanged;
the wide-connectivity representation is untouched. No objective, dominance
certificate, search limit, or benchmark reference changes.

Build baseline and changed sources with identical MSYS2 UCRT64 C++17 flags.
Run plain `--benchmark` sequentially in baseline/changed/changed/baseline
order. Keep the storage change only if the paired measurements show a
measurable improvement; otherwise revert it and stop. Temporary source,
binaries, and logs are under `%TEMP%\doms-flat-neighbors-20261021`.

### Results: retained

Both binaries used MSYS2 UCRT64 GCC 16.1.0 and identical
`-std=c++17 -O3 -DNDEBUG -pthread` flags. The changed build also enabled
`-Wall -Wextra -Wpedantic` and compiled without diagnostics. Four plain
`--benchmark` invocations ran sequentially in A/B/B/A order. Each passed the
fixed Expert references, using 100 warm-up boards per set and three timed
repeats. No other solver or compilation jobs overlapped measurement. The
cells below sum each set's solve seconds across its three timed repeats:

| Fixed Expert set | Baseline 1 | Changed 1 | Changed 2 | Baseline 2 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 56.612283 | 56.092937 | 55.319535 | 56.309386 |
| 2 | 55.822641 | 55.778040 | 54.705420 | 55.553878 |
| 3 | 55.482039 | 55.309123 | 54.327239 | 55.506723 |
| Total solve seconds | 167.916963 | 167.180100 | 164.352194 | 167.369987 |

Mean solve time decreased from **167.643475 s to 165.766147 s (1.12%)**.
Both adjacent pairs improved (0.44% and 1.81%), and all six corresponding
fixed-set totals were lower with the flattened rows. The two changed runs
were also mutually consistent with a 1.70% difference, while baseline totals
differed by 0.33%.

- All four runs passed unchanged board, 3BV, and optimal-click references:
  36,000 timed solves and 1,200 warm-ups over 3,000 distinct Expert boards.
- The representation change preserves each row's word order and exact bits;
  it does not alter connectivity signatures or successor insertion order.
- `git diff --check` passed. Benchmark reference data is unchanged.

The source snapshot, both measured binaries, and raw benchmark logs are
retained outside version control under `%TEMP%\doms-flat-neighbors-20261021`.

## Follow-up candidate: exact separator decomposition for wide frontiers

Investigate an alternate exact solve path for boards whose best available
frontier order still has a large peak boundary. Build the decomposition over
the complete dependency structure: chord adjacency, zero-scope propagation,
and factor scopes must all be represented so that neither connectivity nor
factor rewards are lost across a separator. The separator interface must carry
the selected boundary choices, pending factor hits, and connectivity
equivalence among boundary vertices; combine subproblem costs only after
accounting for shared flags, base rewards, and seed components.

Start with a prototype that only detects candidate separators and reports
their sizes and projected state counts. Then implement a bounded-width exact
composition for small separators and compare it differentially with the
existing frontier DP. Benchmark separately on boards with wide frontiers and
on the fixed Expert set; dispatch to it only when measured total work is lower.
This is a larger algorithm change and is not part of the flat-row experiment.

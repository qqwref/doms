# Solver optimization decision log

This is a concise record of what has worked and what has not. “No measurable
benefit” means timing did not establish a reliable gain; “slower/regression”
means the tested candidate measured worse. Neither result says the idea can
never help another workload. Detailed sample tables, commands, and temporary
harness notes have been removed; fixed benchmark references remain tracked.

## Retained

- **Mandatory zero components at every board size.** Reduced profiled Expert
  sample time by about 23% versus coverage factors, with exact results and
  replay validation. It is the production representation.
- **Representation-aware ordering and bounded refinement.** Further reduced
  profiled Expert time by about 3.6% versus mandatory-zero alone and improved
  the tested large-board aggregate. Beginner was effectively unchanged;
  refinement did not run on the Expert sample, so the Expert gain came from
  order selection.
- **Bound connectivity merges by affordable seed cost** and **stop merge scans
  once connected.** Retained after repeatable aggregate large-board gains and
  correctness checks; the latter was a modest local improvement.
- **Mandatory zero-region connectivity on large boards.** Retained after
  measured improvement.
- **Contiguous reachability-bucket signature IDs; contiguous cached dominance
  entries; direct dominance-pair index; specialized one-word connectivity
  comparisons; completed-layer index release; flattened compact future-neighbor
  rows; zero-opening propagation bitset unions.** Retained after their recorded
  validations and benchmark reviews.
- **Dynamic-band candidate-order merging, reusable width-scoring workspace,
  and sweep-extent filtering.** Retained after measured improvement.
- Earlier review also favored **linear width estimation**, **direct one-word
  connectivity sorting**, and **tighter dominance-target scanning**. Deferred
  hash growth was only a provisional keep: results varied by round.

## Reverted: no reliable or measurable benefit

These did not establish a repeatable aggregate gain in their tested workload:

- Zero-allowance factor dominance; independent solving of pieces separated by
  mandatory zeros.
- Lazy forward-connectivity witnesses; cancellation of opposite identical
  factor scopes; free anchors before connectivity matching.
- Forward-dominance witness-prefix advancement; bounded reverse-merge overlap
  bound; redundant indexed-state liveness-probe removal; DP-index fingerprints;
  reduced DP lookup-index occupancy.
- Short-circuiting smart-order predecessor scores; precomputed last-neighbor
  sweep lines; empty connectivity-row rejection; transformed crossing-count
  buffer reuse; cached opening-absorption matches; ordered one-word transition
  insertion.
- Exact separator decomposition, and several candidate-reduction and
  dominance-setup variants whose results were inconclusive or mixed.

## Reverted: measured slower or clear regression

- **Unbudgeted window refinement alone:** Expert total time rose about 4.6x;
  planning overhead outweighed reduced DP work. Bounded refinement is retained
  as part of the later planner.
- **Refined sweep-selection review:** aggregate time was about 63.5% slower.
- **Cached factor-state hashes:** about 3% slower than baseline in the tested
  comparison.
- **Deferred DP table growth:** about 0.7% slower overall in the recorded
  comparison; evidence was insufficient to keep this variant despite earlier
  provisional results.
- **Visiting only populated words in opening-clique unions:** about 3.6%
  slower in the tested aggregate.
- **Bounded chord replacement with joint cost compensation:** clear regression,
  about 91% slower, with every set and repeat regressing.
- **Three scope/ordering caches** (combined closed-vertex counts, per-cut
  scores, candidate lines): measured about 2.3%, 2.1%, and 1.2% slower in the
  earlier review. Smart-order winner-local storage was also removed after its
  changed runs averaged about 1.4% slower.

## Rules for future work

Preserve exact optimal counts and legal replay, including wide factor and
connectivity representations. Compare total solve time against the current
production baseline on relevant small, Expert, and large workloads; do not
infer a universal benefit from one cohort. If an experiment does not show a
measurable improvement, update this log, revert that solver change, and stop
before follow-on optimization work, as required by `AGENTS.md`.

Build and fixed benchmark instructions are in `AGENTS.md`. Keep temporary
scripts, binaries, logs, and generated results ignored.

## Compact scalar dominance cache records (2026-10-23)

Reduce scalar dominance cache records from 32 to 24 bytes by storing the
two hit counts as bytes after the integer quasi score. A 64-bit factor
state has at most 64 hits; wide factor states retain full integer counts.
All pruning arithmetic, sort keys, witness order and exact results remain
unchanged. This reduces scalar cache-record storage by 25%, without a
search limit or approximation.

### Paired measurements: retained

Both builds used portable MSYS2 UCRT64 C++17 -O3 -DNDEBUG -pthread;
candidate also compiled with -Wall -Wextra -Wpedantic without diagnostics.
Two independent serial sessions used high process priority, unchanged
affinity, one warmup per build/board and six timed samples per build.
First session order was ABBA/BAAB/ABBA; confirmation reversed it to
BAAB/ABBA/BAAB. Compilation and correctness checks finished before timing.
Solve times include planning. No pre-existing doms process was present
at the initial process check.

| Fixed board | First baseline / candidate median | Confirmation baseline / candidate median |
| --- | ---: | ---: |
| random_30x30_225 | 0.4155985 / 0.4128330 s | 0.4167775 / 0.4114595 s |
| random_30x30_250 | 1.0854220 / 1.0648680 s | 1.0852225 / 1.0609840 s |
| random_35x35_335 | 1.0025650 / 1.0034305 s | 0.9877505 / 0.9862180 s |

Session 1 sum of medians: 2.5035855 s baseline versus 2.4811315 s candidate, a 0.90% reduction.
Session 2 sum of medians: 2.4897505 s baseline versus 2.4586615 s candidate, a 1.25% reduction.

Retain the small, repeatable aggregate gain. The 35x35 differences
(-0.09% first session, +0.16% confirmation) are within observed variation;
no independent 35x35 speedup is established. The 30x30/250 reduction is
1.89% and 2.23%, and 30x30/225 improves 0.67% and 1.28%.

All 84 warmup/measured solves match unchanged optimal-click/3BV references
253/419, 279/460 and 361/596. Orders, candidate counts and peak states
(48,533 / 182,979 / 130,474) match between builds on every timed sample.
The assertion-enabled maintained harness passed 8,064 exhaustive-oracle
solves with legal replays, wide factors and 80/130-contact compact/general
connectivity. A temporary layout/arithmetic check confirms 24-byte scalar
and 32-byte wide records on this compiler and 1,085,825 promoted arithmetic
cases including negative differences and all hit counts 0..64.

Temporary snapshots, harnesses, logs and raw samples are ignored under
benchmarks/test_compact_cache*, benchmarks/compact_cache*results*.json and
benchmarks/compact_cache*benchmark.log. Fixed reference data is unchanged.


### Final correctness validation

The retained portable candidate passes the full default doms.exe --benchmark:
all 9,000 timed solves against fixed board, 3BV and optimal-click references,
plus 100 warmup boards per fixed set. Median set solve totals are 9.475249,
8.924530 and 8.593417 seconds. These are validation timings, not a paired
Expert performance claim.

Both builds produce identical legal full replays, all non-timing JSON and
profile search/pruning counters on the first three fixed large boards.
Production JSON construction validates every replay action. The retained
solver change is solely the CachedItem count types, field order and matching
initializer; all existing solver certificates and ordering remain unchanged.

### Bounded 40x40 comparison

Both serial builds completed inside the 120-second limit, baseline in
32.627912 seconds and candidate in 32.429256 seconds. Both match the fixed
484-click optimum / 778 3BV reference, identical non-timing JSON, selected
order and 2,337,318 peak DP states. One sample per build does not establish
a reliable 40x40 speed ratio. Its 68 active factor bits use the wide path,
whose counts remain full integers and cache records remain 32 bytes.

Retained doms.exe is the portable candidate. git diff --check passes.
Pre-existing results.csv and the concurrent shortening of this decision log
are preserved. No follow-on optimization was started.

## Experiment: cache scalar good-bit masks (2026-10-23)

Cache hits XOR base_mask in scalar dominance records to avoid repeated
DP-state hit loads and transforms. The predicate remains
popcount(good(target) AND NOT good(candidate)) <= allowance; cost bounds,
comparison order, witnesses and wide-factor predicates are unchanged.
Scalar cache records grow from 24 to 32 bytes, requiring measured benefit.
Preserve all starting workspace changes in an ignored source snapshot.

Build portable UCRT64 baseline/candidate binaries. Check exhaustive small-board
optima, legal replay, wide factors and compact/general connectivity. Compare
the first three fixed large boards serially with one warmup and six timed
samples per build in balanced ABBA/BAAB/ABBA order, including planning time.
Require fixed optimal counts/3BV and unchanged search statistics. Retain only
measurable aggregate improvement without material per-board regression;
otherwise restore this experiment, document the result, and stop under
AGENTS.md. If retained, confirm timings, validate Expert references and large
replays, and compare the fixed 40x40 board with bounded serial runs.

### Measurements and decision: reverted

Both builds used MSYS2 UCRT64 -std=c++17 -O3 -DNDEBUG -pthread.
Serial high-priority runs used unchanged affinity, one warmup per build and
six measured samples per build per board in ABBA/BAAB/ABBA order. Compilation
and correctness checks finished before timing; no pre-existing doms process
was found. Solve times include planning.

| Fixed board | Baseline median | Candidate median | Candidate change |
| --- | ---: | ---: | ---: |
| random_30x30_225 | 0.4149740 s | 0.4153520 s | 0.09% slower |
| random_30x30_250 | 1.0943180 s | 1.0782380 s | 1.47% faster |
| random_35x35_335 | 0.9780115 s | 0.9924085 s | 1.47% slower |

Sum of medians: 2.4873035 s baseline versus 2.4859985 s candidate,
a nominal 0.052% reduction. This is within observed sample variation and
does not establish measurable improvement; the 35x35 result also regresses.
The larger cache did not demonstrate an aggregate benefit on this workload.

All 42 warmup/measured solves matched fixed optimal-click/3BV references
253/419, 279/460 and 361/596, with identical candidate counts, selected order
and peak DP states between builds. The assertion-enabled existing harness
passed 8,064 exhaustive-oracle solves with legal replays, wide factors and
80/130-contact compact/general connectivity. The warning-enabled candidate
build reported only the scalar penalty helper becoming unused in production;
there were no compile errors.

Restored the solver byte-for-byte from the starting snapshot and rebuilt
production doms.exe via the matching baseline build. All pre-existing solver,
plan and results.csv changes are preserved. No new optimization is retained.
Stopped under AGENTS.md: confirmation, full Expert and bounded 40x40 runs were
conditional on acceptance and were not started. No follow-on experiment ran.
Temporary snapshot, harness and raw samples remain ignored under
benchmarks/test_good_mask* and benchmarks/good_mask_results.json.

## Experiment: triangular DP lookup probing (2026-10-23)

Replace linear probing in FactorTable with triangular offsets (successive
steps 1, 2, 3, ...) to reduce primary clustering at the existing 75% maximum
occupancy. For a power-of-two index capacity these offsets visit every slot.
Use the same sequence in lookup, insertion, erasure and rehash. Dense entry
order, full equality comparisons, objective, dominance and replay stay intact.
Preserve the starting workspace in an ignored source snapshot.

Build baseline and candidate with portable UCRT64 C++17 -O3 -DNDEBUG -pthread.
Validate exhaustive small-board optima/replays and wide representations, then
measure the first three fixed large boards with one warmup and six balanced
serial samples per build, including planning. Require unchanged fixed optima,
3BV and search statistics. Retain only measurable improvement; otherwise
restore this experiment, record the results and stop under AGENTS.md. If
retained, confirm timing and validate Expert and bounded 40x40 solves.

### Measurements and decision: reverted

Both builds used MSYS2 UCRT64 -std=c++17 -O3 -DNDEBUG -pthread.
Serial high-priority runs kept affinity unchanged, with one warmup per build
and six measured samples per build per board in ABBA/BAAB/ABBA order.
Compilation and correctness checks finished before timing. No pre-existing
doms or benchmark process was found during the initial process check.
Reported solve times include planning.

| Fixed board | Baseline median | Candidate median | Candidate change |
| --- | ---: | ---: | ---: |
| random_30x30_225 | 0.4092695 s | 0.4130975 s | 0.94% slower |
| random_30x30_250 | 1.0751205 s | 1.0938610 s | 1.74% slower |
| random_35x35_335 | 0.9674810 s | 0.9950940 s | 2.85% slower |

Sum of medians: 2.4518710 s baseline versus 2.5020525 s candidate,
a 2.05% increase. No board improved; the alternate probing sequence did not
establish a measurable benefit on this workload. Revert this experiment.

All 42 warmup/measured solves matched fixed optimal-click/3BV references
253/419, 279/460 and 361/596, with identical selected orders, candidate counts
and peak DP states. The assertion-enabled existing harness passed 8,064
exhaustive-oracle solves with legal replays, wide factors and compact/general
multiword connectivity. A targeted assertion-enabled harness verified that
triangular offsets cover all slots for power-of-two capacities 8..65,536,
and tested 1,200 colliding states through duplicate detection, indexed
erasure, tombstone reinsertion, rehashing and lookup-index release.

Restored minesweeper_chord_dp.cpp byte-for-byte from the starting snapshot,
preserving the pre-existing compact-cache edit, and restored doms.exe from
the matching baseline build. Pre-existing plan edits and results.csv remain.
No optimization is retained. Stopped under AGENTS.md; confirmation, full
Expert and bounded 40x40 comparisons were conditional on acceptance and
were not started. No follow-on experiment ran. Temporary source snapshots,
harnesses, samples and log remain ignored under benchmarks/test_triangular*,
benchmarks/triangular_results.json and benchmarks/triangular_benchmark.log.

## Experiment: reuse dense DP layer buffers (2026-10-23)

Keep two alternating FactorTable dense entry and liveness buffers rather than
allocating them for every candidate. Clear records before dominance to release
wide-factor and replay-history allocations promptly. Release lookup indexes as
before: reserve rebuilds each layer with its existing capacity calculation, so
hash layout, insertion order, pruning, objective and witnesses stay unchanged.
Retaining dense capacity may increase memory after a large layer; measure total
solve time rather than allocation counts. No search limits or approximations.

Preserve the starting source in ignored benchmarks/test_reuse_snapshot.cpp.
Build portable UCRT64 baseline/candidate binaries, validate exhaustive small
optima, legal replay and wide connectivity/factors, then run balanced serial
large-board samples with fixed references. Retain only measurable improvement;
otherwise restore this experiment, record results and stop under AGENTS.md.
If retained, confirm timings, run fixed Expert validation and bounded 40x40
comparisons. Preserve pre-existing source, documentation and results.csv edits.

### Paired measurements: retained

Both builds used MSYS2 UCRT64 -std=c++17 -O3 -DNDEBUG -pthread.
Two serial high-priority sessions left affinity unchanged, with one warmup
and six measured samples per build/board. The first used ABBA/BAAB/ABBA;
confirmation used BAAB/ABBA/BAAB. Compilation and oracle checks finished
before timing; the initial process check found no existing solver process.
Solve time includes planning. Every sample matches fixed optimum/3BV and
identical candidate counts, selected orders and peak live states.

| Fixed board | First baseline / candidate median | Confirmation baseline / candidate median |
| --- | ---: | ---: |
| random_30x30_225 | 0.4054800 / 0.3969410 s | 0.4193530 / 0.4023320 s |
| random_30x30_250 | 1.0784045 / 1.0727140 s | 1.0815865 / 1.0672295 s |
| random_35x35_335 | 0.9893495 / 0.9578815 s | 1.0555415 / 1.0280625 s |

Sum of medians: 2.4732340 versus 2.4275365 s (1.85% reduction), and
2.5564810 versus 2.4976240 s (2.30% reduction). All boards improve in both
sessions; the 30x30/250 gain is small (0.53% / 1.33%). Retain the repeatable
aggregate improvement, without claiming a general or large speedup.

The assertion-enabled existing harness passed 8,064 exhaustive-oracle solves
with legal replay, wide factors and 80/130-contact compact/general connectivity.
Temporary scripts, snapshot, logs and samples use ignored benchmarks/test_reuse*,
benchmarks/reuse*benchmark.log and benchmarks/reuse*results.json paths.

### Large-board replay, memory and bounded 40x40 validation

Baseline/candidate full legal replays, all non-timing JSON and profile
search/pruning counters match on the first three fixed large boards. Their
optimal-click/3BV references remain 253/419, 279/460 and 361/596.

A separate serial count-only run sampled Windows PeakWorkingSetSize every
5 ms via GetProcessMemoryInfo. Observed baseline/candidate peaks were:

| Fixed board | Baseline peak | Candidate peak |
| --- | ---: | ---: |
| random_30x30_225 | 14.78 MiB | 15.37 MiB |
| random_30x30_250 | 35.64 MiB | 40.09 MiB |
| random_35x35_335 | 23.72 MiB | 26.86 MiB |
| random_40x40_444 | 560.18 MiB | 653.00 MiB |

These are single-process observations, not guaranteed bounds. Retaining two
historical dense capacities increases memory, especially after large layers;
it does not cap states or change optimality. Record destruction still releases
wide factor/replay allocations before dominance. The 40x40 memory increase is
92.83 MiB (16.6%). This is the material tradeoff for fewer dense allocations.

Both bounded 120-second 40x40 runs completed, at 33.623740 seconds baseline and
31.848547 seconds candidate, with unchanged fixed 484-click optimum / 778 3BV,
all non-timing JSON, selected order and 2,337,318 peak live states. One run per
build, with memory polling, does not establish a reliable 40x40 speed ratio.

### Final validation

Rebuilt production doms.exe from the retained source with the required
portable UCRT64 flags. The full default doms.exe --benchmark passed all 9,000
timed solves against fixed board, optimal-click and 3BV references, plus
100 warmup boards per set. Median set solve totals were 9.491139, 9.020950
and 8.875540 seconds. These are correctness validation timings, not a paired
Expert performance claim. Fixed reference data is unchanged.

All required checks passed. Retain buffer reuse with the documented memory
tradeoff. Pre-existing solver/documentation/results.csv changes are preserved;
no follow-on optimization was started. git diff --check passes.

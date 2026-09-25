#!/usr/bin/env python3
"""Exact Minesweeper click optimizer using a frontier connectivity DP.

The board is assumed to be completely known. Input may be a text grid, a
LlamaSweeper board-editor URL, MBF hexadecimal, or a binary .mbf file.
Coordinates printed by the CLI are 1-based unless --zero-based is supplied.

The optimization model counts one click for each flag, ordinary left click,
and chord.  It finds an optimal chord set, then constructs the mechanical
sequence: flag the required mines, seed each chord-propagation component,
perform its chords, and click the remaining 3BV units.
"""

from __future__ import annotations

import argparse
import itertools
import json
import random
import re
import sys
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence
from urllib.parse import parse_qs, urlsplit


Coord = tuple[int, int]
LLAMASWEEPER_ALPHABET = "0123456789abcdefghijklmnopqrstuv"
NEIGHBOR_OFFSETS = tuple(
    (dr, dc)
    for dr in (-1, 0, 1)
    for dc in (-1, 0, 1)
    if (dr, dc) != (0, 0)
)


class StateLimitExceeded(RuntimeError):
    pass


@dataclass
class Model:
    height: int
    width: int
    mines: set[Coord]
    numbers: dict[Coord, int]
    zeros: list[set[Coord]]
    singleton_units: list[Coord]
    candidates: list[Coord]
    # Direct chord-to-chord opening adjacency. Zero-region propagation is
    # represented separately as a hyperedge in zero_scopes.
    graph: list[set[int]]
    zero_scopes: list[tuple[int, ...]]
    mine_scopes: list[tuple[int, ...]]
    base_scopes: list[tuple[int, ...]]
    base_descriptions: list[tuple[str, object]]

    @property
    def three_bv(self) -> int:
        return len(self.base_scopes)


@dataclass
class Solution:
    clicks: int
    selected: set[int]
    flags: set[Coord]
    components: list[list[int]]
    uncovered_units: list[int]
    actions: list[tuple[str, Coord]]
    peak_states: int = 0
    max_boundary_vertices: int = 0
    max_active_factors: int = 0
    order_name: str = ""


def neighbors(cell: Coord, height: int, width: int) -> Iterator[Coord]:
    r, c = cell
    for dr, dc in NEIGHBOR_OFFSETS:
        nr, nc = r + dr, c + dc
        if 0 <= nr < height and 0 <= nc < width:
            yield nr, nc


def parse_board(text: str) -> tuple[int, int, set[Coord]]:
    rows: list[str] = []
    for raw in text.splitlines():
        line = "".join(raw.split())
        if not line or line.startswith(";"):
            continue
        rows.append(line)
    if not rows:
        raise ValueError("the board is empty")
    width = len(rows[0])
    if any(len(row) != width for row in rows):
        raise ValueError("all board rows must have the same width")
    allowed = set(".*M012345678")
    bad = sorted(set("".join(rows)) - allowed)
    if bad:
        raise ValueError(f"unsupported board character(s): {''.join(bad)!r}")
    mines = {
        (r, c)
        for r, row in enumerate(rows)
        for c, ch in enumerate(row)
        if ch in "*M"
    }
    return len(rows), width, mines


def _llamasweeper_parameters(source: str) -> tuple[str, str]:
    value = source.strip()
    parsed = urlsplit(value)
    query = parsed.query
    if parsed.fragment:
        _, separator, fragment_query = parsed.fragment.partition("?")
        if separator:
            query = fragment_query
    if not query and ("b=" in value or "m=" in value):
        query = value.lstrip("?#")
    parameters = parse_qs(query, keep_blank_values=True)
    if len(parameters.get("b", [])) != 1 or len(parameters.get("m", [])) != 1:
        raise ValueError("LlamaSweeper input must contain exactly one b= and one m= parameter")
    return parameters["b"][0], parameters["m"][0].lower()


def _llamasweeper_dimensions(board_code: str, encoded_groups: int) -> tuple[int, int]:
    presets = {"1": (9, 9), "2": (16, 16), "3": (30, 16)}
    if board_code in presets:
        width, height = presets[board_code]
        expected_groups = (height * width + 4) // 5
        if encoded_groups != expected_groups:
            raise ValueError(
                f"LlamaSweeper b={board_code} needs {expected_groups} m characters, "
                f"not {encoded_groups}"
            )
        return height, width

    if not board_code.isdigit() or len(board_code) < 2:
        raise ValueError(f"unsupported LlamaSweeper board code b={board_code!r}")

    # Custom boards serialize as decimal width followed by decimal height,
    # with height left-padded to at least the number of width digits. The m=
    # length disambiguates cases such as 4x11 versus 41x1.
    candidates: list[tuple[int, int]] = []
    for split in range(1, len(board_code)):
        width = int(board_code[:split])
        height = int(board_code[split:])
        if width <= 0 or height <= 0:
            continue
        reconstructed = f"{width}{height:0{len(str(width))}d}"
        if reconstructed != board_code:
            continue
        if (width * height + 4) // 5 == encoded_groups:
            candidates.append((height, width))
    candidates = sorted(set(candidates))
    if not candidates:
        raise ValueError(
            f"cannot reconcile LlamaSweeper b={board_code!r} with "
            f"an {encoded_groups}-character m parameter"
        )
    if len(candidates) > 1:
        descriptions = ", ".join(f"{w}x{h}" for h, w in candidates)
        raise ValueError(f"ambiguous LlamaSweeper dimensions: {descriptions}")
    return candidates[0]


def parse_llamasweeper(source: str) -> tuple[int, int, set[Coord]]:
    board_code, mine_code = _llamasweeper_parameters(source)
    if not mine_code:
        raise ValueError("the LlamaSweeper m parameter is empty")
    invalid = sorted(set(mine_code) - set(LLAMASWEEPER_ALPHABET))
    if invalid:
        raise ValueError(
            "invalid LlamaSweeper m character(s): " + ", ".join(repr(ch) for ch in invalid)
        )
    height, width = _llamasweeper_dimensions(board_code, len(mine_code))
    bits = "".join(
        f"{LLAMASWEEPER_ALPHABET.index(character):05b}" for character in mine_code
    )
    cell_count = height * width
    if any(bit == "1" for bit in bits[cell_count:]):
        raise ValueError("LlamaSweeper m parameter has nonzero padding bits")
    mines = {
        (index // width, index % width)
        for index, bit in enumerate(bits[:cell_count])
        if bit == "1"
    }
    return height, width, mines


def format_llamasweeper_url(height: int, width: int, mines: set[Coord]) -> str:
    """Encode a standard board as a LlamaSweeper board-editor URL."""
    board_codes = {(9, 9): "1", (16, 16): "2", (16, 30): "3"}
    try:
        board_code = board_codes[(height, width)]
    except KeyError as exc:
        raise ValueError(
            "LlamaSweeper URL output currently supports only Beginner, "
            "Intermediate, and Expert dimensions"
        ) from exc
    invalid = sorted(
        cell
        for cell in mines
        if not (0 <= cell[0] < height and 0 <= cell[1] < width)
    )
    if invalid:
        raise ValueError(f"mine {invalid[0]} is outside the {width}x{height} board")
    bits = "".join(
        "1" if (index // width, index % width) in mines else "0"
        for index in range(height * width)
    )
    bits += "0" * (-len(bits) % 5)
    mine_code = "".join(
        LLAMASWEEPER_ALPHABET[int(bits[position : position + 5], 2)]
        for position in range(0, len(bits), 5)
    )
    return (
        "https://llamasweeper.com/#/game/board-editor?"
        f"b={board_code}&m={mine_code}"
    )


def generate_standard_board(
    difficulty: str, seed: int | None = None
) -> tuple[int, int, set[Coord], str, int]:
    """Generate a reproducible uniformly random Intermediate or Expert board."""
    presets = {
        "intermediate": (16, 16, 40),
        "expert": (16, 30, 99),
    }
    try:
        height, width, mine_count = presets[difficulty]
    except KeyError as exc:
        raise ValueError(f"unknown generated difficulty {difficulty!r}") from exc
    effective_seed = (
        random.SystemRandom().getrandbits(64) if seed is None else seed
    )
    rng = random.Random(effective_seed)
    mine_indices = rng.sample(range(height * width), mine_count)
    mines = {(index // width, index % width) for index in mine_indices}
    url = format_llamasweeper_url(height, width, mines)
    return height, width, mines, url, effective_seed


def _looks_like_mbf_hex(text: str) -> bool:
    tokens = [token for token in re.split(r"[\s,]+", text.strip()) if token]
    return len(tokens) >= 4 and all(
        re.fullmatch(r"(?:0x)?[0-9a-fA-F]{2}", token) is not None for token in tokens
    )


def _mbf_hex_bytes(text: str) -> bytes:
    tokens = [token for token in re.split(r"[\s,]+", text.strip()) if token]
    if len(tokens) < 4:
        raise ValueError("MBF hexadecimal must contain at least four bytes")
    invalid = [
        token
        for token in tokens
        if re.fullmatch(r"(?:0x)?[0-9a-fA-F]{2}", token) is None
    ]
    if invalid:
        raise ValueError(f"invalid MBF hexadecimal byte {invalid[0]!r}")
    return bytes(int(token.removeprefix("0x").removeprefix("0X"), 16) for token in tokens)


def parse_mbf(data: bytes | str) -> tuple[int, int, set[Coord]]:
    if isinstance(data, str):
        data = _mbf_hex_bytes(data)
    if len(data) < 4:
        raise ValueError("MBF data must contain a four-byte header")
    width, height = data[0], data[1]
    mine_count = 256 * data[2] + data[3]
    if width == 0 or height == 0:
        raise ValueError("MBF width and height must be nonzero")
    expected_size = 4 + 2 * mine_count
    if len(data) != expected_size:
        raise ValueError(
            f"MBF header declares {mine_count} mines ({expected_size} bytes total), "
            f"but the input contains {len(data)} bytes"
        )
    mines: set[Coord] = set()
    for position in range(4, len(data), 2):
        x, y = data[position], data[position + 1]
        if x >= width or y >= height:
            raise ValueError(
                f"MBF mine coordinate ({x},{y}) is outside the {width}x{height} board"
            )
        cell = (y, x)
        if cell in mines:
            raise ValueError(f"MBF repeats mine coordinate ({x},{y})")
        mines.add(cell)
    return height, width, mines


def _existing_file(source: str) -> Path | None:
    try:
        path = Path(source)
        return path if path.is_file() else None
    except OSError:
        return None


def load_board(source: str, input_format: str = "auto") -> tuple[int, int, set[Coord]]:
    path = _existing_file(source)
    raw: bytes | None = path.read_bytes() if path is not None else None

    if input_format == "mbf" or (
        input_format == "auto" and path is not None and path.suffix.lower() == ".mbf"
    ):
        if raw is None:
            return parse_mbf(source)
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError:
            return parse_mbf(raw)
        return parse_mbf(text) if _looks_like_mbf_hex(text) else parse_mbf(raw)

    if raw is not None:
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(
                "non-text input requires --format mbf or a .mbf filename"
            ) from exc
    else:
        text = source

    if input_format == "grid":
        return parse_board(text)
    if input_format == "llamasweeper":
        return parse_llamasweeper(text)
    if input_format != "auto":
        raise ValueError(f"unknown input format {input_format!r}")

    lowered = text.lower()
    if "llamasweeper.com" in lowered or (
        re.search(r"(?:^|[?&])b=\d+", text) and re.search(r"(?:^|[?&])m=", text)
    ):
        return parse_llamasweeper(text)
    if _looks_like_mbf_hex(text):
        return parse_mbf(text)
    return parse_board(text)


def _zero_components(
    zero_cells: set[Coord], height: int, width: int
) -> list[set[Coord]]:
    unseen = set(zero_cells)
    result: list[set[Coord]] = []
    while unseen:
        start = unseen.pop()
        component = {start}
        queue = [start]
        while queue:
            cell = queue.pop()
            for other in neighbors(cell, height, width):
                if other in unseen:
                    unseen.remove(other)
                    component.add(other)
                    queue.append(other)
        result.append(component)
    result.sort(key=lambda comp: min(comp))
    return result


def build_model(height: int, width: int, mines: set[Coord]) -> Model:
    all_cells = {(r, c) for r in range(height) for c in range(width)}
    safe = all_cells - mines
    numbers = {
        cell: sum(other in mines for other in neighbors(cell, height, width))
        for cell in safe
    }
    zeros = _zero_components(
        {cell for cell, number in numbers.items() if number == 0}, height, width
    )
    adjacent_to_zero = {
        cell
        for zero in (z for comp in zeros for z in comp)
        for cell in neighbors(zero, height, width)
        if cell in safe
    }
    singleton_units = sorted(
        cell
        for cell, number in numbers.items()
        if number > 0 and cell not in adjacent_to_zero
    )
    candidates = sorted(cell for cell, number in numbers.items() if number > 0)
    candidate_index = {cell: i for i, cell in enumerate(candidates)}
    graph = [set() for _ in candidates]

    # Direct opening: chording a square reveals adjacent chord squares.
    for i, cell in enumerate(candidates):
        for other in neighbors(cell, height, width):
            j = candidate_index.get(other)
            if j is not None and j != i:
                graph[i].add(j)
                graph[j].add(i)

    # Flood opening is a hyperedge rather than an explicit clique. An explicit
    # clique can turn one large opening into a huge DP frontier.
    zero_boundaries: list[tuple[int, ...]] = []
    for component in zeros:
        boundary = sorted(
            {
                candidate_index[cell]
                for zero in component
                for cell in neighbors(zero, height, width)
                if cell in candidate_index
            }
        )
        zero_boundaries.append(tuple(boundary))

    mine_list = sorted(mines)
    mine_scopes = [
        tuple(
            sorted(
                candidate_index[cell]
                for cell in neighbors(mine, height, width)
                if cell in candidate_index
            )
        )
        for mine in mine_list
    ]

    base_scopes: list[tuple[int, ...]] = []
    base_descriptions: list[tuple[str, object]] = []
    for component, boundary in zip(zeros, zero_boundaries):
        base_scopes.append(boundary)
        base_descriptions.append(("zero", min(component)))
    for cell in singleton_units:
        # If the square itself is selected, its component's seed/propagation
        # opens it; otherwise an adjacent selected chord can open it.
        scope = {candidate_index[cell]}
        scope.update(
            candidate_index[other]
            for other in neighbors(cell, height, width)
            if other in candidate_index
        )
        base_scopes.append(tuple(sorted(scope)))
        base_descriptions.append(("single", cell))

    return Model(
        height=height,
        width=width,
        mines=set(mines),
        numbers=numbers,
        zeros=zeros,
        singleton_units=singleton_units,
        candidates=candidates,
        graph=graph,
        zero_scopes=zero_boundaries,
        mine_scopes=mine_scopes,
        base_scopes=base_scopes,
        base_descriptions=base_descriptions,
    )


def _ordered_model(model: Model, order: Sequence[int]) -> Model:
    """Return an equivalent model whose variables use the requested order."""
    inverse = {old: new for new, old in enumerate(order)}
    candidates = [model.candidates[old] for old in order]
    graph = [set() for _ in order]
    for old_i in order:
        new_i = inverse[old_i]
        graph[new_i] = {inverse[old_j] for old_j in model.graph[old_i]}

    def remap(scopes: Iterable[tuple[int, ...]]) -> list[tuple[int, ...]]:
        return [tuple(sorted(inverse[v] for v in scope)) for scope in scopes]

    return Model(
        height=model.height,
        width=model.width,
        mines=model.mines,
        numbers=model.numbers,
        zeros=model.zeros,
        singleton_units=model.singleton_units,
        candidates=candidates,
        graph=graph,
        zero_scopes=remap(model.zero_scopes),
        mine_scopes=remap(model.mine_scopes),
        base_scopes=remap(model.base_scopes),
        base_descriptions=model.base_descriptions,
    )


def _order_indices(model: Model, name: str, band_size: int = 1) -> list[int]:
    """Order candidates through horizontal or vertical spatial bands.

    A band of one is ordinary row/column order. Larger bands interpolate
    between those two extremes and can substantially reduce a particular
    board's frontier.
    """
    if band_size < 1:
        raise ValueError("band size must be positive")
    if name == "rows":
        key = lambda i: (
            model.candidates[i][0] // band_size,
            model.candidates[i][1],
            model.candidates[i][0] % band_size,
        )
    elif name == "columns":
        key = lambda i: (
            model.candidates[i][1] // band_size,
            model.candidates[i][0],
            model.candidates[i][1] % band_size,
        )
    else:
        raise ValueError(f"unknown order {name!r}")
    return sorted(range(len(model.candidates)), key=key)


def _width_estimate(model: Model, order: Sequence[int]) -> tuple[int, int, int]:
    inverse = {old: new for new, old in enumerate(order)}
    n = len(order)
    graph_intervals: list[tuple[int, int]] = []
    for old in order:
        i = inverse[old]
        future = [inverse[x] for x in model.graph[old] if inverse[x] > i]
        if future:
            graph_intervals.append((i, max(future) - 1))
    for scope in model.zero_scopes:
        if scope:
            positions = [inverse[x] for x in scope]
            if min(positions) < max(positions):
                # One connector replaces the clique induced by this opening.
                graph_intervals.append((min(positions), max(positions) - 1))
    factor_intervals: list[tuple[int, int]] = []
    for scope in itertools.chain(model.mine_scopes, model.base_scopes):
        if scope:
            positions = [inverse[x] for x in scope]
            if min(positions) < max(positions):
                factor_intervals.append((min(positions), max(positions) - 1))
    max_graph = max_factors = max_total = 0
    for cut in range(max(0, n - 1)):
        g = sum(lo <= cut <= hi for lo, hi in graph_intervals)
        f = sum(lo <= cut <= hi for lo, hi in factor_intervals)
        max_graph = max(max_graph, g)
        max_factors = max(max_factors, f)
        max_total = max(max_total, g + f)
    return max_total, max_graph, max_factors


def _candidate_band_sizes(size: int) -> list[int]:
    values = {1, size}
    value = 2
    while value < size:
        values.add(value)
        value *= 2
    return sorted(values)


def choose_order(
    model: Model, requested: str, band_size: int | None = None
) -> tuple[Model, str]:
    if requested != "auto":
        size = band_size or 1
        order = _order_indices(model, requested, size)
        name = requested if size == 1 else f"{requested}-band-{size}"
        return _ordered_model(model, order), name
    choices = []
    if band_size is not None:
        candidates = ((name, band_size) for name in ("columns", "rows"))
    else:
        standard = []
        for name in ("columns", "rows"):
            order = _order_indices(model, name, 1)
            standard.append((_width_estimate(model, order), name, order))
        standard_best = min(standard)[0][0]
        choices.extend(standard)
        # Admit a wider band automatically only for a clear width reduction.
        # Ties often lose because the width estimate cannot predict how many
        # connectivity partitions each boundary generates.
        candidates = itertools.chain(
            (("rows", size) for size in _candidate_band_sizes(model.height)[1:]),
            (("columns", size) for size in _candidate_band_sizes(model.width)[1:]),
        )
    for name, size in candidates:
        order = _order_indices(model, name, size)
        display = name if size == 1 else f"{name}-band-{size}"
        estimate = _width_estimate(model, order)
        if band_size is not None or estimate[0] <= standard_best - 2:
            choices.append((estimate, display, order))
    _, name, order = min(choices)
    return _ordered_model(model, order), name


def _canonical_tokens(tokens: list[int], largest: int) -> tuple[int, ...]:
    """Canonicalize component tokens without allocating a dictionary."""
    renumber = [0] * (largest + 1)
    next_label = 1
    for position, token in enumerate(tokens):
        if token:
            label = renumber[token]
            if not label:
                label = next_label
                next_label += 1
                renumber[token] = label
            tokens[position] = label
    return tuple(tokens)


def _prune_dominated(
    table: dict[tuple[tuple[int, ...], int], tuple[int, int]],
    comparison_limit: int,
    base_factor_mask: int,
    active_factor_mask: int,
) -> tuple[dict[tuple[tuple[int, ...], int], tuple[int, int]], int]:
    """Prune a state when a no-costlier hit-mask superset has equal labels."""
    if comparison_limit <= 0 or len(table) < 2:
        return table, 0
    groups: dict[tuple[int, ...], list[tuple[int, int, int]]] = {}
    for (labels, hits), (cost, chosen) in table.items():
        groups.setdefault(labels, []).append((cost, hits, chosen))
    result: dict[tuple[tuple[int, ...], int], tuple[int, int]] = {}
    removed = 0
    comparisons_left = comparison_limit
    for labels, entries in groups.items():
        if len(entries) == 1:
            cost, hits, chosen = entries[0]
            result[(labels, hits)] = (cost, chosen)
            continue
        entries.sort(
            key=lambda item: (
                item[0] + (item[1] & base_factor_mask).bit_count(),
                item[0],
                -item[1].bit_count(),
            )
        )
        kept: list[tuple[int, int, int]] = []
        kept_cost: dict[int, int] = {}
        complete = True
        for cost, hits, chosen in entries:
            free_mask = active_factor_mask & ~hits
            supersets = 1 << free_mask.bit_count()
            work = min(len(kept), supersets)
            if comparisons_left < work:
                complete = False
                break
            comparisons_left -= work
            dominated = False
            if supersets < len(kept):
                extra = free_mask
                while True:
                    prior_cost = kept_cost.get(hits | extra)
                    if prior_cost is not None and prior_cost <= cost:
                        dominated = True
                        break
                    if extra == 0:
                        break
                    extra = (extra - 1) & free_mask
            else:
                for k_cost, k_hits, _ in kept:
                    if k_cost <= cost and (k_hits | hits) == k_hits:
                        dominated = True
                        break
            if dominated:
                removed += 1
            else:
                kept.append((cost, hits, chosen))
                kept_cost[hits] = cost
        if not complete:
            for cost, hits, chosen in entries:
                result[(labels, hits)] = (cost, chosen)
        else:
            for cost, hits, chosen in kept:
                result[(labels, hits)] = (cost, chosen)
    return result, removed


def _ordered_region_ranks(model: Model, order_name: str) -> list[int]:
    """Number of board tiles in the spatial sweep through each candidate."""
    direction = "rows" if order_name.startswith("rows") else "columns"
    match = re.search(r"-band-(\d+)$", order_name)
    band = int(match.group(1)) if match else 1

    def key(cell: Coord) -> tuple[int, int, int]:
        r, c = cell
        if direction == "rows":
            return r // band, c, r % band
        return c // band, r, c % band

    cells = sorted(
        ((r, c) for r in range(model.height) for c in range(model.width)), key=key
    )
    rank = {cell: i + 1 for i, cell in enumerate(cells)}
    return [rank[cell] for cell in model.candidates]


def solve_frontier(
    original_model: Model,
    order: str = "auto",
    max_states: int = 2_000_000,
    band_size: int | None = None,
    progress: bool = False,
    progress_every: int = 10,
    progress_stream: object | None = None,
    dominance_comparisons: int = 1_000_000,
) -> Solution:
    model, order_name = choose_order(original_model, order, band_size)
    n = len(model.candidates)

    # Charge the 3BV baseline up front. Selecting a chord then pays for each
    # newly required mine flag and credits each newly covered 3BV unit. Factor
    # state is one integer bitset, avoiding per-transition objects.
    scopes: list[tuple[int, ...]] = []
    flag_factor_mask = 0
    base_factor_mask = 0
    for is_flag, source in (
        (True, model.mine_scopes),
        (False, model.base_scopes),
    ):
        for scope in source:
            if not scope:
                continue
            factor_id = len(scopes)
            scopes.append(scope)
            if is_flag:
                flag_factor_mask |= 1 << factor_id
            else:
                base_factor_mask |= 1 << factor_id
    factor_member_mask = [0] * n
    factor_min = [0] * len(scopes)
    factor_max = [0] * len(scopes)
    for factor_id, scope in enumerate(scopes):
        factor_min[factor_id] = scope[0]
        factor_max[factor_id] = scope[-1]
        bit = 1 << factor_id
        for variable in scope:
            factor_member_mask[variable] |= bit
    active_after_masks: list[int] = []
    for i in range(n):
        mask = 0
        for factor_id in range(len(scopes)):
            if factor_min[factor_id] <= i < factor_max[factor_id]:
                mask |= 1 << factor_id
        active_after_masks.append(mask)

    # Direct adjacency and zero-opening hyperedge membership are precomputed.
    adjacency_masks = [sum(1 << other for other in edges) for edges in model.graph]
    zero_membership_masks = [0] * n
    zero_limits: list[tuple[int, int]] = []
    for zero_id, scope in enumerate(model.zero_scopes):
        if scope:
            zero_limits.append((scope[0], scope[-1]))
            bit = 1 << zero_id
            for variable in scope:
                zero_membership_masks[variable] |= bit
        else:
            zero_limits.append((0, -1))

    last_future_neighbor = [
        max((other for other in model.graph[i] if other > i), default=i)
        for i in range(n)
    ]
    boundaries: list[tuple[int, ...]] = [tuple()]
    boundary_pos: list[dict[int, int]] = [{}]
    for i in range(n):
        boundary = [v for v in range(i + 1) if last_future_neighbor[v] > i]
        boundary.extend(
            n + zero_id
            for zero_id, (first, last) in enumerate(zero_limits)
            if first <= i < last
        )
        boundary = tuple(boundary)
        boundaries.append(boundary)
        boundary_pos.append({v: p for p, v in enumerate(boundary)})

    if progress_every < 1:
        raise ValueError("progress interval must be positive")
    region_ranks = _ordered_region_ranks(model, order_name) if progress else []
    output = progress_stream if progress_stream is not None else sys.stderr

    # state -> (cost, complete selected-variable bit mask)
    table: dict[tuple[tuple[int, ...], int], tuple[int, int]] = {
        (tuple(), 0): (len(model.base_scopes), 0)
    }
    peak_states = 1
    max_boundary_vertices = 0
    max_active_count = 0
    dominated_total = 0

    for i in range(n):
        old_boundary = boundaries[i]
        new_boundary = boundaries[i + 1]
        old_position = boundary_pos[i]
        active_after = active_after_masks[i]
        member_mask = factor_member_mask[i]
        zero_mask = zero_membership_masks[i]
        adjacent = adjacency_masks[i]
        touch_positions: list[int] = []
        for position, item in enumerate(old_boundary):
            if item < n:
                if adjacent & (1 << item):
                    touch_positions.append(position)
            elif zero_mask & (1 << (item - n)):
                touch_positions.append(position)
        source_positions = tuple(old_position.get(item, -1) for item in new_boundary)
        activate_positions = tuple(
            position
            for position, item in enumerate(new_boundary)
            if item == i
            or (item >= n and zero_mask & (1 << (item - n)))
        )
        next_table: dict[tuple[tuple[int, ...], int], tuple[int, int]] = {}
        connectivity_cache: dict[
            tuple[int, ...],
            tuple[tuple[tuple[int, ...], int], tuple[tuple[int, ...], int]],
        ] = {}

        for (old_labels, old_hits), (old_cost, chosen_mask) in table.items():
            transitions = connectivity_cache.get(old_labels)
            if transitions is None:
                largest_old = max(old_labels, default=0)
                computed: list[tuple[tuple[int, ...], int]] = []
                for selected in (False, True):
                    merge_mask = 0
                    if selected:
                        for position in touch_positions:
                            label = old_labels[position]
                            if label:
                                merge_mask |= 1 << (label - 1)
                    if selected and merge_mask:
                        current_token = (merge_mask & -merge_mask).bit_length()
                    elif selected:
                        current_token = largest_old + 1
                    else:
                        current_token = 0

                    outgoing_tokens = [
                        old_labels[source] if source >= 0 else 0
                        for source in source_positions
                    ]
                    if merge_mask:
                        for position, token in enumerate(outgoing_tokens):
                            if token and merge_mask & (1 << (token - 1)):
                                outgoing_tokens[position] = current_token
                    if selected:
                        for position in activate_positions:
                            outgoing_tokens[position] = current_token

                    represented_mask = 0
                    for token in outgoing_tokens:
                        if token:
                            represented_mask |= 1 << (token - 1)
                    all_components = (1 << largest_old) - 1 if largest_old else 0
                    if merge_mask:
                        all_components = (
                            (all_components & ~merge_mask)
                            | (1 << (current_token - 1))
                        )
                    elif selected:
                        all_components |= 1 << (current_token - 1)
                    closed = (all_components & ~represented_mask).bit_count()
                    labels = _canonical_tokens(
                        outgoing_tokens, max(largest_old, current_token)
                    )
                    computed.append((labels, closed))
                transitions = (computed[0], computed[1])
                connectivity_cache[old_labels] = transitions

            for selected in (False, True):
                new_labels, closed_components = transitions[int(selected)]

                new_hits = old_hits
                factor_cost = 0
                if selected:
                    newly_hit = member_mask & ~old_hits
                    factor_cost = (newly_hit & flag_factor_mask).bit_count()
                    factor_cost -= (newly_hit & base_factor_mask).bit_count()
                    new_hits |= member_mask
                new_hits &= active_after

                new_cost = old_cost + int(selected) + closed_components + factor_cost
                state = (new_labels, new_hits)
                previous = next_table.get(state)
                new_mask = chosen_mask | ((1 << i) if selected else 0)
                if previous is None or new_cost < previous[0]:
                    next_table[state] = (new_cost, new_mask)

        table, removed = _prune_dominated(
            next_table,
            dominance_comparisons,
            base_factor_mask,
            active_after,
        )
        dominated_total += removed
        peak_states = max(peak_states, len(table))
        max_boundary_vertices = max(max_boundary_vertices, len(new_boundary))
        active_count = active_after.bit_count()
        max_active_count = max(max_active_count, active_count)
        if progress and ((i + 1) % progress_every == 0 or i + 1 == n):
            print(
                f"DP: region contains {region_ranks[i]}/{model.height * model.width} tiles; "
                f"processed {i + 1}/{n} chord candidates; "
                f"{len(table):,} valid boundary states; boundary "
                f"{len(new_boundary)} connectivity items + {active_count} factor bits; "
                f"pruned {dominated_total:,} dominated states",
                file=output,
            )
        if len(table) > max_states:
            raise StateLimitExceeded(
                f"frontier grew to {len(table):,} states after variable {i + 1}/{n}; "
                f"increase --max-states or try the other --order"
            )

    final = table.get((tuple(), 0))
    if final is None:
        raise AssertionError("frontier DP did not reach an empty final state")
    clicks, selected_mask = final
    selected_in_ordered_model = {i for i in range(n) if selected_mask & (1 << i)}
    original_index = {cell: i for i, cell in enumerate(original_model.candidates)}
    selected = {
        original_index[model.candidates[i]] for i in selected_in_ordered_model
    }
    solution = construct_solution(original_model, selected, clicks)
    solution.peak_states = peak_states
    solution.max_boundary_vertices = max_boundary_vertices
    solution.max_active_factors = max_active_count
    solution.order_name = order_name
    return solution


def evaluate_set(model: Model, selected: set[int]) -> tuple[int, set[Coord], list[list[int]], list[int]]:
    flags = {
        mine
        for mine, scope in zip(sorted(model.mines), model.mine_scopes)
        if selected.intersection(scope)
    }
    zero_memberships: list[list[int]] = [[] for _ in model.candidates]
    for zero_id, scope in enumerate(model.zero_scopes):
        for candidate in scope:
            zero_memberships[candidate].append(zero_id)
    components: list[list[int]] = []
    unseen = set(selected)
    unused_zeros = set(range(len(model.zero_scopes)))
    while unseen:
        start = unseen.pop()
        component = [start]
        queue = [start]
        while queue:
            v = queue.pop()
            for other in model.graph[v]:
                if other in unseen:
                    unseen.remove(other)
                    component.append(other)
                    queue.append(other)
            for zero_id in zero_memberships[v]:
                if zero_id not in unused_zeros:
                    continue
                unused_zeros.remove(zero_id)
                for other in model.zero_scopes[zero_id]:
                    if other in unseen:
                        unseen.remove(other)
                        component.append(other)
                        queue.append(other)
        components.append(sorted(component))
    components.sort(key=lambda comp: model.candidates[comp[0]])
    uncovered = [
        i for i, scope in enumerate(model.base_scopes) if not selected.intersection(scope)
    ]
    clicks = len(selected) + len(flags) + len(components) + len(uncovered)
    return clicks, flags, components, uncovered


def _component_chord_order(model: Model, component: list[int]) -> list[int]:
    allowed = set(component)
    seed = min(component, key=lambda i: model.candidates[i])
    order = []
    seen = {seed}
    queue = deque([seed])
    zero_memberships: list[list[int]] = [[] for _ in model.candidates]
    for zero_id, scope in enumerate(model.zero_scopes):
        for candidate in scope:
            zero_memberships[candidate].append(zero_id)
    unused_zeros = set(range(len(model.zero_scopes)))
    while queue:
        v = queue.popleft()
        order.append(v)
        for other in sorted(model.graph[v], key=lambda i: model.candidates[i]):
            if other in allowed and other not in seen:
                seen.add(other)
                queue.append(other)
        for zero_id in zero_memberships[v]:
            if zero_id not in unused_zeros:
                continue
            unused_zeros.remove(zero_id)
            for other in sorted(
                model.zero_scopes[zero_id], key=lambda i: model.candidates[i]
            ):
                if other in allowed and other not in seen:
                    seen.add(other)
                    queue.append(other)
    if len(order) != len(component):
        raise AssertionError("reported chord component is disconnected")
    return order


def construct_solution(model: Model, selected: set[int], expected_clicks: int | None = None) -> Solution:
    clicks, flags, components, uncovered = evaluate_set(model, selected)
    if expected_clicks is not None and clicks != expected_clicks:
        raise AssertionError(f"DP cost {expected_clicks} disagrees with evaluator {clicks}")
    actions: list[tuple[str, Coord]] = []
    actions.extend(("flag", cell) for cell in sorted(flags))
    for component in components:
        order = _component_chord_order(model, component)
        actions.append(("left", model.candidates[order[0]]))
        actions.extend(("chord", model.candidates[i]) for i in order)
    for unit in uncovered:
        _, representative = model.base_descriptions[unit]
        actions.append(("left", representative))
    if len(actions) != clicks:
        raise AssertionError("constructed action count disagrees with objective")
    solution = Solution(clicks, selected, flags, components, uncovered, actions)
    validate_actions(model, solution)
    return solution


def validate_actions(model: Model, solution: Solution) -> None:
    """Simulate the generated sequence and assert that it legally clears."""
    opened: set[Coord] = set()
    flagged: set[Coord] = set()

    zero_by_cell = {
        cell: component for component in model.zeros for cell in component
    }

    def reveal(cell: Coord) -> None:
        if cell in model.mines or cell in flagged:
            raise AssertionError(f"attempted to reveal mine/flag at {cell}")
        if cell in opened:
            return
        opened.add(cell)
        if model.numbers[cell] == 0:
            component = zero_by_cell[cell]
            opened.update(component)
            for zero in component:
                opened.update(
                    other
                    for other in neighbors(zero, model.height, model.width)
                    if other not in model.mines
                )

    for action, cell in solution.actions:
        if action == "flag":
            if cell not in model.mines or cell in opened:
                raise AssertionError(f"illegal flag action at {cell}")
            flagged.add(cell)
        elif action == "left":
            reveal(cell)
        elif action == "chord":
            if cell not in opened or model.numbers.get(cell, 0) == 0:
                raise AssertionError(f"illegal chord center at {cell}")
            adjacent = list(neighbors(cell, model.height, model.width))
            if sum(other in flagged for other in adjacent) != model.numbers[cell]:
                raise AssertionError(f"wrong adjacent flag count for chord at {cell}")
            for other in adjacent:
                if other not in flagged and other not in model.mines:
                    reveal(other)
        else:
            raise AssertionError(f"unknown action {action!r}")

    safe = set(model.numbers)
    if opened != safe:
        missing = sorted(safe - opened)
        raise AssertionError(f"action sequence left {len(missing)} safe cells covered")


def solve_bruteforce(model: Model, max_candidates: int = 25) -> Solution:
    n = len(model.candidates)
    if n > max_candidates:
        raise ValueError(
            f"brute force is limited to {max_candidates} candidates; board has {n}"
        )
    best: tuple[int, set[int]] | None = None
    for mask in range(1 << n):
        selected = {i for i in range(n) if mask & (1 << i)}
        clicks = evaluate_set(model, selected)[0]
        if best is None or clicks < best[0]:
            best = clicks, selected
    assert best is not None
    solution = construct_solution(model, best[1], best[0])
    solution.order_name = "brute-force"
    return solution


def solution_as_dict(model: Model, solution: Solution, offset: int) -> dict[str, object]:
    def coord(cell: Coord) -> list[int]:
        return [cell[0] + offset, cell[1] + offset]

    return {
        "optimal_clicks": solution.clicks,
        "three_bv": model.three_bv,
        "chord_squares": [coord(model.candidates[i]) for i in sorted(solution.selected)],
        "flags": [coord(cell) for cell in sorted(solution.flags)],
        "chord_components": [
            [coord(model.candidates[i]) for i in component]
            for component in solution.components
        ],
        "remaining_base_clicks": [
            coord(model.base_descriptions[i][1]) for i in solution.uncovered_units
        ],
        "actions": [
            {"action": action, "row": cell[0] + offset, "column": cell[1] + offset}
            for action, cell in solution.actions
        ],
        "clicks": [list(click) for click in click_tuples(solution, offset)],
        "statistics": {
            "candidate_chords": len(model.candidates),
            "selected_chords": len(solution.selected),
            "flag_clicks": len(solution.flags),
            "component_seed_clicks": len(solution.components),
            "remaining_3bv_clicks": len(solution.uncovered_units),
            "order": solution.order_name,
            "peak_dp_states": solution.peak_states,
            "max_boundary_vertices": solution.max_boundary_vertices,
            "max_active_factors": solution.max_active_factors,
        },
    }


def _format_coord(cell: Coord, offset: int) -> str:
    return f"({cell[0] + offset},{cell[1] + offset})"


def click_tuples(solution: Solution, offset: int = 1) -> list[tuple[str, int, int]]:
    """Return the executable solution as (click_type, x, y) tuples."""
    click_name = {"flag": "right", "left": "left", "chord": "chord"}
    return [
        (click_name[action], cell[1] + offset, cell[0] + offset)
        for action, cell in solution.actions
    ]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "board",
        nargs="?",
        help="grid filename, LlamaSweeper URL, MBF filename, or quoted MBF hex",
    )
    parser.add_argument(
        "--generate",
        choices=("intermediate", "expert"),
        help="generate a random standard board, print its URL, and solve it",
    )
    parser.add_argument(
        "--seed",
        type=int,
        help="random seed for --generate (an effective seed is printed if omitted)",
    )
    parser.add_argument(
        "--format",
        choices=("auto", "grid", "llamasweeper", "mbf"),
        default="auto",
        help="input format (default: detect automatically)",
    )
    parser.add_argument(
        "--method", choices=("frontier", "bruteforce"), default="frontier"
    )
    parser.add_argument("--order", choices=("auto", "rows", "columns"), default="auto")
    parser.add_argument(
        "--band-size",
        type=int,
        help="spatial sweep band width/height (auto tries several sizes)",
    )
    parser.add_argument("--max-states", type=int, default=2_000_000)
    parser.add_argument(
        "--progress", action="store_true", help="print DP frontier progress to stderr"
    )
    parser.add_argument(
        "--progress-every",
        type=int,
        default=10,
        metavar="CANDIDATES",
        help="progress interval (default: 10 chord candidates)",
    )
    parser.add_argument(
        "--dominance-comparisons",
        type=int,
        default=1_000_000,
        help="maximum dominance checks per DP layer; 0 disables pruning",
    )
    parser.add_argument(
        "--verify", action="store_true", help="compare with brute force when small"
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    parser.add_argument(
        "--click-tuples",
        action="store_true",
        help="emit only a list of (click_type, x, y) tuples",
    )
    parser.add_argument("--zero-based", action="store_true")
    args = parser.parse_args(argv)

    try:
        generated_url: str | None = None
        generated_seed: int | None = None
        if args.generate is not None:
            if args.board is not None:
                parser.error("do not supply a board argument with --generate")
            if args.format != "auto":
                parser.error("--format cannot be used with --generate")
            height, width, mines, generated_url, generated_seed = (
                generate_standard_board(args.generate, args.seed)
            )
            print(f"Generated board: {generated_url}", file=sys.stderr, flush=True)
            print(f"Random seed: {generated_seed}", file=sys.stderr, flush=True)
        else:
            if args.board is None:
                parser.error("a board argument or --generate is required")
            if args.seed is not None:
                parser.error("--seed requires --generate")
            height, width, mines = load_board(args.board, args.format)
        model = build_model(height, width, mines)
        if args.method == "bruteforce":
            solution = solve_bruteforce(model)
        else:
            solution = solve_frontier(
                model,
                args.order,
                args.max_states,
                band_size=args.band_size,
                progress=args.progress,
                progress_every=args.progress_every,
                dominance_comparisons=args.dominance_comparisons,
            )
        if args.verify and len(model.candidates) <= 25:
            brute = solve_bruteforce(model)
            if brute.clicks != solution.clicks:
                raise AssertionError(
                    f"frontier result {solution.clicks} != brute-force result {brute.clicks}"
                )
    except (OSError, ValueError, StateLimitExceeded) as exc:
        parser.error(str(exc))

    offset = 0 if args.zero_based else 1
    if args.click_tuples:
        print(click_tuples(solution, offset))
        return 0
    data = solution_as_dict(model, solution, offset)
    if generated_url is not None:
        data["generated_board"] = {
            "difficulty": args.generate,
            "seed": generated_seed,
            "url": generated_url,
        }
    if args.json:
        print(json.dumps(data, indent=2))
        return 0

    print(f"Optimal clicks: {solution.clicks} (3BV without chording: {model.three_bv})")
    print(
        "Breakdown: "
        f"{len(solution.flags)} flags + "
        f"{len(solution.components)} seed left-clicks + "
        f"{len(solution.selected)} chords + "
        f"{len(solution.uncovered_units)} remaining 3BV clicks"
    )
    if solution.order_name != "brute-force":
        print(
            f"DP: {solution.order_name} order, {solution.peak_states:,} peak states, "
            f"boundary {solution.max_boundary_vertices} connectivity items + "
            f"{solution.max_active_factors} factor bits"
        )
    print("Actions:")
    names = {"flag": "FLAG ", "left": "LEFT ", "chord": "CHORD"}
    for number, (action, cell) in enumerate(solution.actions, 1):
        print(f"{number:4d}. {names[action]} {_format_coord(cell, offset)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

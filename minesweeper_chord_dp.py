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
import re
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


@dataclass(frozen=True)
class Factor:
    """An OR factor over selected chord variables.

    kind == "flag": contributes 1 iff at least one variable is selected.
    kind == "base": contributes 1 iff no variable is selected.
    """

    kind: str
    scope: tuple[int, ...]
    item: int


@dataclass
class Model:
    height: int
    width: int
    mines: set[Coord]
    numbers: dict[Coord, int]
    zeros: list[set[Coord]]
    singleton_units: list[Coord]
    candidates: list[Coord]
    graph: list[set[int]]
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

    # Flood opening: a chord on the boundary of a zero component reveals the
    # component, whose flood-fill reveals every other boundary chord square.
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
        for pos, i in enumerate(boundary):
            for j in boundary[pos + 1 :]:
                graph[i].add(j)
                graph[j].add(i)

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
        mine_scopes=remap(model.mine_scopes),
        base_scopes=remap(model.base_scopes),
        base_descriptions=model.base_descriptions,
    )


def _order_indices(model: Model, name: str) -> list[int]:
    if name == "rows":
        key = lambda i: (model.candidates[i][0], model.candidates[i][1])
    elif name == "columns":
        key = lambda i: (model.candidates[i][1], model.candidates[i][0])
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


def choose_order(model: Model, requested: str) -> tuple[Model, str]:
    if requested != "auto":
        order = _order_indices(model, requested)
        return _ordered_model(model, order), requested
    choices = []
    for name in ("columns", "rows"):
        order = _order_indices(model, name)
        choices.append((_width_estimate(model, order), name, order))
    _, name, order = min(choices)
    return _ordered_model(model, order), name


def _canonical_labels(roots: list[int | None]) -> tuple[int, ...]:
    labels: dict[int, int] = {}
    result = []
    for root in roots:
        if root is None:
            result.append(0)
        else:
            if root not in labels:
                labels[root] = len(labels) + 1
            result.append(labels[root])
    return tuple(result)


def solve_frontier(
    original_model: Model,
    order: str = "auto",
    max_states: int = 2_000_000,
) -> Solution:
    model, order_name = choose_order(original_model, order)
    n = len(model.candidates)
    factors = [
        *(Factor("flag", scope, i) for i, scope in enumerate(model.mine_scopes)),
        *(Factor("base", scope, i) for i, scope in enumerate(model.base_scopes)),
    ]
    constant_cost = sum(f.kind == "base" for f in factors if not f.scope)
    factors = [f for f in factors if f.scope]

    factor_min = [min(f.scope) for f in factors]
    factor_max = [max(f.scope) for f in factors]
    factor_members_at: list[list[int]] = [[] for _ in range(n)]
    for f_id, factor in enumerate(factors):
        for variable in factor.scope:
            factor_members_at[variable].append(f_id)

    active_factors: list[tuple[int, ...]] = []
    active_factor_pos: list[dict[int, int]] = []
    for i in range(-1, n):
        current = tuple(
            f_id
            for f_id in range(len(factors))
            if factor_min[f_id] <= i < factor_max[f_id]
        )
        active_factors.append(current)
        active_factor_pos.append({f_id: p for p, f_id in enumerate(current)})
    # Index i+1 corresponds to the state after processing variable i.

    last_future_neighbor = [
        max((other for other in model.graph[i] if other > i), default=i)
        for i in range(n)
    ]
    boundaries: list[tuple[int, ...]] = [tuple()]
    boundary_pos: list[dict[int, int]] = [{}]
    for i in range(n):
        boundary = tuple(v for v in range(i + 1) if last_future_neighbor[v] > i)
        boundaries.append(boundary)
        boundary_pos.append({v: p for p, v in enumerate(boundary)})

    # state -> (cost, complete selected-variable bit mask)
    table: dict[tuple[tuple[int, ...], int], tuple[int, int]] = {
        (tuple(), 0): (constant_cost, 0)
    }
    peak_states = 1
    max_boundary_vertices = 0
    max_active_count = 0

    for i in range(n):
        old_boundary = boundaries[i]
        new_boundary = boundaries[i + 1]
        old_position = boundary_pos[i]
        incoming_factors = active_factors[i]  # after i-1
        outgoing_factors = active_factors[i + 1]  # after i
        incoming_factor_pos = active_factor_pos[i]
        member_factors = factor_members_at[i]
        next_table: dict[tuple[tuple[int, ...], int], tuple[int, int]] = {}

        for (old_labels, old_hits), (old_cost, chosen_mask) in table.items():
            for selected in (False, True):
                # Tiny DSU over selected old-boundary vertices plus i.
                nodes = [v for p, v in enumerate(old_boundary) if old_labels[p] != 0]
                if selected:
                    nodes.append(i)
                parent = {v: v for v in nodes}

                def find(v: int) -> int:
                    while parent[v] != v:
                        parent[v] = parent[parent[v]]
                        v = parent[v]
                    return v

                def union(a: int, b: int) -> None:
                    ra, rb = find(a), find(b)
                    if ra != rb:
                        parent[rb] = ra

                first_for_label: dict[int, int] = {}
                for p, v in enumerate(old_boundary):
                    label = old_labels[p]
                    if not label:
                        continue
                    if label in first_for_label:
                        union(v, first_for_label[label])
                    else:
                        first_for_label[label] = v
                if selected:
                    for other in model.graph[i]:
                        p = old_position.get(other)
                        if p is not None and old_labels[p] != 0:
                            union(i, other)

                outgoing_roots: list[int | None] = []
                represented_roots: set[int] = set()
                for v in new_boundary:
                    is_selected = selected if v == i else (
                        old_labels[old_position[v]] != 0
                    )
                    if is_selected:
                        root = find(v)
                        outgoing_roots.append(root)
                        represented_roots.add(root)
                    else:
                        outgoing_roots.append(None)
                all_roots = {find(v) for v in nodes}
                closed_components = len(all_roots - represented_roots)
                new_labels = _canonical_labels(outgoing_roots)

                hit_by_factor = {
                    f_id: bool(old_hits & (1 << p))
                    for f_id, p in incoming_factor_pos.items()
                }
                for f_id in member_factors:
                    hit_by_factor[f_id] = hit_by_factor.get(f_id, False) or selected

                factor_cost = 0
                for f_id in member_factors:
                    if factor_max[f_id] != i:
                        continue
                    hit = hit_by_factor[f_id]
                    factor_cost += hit if factors[f_id].kind == "flag" else not hit

                new_hits = 0
                for p, f_id in enumerate(outgoing_factors):
                    if hit_by_factor.get(f_id, False):
                        new_hits |= 1 << p

                new_cost = old_cost + int(selected) + closed_components + factor_cost
                state = (new_labels, new_hits)
                previous = next_table.get(state)
                new_mask = chosen_mask | ((1 << i) if selected else 0)
                if previous is None or new_cost < previous[0]:
                    next_table[state] = (new_cost, new_mask)

        table = next_table
        peak_states = max(peak_states, len(table))
        max_boundary_vertices = max(max_boundary_vertices, len(new_boundary))
        max_active_count = max(max_active_count, len(outgoing_factors))
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
    components: list[list[int]] = []
    unseen = set(selected)
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
    while queue:
        v = queue.popleft()
        order.append(v)
        for other in sorted(model.graph[v], key=lambda i: model.candidates[i]):
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


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "board",
        help="grid filename, LlamaSweeper URL, MBF filename, or quoted MBF hex",
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
    parser.add_argument("--max-states", type=int, default=2_000_000)
    parser.add_argument(
        "--verify", action="store_true", help="compare with brute force when small"
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    parser.add_argument("--zero-based", action="store_true")
    args = parser.parse_args(argv)

    try:
        height, width, mines = load_board(args.board, args.format)
        model = build_model(height, width, mines)
        if args.method == "bruteforce":
            solution = solve_bruteforce(model)
        else:
            solution = solve_frontier(model, args.order, args.max_states)
        if args.verify and len(model.candidates) <= 25:
            brute = solve_bruteforce(model)
            if brute.clicks != solution.clicks:
                raise AssertionError(
                    f"frontier result {solution.clicks} != brute-force result {brute.clicks}"
                )
    except (OSError, ValueError, StateLimitExceeded) as exc:
        parser.error(str(exc))

    offset = 0 if args.zero_based else 1
    data = solution_as_dict(model, solution, offset)
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
            f"boundary {solution.max_boundary_vertices} chord vertices + "
            f"{solution.max_active_factors} factor bits"
        )
    print("Actions:")
    names = {"flag": "FLAG ", "left": "LEFT ", "chord": "CHORD"}
    for number, (action, cell) in enumerate(solution.actions, 1):
        print(f"{number:4d}. {names[action]} {_format_coord(cell, offset)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

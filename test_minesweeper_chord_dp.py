import io
import random
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

from minesweeper_chord_dp import (
    Solution,
    build_model,
    click_tuples,
    format_llamasweeper_url,
    generate_standard_board,
    load_board,
    main,
    parse_board,
    parse_llamasweeper,
    parse_mbf,
    solve_bruteforce,
    solve_frontier,
)


class ChordDpTests(unittest.TestCase):
    def solve(self, text):
        h, w, mines = parse_board(text)
        model = build_model(h, w, mines)
        dp = solve_frontier(model, max_states=1_000_000)
        brute = solve_bruteforce(model)
        self.assertEqual(dp.clicks, brute.clicks)
        self.assertEqual(len(dp.actions), dp.clicks)
        return model, dp

    def test_empty_board(self):
        model, solution = self.solve("...\n...\n")
        self.assertEqual(model.three_bv, 1)
        self.assertEqual(solution.clicks, 1)

    def test_all_mines(self):
        model, solution = self.solve("**\n**\n")
        self.assertEqual(model.three_bv, 0)
        self.assertEqual(solution.clicks, 0)

    def test_simple_chord(self):
        model, solution = self.solve("*..\n...\n...\n")
        self.assertLessEqual(solution.clicks, model.three_bv)

    def test_progress_and_band_order(self):
        height, width, mines = parse_board("*...\n....\n..*.\n....\n")
        model = build_model(height, width, mines)
        stream = io.StringIO()
        solution = solve_frontier(
            model,
            order="rows",
            band_size=2,
            progress=True,
            progress_every=2,
            progress_stream=stream,
        )
        self.assertEqual(solution.clicks, solve_bruteforce(model).clicks)
        self.assertEqual(solution.order_name, "rows-band-2")
        output = stream.getvalue()
        self.assertIn("region contains", output)
        self.assertIn("valid boundary states", output)

    def test_zero_openings_are_hyperedges_not_cliques(self):
        model = build_model(4, 4, {(1, 2), (3, 0), (3, 3)})
        self.assertTrue(model.zero_scopes)
        largest = max(model.zero_scopes, key=len)
        self.assertGreater(len(largest), 2)
        self.assertTrue(
            any(b not in model.graph[a] for a in largest for b in largest if a != b)
        )
        self.assertEqual(
            solve_frontier(model).clicks,
            solve_bruteforce(model).clicks,
        )

    def test_llamasweeper_standard_board(self):
        url = (
            "https://llamasweeper.com/#/game/board-editor?b=2&"
            "m=0000000000000000220c8014k0pg0i80h30cc01140oo0ii0hh00"
        )
        height, width, mines = parse_llamasweeper(url)
        self.assertEqual((height, width), (16, 16))
        self.assertEqual(len(mines), 39)
        self.assertIn((5, 3), mines)
        self.assertIn((15, 5), mines)
        self.assertNotIn((0, 0), mines)
        self.assertEqual(load_board(url), (height, width, mines))

    def test_llamasweeper_custom_board(self):
        self.assertEqual(
            parse_llamasweeper("b=43&m=g20"),
            (3, 4, {(0, 0), (2, 0)}),
        )
        self.assertEqual(parse_llamasweeper("b=411&m=000000000"), (11, 4, set()))
        self.assertEqual(parse_llamasweeper("b=1104&m=000000000"), (4, 11, set()))

    def test_generate_standard_boards(self):
        for difficulty, dimensions, mine_count, board_code in (
            ("intermediate", (16, 16), 40, "b=2&"),
            ("expert", (16, 30), 99, "b=3&"),
        ):
            generated = generate_standard_board(difficulty, seed=123456)
            height, width, mines, url, seed = generated
            self.assertEqual((height, width), dimensions)
            self.assertEqual(len(mines), mine_count)
            self.assertEqual(seed, 123456)
            self.assertIn(board_code, url)
            self.assertEqual(parse_llamasweeper(url), (height, width, mines))
            self.assertEqual(
                format_llamasweeper_url(height, width, mines),
                url,
            )
            self.assertEqual(
                generate_standard_board(difficulty, seed=123456),
                generated,
            )

    def test_generate_cli_prints_url_then_solves(self):
        fake_solution = Solution(
            clicks=0,
            selected=set(),
            flags=set(),
            components=[],
            uncovered_units=[],
            actions=[],
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with patch(
            "minesweeper_chord_dp.solve_frontier", return_value=fake_solution
        ) as solve, redirect_stdout(stdout), redirect_stderr(stderr):
            result = main(
                ["--generate", "intermediate", "--seed", "8675309", "--click-tuples"]
            )
        self.assertEqual(result, 0)
        self.assertEqual(stdout.getvalue().strip(), "[]")
        self.assertIn("https://llamasweeper.com/#/game/board-editor?b=2&m=", stderr.getvalue())
        self.assertIn("Random seed: 8675309", stderr.getvalue())
        generated_model = solve.call_args.args[0]
        self.assertEqual((generated_model.height, generated_model.width), (16, 16))
        self.assertEqual(len(generated_model.mines), 40)

    def test_mbf_hex(self):
        code = "1e 10 00 08 10 03 0a 05 15 06 11 08 0a 09 19 0a 15 0c 1d 0e"
        expected = {
            (3, 16),
            (5, 10),
            (6, 21),
            (8, 17),
            (9, 10),
            (10, 25),
            (12, 21),
            (14, 29),
        }
        self.assertEqual(parse_mbf(code), (16, 30, expected))
        self.assertEqual(load_board(code), (16, 30, expected))

    def test_mbf_binary(self):
        data = bytes([4, 3, 0, 2, 0, 0, 3, 2])
        self.assertEqual(parse_mbf(data), (3, 4, {(0, 0), (2, 3)}))

    def test_bad_serialized_inputs(self):
        with self.assertRaisesRegex(ValueError, "padding"):
            parse_llamasweeper("b=43&m=001")
        with self.assertRaisesRegex(ValueError, "declares"):
            parse_mbf("04 03 00 02 00 00")

    def test_click_tuple_output(self):
        solution = Solution(
            clicks=3,
            selected=set(),
            flags=set(),
            components=[],
            uncovered_units=[],
            actions=[("flag", (2, 4)), ("left", (0, 1)), ("chord", (3, 2))],
        )
        self.assertEqual(
            click_tuples(solution),
            [("right", 5, 3), ("left", 2, 1), ("chord", 3, 4)],
        )
        self.assertEqual(
            click_tuples(solution, offset=0),
            [("right", 4, 2), ("left", 1, 0), ("chord", 2, 3)],
        )

    def test_random_small_boards(self):
        rng = random.Random(20260921)
        for height, width in ((2, 3), (3, 3), (3, 4), (4, 3)):
            cells = [(r, c) for r in range(height) for c in range(width)]
            for _ in range(25):
                mines = {cell for cell in cells if rng.random() < 0.25}
                model = build_model(height, width, mines)
                if len(model.candidates) > 20:
                    continue
                with self.subTest(height=height, width=width, mines=mines):
                    dp = solve_frontier(model, order="rows", max_states=1_000_000)
                    dp_columns = solve_frontier(
                        model, order="columns", max_states=1_000_000
                    )
                    brute = solve_bruteforce(model)
                    self.assertEqual(dp.clicks, brute.clicks)
                    self.assertEqual(dp_columns.clicks, brute.clicks)

    def test_random_four_by_four(self):
        rng = random.Random(4404)
        cells = [(r, c) for r in range(4) for c in range(4)]
        for _ in range(40):
            mines = {cell for cell in cells if rng.random() < 0.30}
            model = build_model(4, 4, mines)
            with self.subTest(mines=mines):
                dp = solve_frontier(model, max_states=1_000_000)
                brute = solve_bruteforce(model)
                self.assertEqual(dp.clicks, brute.clicks)


if __name__ == "__main__":
    unittest.main()

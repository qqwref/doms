import random
import unittest

from minesweeper_chord_dp import (
    build_model,
    load_board,
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

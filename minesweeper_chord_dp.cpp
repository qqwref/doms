// Exact Minesweeper click optimizer using a frontier connectivity DP.
// C++17 port of minesweeper_chord_dp.py.

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

constexpr std::string_view LLAMA_ALPHABET = "0123456789abcdefghijklmnopqrstuv";

struct UserError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct StateLimitExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Board {
    int height = 0;
    int width = 0;
    std::vector<uint8_t> mines;
};

struct BaseDescription {
    enum class Kind { Zero, Single } kind = Kind::Single;
    int representative = -1;
};

struct Model {
    int height = 0;
    int width = 0;
    std::vector<uint8_t> mines;
    std::vector<int8_t> numbers;  // -1 for mines
    std::vector<std::vector<int>> zeros;
    std::vector<int> singleton_units;
    std::vector<int> candidates;  // board-cell indexes
    std::vector<int> candidate_index;
    std::vector<std::vector<int>> graph;
    std::vector<std::vector<int>> zero_scopes;
    std::vector<int> mine_cells;
    std::vector<std::vector<int>> mine_scopes;
    std::vector<std::vector<int>> base_scopes;
    std::vector<BaseDescription> base_descriptions;

    int three_bv() const { return static_cast<int>(base_scopes.size()); }
};

enum class ActionType { Flag, Left, Chord };

struct Action {
    ActionType type;
    int cell;
};

struct Solution {
    int clicks = 0;
    std::vector<int> selected;
    std::vector<int> flags;
    std::vector<std::vector<int>> components;
    std::vector<int> uncovered_units;
    std::vector<Action> actions;
    size_t peak_states = 0;
    int max_boundary_vertices = 0;
    int max_active_factors = 0;
    std::string order_name;
    double solve_seconds = 0.0;
};

static std::vector<int> neighbors(int cell, int height, int width) {
    const int r = cell / width;
    const int c = cell % width;
    std::vector<int> out;
    out.reserve(8);
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) continue;
            const int nr = r + dr;
            const int nc = c + dc;
            if (0 <= nr && nr < height && 0 <= nc && nc < width) {
                out.push_back(nr * width + nc);
            }
        }
    }
    return out;
}

static std::string trim(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static Board parse_grid(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> rows;
    std::string raw;
    while (std::getline(input, raw)) {
        std::string line;
        for (unsigned char ch : raw) {
            if (!std::isspace(ch)) line.push_back(static_cast<char>(ch));
        }
        if (line.empty() || line.front() == ';') continue;
        rows.push_back(std::move(line));
    }
    if (rows.empty()) throw UserError("the board is empty");
    const int width = static_cast<int>(rows.front().size());
    for (const auto& row : rows) {
        if (static_cast<int>(row.size()) != width) {
            throw UserError("all board rows must have the same width");
        }
    }
    Board board{static_cast<int>(rows.size()), width,
                std::vector<uint8_t>(rows.size() * width, 0)};
    for (int r = 0; r < board.height; ++r) {
        for (int c = 0; c < width; ++c) {
            const char ch = rows[r][c];
            if (ch == '*' || ch == 'M') {
                board.mines[r * width + c] = 1;
            } else if (ch != '.' && !(ch >= '0' && ch <= '8')) {
                throw UserError(std::string("unsupported board character: '") + ch + "'");
            }
        }
    }
    return board;
}

static std::pair<std::string, std::string> llama_parameters(std::string source) {
    source = trim(source);
    std::string query;
    const size_t hash = source.find('#');
    if (hash != std::string::npos) {
        const size_t question = source.find('?', hash);
        if (question != std::string::npos) query = source.substr(question + 1);
    }
    if (query.empty()) {
        const size_t question = source.find('?');
        if (question != std::string::npos) query = source.substr(question + 1);
    }
    if (query.empty() && (source.find("b=") != std::string::npos ||
                          source.find("m=") != std::string::npos)) {
        query = source;
        while (!query.empty() && (query.front() == '?' || query.front() == '#')) {
            query.erase(query.begin());
        }
    }
    std::vector<std::string> bs;
    std::vector<std::string> ms;
    std::istringstream parts(query);
    std::string part;
    while (std::getline(parts, part, '&')) {
        const size_t eq = part.find('=');
        const std::string key = part.substr(0, eq);
        const std::string value = eq == std::string::npos ? "" : part.substr(eq + 1);
        if (key == "b") bs.push_back(value);
        if (key == "m") ms.push_back(lower_copy(value));
    }
    if (bs.size() != 1 || ms.size() != 1) {
        throw UserError("LlamaSweeper input must contain exactly one b= and one m= parameter");
    }
    return {bs.front(), ms.front()};
}

static std::pair<int, int> llama_dimensions(const std::string& code, size_t groups) {
    if (code == "1" || code == "2" || code == "3") {
        const int width = code == "1" ? 9 : (code == "2" ? 16 : 30);
        const int height = code == "1" ? 9 : 16;
        const size_t expected = (static_cast<size_t>(width) * height + 4) / 5;
        if (groups != expected) {
            throw UserError("LlamaSweeper b=" + code + " needs " +
                            std::to_string(expected) + " m characters, not " +
                            std::to_string(groups));
        }
        return {height, width};
    }
    if (code.size() < 2 ||
        !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        throw UserError("unsupported LlamaSweeper board code b='" + code + "'");
    }
    std::vector<std::pair<int, int>> candidates;
    for (size_t split = 1; split < code.size(); ++split) {
        const int width = std::stoi(code.substr(0, split));
        const int height = std::stoi(code.substr(split));
        if (width <= 0 || height <= 0) continue;
        std::ostringstream reconstructed;
        reconstructed << width << std::setw(static_cast<int>(std::to_string(width).size()))
                      << std::setfill('0') << height;
        if (reconstructed.str() != code) continue;
        if ((static_cast<size_t>(width) * height + 4) / 5 == groups) {
            candidates.emplace_back(height, width);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.empty()) {
        throw UserError("cannot reconcile LlamaSweeper dimensions with m parameter");
    }
    if (candidates.size() > 1) throw UserError("ambiguous LlamaSweeper dimensions");
    return candidates.front();
}

static Board parse_llamasweeper(const std::string& source) {
    auto [code, mine_code] = llama_parameters(source);
    if (mine_code.empty()) throw UserError("the LlamaSweeper m parameter is empty");
    auto [height, width] = llama_dimensions(code, mine_code.size());
    Board board{height, width, std::vector<uint8_t>(height * width, 0)};
    size_t bit_index = 0;
    for (char ch : mine_code) {
        const size_t position = LLAMA_ALPHABET.find(ch);
        if (position == std::string_view::npos) {
            throw UserError(std::string("invalid LlamaSweeper m character: '") + ch + "'");
        }
        for (int shift = 4; shift >= 0; --shift, ++bit_index) {
            const bool bit = (position >> shift) & 1U;
            if (bit_index < board.mines.size()) {
                board.mines[bit_index] = bit;
            } else if (bit) {
                throw UserError("LlamaSweeper m parameter has nonzero padding bits");
            }
        }
    }
    return board;
}

static std::string format_llamasweeper_url(const Board& board) {
    std::string code;
    if (board.height == 9 && board.width == 9) code = "1";
    else if (board.height == 16 && board.width == 16) code = "2";
    else if (board.height == 16 && board.width == 30) code = "3";
    else throw UserError("LlamaSweeper URL output supports only standard dimensions");
    std::string mine_code;
    for (size_t start = 0; start < board.mines.size(); start += 5) {
        unsigned value = 0;
        for (size_t offset = 0; offset < 5; ++offset) {
            value <<= 1;
            if (start + offset < board.mines.size() && board.mines[start + offset]) value |= 1;
        }
        mine_code.push_back(LLAMA_ALPHABET[value]);
    }
    return "https://llamasweeper.com/#/game/board-editor?b=" + code + "&m=" + mine_code;
}

static std::vector<std::string> split_tokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char ch : text) {
        if (std::isspace(ch) || ch == ',') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(ch));
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

static bool looks_like_mbf_hex(const std::string& text) {
    const auto tokens = split_tokens(trim(text));
    if (tokens.size() < 4) return false;
    static const std::regex byte_re(R"((?:0x)?[0-9a-fA-F]{2})");
    return std::all_of(tokens.begin(), tokens.end(),
                       [](const std::string& token) { return std::regex_match(token, byte_re); });
}

static std::vector<uint8_t> mbf_hex_bytes(const std::string& text) {
    const auto tokens = split_tokens(trim(text));
    if (tokens.size() < 4) throw UserError("MBF hexadecimal must contain at least four bytes");
    std::vector<uint8_t> bytes;
    bytes.reserve(tokens.size());
    static const std::regex byte_re(R"((?:0x)?[0-9a-fA-F]{2})");
    for (std::string token : tokens) {
        if (!std::regex_match(token, byte_re)) {
            throw UserError("invalid MBF hexadecimal byte '" + token + "'");
        }
        if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) token.erase(0, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
    }
    return bytes;
}

static Board parse_mbf_bytes(const std::vector<uint8_t>& data) {
    if (data.size() < 4) throw UserError("MBF data must contain a four-byte header");
    const int width = data[0];
    const int height = data[1];
    const int mine_count = 256 * data[2] + data[3];
    if (width == 0 || height == 0) throw UserError("MBF width and height must be nonzero");
    const size_t expected = 4 + 2 * static_cast<size_t>(mine_count);
    if (data.size() != expected) {
        throw UserError("MBF header declares " + std::to_string(mine_count) +
                        " mines (" + std::to_string(expected) + " bytes total), but input contains " +
                        std::to_string(data.size()) + " bytes");
    }
    Board board{height, width, std::vector<uint8_t>(height * width, 0)};
    for (size_t pos = 4; pos < data.size(); pos += 2) {
        const int x = data[pos];
        const int y = data[pos + 1];
        if (x >= width || y >= height) throw UserError("MBF mine coordinate is outside the board");
        const int cell = y * width + x;
        if (board.mines[cell]) throw UserError("MBF repeats a mine coordinate");
        board.mines[cell] = 1;
    }
    return board;
}

static std::string read_file_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw UserError("cannot open input file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

static Board load_board(const std::string& source, const std::string& format) {
    std::error_code ec;
    const fs::path path(source);
    const bool is_file = fs::is_regular_file(path, ec);
    std::string raw = is_file ? read_file_text(path) : source;
    const bool mbf_file = is_file && lower_copy(path.extension().string()) == ".mbf";
    if (format == "mbf" || (format == "auto" && mbf_file)) {
        if (looks_like_mbf_hex(raw)) return parse_mbf_bytes(mbf_hex_bytes(raw));
        return parse_mbf_bytes(std::vector<uint8_t>(raw.begin(), raw.end()));
    }
    if (format == "grid") return parse_grid(raw);
    if (format == "llamasweeper") return parse_llamasweeper(raw);
    if (format != "auto") throw UserError("unknown input format '" + format + "'");
    const std::string lowered = lower_copy(raw);
    if (lowered.find("llamasweeper.com") != std::string::npos ||
        (raw.find("b=") != std::string::npos && raw.find("m=") != std::string::npos)) {
        return parse_llamasweeper(raw);
    }
    if (looks_like_mbf_hex(raw)) return parse_mbf_bytes(mbf_hex_bytes(raw));
    return parse_grid(raw);
}

static Board generate_standard_board(const std::string& difficulty,
                                     std::optional<uint64_t> seed,
                                     uint64_t& effective_seed) {
    int height, width, mine_count;
    if (difficulty == "intermediate") {
        height = 16; width = 16; mine_count = 40;
    } else if (difficulty == "expert") {
        height = 16; width = 30; mine_count = 99;
    } else {
        throw UserError("unknown generated difficulty '" + difficulty + "'");
    }
    if (seed) effective_seed = *seed;
    else {
        std::random_device rd;
        effective_seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    }
    std::mt19937_64 rng(effective_seed);
    std::vector<int> cells(height * width);
    std::iota(cells.begin(), cells.end(), 0);
    auto bounded_random = [&](uint64_t bound) {
        // Rejection sampling makes seeded boards independent of the standard
        // library's uniform_int_distribution/shuffle implementation.
        const uint64_t threshold = (uint64_t{0} - bound) % bound;
        uint64_t value;
        do value = rng(); while (value < threshold);
        return value % bound;
    };
    for (int i = 0; i < mine_count; ++i) {
        const int j = i + static_cast<int>(bounded_random(cells.size() - i));
        std::swap(cells[i], cells[j]);
    }
    Board board{height, width, std::vector<uint8_t>(height * width, 0)};
    for (int i = 0; i < mine_count; ++i) board.mines[cells[i]] = 1;
    return board;
}

static Model build_model(const Board& board) {
    Model model;
    model.height = board.height;
    model.width = board.width;
    model.mines = board.mines;
    const int cells = board.height * board.width;
    model.numbers.assign(cells, -1);
    for (int cell = 0; cell < cells; ++cell) {
        if (board.mines[cell]) continue;
        int number = 0;
        for (int other : neighbors(cell, board.height, board.width)) number += board.mines[other];
        model.numbers[cell] = static_cast<int8_t>(number);
    }

    std::vector<uint8_t> unseen_zero(cells, 0);
    for (int cell = 0; cell < cells; ++cell) unseen_zero[cell] = model.numbers[cell] == 0;
    for (int start = 0; start < cells; ++start) {
        if (!unseen_zero[start]) continue;
        unseen_zero[start] = 0;
        std::vector<int> component;
        std::vector<int> stack{start};
        while (!stack.empty()) {
            const int cell = stack.back();
            stack.pop_back();
            component.push_back(cell);
            for (int other : neighbors(cell, board.height, board.width)) {
                if (unseen_zero[other]) {
                    unseen_zero[other] = 0;
                    stack.push_back(other);
                }
            }
        }
        std::sort(component.begin(), component.end());
        model.zeros.push_back(std::move(component));
    }
    std::sort(model.zeros.begin(), model.zeros.end(),
              [](const auto& a, const auto& b) { return a.front() < b.front(); });

    std::vector<uint8_t> adjacent_to_zero(cells, 0);
    for (const auto& component : model.zeros) {
        for (int zero : component) {
            for (int other : neighbors(zero, board.height, board.width)) {
                if (!board.mines[other]) adjacent_to_zero[other] = 1;
            }
        }
    }
    for (int cell = 0; cell < cells; ++cell) {
        if (model.numbers[cell] > 0 && !adjacent_to_zero[cell]) {
            model.singleton_units.push_back(cell);
        }
        if (model.numbers[cell] > 0) model.candidates.push_back(cell);
        if (board.mines[cell]) model.mine_cells.push_back(cell);
    }
    model.candidate_index.assign(cells, -1);
    for (int i = 0; i < static_cast<int>(model.candidates.size()); ++i) {
        model.candidate_index[model.candidates[i]] = i;
    }
    model.graph.resize(model.candidates.size());
    for (int i = 0; i < static_cast<int>(model.candidates.size()); ++i) {
        for (int other_cell : neighbors(model.candidates[i], board.height, board.width)) {
            const int j = model.candidate_index[other_cell];
            if (j >= 0 && j != i) model.graph[i].push_back(j);
        }
        std::sort(model.graph[i].begin(), model.graph[i].end());
        model.graph[i].erase(std::unique(model.graph[i].begin(), model.graph[i].end()),
                             model.graph[i].end());
    }
    for (const auto& component : model.zeros) {
        std::set<int> boundary;
        for (int zero : component) {
            for (int cell : neighbors(zero, board.height, board.width)) {
                const int candidate = model.candidate_index[cell];
                if (candidate >= 0) boundary.insert(candidate);
            }
        }
        model.zero_scopes.emplace_back(boundary.begin(), boundary.end());
    }
    for (int mine : model.mine_cells) {
        std::vector<int> scope;
        for (int cell : neighbors(mine, board.height, board.width)) {
            const int candidate = model.candidate_index[cell];
            if (candidate >= 0) scope.push_back(candidate);
        }
        std::sort(scope.begin(), scope.end());
        model.mine_scopes.push_back(std::move(scope));
    }
    for (size_t i = 0; i < model.zeros.size(); ++i) {
        model.base_scopes.push_back(model.zero_scopes[i]);
        model.base_descriptions.push_back({BaseDescription::Kind::Zero, model.zeros[i].front()});
    }
    for (int cell : model.singleton_units) {
        std::set<int> scope{model.candidate_index[cell]};
        for (int other : neighbors(cell, board.height, board.width)) {
            const int candidate = model.candidate_index[other];
            if (candidate >= 0) scope.insert(candidate);
        }
        model.base_scopes.emplace_back(scope.begin(), scope.end());
        model.base_descriptions.push_back({BaseDescription::Kind::Single, cell});
    }
    return model;
}

static Model ordered_model(const Model& model, const std::vector<int>& order) {
    Model result = model;
    const int q = static_cast<int>(order.size());
    std::vector<int> inverse(q);
    result.candidates.clear();
    result.candidates.reserve(q);
    for (int next = 0; next < q; ++next) {
        inverse[order[next]] = next;
        result.candidates.push_back(model.candidates[order[next]]);
    }
    result.candidate_index.assign(model.height * model.width, -1);
    for (int i = 0; i < q; ++i) result.candidate_index[result.candidates[i]] = i;
    result.graph.assign(q, {});
    for (int old_i : order) {
        const int next_i = inverse[old_i];
        for (int old_j : model.graph[old_i]) result.graph[next_i].push_back(inverse[old_j]);
        std::sort(result.graph[next_i].begin(), result.graph[next_i].end());
    }
    auto remap = [&](const std::vector<std::vector<int>>& scopes) {
        std::vector<std::vector<int>> mapped;
        mapped.reserve(scopes.size());
        for (const auto& scope : scopes) {
            std::vector<int> next;
            next.reserve(scope.size());
            for (int value : scope) next.push_back(inverse[value]);
            std::sort(next.begin(), next.end());
            mapped.push_back(std::move(next));
        }
        return mapped;
    };
    result.zero_scopes = remap(model.zero_scopes);
    result.mine_scopes = remap(model.mine_scopes);
    result.base_scopes = remap(model.base_scopes);
    return result;
}

static std::vector<int> order_indices(const Model& model, const std::string& name,
                                      int band_size = 1) {
    if (band_size < 1) throw UserError("band size must be positive");
    std::vector<int> order(model.candidates.size());
    std::iota(order.begin(), order.end(), 0);
    auto key = [&](int i) {
        const int r = model.candidates[i] / model.width;
        const int c = model.candidates[i] % model.width;
        if (name == "rows") return std::tuple<int, int, int>{r / band_size, c, r % band_size};
        if (name == "columns") return std::tuple<int, int, int>{c / band_size, r, c % band_size};
        throw UserError("unknown order '" + name + "'");
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    return order;
}

using WidthEstimate = std::tuple<int, int, int>;

static WidthEstimate width_estimate(const Model& model, const std::vector<int>& order) {
    const int q = static_cast<int>(order.size());
    std::vector<int> inverse(q);
    for (int i = 0; i < q; ++i) inverse[order[i]] = i;
    std::vector<std::pair<int, int>> graph_intervals;
    for (int old : order) {
        const int position = inverse[old];
        int last = position;
        for (int other : model.graph[old]) {
            if (inverse[other] > position) last = std::max(last, inverse[other]);
        }
        if (last > position) graph_intervals.emplace_back(position, last - 1);
    }
    for (const auto& scope : model.zero_scopes) {
        if (scope.empty()) continue;
        int first = q;
        int last = -1;
        for (int item : scope) {
            first = std::min(first, inverse[item]);
            last = std::max(last, inverse[item]);
        }
        if (first < last) graph_intervals.emplace_back(first, last - 1);
    }
    std::vector<std::pair<int, int>> factor_intervals;
    auto add_factors = [&](const std::vector<std::vector<int>>& scopes) {
        for (const auto& scope : scopes) {
            if (scope.empty()) continue;
            int first = q;
            int last = -1;
            for (int item : scope) {
                first = std::min(first, inverse[item]);
                last = std::max(last, inverse[item]);
            }
            if (first < last) factor_intervals.emplace_back(first, last - 1);
        }
    };
    add_factors(model.mine_scopes);
    add_factors(model.base_scopes);
    int max_graph = 0;
    int max_factors = 0;
    int max_total = 0;
    for (int cut = 0; cut < std::max(0, q - 1); ++cut) {
        int graph = 0;
        int factors = 0;
        for (auto [lo, hi] : graph_intervals) graph += lo <= cut && cut <= hi;
        for (auto [lo, hi] : factor_intervals) factors += lo <= cut && cut <= hi;
        max_graph = std::max(max_graph, graph);
        max_factors = std::max(max_factors, factors);
        max_total = std::max(max_total, graph + factors);
    }
    return {max_total, max_graph, max_factors};
}

static std::vector<int> candidate_band_sizes(int size) {
    std::set<int> values{1, size};
    for (int value = 2; value < size; value *= 2) values.insert(value);
    return {values.begin(), values.end()};
}

struct OrderChoice {
    WidthEstimate estimate;
    std::string name;
    std::vector<int> order;
};

static std::pair<Model, std::string> choose_order(const Model& model,
                                                   const std::string& requested,
                                                   std::optional<int> band_size) {
    if (requested != "auto") {
        const int size = band_size.value_or(1);
        auto order = order_indices(model, requested, size);
        const std::string name = size == 1 ? requested : requested + "-band-" + std::to_string(size);
        return {ordered_model(model, order), name};
    }
    std::vector<OrderChoice> choices;
    int standard_best = std::numeric_limits<int>::max();
    if (!band_size) {
        for (const std::string name : {"columns", "rows"}) {
            auto order = order_indices(model, name, 1);
            auto estimate = width_estimate(model, order);
            standard_best = std::min(standard_best, std::get<0>(estimate));
            choices.push_back({estimate, name, std::move(order)});
        }
    }
    std::vector<std::pair<std::string, int>> candidates;
    if (band_size) {
        candidates.emplace_back("columns", *band_size);
        candidates.emplace_back("rows", *band_size);
    } else {
        auto row_sizes = candidate_band_sizes(model.height);
        auto column_sizes = candidate_band_sizes(model.width);
        for (size_t i = 1; i < row_sizes.size(); ++i) candidates.emplace_back("rows", row_sizes[i]);
        for (size_t i = 1; i < column_sizes.size(); ++i) candidates.emplace_back("columns", column_sizes[i]);
    }
    for (const auto& [name, size] : candidates) {
        auto order = order_indices(model, name, size);
        auto estimate = width_estimate(model, order);
        if (band_size || std::get<0>(estimate) <= standard_best - 2) {
            choices.push_back({estimate, name + "-band-" + std::to_string(size), std::move(order)});
        }
    }
    auto best = std::min_element(choices.begin(), choices.end(), [](const auto& a, const auto& b) {
        return std::tie(a.estimate, a.name) < std::tie(b.estimate, b.name);
    });
    return {ordered_model(model, best->order), best->name};
}

// Most standard-board states need at most four words. Keeping those words in
// the object avoids millions of tiny heap allocations in the DP hash tables.
class Bits {
public:
    static constexpr size_t INLINE_WORDS = 4;

    Bits() = default;
    explicit Bits(size_t count) : size_(count) {
        if (size_ > INLINE_WORDS) heap_ = std::make_unique<uint64_t[]>(size_);
    }
    Bits(const Bits& other) : Bits(other.size_) {
        std::copy_n(other.data(), size_, data());
    }
    Bits& operator=(const Bits& other) {
        if (this == &other) return *this;
        if (size_ != other.size_) {
            size_ = other.size_;
            heap_.reset();
            if (size_ > INLINE_WORDS) heap_ = std::make_unique<uint64_t[]>(size_);
        }
        std::copy_n(other.data(), size_, data());
        return *this;
    }
    Bits(Bits&&) noexcept = default;
    Bits& operator=(Bits&&) noexcept = default;

    size_t size() const { return size_; }
    uint64_t* data() { return size_ > INLINE_WORDS ? heap_.get() : inline_.data(); }
    const uint64_t* data() const { return size_ > INLINE_WORDS ? heap_.get() : inline_.data(); }
    uint64_t& operator[](size_t index) { return data()[index]; }
    uint64_t operator[](size_t index) const { return data()[index]; }
    bool operator==(const Bits& other) const {
        return size_ == other.size_ && std::equal(data(), data() + size_, other.data());
    }

private:
    size_t size_ = 0;
    std::array<uint64_t, INLINE_WORDS> inline_{};
    std::unique_ptr<uint64_t[]> heap_;
};

static void set_bit(Bits& bits, int position) {
    bits[position / 64] |= uint64_t{1} << (position % 64);
}

static bool test_bit(const Bits& bits, int position) {
    return (bits[position / 64] >> (position % 64)) & 1U;
}

static int bit_count(const Bits& bits) {
    int count = 0;
    for (size_t i = 0; i < bits.size(); ++i) count += __builtin_popcountll(bits[i]);
    return count;
}

static int bit_count_new_and(const Bits& member, const Bits& old, const Bits& filter) {
    int count = 0;
    for (size_t i = 0; i < member.size(); ++i) {
        count += __builtin_popcountll(member[i] & ~old[i] & filter[i]);
    }
    return count;
}

// Test whether the candidate's worst extra cost relative to the other state
// fits within allowance. In "good-bit" form, flag hits and unhit base factors
// are both desirable, and the penalty is simply good(other) & ~good(candidate).
// Stop as soon as the candidate can no longer dominate.
static bool dominance_penalty_at_most(const Bits& candidate, const Bits& other,
                                      const Bits& base_mask, int allowance) {
    int penalty = 0;
    for (size_t i = 0; i < candidate.size(); ++i) {
        const uint64_t candidate_good = candidate[i] ^ base_mask[i];
        const uint64_t other_good = other[i] ^ base_mask[i];
        penalty += __builtin_popcountll(other_good & ~candidate_good);
        if (penalty > allowance) return false;
    }
    return true;
}

static size_t hash_combine(size_t seed, uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

struct WordsHash {
    size_t operator()(const std::vector<uint64_t>& words) const {
        size_t hash = words.size();
        for (uint64_t word : words) hash = hash_combine(hash, word);
        return hash;
    }
};

// A connectivity state is a sorted multiset of future-reachability bitsets,
// one per unfinished selected component. Intern it once per layer so each DP
// state needs only a 32-bit ID.
class ConnectivityPool {
public:
    ConnectivityPool() { intern(std::vector<uint64_t>{}); }

    uint32_t intern(const std::vector<uint64_t>& signature) {
        auto found = ids_.find(signature);
        if (found != ids_.end()) return found->second;
        const uint32_t id = static_cast<uint32_t>(by_id_.size());
        auto inserted = ids_.emplace(signature, id).first;
        by_id_.push_back(&inserted->first);
        return id;
    }

    const std::vector<uint64_t>& operator[](uint32_t id) const { return *by_id_[id]; }
    size_t size() const { return by_id_.size(); }
    void reserve(size_t count) {
        ids_.reserve(count);
        by_id_.reserve(count);
    }
    void reset(size_t reserve_count = 0) {
        ids_.clear();
        by_id_.clear();
        reserve(reserve_count);
        intern(std::vector<uint64_t>{});
    }
    void swap(ConnectivityPool& other) noexcept {
        ids_.swap(other.ids_);
        by_id_.swap(other.by_id_);
    }

private:
    std::unordered_map<std::vector<uint64_t>, uint32_t, WordsHash> ids_;
    std::vector<const std::vector<uint64_t>*> by_id_;
};

struct State {
    uint32_t connectivity_id = 0;
    Bits hits;
    bool operator==(const State& other) const {
        return connectivity_id == other.connectivity_id && hits == other.hits;
    }
};

struct StateHash {
    size_t operator()(const State& state) const {
        size_t hash = hash_combine(0, state.connectivity_id);
        for (size_t i = 0; i < state.hits.size(); ++i) hash = hash_combine(hash, state.hits[i]);
        return hash;
    }
};

struct Record {
    int cost = 0;
    Bits chosen;
};

// The DP creates a fresh table for each layer, fills it, then only erases from
// it during dominance pruning. Store values densely and keep a compact open-
// addressed index alongside them. This avoids one allocation and one pointer
// chase per state while preserving stable dense-entry indexes during pruning.
class Table {
public:
    using value_type = std::pair<State, Record>;

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Table::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;

        iterator() = default;
        reference operator*() const { return owner_->entries_[index_]; }
        pointer operator->() const { return &owner_->entries_[index_]; }
        iterator& operator++() {
            ++index_;
            skip_dead();
            return *this;
        }
        iterator operator++(int) {
            iterator copy = *this;
            ++*this;
            return copy;
        }
        bool operator==(const iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        friend class Table;
        iterator(Table* owner, size_t index) : owner_(owner), index_(index) { skip_dead(); }
        void skip_dead() {
            if (owner_ == nullptr) return;
            while (index_ < owner_->entries_.size() && !owner_->alive_[index_]) ++index_;
        }

        Table* owner_ = nullptr;
        size_t index_ = 0;
    };

    Table() = default;
    Table(Table&&) noexcept = default;
    Table& operator=(Table&&) noexcept = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    size_t size() const { return live_size_; }
    bool empty() const { return live_size_ == 0; }
    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, entries_.size()); }

    void reserve(size_t count) {
        if (count > static_cast<size_t>(DELETED) - 1) {
            throw std::length_error("too many DP states for the flat table");
        }
        entries_.reserve(count);
        alive_.reserve(count);
        size_t capacity = 8;
        while (capacity - capacity / 4 < count) {
            if (capacity > std::numeric_limits<size_t>::max() / 2) {
                throw std::length_error("DP hash table capacity overflow");
            }
            capacity *= 2;
        }
        if (capacity > slots_.size()) rehash(capacity);
    }

    iterator find(const State& state) {
        if (slots_.empty()) {
            for (size_t index = 0; index < entries_.size(); ++index) {
                if (alive_[index] && entries_[index].first == state) {
                    return iterator(this, index);
                }
            }
            return end();
        }
        const size_t mask = slots_.size() - 1;
        size_t slot = StateHash{}(state) & mask;
        for (;;) {
            const uint32_t entry = slots_[slot];
            if (entry == EMPTY) return end();
            if (entry != DELETED && alive_[entry] && entries_[entry].first == state) {
                return iterator(this, entry);
            }
            slot = (slot + 1) & mask;
        }
    }

    // Callers use find() first when duplicates are possible, so this insertion
    // path deliberately avoids a second equality scan.
    std::pair<iterator, bool> emplace(State state, Record record) {
        ensure_insert_capacity();
        const size_t entry_index = entries_.size();
        entries_.emplace_back(std::move(state), std::move(record));
        alive_.push_back(1);

        const size_t mask = slots_.size() - 1;
        size_t slot = StateHash{}(entries_.back().first) & mask;
        size_t first_deleted = std::numeric_limits<size_t>::max();
        for (;;) {
            if (slots_[slot] == EMPTY) {
                if (first_deleted != std::numeric_limits<size_t>::max()) {
                    slot = first_deleted;
                    --deleted_slots_;
                } else {
                    ++used_slots_;
                }
                slots_[slot] = static_cast<uint32_t>(entry_index);
                ++live_size_;
                return {iterator(this, entry_index), true};
            }
            if (slots_[slot] == DELETED &&
                first_deleted == std::numeric_limits<size_t>::max()) {
                first_deleted = slot;
            }
            slot = (slot + 1) & mask;
        }
    }

    void erase(iterator where) {
        const size_t entry_index = where.index_;
        if (entry_index >= entries_.size() || !alive_[entry_index]) return;
        const size_t mask = slots_.size() - 1;
        size_t slot = StateHash{}(entries_[entry_index].first) & mask;
        while (slots_[slot] != EMPTY) {
            if (slots_[slot] == entry_index) {
                slots_[slot] = DELETED;
                ++deleted_slots_;
                alive_[entry_index] = 0;
                --live_size_;
                // Inline words occupy the dense slot regardless, but release
                // any wide-board overflow allocations immediately.
                if (entries_[entry_index].first.hits.size() > Bits::INLINE_WORDS) {
                    entries_[entry_index].first.hits = Bits{};
                }
                if (entries_[entry_index].second.chosen.size() > Bits::INLINE_WORDS) {
                    entries_[entry_index].second.chosen = Bits{};
                }
                return;
            }
            slot = (slot + 1) & mask;
        }
        throw std::logic_error("flat DP table lost an indexed state");
    }

    void swap(Table& other) noexcept {
        entries_.swap(other.entries_);
        alive_.swap(other.alive_);
        slots_.swap(other.slots_);
        std::swap(live_size_, other.live_size_);
        std::swap(used_slots_, other.used_slots_);
        std::swap(deleted_slots_, other.deleted_slots_);
    }

private:
    static constexpr uint32_t EMPTY = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t DELETED = EMPTY - 1;

    void ensure_insert_capacity() {
        if (slots_.empty()) {
            rehash(8);
        } else if ((used_slots_ + 1) * 4 > slots_.size() * 3) {
            rehash(slots_.size() * 2);
        }
    }

    void rehash(size_t capacity) {
        std::vector<uint32_t> replacement(capacity, EMPTY);
        const size_t mask = capacity - 1;
        for (size_t index = 0; index < entries_.size(); ++index) {
            if (!alive_[index]) continue;
            size_t slot = StateHash{}(entries_[index].first) & mask;
            while (replacement[slot] != EMPTY) slot = (slot + 1) & mask;
            replacement[slot] = static_cast<uint32_t>(index);
        }
        slots_.swap(replacement);
        used_slots_ = live_size_;
        deleted_slots_ = 0;
    }

    std::vector<value_type> entries_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> slots_;
    size_t live_size_ = 0;
    size_t used_slots_ = 0;
    size_t deleted_slots_ = 0;
};

struct ConnectivityTransition {
    uint32_t connectivity_id = 0;
    int closed = 0;
};

using DominanceKey = std::tuple<int, int, int>;

// A coarser signature can represent several finer components as one, provided
// every finer component's future reachability is contained in its representative.
// Requiring every coarse component to represent at least one fine component
// prevents an unmatched component from adding a future seed-click liability.
static bool connectivity_coarsens(const std::vector<uint64_t>& coarse,
                                  const std::vector<uint64_t>& fine,
                                  size_t signature_words) {
    const size_t coarse_count = coarse.size() / signature_words;
    const size_t fine_count = fine.size() / signature_words;
    if (coarse_count > fine_count) return false;
    if (coarse_count == 0) return fine_count == 0;

    // Normal Minesweeper frontiers have far fewer than 64 live components.
    // Keep the entire eligibility graph and matching state on the stack in
    // that common case instead of allocating several nested vectors for every
    // pair of connectivity signatures.
    constexpr size_t kInlineComponents = 64;
    if (coarse_count <= kInlineComponents && fine_count <= kInlineComponents) {
        std::array<uint64_t, kInlineComponents> eligible{};
        uint64_t represented = 0;
        for (size_t c = 0; c < coarse_count; ++c) {
            uint64_t mask = 0;
            for (size_t f = 0; f < fine_count; ++f) {
                bool subset = true;
                for (size_t word = 0; word < signature_words; ++word) {
                    const uint64_t fine_word = fine[f * signature_words + word];
                    const uint64_t coarse_word = coarse[c * signature_words + word];
                    if ((fine_word | coarse_word) != coarse_word) {
                        subset = false;
                        break;
                    }
                }
                if (subset) mask |= uint64_t{1} << f;
            }
            eligible[c] = mask;
            represented |= mask;
        }
        const uint64_t all_fine = fine_count == 64
            ? ~uint64_t{0} : ((uint64_t{1} << fine_count) - 1);
        if (represented != all_fine) return false;

        std::array<int, kInlineComponents> fine_match;
        fine_match.fill(-1);
        for (size_t c = 0; c < coarse_count; ++c) {
            uint64_t seen = 0;
            auto augment = [&](auto&& self, size_t candidate) -> bool {
                uint64_t choices = eligible[candidate] & ~seen;
                while (choices != 0) {
                    const size_t f = static_cast<size_t>(__builtin_ctzll(choices));
                    const uint64_t bit = uint64_t{1} << f;
                    choices &= choices - 1;
                    seen |= bit;
                    if (fine_match[f] < 0 ||
                        self(self, static_cast<size_t>(fine_match[f]))) {
                        fine_match[f] = static_cast<int>(candidate);
                        return true;
                    }
                }
                return false;
            };
            if (!augment(augment, c)) return false;
        }
        return true;
    }

    // Extremely wide custom boards retain the general dynamically-sized path.
    std::vector<std::vector<uint8_t>> eligible(
        coarse_count, std::vector<uint8_t>(fine_count, 0));
    for (size_t f = 0; f < fine_count; ++f) {
        bool represented = false;
        for (size_t c = 0; c < coarse_count; ++c) {
            bool subset = true;
            for (size_t word = 0; word < signature_words; ++word) {
                const uint64_t fine_word = fine[f * signature_words + word];
                const uint64_t coarse_word = coarse[c * signature_words + word];
                if ((fine_word | coarse_word) != coarse_word) {
                    subset = false;
                    break;
                }
            }
            eligible[c][f] = subset;
            represented = represented || subset;
        }
        if (!represented) return false;
    }

    // Find distinct fine components to anchor every coarse component. All
    // remaining fine components may map many-to-one to any eligible anchor.
    std::vector<int> fine_match(fine_count, -1);
    for (size_t c = 0; c < coarse_count; ++c) {
        std::vector<uint8_t> seen(fine_count, 0);
        auto augment = [&](auto&& self, size_t candidate) -> bool {
            for (size_t f = 0; f < fine_count; ++f) {
                if (!eligible[candidate][f] || seen[f]) continue;
                seen[f] = 1;
                if (fine_match[f] < 0 || self(self, static_cast<size_t>(fine_match[f]))) {
                    fine_match[f] = static_cast<int>(candidate);
                    return true;
                }
            }
            return false;
        };
        if (!augment(augment, c)) return false;
    }
    return true;
}

static size_t prune_dominated(Table& table,
                              uint64_t comparison_limit,
                              const Bits& base_mask,
                              const ConnectivityPool& connectivity_pool,
                              size_t signature_words) {
    if (comparison_limit == 0 || table.size() < 2) return 0;
    using Item = Table::iterator;
    struct CachedItem {
        Item item;
        DominanceKey key;
        int base_hits = 0;
        int flag_hits = 0;
        int quasi_score = 0;
        bool exact_dominated = false;
        bool cross_dominated = false;
    };
    struct Bucket {
        std::vector<uint32_t> connectivity_ids;
        std::vector<CachedItem> entries;
    };

    const size_t connectivity_count = connectivity_pool.size();
    std::vector<size_t> state_counts(connectivity_count, 0);
    for (auto item = table.begin(); item != table.end(); ++item) {
        ++state_counts[item->first.connectivity_id];
    }

    // Group connectivity signatures by total future reachability. Both exact
    // and coarsening dominance can then reuse one cached key and one sort per
    // union bucket instead of rebuilding and sorting the table twice.
    std::unordered_map<std::vector<uint64_t>, size_t, WordsHash> bucket_by_union;
    bucket_by_union.reserve(connectivity_count);
    std::vector<Bucket> buckets;
    std::vector<size_t> connectivity_bucket(connectivity_count,
                                             std::numeric_limits<size_t>::max());
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (state_counts[id] == 0) continue;
        std::vector<uint64_t> combined(signature_words, 0);
        const auto& signature = connectivity_pool[id];
        for (size_t offset = 0; offset < signature.size(); offset += signature_words) {
            for (size_t word = 0; word < signature_words; ++word) {
                combined[word] |= signature[offset + word];
            }
        }
        auto [where, inserted] = bucket_by_union.emplace(std::move(combined), buckets.size());
        if (inserted) buckets.emplace_back();
        const size_t bucket = where->second;
        connectivity_bucket[id] = bucket;
        buckets[bucket].connectivity_ids.push_back(id);
    }
    std::vector<size_t> bucket_state_counts(buckets.size(), 0);
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (state_counts[id] != 0) {
            bucket_state_counts[connectivity_bucket[id]] += state_counts[id];
        }
    }
    for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
        buckets[bucket].entries.reserve(bucket_state_counts[bucket]);
    }
    for (auto item = table.begin(); item != table.end(); ++item) {
        const size_t bucket = connectivity_bucket[item->first.connectivity_id];
        int base_hits = 0;
        int total_hits = 0;
        for (size_t word = 0; word < item->first.hits.size(); ++word) {
            const uint64_t hits = item->first.hits[word];
            base_hits += __builtin_popcountll(hits & base_mask[word]);
            total_hits += __builtin_popcountll(hits);
        }
        const int flag_hits = total_hits - base_hits;
        const DominanceKey key{
            item->second.cost + base_hits, item->second.cost, -total_hits};
        const int quasi_score = item->second.cost + base_hits - flag_hits;
        buckets[bucket].entries.push_back(
            CachedItem{item, key, base_hits, flag_hits, quasi_score, false, false});
    }
    for (auto& bucket : buckets) {
        std::sort(bucket.entries.begin(), bucket.entries.end(),
                  [](const CachedItem& a, const CachedItem& b) {
                      return a.key < b.key;
                  });
    }

    uint64_t remaining = comparison_limit;
    std::vector<std::vector<CachedItem*>> survivors(connectivity_count);
    std::vector<int> minimum_quasi(connectivity_count, std::numeric_limits<int>::max());
    std::vector<int> maximum_quasi(connectivity_count, std::numeric_limits<int>::min());
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        survivors[id].reserve(state_counts[id]);
    }
    auto dominates = [&](const CachedItem* candidate, const CachedItem* target) {
        const int allowance = target->item->second.cost - candidate->item->second.cost;
        if (allowance < 0) return false;
        // Count differences give a cheap lower bound on the directed bit
        // penalty before touching either state's bitset.
        const int lower_bound =
            std::max(0, target->flag_hits - candidate->flag_hits) +
            std::max(0, candidate->base_hits - target->base_hits);
        if (lower_bound > allowance) return false;
        return dominance_penalty_at_most(
            candidate->item->first.hits, target->item->first.hits,
            base_mask, allowance);
    };

    // First remove factor-dominated states with identical connectivity. The
    // per-ID survivor lists inherit the bucket's cached sort order.
    for (auto& bucket : buckets) {
        for (CachedItem& cached : bucket.entries) {
            const uint32_t connectivity_id = cached.item->first.connectivity_id;
            auto& kept = survivors[connectivity_id];
            bool is_dominated = false;
            if (remaining >= kept.size()) {
                remaining -= kept.size();
                for (CachedItem* prior : kept) {
                    if (dominates(prior, &cached)) {
                        is_dominated = true;
                        break;
                    }
                }
            }
            cached.exact_dominated = is_dominated;
            if (!is_dominated) {
                kept.push_back(&cached);
                minimum_quasi[connectivity_id] =
                    std::min(minimum_quasi[connectivity_id], cached.quasi_score);
                maximum_quasi[connectivity_id] =
                    std::max(maximum_quasi[connectivity_id], cached.quasi_score);
            }
        }
    }

    std::vector<size_t> component_counts(connectivity_count, 0);
    std::vector<std::vector<int>> component_populations(connectivity_count);
    std::vector<int> minimum_component_population(connectivity_count,
                                                   std::numeric_limits<int>::max());
    std::vector<int> maximum_component_population(connectivity_count, 0);
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (!survivors[id].empty()) {
            const auto& signature = connectivity_pool[id];
            component_counts[id] = signature.size() / signature_words;
            for (size_t offset = 0; offset < signature.size(); offset += signature_words) {
                int population = 0;
                for (size_t word = 0; word < signature_words; ++word) {
                    population += __builtin_popcountll(signature[offset + word]);
                }
                minimum_component_population[id] =
                    std::min(minimum_component_population[id], population);
                maximum_component_population[id] =
                    std::max(maximum_component_population[id], population);
                component_populations[id].push_back(population);
            }
            std::sort(component_populations[id].begin(), component_populations[id].end());
        }
    }

    // Then apply connectivity-coarsening dominance to exact survivors. Keep
    // cross-dominated states available as witnesses until the scan finishes,
    // matching the previous deferred-erasure behavior. Discover coarsening
    // relationships lazily: many signature pairs cannot have even one
    // scalar-feasible dominance comparison, and later IDs need no work once
    // the layer's comparison budget is exhausted.
    std::vector<uint32_t> coarser;
    bool exhausted = false;
    for (uint32_t fine_id = 0; fine_id < connectivity_count && !exhausted; ++fine_id) {
        if (survivors[fine_id].empty()) continue;
        coarser.clear();
        const auto& candidate_ids =
            buckets[connectivity_bucket[fine_id]].connectivity_ids;
        coarser.reserve(candidate_ids.size());
        for (uint32_t coarse_id : candidate_ids) {
            if (coarse_id == fine_id || survivors[coarse_id].empty()) continue;
            if (component_counts[coarse_id] > component_counts[fine_id]) continue;
            if (maximum_component_population[fine_id] >
                    maximum_component_population[coarse_id] ||
                minimum_component_population[fine_id] >
                    minimum_component_population[coarse_id]) {
                continue;
            }
            bool population_matching_possible = true;
            for (size_t component = 0; component < component_counts[coarse_id];
                 ++component) {
                if (component_populations[fine_id][component] >
                    component_populations[coarse_id][component]) {
                    population_matching_possible = false;
                    break;
                }
            }
            if (!population_matching_possible) continue;
            if (survivors[coarse_id].front()->key > survivors[fine_id].back()->key) continue;
            if (minimum_quasi[coarse_id] > maximum_quasi[fine_id]) continue;

            bool relation;
            if (component_counts[coarse_id] == 1) {
                // Inside a common-union bucket, its sole component contains
                // every finer component and is automatically a valid anchor.
                relation = true;
            } else {
                relation = connectivity_coarsens(connectivity_pool[coarse_id],
                                                  connectivity_pool[fine_id],
                                                  signature_words);
            }
            if (relation) {
                coarser.push_back(coarse_id);
            }
        }
        if (coarser.empty()) continue;
        for (CachedItem* cached : survivors[fine_id]) {
            bool is_dominated = false;
            for (uint32_t coarse_id : coarser) {
                for (CachedItem* prior : survivors[coarse_id]) {
                    if (prior->key > cached->key) break;
                    if (remaining == 0) {
                        exhausted = true;
                        break;
                    }
                    --remaining;
                    if (dominates(prior, cached)) {
                        is_dominated = true;
                        break;
                    }
                }
                if (is_dominated || exhausted) break;
            }
            cached->cross_dominated = is_dominated;
            if (exhausted) break;
        }
    }

    size_t removed = 0;
    for (auto& bucket : buckets) {
        for (CachedItem& cached : bucket.entries) {
            if (!cached.exact_dominated && !cached.cross_dominated) continue;
            table.erase(cached.item);
            ++removed;
        }
    }
    return removed;
}

static std::vector<int> ordered_region_ranks(const Model& model, const std::string& order_name) {
    const bool rows = order_name.rfind("rows", 0) == 0;
    int band = 1;
    const size_t marker = order_name.rfind("-band-");
    if (marker != std::string::npos) band = std::stoi(order_name.substr(marker + 6));
    std::vector<int> cells(model.height * model.width);
    std::iota(cells.begin(), cells.end(), 0);
    auto key = [&](int cell) {
        const int r = cell / model.width;
        const int c = cell % model.width;
        if (rows) return std::tuple<int, int, int>{r / band, c, r % band};
        return std::tuple<int, int, int>{c / band, r, c % band};
    };
    std::sort(cells.begin(), cells.end(), [&](int a, int b) { return key(a) < key(b); });
    std::vector<int> rank(cells.size());
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) rank[cells[i]] = i + 1;
    std::vector<int> result;
    result.reserve(model.candidates.size());
    for (int cell : model.candidates) result.push_back(rank[cell]);
    return result;
}

static std::string comma_number(uint64_t value) {
    std::string text = std::to_string(value);
    for (int position = static_cast<int>(text.size()) - 3; position > 0; position -= 3) {
        text.insert(position, ",");
    }
    return text;
}

static Solution construct_solution(const Model& model, const std::vector<int>& selected,
                                   std::optional<int> expected_clicks = std::nullopt);

static Solution solve_frontier(const Model& original_model,
                               const std::string& requested_order,
                               size_t max_states,
                               std::optional<int> band_size,
                               bool progress,
                               int progress_every,
                               uint64_t dominance_comparisons) {
    auto [model, order_name] = choose_order(original_model, requested_order, band_size);
    const int q = static_cast<int>(model.candidates.size());

    std::vector<std::vector<int>> scopes;
    std::vector<uint8_t> is_flag;
    auto add_scopes = [&](const std::vector<std::vector<int>>& source, bool flag) {
        for (const auto& scope : source) {
            if (scope.empty()) continue;
            scopes.push_back(scope);
            is_flag.push_back(flag);
        }
    };
    add_scopes(model.mine_scopes, true);
    add_scopes(model.base_scopes, false);
    const size_t factor_words = (scopes.size() + 63) / 64;
    const size_t chosen_words = (q + 63) / 64;
    Bits flag_mask(factor_words), base_mask(factor_words);
    for (int f = 0; f < static_cast<int>(scopes.size()); ++f) {
        set_bit(is_flag[f] ? flag_mask : base_mask, f);
    }
    std::vector<Bits> factor_member(q, Bits(factor_words));
    std::vector<int> factor_min(scopes.size());
    std::vector<int> factor_max(scopes.size());
    for (int f = 0; f < static_cast<int>(scopes.size()); ++f) {
        factor_min[f] = scopes[f].front();
        factor_max[f] = scopes[f].back();
        for (int variable : scopes[f]) set_bit(factor_member[variable], f);
    }
    std::vector<Bits> active_after(q, Bits(factor_words));
    for (int i = 0; i < q; ++i) {
        for (int f = 0; f < static_cast<int>(scopes.size()); ++f) {
            if (factor_min[f] <= i && i < factor_max[f]) set_bit(active_after[i], f);
        }
    }

    const int zero_count = static_cast<int>(model.zero_scopes.size());
    std::vector<std::vector<int>> zero_memberships(q);
    std::vector<std::pair<int, int>> zero_limits;
    for (int z = 0; z < zero_count; ++z) {
        if (model.zero_scopes[z].empty()) zero_limits.emplace_back(0, -1);
        else {
            zero_limits.emplace_back(model.zero_scopes[z].front(), model.zero_scopes[z].back());
            for (int variable : model.zero_scopes[z]) zero_memberships[variable].push_back(z);
        }
    }
    std::vector<Bits> future_neighbors(q, Bits(chosen_words));
    for (int i = 0; i < q; ++i) {
        for (int other : model.graph[i]) {
            if (other > i) set_bit(future_neighbors[i], other);
        }
        for (int z : zero_memberships[i]) {
            for (int other : model.zero_scopes[z]) {
                if (other > i) set_bit(future_neighbors[i], other);
            }
        }
    }
    std::vector<int> last_future(q);
    for (int i = 0; i < q; ++i) {
        last_future[i] = i;
        for (int other : model.graph[i]) if (other > i) last_future[i] = std::max(last_future[i], other);
    }
    std::vector<std::vector<int>> boundaries(q + 1);
    for (int i = 0; i < q; ++i) {
        for (int v = 0; v <= i; ++v) if (last_future[v] > i) boundaries[i + 1].push_back(v);
        for (int z = 0; z < zero_count; ++z) {
            if (zero_limits[z].first <= i && i < zero_limits[z].second) {
                boundaries[i + 1].push_back(q + z);
            }
        }
    }
    if (progress_every < 1) throw UserError("progress interval must be positive");
    const auto region_ranks = progress ? ordered_region_ranks(model, order_name) : std::vector<int>{};

    Table table;
    table.reserve(16);
    table.emplace(State{0, Bits(factor_words)}, Record{model.three_bv(), Bits(chosen_words)});
    ConnectivityPool connectivity_pool;
    ConnectivityPool next_connectivity_pool;
    size_t peak_states = 1;
    int max_boundary = 0;
    int max_active = 0;
    size_t dominated_total = 0;

    for (int i = 0; i < q; ++i) {
        const auto& new_boundary = boundaries[i + 1];

        Table next;
        const size_t desired_capacity = table.size() > (std::numeric_limits<size_t>::max() - 16) / 2
            ? std::numeric_limits<size_t>::max() : table.size() * 2 + 16;
        const size_t state_capacity = max_states == std::numeric_limits<size_t>::max()
            ? desired_capacity : std::min(max_states + 1, desired_capacity);
        next.reserve(state_capacity);
        next_connectivity_pool.reset(connectivity_pool.size() * 2 + 16);
        {
            std::vector<std::array<ConnectivityTransition, 2>> connectivity_cache(
                connectivity_pool.size());
            std::vector<uint8_t> connectivity_ready(connectivity_pool.size(), 0);
            // Reuse flat scratch buffers for every connectivity transition in
            // this layer. Component offsets remain valid if the word buffer
            // grows, unlike pointers into a vector.
            std::vector<uint64_t> outgoing_words;
            std::vector<size_t> outgoing_offsets;
            std::vector<uint64_t> merged(chosen_words);
            std::vector<uint64_t> canonical;
            for (const auto& [old_state, old_record] : table) {
                if (!connectivity_ready[old_state.connectivity_id]) {
                    const auto& old_signature = connectivity_pool[old_state.connectivity_id];
                    std::array<ConnectivityTransition, 2> computed;
                    for (int selected = 0; selected <= 1; ++selected) {
                        int closed = 0;
                        outgoing_words.clear();
                        outgoing_offsets.clear();
                        std::fill(merged.begin(), merged.end(), 0);
                        outgoing_words.reserve(old_signature.size() +
                                               selected * chosen_words);
                        outgoing_offsets.reserve(old_signature.size() / chosen_words +
                                                 selected);
                        if (selected) {
                            for (size_t word = 0; word < chosen_words; ++word) {
                                merged[word] = future_neighbors[i][word];
                            }
                        }
                        for (size_t offset = 0; offset < old_signature.size();
                             offset += chosen_words) {
                            const bool touches =
                                (old_signature[offset + i / 64] >> (i % 64)) & 1U;
                            if (selected && touches) {
                                for (size_t word = 0; word < chosen_words; ++word) {
                                    merged[word] |= old_signature[offset + word];
                                }
                                continue;
                            }
                            const size_t start = outgoing_words.size();
                            bool nonempty = false;
                            for (size_t word = 0; word < chosen_words; ++word) {
                                uint64_t value = old_signature[offset + word];
                                if (word == static_cast<size_t>(i / 64)) {
                                    value &= ~(uint64_t{1} << (i % 64));
                                }
                                outgoing_words.push_back(value);
                                nonempty = nonempty || value != 0;
                            }
                            if (nonempty) outgoing_offsets.push_back(start);
                            else {
                                outgoing_words.resize(start);
                                ++closed;
                            }
                        }
                        if (selected) {
                            merged[i / 64] &= ~(uint64_t{1} << (i % 64));
                            const bool nonempty = std::any_of(
                                merged.begin(), merged.end(),
                                [](uint64_t word) { return word != 0; });
                            if (nonempty) {
                                outgoing_offsets.push_back(outgoing_words.size());
                                outgoing_words.insert(
                                    outgoing_words.end(), merged.begin(), merged.end());
                            } else ++closed;
                        }
                        std::sort(outgoing_offsets.begin(), outgoing_offsets.end(),
                                  [&](size_t a, size_t b) {
                                      for (size_t word = 0; word < chosen_words; ++word) {
                                          if (outgoing_words[a + word] !=
                                              outgoing_words[b + word]) {
                                              return outgoing_words[a + word] <
                                                     outgoing_words[b + word];
                                          }
                                      }
                                      return false;
                                  });
                        canonical.clear();
                        canonical.reserve(outgoing_offsets.size() * chosen_words);
                        for (size_t offset : outgoing_offsets) {
                            canonical.insert(canonical.end(),
                                             outgoing_words.begin() +
                                                 static_cast<std::ptrdiff_t>(offset),
                                             outgoing_words.begin() +
                                                 static_cast<std::ptrdiff_t>(offset + chosen_words));
                        }
                        computed[selected] = {next_connectivity_pool.intern(canonical),
                                              closed};
                    }
                    connectivity_cache[old_state.connectivity_id] = std::move(computed);
                    connectivity_ready[old_state.connectivity_id] = 1;
                }

                for (int selected = 0; selected <= 1; ++selected) {
                    Bits new_hits = old_state.hits;
                    int factor_cost = 0;
                    if (selected) {
                        factor_cost += bit_count_new_and(factor_member[i], old_state.hits, flag_mask);
                        factor_cost -= bit_count_new_and(factor_member[i], old_state.hits, base_mask);
                        for (size_t word = 0; word < factor_words; ++word) {
                            new_hits[word] |= factor_member[i][word];
                        }
                    }
                    for (size_t word = 0; word < factor_words; ++word) {
                        new_hits[word] &= active_after[i][word];
                    }
                    const auto& connection =
                        connectivity_cache[old_state.connectivity_id][selected];
                    const int new_cost = old_record.cost + selected + connection.closed + factor_cost;
                    State state{connection.connectivity_id, std::move(new_hits)};
                    auto found = next.find(state);
                    if (found == next.end()) {
                        Bits new_chosen = old_record.chosen;
                        if (selected) set_bit(new_chosen, i);
                        next.emplace(std::move(state), Record{new_cost, std::move(new_chosen)});
                    } else if (new_cost < found->second.cost) {
                        found->second.cost = new_cost;
                        found->second.chosen = old_record.chosen;
                        if (selected) set_bit(found->second.chosen, i);
                    }
                }
            }
        }

        // The previous layer and connectivity pool are no longer needed. Release
        // them before dominance pruning so they do not overlap the largest
        // temporary structures of the new layer.
        {
            Table released;
            table.swap(released);
        }
        {
            ConnectivityPool released;
            connectivity_pool.swap(released);
        }

        const size_t removed = prune_dominated(
            next, dominance_comparisons, base_mask,
            next_connectivity_pool, chosen_words);
        table = std::move(next);
        connectivity_pool.swap(next_connectivity_pool);
        dominated_total += removed;
        peak_states = std::max(peak_states, table.size());
        max_boundary = std::max(max_boundary, static_cast<int>(new_boundary.size()));
        const int active_count = bit_count(active_after[i]);
        max_active = std::max(max_active, active_count);
        if (progress && ((i + 1) % progress_every == 0 || i + 1 == q)) {
            std::cerr << "DP: region contains " << region_ranks[i] << "/"
                      << model.height * model.width << " tiles; processed " << i + 1
                      << "/" << q << " chord candidates; " << comma_number(table.size())
                      << " valid boundary states; boundary " << new_boundary.size()
                      << " connectivity items + " << active_count << " factor bits; pruned "
                      << comma_number(dominated_total) << " dominated states\n";
        }
        if (table.size() > max_states) {
            throw StateLimitExceeded("frontier grew to " + comma_number(table.size()) +
                                     " states after variable " + std::to_string(i + 1) + "/" +
                                     std::to_string(q) + "; increase --max-states or try the other --order");
        }
    }

    State final_state{0, Bits(factor_words)};
    auto final = table.find(final_state);
    if (final == table.end()) throw std::logic_error("frontier DP did not reach an empty final state");
    std::vector<int> selected;
    selected.reserve(q);
    for (int i = 0; i < q; ++i) {
        if (!test_bit(final->second.chosen, i)) continue;
        const int cell = model.candidates[i];
        selected.push_back(original_model.candidate_index[cell]);
    }
    std::sort(selected.begin(), selected.end());
    Solution solution = construct_solution(original_model, selected, final->second.cost);
    solution.peak_states = peak_states;
    solution.max_boundary_vertices = max_boundary;
    solution.max_active_factors = max_active;
    solution.order_name = order_name;
    return solution;
}

struct Evaluation {
    int clicks = 0;
    std::vector<int> flags;
    std::vector<std::vector<int>> components;
    std::vector<int> uncovered;
};

static bool scope_hit(const std::vector<int>& scope, const std::vector<uint8_t>& selected) {
    for (int candidate : scope) if (selected[candidate]) return true;
    return false;
}

static Evaluation evaluate_set(const Model& model, const std::vector<int>& selected_indexes) {
    const int q = static_cast<int>(model.candidates.size());
    std::vector<uint8_t> selected(q, 0);
    for (int candidate : selected_indexes) selected[candidate] = 1;
    Evaluation evaluation;
    for (size_t i = 0; i < model.mine_scopes.size(); ++i) {
        if (scope_hit(model.mine_scopes[i], selected)) evaluation.flags.push_back(model.mine_cells[i]);
    }
    std::vector<std::vector<int>> zero_memberships(q);
    for (int z = 0; z < static_cast<int>(model.zero_scopes.size()); ++z) {
        for (int candidate : model.zero_scopes[z]) zero_memberships[candidate].push_back(z);
    }
    std::vector<uint8_t> unseen = selected;
    std::vector<uint8_t> unused_zero(model.zero_scopes.size(), 1);
    for (int start = 0; start < q; ++start) {
        if (!unseen[start]) continue;
        unseen[start] = 0;
        std::vector<int> component;
        std::vector<int> stack{start};
        while (!stack.empty()) {
            const int candidate = stack.back();
            stack.pop_back();
            component.push_back(candidate);
            for (int other : model.graph[candidate]) {
                if (unseen[other]) {
                    unseen[other] = 0;
                    stack.push_back(other);
                }
            }
            for (int z : zero_memberships[candidate]) {
                if (!unused_zero[z]) continue;
                unused_zero[z] = 0;
                for (int other : model.zero_scopes[z]) {
                    if (unseen[other]) {
                        unseen[other] = 0;
                        stack.push_back(other);
                    }
                }
            }
        }
        std::sort(component.begin(), component.end());
        evaluation.components.push_back(std::move(component));
    }
    std::sort(evaluation.components.begin(), evaluation.components.end(), [&](const auto& a, const auto& b) {
        return model.candidates[a.front()] < model.candidates[b.front()];
    });
    for (int i = 0; i < static_cast<int>(model.base_scopes.size()); ++i) {
        if (!scope_hit(model.base_scopes[i], selected)) evaluation.uncovered.push_back(i);
    }
    evaluation.clicks = static_cast<int>(selected_indexes.size() + evaluation.flags.size() +
                                         evaluation.components.size() + evaluation.uncovered.size());
    return evaluation;
}

static std::vector<int> component_chord_order(const Model& model,
                                               const std::vector<int>& component) {
    const int q = static_cast<int>(model.candidates.size());
    std::vector<uint8_t> allowed(q, 0), seen(q, 0);
    for (int candidate : component) allowed[candidate] = 1;
    const int seed = *std::min_element(component.begin(), component.end(), [&](int a, int b) {
        return model.candidates[a] < model.candidates[b];
    });
    std::vector<std::vector<int>> zero_memberships(q);
    for (int z = 0; z < static_cast<int>(model.zero_scopes.size()); ++z) {
        for (int candidate : model.zero_scopes[z]) zero_memberships[candidate].push_back(z);
    }
    std::vector<uint8_t> unused_zero(model.zero_scopes.size(), 1);
    std::queue<int> queue;
    queue.push(seed);
    seen[seed] = 1;
    std::vector<int> order;
    while (!queue.empty()) {
        const int candidate = queue.front();
        queue.pop();
        order.push_back(candidate);
        auto adjacent = model.graph[candidate];
        std::sort(adjacent.begin(), adjacent.end(), [&](int a, int b) {
            return model.candidates[a] < model.candidates[b];
        });
        for (int other : adjacent) {
            if (allowed[other] && !seen[other]) {
                seen[other] = 1;
                queue.push(other);
            }
        }
        for (int z : zero_memberships[candidate]) {
            if (!unused_zero[z]) continue;
            unused_zero[z] = 0;
            auto scope = model.zero_scopes[z];
            std::sort(scope.begin(), scope.end(), [&](int a, int b) {
                return model.candidates[a] < model.candidates[b];
            });
            for (int other : scope) {
                if (allowed[other] && !seen[other]) {
                    seen[other] = 1;
                    queue.push(other);
                }
            }
        }
    }
    if (order.size() != component.size()) throw std::logic_error("reported chord component is disconnected");
    return order;
}

static void validate_actions(const Model& model, const Solution& solution) {
    const int cells = model.height * model.width;
    std::vector<uint8_t> opened(cells, 0), flagged(cells, 0);
    std::vector<int> zero_by_cell(cells, -1);
    for (int z = 0; z < static_cast<int>(model.zeros.size()); ++z) {
        for (int cell : model.zeros[z]) zero_by_cell[cell] = z;
    }
    auto reveal = [&](int start) {
        if (model.mines[start] || flagged[start]) throw std::logic_error("attempted to reveal mine/flag");
        if (opened[start]) return;
        opened[start] = 1;
        if (model.numbers[start] == 0) {
            const int z = zero_by_cell[start];
            for (int zero : model.zeros[z]) {
                opened[zero] = 1;
                for (int other : neighbors(zero, model.height, model.width)) {
                    if (!model.mines[other]) opened[other] = 1;
                }
            }
        }
    };
    for (const Action& action : solution.actions) {
        if (action.type == ActionType::Flag) {
            if (!model.mines[action.cell] || opened[action.cell]) throw std::logic_error("illegal flag action");
            flagged[action.cell] = 1;
        } else if (action.type == ActionType::Left) {
            reveal(action.cell);
        } else {
            if (!opened[action.cell] || model.numbers[action.cell] <= 0) {
                throw std::logic_error("illegal chord center");
            }
            int flag_count = 0;
            const auto adjacent = neighbors(action.cell, model.height, model.width);
            for (int other : adjacent) flag_count += flagged[other];
            if (flag_count != model.numbers[action.cell]) throw std::logic_error("wrong adjacent flag count");
            for (int other : adjacent) {
                if (!flagged[other] && !model.mines[other]) reveal(other);
            }
        }
    }
    int missing = 0;
    for (int cell = 0; cell < cells; ++cell) if (!model.mines[cell] && !opened[cell]) ++missing;
    if (missing) throw std::logic_error("action sequence left " + std::to_string(missing) + " safe cells covered");
}

static Solution construct_solution(const Model& model, const std::vector<int>& selected,
                                   std::optional<int> expected_clicks) {
    Evaluation evaluation = evaluate_set(model, selected);
    if (expected_clicks && evaluation.clicks != *expected_clicks) {
        throw std::logic_error("DP cost disagrees with evaluator");
    }
    Solution solution;
    solution.clicks = evaluation.clicks;
    solution.selected = selected;
    solution.flags = evaluation.flags;
    solution.components = evaluation.components;
    solution.uncovered_units = evaluation.uncovered;
    for (int cell : solution.flags) solution.actions.push_back({ActionType::Flag, cell});
    for (const auto& component : solution.components) {
        const auto order = component_chord_order(model, component);
        solution.actions.push_back({ActionType::Left, model.candidates[order.front()]});
        for (int candidate : order) {
            solution.actions.push_back({ActionType::Chord, model.candidates[candidate]});
        }
    }
    for (int unit : solution.uncovered_units) {
        solution.actions.push_back({ActionType::Left, model.base_descriptions[unit].representative});
    }
    if (solution.actions.size() != static_cast<size_t>(solution.clicks)) {
        throw std::logic_error("constructed action count disagrees with objective");
    }
    validate_actions(model, solution);
    return solution;
}

static Solution solve_bruteforce(const Model& model, int max_candidates = 25) {
    const int q = static_cast<int>(model.candidates.size());
    if (q > max_candidates) {
        throw UserError("brute force is limited to " + std::to_string(max_candidates) +
                        " candidates; board has " + std::to_string(q));
    }
    int best_cost = std::numeric_limits<int>::max();
    std::vector<int> best;
    const uint64_t limit = uint64_t{1} << q;
    for (uint64_t mask = 0; mask < limit; ++mask) {
        std::vector<int> selected;
        for (int i = 0; i < q; ++i) if ((mask >> i) & 1U) selected.push_back(i);
        const int cost = evaluate_set(model, selected).clicks;
        if (cost < best_cost) {
            best_cost = cost;
            best = std::move(selected);
        }
    }
    Solution solution = construct_solution(model, best, best_cost);
    solution.order_name = "brute-force";
    return solution;
}

static std::string action_word(ActionType type) {
    if (type == ActionType::Flag) return "flag";
    if (type == ActionType::Left) return "left";
    return "chord";
}

static std::string click_word(ActionType type) {
    return type == ActionType::Flag ? "right" : action_word(type);
}

static std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(ch)
                        << std::dec << std::setfill(' ');
                } else out << ch;
        }
    }
    return out.str();
}

static void print_coord_json(std::ostream& out, int cell, const Model& model, int offset) {
    out << '[' << cell / model.width + offset << ',' << cell % model.width + offset << ']';
}

static void print_json(const Model& model, const Solution& solution, int offset,
                       const std::optional<std::string>& generated_difficulty,
                       const std::optional<uint64_t>& generated_seed,
                       const std::optional<std::string>& generated_url) {
    std::cout << "{\n  \"optimal_clicks\": " << solution.clicks
              << ",\n  \"three_bv\": " << model.three_bv() << ",\n";
    auto print_candidate_coords = [&](const std::vector<int>& candidates) {
        std::cout << '[';
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (i) std::cout << ',';
            print_coord_json(std::cout, model.candidates[candidates[i]], model, offset);
        }
        std::cout << ']';
    };
    std::cout << "  \"chord_squares\": "; print_candidate_coords(solution.selected); std::cout << ",\n";
    std::cout << "  \"flags\": [";
    for (size_t i = 0; i < solution.flags.size(); ++i) {
        if (i) std::cout << ',';
        print_coord_json(std::cout, solution.flags[i], model, offset);
    }
    std::cout << "],\n  \"chord_components\": [";
    for (size_t i = 0; i < solution.components.size(); ++i) {
        if (i) std::cout << ',';
        print_candidate_coords(solution.components[i]);
    }
    std::cout << "],\n  \"remaining_base_clicks\": [";
    for (size_t i = 0; i < solution.uncovered_units.size(); ++i) {
        if (i) std::cout << ',';
        print_coord_json(std::cout,
                         model.base_descriptions[solution.uncovered_units[i]].representative,
                         model, offset);
    }
    std::cout << "],\n  \"actions\": [";
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ',';
        const auto& action = solution.actions[i];
        std::cout << "{\"action\":\"" << action_word(action.type) << "\",\"row\":"
                  << action.cell / model.width + offset << ",\"column\":"
                  << action.cell % model.width + offset << '}';
    }
    std::cout << "],\n  \"clicks\": [";
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ',';
        const auto& action = solution.actions[i];
        std::cout << "[\"" << click_word(action.type) << "\"," << action.cell % model.width + offset
                  << ',' << action.cell / model.width + offset << ']';
    }
    std::cout << "],\n  \"statistics\": {"
              << "\"candidate_chords\":" << model.candidates.size()
              << ",\"selected_chords\":" << solution.selected.size()
              << ",\"flag_clicks\":" << solution.flags.size()
              << ",\"component_seed_clicks\":" << solution.components.size()
              << ",\"remaining_3bv_clicks\":" << solution.uncovered_units.size()
              << ",\"order\":\"" << json_escape(solution.order_name) << "\""
              << ",\"peak_dp_states\":" << solution.peak_states
              << ",\"max_boundary_vertices\":" << solution.max_boundary_vertices
              << ",\"max_active_factors\":" << solution.max_active_factors
              << ",\"solve_seconds\":" << std::fixed << std::setprecision(6)
              << solution.solve_seconds << '}';
    if (generated_url) {
        std::cout << ",\n  \"generated_board\": {\"difficulty\":\""
                  << json_escape(*generated_difficulty) << "\",\"seed\":" << *generated_seed
                  << ",\"url\":\"" << json_escape(*generated_url) << "\"}";
    }
    std::cout << "\n}\n";
}

static void print_click_tuples(const Model& model, const Solution& solution, int offset) {
    std::cout << '[';
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ", ";
        const auto& action = solution.actions[i];
        std::cout << "('" << click_word(action.type) << "', "
                  << action.cell % model.width + offset << ", "
                  << action.cell / model.width + offset << ')';
    }
    std::cout << "]\n";
}

struct Options {
    std::optional<std::string> board;
    std::optional<std::string> generate;
    std::optional<uint64_t> seed;
    std::string format = "auto";
    std::string method = "frontier";
    std::string order = "auto";
    std::optional<int> band_size;
    size_t max_states = 2'000'000;
    bool progress = false;
    int progress_every = 10;
    uint64_t dominance_comparisons = 1'000'000;
    bool verify = false;
    bool json = false;
    bool click_tuples = false;
    bool zero_based = false;
};

static void print_help(const char* program) {
    std::cout
        << "Exact Minesweeper click optimizer (C++17)\n\n"
        << "Usage: " << program << " [BOARD] [options]\n\n"
        << "BOARD may be a grid filename, LlamaSweeper URL, MBF filename, or quoted MBF hex.\n\n"
        << "Options:\n"
        << "  --generate intermediate|expert  Generate, print, and solve a random board\n"
        << "  --seed N                       Reproduce a generated board\n"
        << "  --format auto|grid|llamasweeper|mbf\n"
        << "  --method frontier|bruteforce\n"
        << "  --order auto|rows|columns\n"
        << "  --band-size N\n"
        << "  --max-states N\n"
        << "  --progress [--progress-every N]\n"
        << "  --dominance-comparisons N\n"
        << "  --verify\n"
        << "  --json\n"
        << "  --click-tuples\n"
        << "  --zero-based\n"
        << "  -h, --help\n";
}

static uint64_t parse_u64(const std::string& value, const std::string& option) {
    if (value.empty() || value.front() == '-') {
        throw UserError(option + " requires a nonnegative integer");
    }
    size_t used = 0;
    uint64_t result;
    try { result = std::stoull(value, &used, 10); }
    catch (...) { throw UserError(option + " requires a nonnegative integer"); }
    if (used != value.size()) throw UserError(option + " requires a nonnegative integer");
    return result;
}

static int parse_positive_int(const std::string& value, const std::string& option) {
    const uint64_t parsed = parse_u64(value, option);
    if (parsed == 0 || parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw UserError(option + " requires a positive integer");
    }
    return static_cast<int>(parsed);
}

static Options parse_options(int argc, char** argv) {
    Options options;
    auto value_after = [&](int& i, const std::string& option) {
        if (++i >= argc) throw UserError(option + " requires a value");
        return std::string(argv[i]);
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else if (arg == "--generate") options.generate = value_after(i, arg);
        else if (arg == "--seed") options.seed = parse_u64(value_after(i, arg), arg);
        else if (arg == "--format") options.format = value_after(i, arg);
        else if (arg == "--method") options.method = value_after(i, arg);
        else if (arg == "--order") options.order = value_after(i, arg);
        else if (arg == "--band-size") options.band_size = parse_positive_int(value_after(i, arg), arg);
        else if (arg == "--max-states") options.max_states = parse_u64(value_after(i, arg), arg);
        else if (arg == "--progress") options.progress = true;
        else if (arg == "--progress-every") options.progress_every = parse_positive_int(value_after(i, arg), arg);
        else if (arg == "--dominance-comparisons") options.dominance_comparisons = parse_u64(value_after(i, arg), arg);
        else if (arg == "--verify") options.verify = true;
        else if (arg == "--json") options.json = true;
        else if (arg == "--click-tuples") options.click_tuples = true;
        else if (arg == "--zero-based") options.zero_based = true;
        else if (!arg.empty() && arg.front() == '-') throw UserError("unknown option: " + arg);
        else if (options.board) throw UserError("only one board argument is allowed");
        else options.board = arg;
    }
    if (options.generate) {
        if (*options.generate != "intermediate" && *options.generate != "expert") {
            throw UserError("--generate must be intermediate or expert");
        }
        if (options.board) throw UserError("do not supply a board argument with --generate");
        if (options.format != "auto") throw UserError("--format cannot be used with --generate");
    } else {
        if (!options.board) throw UserError("a board argument or --generate is required");
        if (options.seed) throw UserError("--seed requires --generate");
    }
    if (options.format != "auto" && options.format != "grid" &&
        options.format != "llamasweeper" && options.format != "mbf") {
        throw UserError("invalid --format value");
    }
    if (options.method != "frontier" && options.method != "bruteforce") {
        throw UserError("invalid --method value");
    }
    if (options.order != "auto" && options.order != "rows" && options.order != "columns") {
        throw UserError("invalid --order value");
    }
    if (options.json && options.click_tuples) throw UserError("choose only one of --json and --click-tuples");
    return options;
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        Board board;
        std::optional<std::string> generated_url;
        std::optional<uint64_t> generated_seed;
        if (options.generate) {
            uint64_t seed;
            board = generate_standard_board(*options.generate, options.seed, seed);
            generated_seed = seed;
            generated_url = format_llamasweeper_url(board);
            std::cerr << "Generated board: " << *generated_url << "\n"
                      << "Random seed: " << seed << "\n";
        } else {
            board = load_board(*options.board, options.format);
        }

        const auto start = std::chrono::steady_clock::now();
        Model model = build_model(board);
        Solution solution;
        if (options.method == "bruteforce") solution = solve_bruteforce(model);
        else {
            solution = solve_frontier(model, options.order, options.max_states,
                                      options.band_size, options.progress,
                                      options.progress_every,
                                      options.dominance_comparisons);
        }
        const auto end = std::chrono::steady_clock::now();
        solution.solve_seconds = std::chrono::duration<double>(end - start).count();
        std::cerr << "Solve time: " << std::fixed << std::setprecision(3)
                  << solution.solve_seconds << " seconds\n";

        if (options.verify && model.candidates.size() <= 25) {
            const Solution brute = solve_bruteforce(model);
            if (brute.clicks != solution.clicks) throw std::logic_error("frontier and brute-force results differ");
        }
        const int offset = options.zero_based ? 0 : 1;
        if (options.click_tuples) {
            print_click_tuples(model, solution, offset);
            return 0;
        }
        if (options.json) {
            print_json(model, solution, offset, options.generate, generated_seed, generated_url);
            return 0;
        }
        std::cout << "Optimal clicks: " << solution.clicks << " (3BV without chording: "
                  << model.three_bv() << ")\n"
                  << "Breakdown: " << solution.flags.size() << " flags + "
                  << solution.components.size() << " seed left-clicks + "
                  << solution.selected.size() << " chords + "
                  << solution.uncovered_units.size() << " remaining 3BV clicks\n";
        if (solution.order_name != "brute-force") {
            std::cout << "DP: " << solution.order_name << " order, "
                      << comma_number(solution.peak_states) << " peak states, boundary "
                      << solution.max_boundary_vertices << " connectivity items + "
                      << solution.max_active_factors << " factor bits\n";
        }
        std::cout << "Actions:\n";
        for (size_t i = 0; i < solution.actions.size(); ++i) {
            const auto& action = solution.actions[i];
            const char* name = action.type == ActionType::Flag ? "FLAG " :
                               (action.type == ActionType::Left ? "LEFT " : "CHORD");
            std::cout << std::setw(4) << i + 1 << ". " << name << " ("
                      << action.cell / model.width + offset << ','
                      << action.cell % model.width + offset << ")\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}

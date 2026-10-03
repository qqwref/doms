// Exact Minesweeper click optimizer using a frontier connectivity DP.
// C++17 port of minesweeper_chord_dp.py.

#include <algorithm>
#include <atomic>
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
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

constexpr std::string_view LLAMA_ALPHABET = "0123456789abcdefghijklmnopqrstuv";
constexpr int PROGRESS_INTERVAL = 10;
constexpr uint64_t DOMINANCE_COMPARISONS = 1'000'000;

struct UserError : std::runtime_error {
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
    bool count_only = false;
    std::vector<int> selected;
    std::vector<int> flags;
    std::vector<std::vector<int>> components;
    std::vector<int> uncovered_units;
    std::vector<Action> actions;
    size_t peak_states = 0;
    int max_boundary_vertices = 0;
    int max_active_factors = 0;
    int candidate_chords_before_reduction = 0;
    int candidate_chords_after_reduction = 0;
    int swap_dominated_chords = 0;
    int left_click_dominated_chords = 0;
    size_t opening_chain_absorptions = 0;
    double reduction_seconds = 0.0;
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

static std::string format_pttacg_string(const Board& board) {
    std::string code;
    if (board.height == 9 && board.width == 9) code = "1";
    else if (board.height == 16 && board.width == 16) code = "2";
    else if (board.height == 16 && board.width == 30) code = "3";
    else throw UserError("PTTACG output supports only standard dimensions");
    std::string mine_code;
    for (size_t start = 0; start < board.mines.size(); start += 5) {
        unsigned value = 0;
        for (size_t offset = 0; offset < 5; ++offset) {
            value <<= 1;
            if (start + offset < board.mines.size() && board.mines[start + offset]) value |= 1;
        }
        mine_code.push_back(LLAMA_ALPHABET[value]);
    }
    return "b=" + code + "&m=" + mine_code;
}

static std::string format_llamasweeper_url(const Board& board) {
    return "https://llamasweeper.com/#/game/board-editor?" + format_pttacg_string(board);
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

static Board load_board(const std::string& source) {
    std::error_code ec;
    const fs::path path(source);
    const bool is_file = fs::is_regular_file(path, ec);
    std::string raw = is_file ? read_file_text(path) : source;
    const std::string lowered = lower_copy(raw);
    if (lowered.find("llamasweeper.com") != std::string::npos ||
        (raw.find("b=") != std::string::npos && raw.find("m=") != std::string::npos)) {
        return parse_llamasweeper(raw);
    }
    if (looks_like_mbf_hex(raw)) return parse_mbf_bytes(mbf_hex_bytes(raw));
    if (is_file) {
        return parse_mbf_bytes(std::vector<uint8_t>(raw.begin(), raw.end()));
    }
    throw UserError("input must be a PTTACG string or compatible LlamaSweeper URL, "
                    "an MBF file, or quoted MBF hexadecimal");
}

static std::tuple<int, int, int> standard_board_spec(const std::string& difficulty) {
    int height, width, mine_count;
    if (difficulty == "beginner") {
        height = 9; width = 9; mine_count = 10;
    } else if (difficulty == "intermediate") {
        height = 16; width = 16; mine_count = 40;
    } else if (difficulty == "expert") {
        height = 16; width = 30; mine_count = 99;
    } else {
        throw UserError("unknown generated difficulty '" + difficulty + "'");
    }
    return {height, width, mine_count};
}

static uint64_t random_seed() {
    std::random_device rd;
    uint64_t seed = 0;
    for (int i = 0; i < 4; ++i) {
        seed ^= static_cast<uint64_t>(rd()) << (16 * i);
        seed = seed * UINT64_C(0x9e3779b97f4a7c15) + UINT64_C(0xbf58476d1ce4e5b9);
    }
    return seed;
}

static Board generate_standard_board(const std::string& difficulty,
                                     std::mt19937_64& rng) {
    const auto [height, width, mine_count] = standard_board_spec(difficulty);
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

static Board generate_standard_board(const std::string& difficulty,
                                     std::optional<uint64_t> seed,
                                     uint64_t& effective_seed) {
    effective_seed = seed.value_or(random_seed());
    std::mt19937_64 rng(effective_seed);
    return generate_standard_board(difficulty, rng);
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

// Return the same optimization problem restricted to the candidates marked in
// keep. Empty factor scopes are deliberately retained: they represent a 3BV
// unit which no remaining chord can open (or a mine which no remaining chord
// can require).
static Model restricted_model(const Model& model, const std::vector<uint8_t>& keep) {
    const int old_q = static_cast<int>(model.candidates.size());
    std::vector<int> old_to_new(old_q, -1);
    std::vector<int> retained;
    retained.reserve(old_q);
    for (int old = 0; old < old_q; ++old) {
        if (!keep[old]) continue;
        old_to_new[old] = static_cast<int>(retained.size());
        retained.push_back(old);
    }

    Model result = model;
    result.candidates.clear();
    result.candidates.reserve(retained.size());
    for (int old : retained) result.candidates.push_back(model.candidates[old]);
    result.candidate_index.assign(model.height * model.width, -1);
    for (int next = 0; next < static_cast<int>(result.candidates.size()); ++next) {
        result.candidate_index[result.candidates[next]] = next;
    }
    result.graph.assign(retained.size(), {});
    for (int next = 0; next < static_cast<int>(retained.size()); ++next) {
        for (int old_other : model.graph[retained[next]]) {
            if (old_to_new[old_other] >= 0) {
                result.graph[next].push_back(old_to_new[old_other]);
            }
        }
    }
    auto restrict_scopes = [&](const std::vector<std::vector<int>>& scopes) {
        std::vector<std::vector<int>> restricted;
        restricted.reserve(scopes.size());
        for (const auto& scope : scopes) {
            std::vector<int> next;
            next.reserve(scope.size());
            for (int old : scope) {
                if (old_to_new[old] >= 0) next.push_back(old_to_new[old]);
            }
            restricted.push_back(std::move(next));
        }
        return restricted;
    };
    result.zero_scopes = restrict_scopes(model.zero_scopes);
    result.mine_scopes = restrict_scopes(model.mine_scopes);
    result.base_scopes = restrict_scopes(model.base_scopes);
    return result;
}

struct CandidateReduction {
    Model model;
    int swap_dominated = 0;
    int left_click_dominated = 0;
    double seconds = 0.0;
};

struct CandidateRelations {
    std::vector<std::vector<uint64_t>> propagation;
    std::vector<std::vector<uint64_t>> mine_factors;
    std::vector<std::vector<uint64_t>> base_factors;
};

static CandidateRelations candidate_relations(const Model& model) {
    const int q = static_cast<int>(model.candidates.size());
    const size_t candidate_words = (q + 63) / 64;
    const size_t mine_words = (model.mine_scopes.size() + 63) / 64;
    const size_t base_words = (model.base_scopes.size() + 63) / 64;
    CandidateRelations relations;
    relations.propagation.assign(q, std::vector<uint64_t>(candidate_words, 0));
    relations.mine_factors.assign(q, std::vector<uint64_t>(mine_words, 0));
    relations.base_factors.assign(q, std::vector<uint64_t>(base_words, 0));
    auto connect = [&](int a, int b) {
        if (a == b) return;
        relations.propagation[a][b / 64] |= uint64_t{1} << (b % 64);
        relations.propagation[b][a / 64] |= uint64_t{1} << (a % 64);
    };
    for (int candidate = 0; candidate < q; ++candidate) {
        for (int other : model.graph[candidate]) connect(candidate, other);
    }
    for (const auto& scope : model.zero_scopes) {
        for (size_t i = 0; i < scope.size(); ++i) {
            for (size_t j = i + 1; j < scope.size(); ++j) connect(scope[i], scope[j]);
        }
    }
    for (size_t factor = 0; factor < model.mine_scopes.size(); ++factor) {
        for (int candidate : model.mine_scopes[factor]) {
            relations.mine_factors[candidate][factor / 64] |= uint64_t{1} << (factor % 64);
        }
    }
    for (size_t factor = 0; factor < model.base_scopes.size(); ++factor) {
        for (int candidate : model.base_scopes[factor]) {
            relations.base_factors[candidate][factor / 64] |= uint64_t{1} << (factor % 64);
        }
    }
    return relations;
}

static bool bits_subset(const std::vector<uint64_t>& subset,
                        const std::vector<uint64_t>& superset) {
    for (size_t word = 0; word < subset.size(); ++word) {
        if (subset[word] & ~superset[word]) return false;
    }
    return true;
}

// If every role played by victim is also played at least as cheaply by
// replacement, any solution using victim can replace it (or simply delete it
// when replacement is already selected). This preserves 3BV coverage and
// chord-component connectivity while never adding a flag.
static bool swap_dominated_candidate(const CandidateRelations& relations,
                                     int victim, int replacement) {
    if (victim == replacement) return false;
    if (!(relations.propagation[victim][replacement / 64] >> (replacement % 64) & 1U)) {
        return false;
    }
    if (!bits_subset(relations.base_factors[victim],
                     relations.base_factors[replacement])) return false;
    if (!bits_subset(relations.mine_factors[replacement],
                     relations.mine_factors[victim])) return false;
    for (size_t word = 0; word < relations.propagation[victim].size(); ++word) {
        uint64_t needed = relations.propagation[victim][word];
        if (word == static_cast<size_t>(replacement / 64)) {
            needed &= ~(uint64_t{1} << (replacement % 64));
        }
        if (needed & ~relations.propagation[replacement][word]) return false;
    }
    return true;
}

// Certify that deleting candidate v can never increase the objective. Let X be
// the selected propagation-neighbors of v. Removing v saves its chord click,
// and saves a seed click when X is empty. Otherwise it can split its component
// into at most cc(G[X]) pieces. Choosing one representative from every piece
// gives an independent set I, so the worst case is exactly bounded by
//
//   bases(v) - 2 - private_mines(v) + |I| - bases_hit(I).
//
// The branch-and-bound below maximizes the last two terms over all independent
// sets. A nonpositive maximum is therefore a proof that v is unnecessary.
static bool left_click_dominated_candidate(const CandidateRelations& relations,
                                           int candidate,
                                           const std::vector<int>& private_mine_counts) {
    std::vector<int> adjacent;
    for (size_t word = 0; word < relations.propagation[candidate].size(); ++word) {
        uint64_t neighbors = relations.propagation[candidate][word];
        while (neighbors) {
            adjacent.push_back(static_cast<int>(word * 64 + __builtin_ctzll(neighbors)));
            neighbors &= neighbors - 1;
        }
    }
    // This representation keeps the proof search allocation-free and covers
    // ordinary standard boards. Skipping a larger neighborhood is conservative.
    if (adjacent.size() > 63) return false;

    std::vector<int> base_ids;
    for (size_t word = 0; word < relations.base_factors[candidate].size(); ++word) {
        uint64_t bases = relations.base_factors[candidate][word];
        while (bases) {
            base_ids.push_back(static_cast<int>(word * 64 + __builtin_ctzll(bases)));
            bases &= bases - 1;
        }
    }
    if (base_ids.size() > 63) return false;
    const int threshold = 2 + private_mine_counts[candidate] -
                          static_cast<int>(base_ids.size());
    if (threshold < 0) return false;  // The empty independent set is already a counterexample.

    const int degree = static_cast<int>(adjacent.size());
    std::vector<uint64_t> local_edges(degree, 0);
    std::vector<uint64_t> local_base_hits(degree, 0);
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            if (relations.propagation[adjacent[i]][adjacent[j] / 64] >>
                    (adjacent[j] % 64) & 1U) {
                local_edges[i] |= uint64_t{1} << j;
            }
        }
        for (int b = 0; b < static_cast<int>(base_ids.size()); ++b) {
            const int factor = base_ids[b];
            if (relations.base_factors[adjacent[i]][factor / 64] >> (factor % 64) & 1U) {
                local_base_hits[i] |= uint64_t{1} << b;
            }
        }
    }

    int best = 0;
    bool exceeds_threshold = false;
    auto search = [&](auto&& self, uint64_t available, int chosen,
                      uint64_t covered) -> void {
        if (exceeds_threshold) return;
        best = std::max(best, chosen - __builtin_popcountll(covered));
        if (best > threshold) {
            exceeds_threshold = true;
            return;
        }
        const int optimistic = chosen + __builtin_popcountll(available) -
                               __builtin_popcountll(covered);
        if (optimistic <= best || !available) return;

        int pivot = __builtin_ctzll(available);
        int pivot_degree = -1;
        uint64_t scan = available;
        while (scan) {
            const int vertex = __builtin_ctzll(scan);
            scan &= scan - 1;
            const int current_degree =
                __builtin_popcountll(local_edges[vertex] & available);
            if (current_degree > pivot_degree) {
                pivot = vertex;
                pivot_degree = current_degree;
            }
        }
        const uint64_t pivot_bit = uint64_t{1} << pivot;
        self(self, available & ~pivot_bit & ~local_edges[pivot], chosen + 1,
             covered | local_base_hits[pivot]);
        self(self, available & ~pivot_bit, chosen, covered);
    };
    const uint64_t all = degree == 0 ? 0 :
        (degree == 64 ? ~uint64_t{0} : (uint64_t{1} << degree) - 1);
    search(search, all, 0, 0);
    return !exceeds_threshold;
}

static CandidateReduction reduce_candidates(const Model& original, bool progress) {
    const auto start = std::chrono::steady_clock::now();
    CandidateReduction result{original};
    while (true) {
        const int q = static_cast<int>(result.model.candidates.size());
        const CandidateRelations relations = candidate_relations(result.model);
        std::vector<uint8_t> keep(q, 1);
        int swaps_removed = 0;
        for (int victim = 0; victim < q; ++victim) {
            for (int replacement = 0; replacement < q; ++replacement) {
                if (keep[replacement] &&
                    swap_dominated_candidate(relations, victim, replacement)) {
                    keep[victim] = 0;
                    ++swaps_removed;
                    break;
                }
            }
        }
        if (swaps_removed) {
            // Each deletion keeps its replacement. All three relation sets
            // only shrink when candidates are removed, preserving certificates
            // for the subsequent deletions in this batch.
            result.model = restricted_model(result.model, keep);
            result.swap_dominated += swaps_removed;
            continue;
        }
        {
            std::vector<int> private_mine_counts(q, 0);
            for (const auto& scope : result.model.mine_scopes) {
                if (scope.size() == 1) ++private_mine_counts[scope.front()];
            }
            int removed = 0;
            for (int candidate = 0; candidate < q; ++candidate) {
                if (left_click_dominated_candidate(
                        relations, candidate, private_mine_counts)) {
                    keep[candidate] = 0;
                    ++removed;
                }
            }
            if (removed == 0) break;
            // This certificate remains valid after deleting other candidates:
            // its neighborhood can only shrink, and a mine may become private.
            // Therefore all certified candidates may be removed together.
            result.model = restricted_model(result.model, keep);
            result.left_click_dominated += removed;
            continue;
        }
    }
    result.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    if (progress) {
        std::cerr << "Candidate reduction: " << original.candidates.size() << " -> "
                  << result.model.candidates.size() << " chords ("
                  << result.swap_dominated << " swap-dominated, "
                  << result.left_click_dominated << " left-click-dominated) in "
                  << std::fixed << std::setprecision(3) << result.seconds << " seconds\n";
    }
    return result;
}

static std::vector<int> order_indices(const Model& model, const std::string& name,
                                      int band_size = 1, bool reverse_lines = false) {
    if (band_size < 1) throw UserError("band size must be positive");
    std::vector<int> order(model.candidates.size());
    std::iota(order.begin(), order.end(), 0);
    auto key = [&](int i) {
        const int r = model.candidates[i] / model.width;
        const int c = model.candidates[i] % model.width;
        if (name == "rows") {
            const int line = reverse_lines ? model.height - 1 - r : r;
            return std::tuple<int, int, int>{line / band_size, c, line % band_size};
        }
        if (name == "columns") {
            const int line = reverse_lines ? model.width - 1 - c : c;
            return std::tuple<int, int, int>{line / band_size, r, line % band_size};
        }
        throw UserError("unknown order '" + name + "'");
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    return order;
}

using WidthEstimate = std::tuple<int, int, int, uint64_t>;

static uint64_t saturated_add(uint64_t a, uint64_t b) {
    return a > std::numeric_limits<uint64_t>::max() - b
        ? std::numeric_limits<uint64_t>::max() : a + b;
}

static uint64_t estimated_cut_work(int width) {
    return uint64_t{1} << std::min(width, 60);
}

static WidthEstimate width_estimate(const Model& model, const std::vector<int>& order,
                                    int first_cut = 0, int end_cut = -1) {
    const int q = static_cast<int>(order.size());
    if (end_cut < 0) end_cut = q - 1;
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
    uint64_t work = 0;
    for (int cut = first_cut; cut < end_cut; ++cut) {
        int graph = 0;
        int factors = 0;
        for (auto [lo, hi] : graph_intervals) graph += lo <= cut && cut <= hi;
        for (auto [lo, hi] : factor_intervals) factors += lo <= cut && cut <= hi;
        max_graph = std::max(max_graph, graph);
        max_factors = std::max(max_factors, factors);
        max_total = std::max(max_total, graph + factors);
        work = saturated_add(work, estimated_cut_work(graph + factors));
    }
    return {max_total, max_graph, max_factors, work};
}

// Optimize the order inside each row or column while retaining the global
// strip sweep.  A 16-row expert board has at most 16 candidates in a column,
// so an exact subset DP over the possible partial-column cuts is inexpensive.
static std::vector<int> smart_line_order_indices(const Model& model,
                                                 const std::string& name,
                                                 bool reverse_lines = false) {
    const bool by_columns = name == "columns";
    if (!by_columns && name != "rows") throw UserError("unknown smart order '" + name + "'");
    const int q = static_cast<int>(model.candidates.size());
    const int line_count = by_columns ? model.width : model.height;
    auto primary = [&](int candidate) {
        const int cell = model.candidates[candidate];
        return by_columns ? cell % model.width : cell / model.width;
    };
    auto secondary = [&](int candidate) {
        const int cell = model.candidates[candidate];
        return by_columns ? cell / model.width : cell % model.width;
    };
    auto sweep_line = [&](int candidate) {
        const int line = primary(candidate);
        return reverse_lines ? line_count - 1 - line : line;
    };

    std::vector<std::vector<int>> lines(line_count);
    for (int candidate = 0; candidate < q; ++candidate) {
        lines[primary(candidate)].push_back(candidate);
    }
    for (auto& line : lines) {
        std::sort(line.begin(), line.end(), [&](int a, int b) {
            return secondary(a) < secondary(b);
        });
    }

    std::vector<int> result;
    result.reserve(q);
    for (int line_number = 0; line_number < line_count; ++line_number) {
        const int physical_line = reverse_lines ? line_count - 1 - line_number : line_number;
        const auto& line = lines[physical_line];
        const int p = static_cast<int>(line.size());
        if (p <= 1) {
            result.insert(result.end(), line.begin(), line.end());
            continue;
        }
        // Wide rows can make 2^p impractical.  The important expert-board
        // case is a column of at most 16 candidates.
        if (p > 20) {
            result.insert(result.end(), line.begin(), line.end());
            continue;
        }

        const uint32_t state_count = uint32_t{1} << p;
        const uint32_t all = state_count - 1;
        std::vector<int> local_bit(q, -1);
        for (int bit = 0; bit < p; ++bit) local_bit[line[bit]] = bit;

        auto crossing_counts = [&](const std::vector<std::vector<int>>& first,
                                   const std::vector<std::vector<int>>* second = nullptr) {
            std::vector<int> before_only(state_count, 0);
            std::vector<int> after_only(state_count, 0);
            std::vector<int> current_only(state_count, 0);
            int always = 0;
            int before_total = 0;
            int after_total = 0;
            int current_total = 0;
            auto add_scope = [&](const std::vector<int>& scope) {
                bool before = false;
                bool after = false;
                uint32_t mask = 0;
                for (int candidate : scope) {
                    const int candidate_line = sweep_line(candidate);
                    if (candidate_line < line_number) before = true;
                    else if (candidate_line > line_number) after = true;
                    else mask |= uint32_t{1} << local_bit[candidate];
                }
                if (before && after) ++always;
                else if (before) {
                    ++before_only[mask];
                    ++before_total;
                } else if (after) {
                    ++after_only[mask];
                    ++after_total;
                } else if (mask) {
                    ++current_only[mask];
                    ++current_total;
                }
            };
            for (const auto& scope : first) if (!scope.empty()) add_scope(scope);
            if (second) for (const auto& scope : *second) if (!scope.empty()) add_scope(scope);
            auto zeta = [&](std::vector<int>& counts) {
                for (int bit = 0; bit < p; ++bit) {
                    for (uint32_t mask = 0; mask < state_count; ++mask) {
                        if (mask & (uint32_t{1} << bit)) {
                            counts[mask] += counts[mask ^ (uint32_t{1} << bit)];
                        }
                    }
                }
            };
            zeta(before_only);
            zeta(after_only);
            zeta(current_only);
            std::vector<int> answer(state_count, always);
            for (uint32_t mask = 0; mask < state_count; ++mask) {
                answer[mask] += before_total - before_only[mask];
                answer[mask] += after_total - after_only[all ^ mask];
                answer[mask] += current_total - current_only[mask]
                    - current_only[all ^ mask];
            }
            return answer;
        };

        std::vector<int> graph_cost = crossing_counts(model.zero_scopes);
        std::vector<int> factor_cost = crossing_counts(model.mine_scopes, &model.base_scopes);

        // Candidate connectivity vertices are live when the vertex has been
        // processed but at least one graph neighbor has not.
        std::vector<int> old_masks(state_count, 0);
        int old_total = 0;
        int old_always = 0;
        for (int candidate = 0; candidate < q; ++candidate) {
            if (sweep_line(candidate) >= line_number) continue;
            bool later = false;
            uint32_t mask = 0;
            for (int neighbor : model.graph[candidate]) {
                const int neighbor_line = sweep_line(neighbor);
                if (neighbor_line > line_number) later = true;
                else if (neighbor_line == line_number) {
                    mask |= uint32_t{1} << local_bit[neighbor];
                }
            }
            if (later) ++old_always;
            else if (mask) {
                ++old_masks[mask];
                ++old_total;
            }
        }
        for (int bit = 0; bit < p; ++bit) {
            for (uint32_t mask = 0; mask < state_count; ++mask) {
                if (mask & (uint32_t{1} << bit)) {
                    old_masks[mask] += old_masks[mask ^ (uint32_t{1} << bit)];
                }
            }
        }
        std::vector<uint32_t> same_line_neighbors(p, 0);
        std::vector<uint8_t> has_later_neighbor(p, 0);
        for (int bit = 0; bit < p; ++bit) {
            for (int neighbor : model.graph[line[bit]]) {
                const int neighbor_line = sweep_line(neighbor);
                if (neighbor_line > line_number) has_later_neighbor[bit] = 1;
                else if (neighbor_line == line_number) {
                    same_line_neighbors[bit] |= uint32_t{1} << local_bit[neighbor];
                }
            }
        }
        for (uint32_t mask = 0; mask < state_count; ++mask) {
            graph_cost[mask] += old_always + old_total - old_masks[mask];
            uint32_t selected = mask;
            while (selected) {
                const int bit = __builtin_ctz(selected);
                selected &= selected - 1;
                if (has_later_neighbor[bit] || (same_line_neighbors[bit] & ~mask & all)) {
                    ++graph_cost[mask];
                }
            }
        }

        struct PathScore {
            int max_total = std::numeric_limits<int>::max();
            int max_graph = std::numeric_limits<int>::max();
            int max_factors = std::numeric_limits<int>::max();
            uint64_t work = std::numeric_limits<uint64_t>::max();
        };
        auto less_score = [](const PathScore& a, const PathScore& b) {
            return std::tie(a.max_total, a.max_graph, a.max_factors, a.work) <
                   std::tie(b.max_total, b.max_graph, b.max_factors, b.work);
        };
        std::vector<PathScore> best(state_count);
        std::vector<int8_t> predecessor(state_count, -1);
        best[0] = {graph_cost[0] + factor_cost[0], graph_cost[0], factor_cost[0],
                   estimated_cut_work(graph_cost[0] + factor_cost[0])};
        for (uint32_t mask = 1; mask < state_count; ++mask) {
            uint32_t choices = mask;
            while (choices) {
                const int bit = __builtin_ctz(choices);
                choices &= choices - 1;
                const uint32_t previous = mask ^ (uint32_t{1} << bit);
                PathScore score = best[previous];
                const int total = graph_cost[mask] + factor_cost[mask];
                score.max_total = std::max(score.max_total, total);
                score.max_graph = std::max(score.max_graph, graph_cost[mask]);
                score.max_factors = std::max(score.max_factors, factor_cost[mask]);
                score.work = saturated_add(score.work, estimated_cut_work(total));
                if (predecessor[mask] < 0 || less_score(score, best[mask])) {
                    best[mask] = score;
                    predecessor[mask] = static_cast<int8_t>(bit);
                }
            }
        }
        std::vector<int> reversed;
        reversed.reserve(p);
        for (uint32_t mask = all; mask; ) {
            const int bit = predecessor[mask];
            reversed.push_back(line[bit]);
            mask ^= uint32_t{1} << bit;
        }
        std::reverse(reversed.begin(), reversed.end());
        result.insert(result.end(), reversed.begin(), reversed.end());
    }
    return result;
}

static std::vector<int> candidate_band_sizes(int size) {
    std::set<int> values{1, size};
    for (int value = 2; value < size; value *= 2) values.insert(value);
    return {values.begin(), values.end()};
}

struct DynamicBandOrder {
    std::vector<int> order;
    std::vector<int> widths;
};

// A cut inside [start,end) depends only on the candidates in that band:
// all earlier lines have been processed, and all later lines are unprocessed.
// Precompute each possible band profile, then find the partition minimizing
// peak total width, peak connectivity width, and estimated work in that order.
static DynamicBandOrder dynamic_band_order(const Model& model,
                                           const std::string& orientation,
                                           bool reverse) {
    const bool columns = orientation == "columns";
    const int lines = columns ? model.width : model.height;
    const int max_band = std::min(lines, 8);
    const int q = static_cast<int>(model.candidates.size());
    std::vector<std::vector<int>> by_line(lines);
    for (int i = 0; i < q; ++i) {
        const int cell = model.candidates[i];
        const int physical = columns ? cell % model.width : cell / model.width;
        by_line[reverse ? lines - 1 - physical : physical].push_back(i);
    }
    auto secondary = [&](int i) {
        const int cell = model.candidates[i];
        return columns ? cell / model.width : cell % model.width;
    };
    std::vector<int> prefix(lines + 1, 0);
    for (int line = 0; line < lines; ++line) {
        std::sort(by_line[line].begin(), by_line[line].end(), [&](int a, int b) {
            return secondary(a) < secondary(b);
        });
        prefix[line + 1] = prefix[line] + static_cast<int>(by_line[line].size());
    }

    struct Edge {
        WidthEstimate estimate;
        std::vector<int> inside;
    };
    std::vector<std::vector<Edge>> edge(lines);
    for (int start = 0; start < lines; ++start) {
        for (int end = start + 1; end <= std::min(lines, start + max_band); ++end) {
            auto& current = edge[start].emplace_back();
            for (int line = start; line < end; ++line) {
                current.inside.insert(current.inside.end(),
                                      by_line[line].begin(), by_line[line].end());
            }
            std::sort(current.inside.begin(), current.inside.end(),
                      [&](int a, int b) {
                          const int sa = secondary(a), sb = secondary(b);
                          if (sa != sb) return sa < sb;
                          const int ca = model.candidates[a], cb = model.candidates[b];
                          const int la = columns ? ca % model.width : ca / model.width;
                          const int lb = columns ? cb % model.width : cb / model.width;
                          return (reverse ? la > lb : la < lb);
                      });
            std::vector<int> trial;
            trial.reserve(q);
            for (int line = 0; line < start; ++line) {
                trial.insert(trial.end(), by_line[line].begin(), by_line[line].end());
            }
            trial.insert(trial.end(), current.inside.begin(), current.inside.end());
            for (int line = end; line < lines; ++line) {
                trial.insert(trial.end(), by_line[line].begin(), by_line[line].end());
            }
            current.estimate = width_estimate(model, trial, prefix[start],
                                               std::min(prefix[end], q - 1));
        }
    }

    constexpr int infinity = std::numeric_limits<int>::max();
    std::vector<int> min_total(lines + 1, infinity);
    min_total[0] = 0;
    for (int end = 1; end <= lines; ++end) {
        for (int start = std::max(0, end - max_band); start < end; ++start) {
            min_total[end] = std::min(min_total[end],
                std::max(min_total[start], std::get<0>(edge[start][end - start - 1].estimate)));
        }
    }
    std::vector<int> min_graph(lines + 1, infinity);
    min_graph[0] = 0;
    for (int end = 1; end <= lines; ++end) {
        for (int start = std::max(0, end - max_band); start < end; ++start) {
            const auto& estimate = edge[start][end - start - 1].estimate;
            if (std::get<0>(estimate) > min_total[lines] || min_graph[start] == infinity) continue;
            min_graph[end] = std::min(min_graph[end],
                std::max(min_graph[start], std::get<1>(estimate)));
        }
    }
    std::vector<uint64_t> min_work(lines + 1, std::numeric_limits<uint64_t>::max());
    std::vector<int> previous(lines + 1, -1);
    min_work[0] = 0;
    for (int end = 1; end <= lines; ++end) {
        for (int start = std::max(0, end - max_band); start < end; ++start) {
            const auto& estimate = edge[start][end - start - 1].estimate;
            if (std::get<0>(estimate) > min_total[lines] ||
                std::get<1>(estimate) > min_graph[lines] ||
                (start != 0 && previous[start] < 0)) continue;
            const uint64_t work = saturated_add(min_work[start], std::get<3>(estimate));
            if (previous[end] < 0 || work < min_work[end]) {
                min_work[end] = work;
                previous[end] = start;
            }
        }
    }
    if (previous[lines] < 0) throw std::logic_error("dynamic band partition not found");
    std::vector<std::pair<int, int>> bands;
    for (int end = lines; end > 0; end = previous[end]) {
        bands.emplace_back(previous[end], end);
    }
    std::reverse(bands.begin(), bands.end());
    DynamicBandOrder result;
    result.order.reserve(q);
    for (auto [start, end] : bands) {
        result.widths.push_back(end - start);
        const auto& inside = edge[start][end - start - 1].inside;
        result.order.insert(result.order.end(), inside.begin(), inside.end());
    }
    return result;
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
        if (requested == "columns-smart" || requested == "rows-smart") {
            if (band_size) throw UserError("--band-size cannot be combined with a smart order");
            const std::string base = requested.substr(0, requested.find('-'));
            auto order = smart_line_order_indices(model, base);
            return {ordered_model(model, order), requested};
        }
        const int size = band_size.value_or(1);
        auto order = order_indices(model, requested, size);
        const std::string name = size == 1 ? requested : requested + "-band-" + std::to_string(size);
        return {ordered_model(model, order), name};
    }
    std::vector<OrderChoice> choices;
    int standard_best = std::numeric_limits<int>::max();
    if (!band_size) {
        struct StandardEstimate {
            std::string name;
            bool reverse = false;
            WidthEstimate estimate;
        };
        std::vector<StandardEstimate> standard_estimates;
        for (const std::string name : {"columns", "rows"}) {
            for (bool reverse : {false, true}) {
                auto order = order_indices(model, name, 1, reverse);
                auto estimate = width_estimate(model, order);
                standard_best = std::min(standard_best, std::get<0>(estimate));
                const std::string label = name + (reverse ? "-reverse" : "");
                choices.push_back({estimate, label, std::move(order)});
                standard_estimates.push_back({name, reverse, estimate});
            }
        }
        for (const auto& standard : standard_estimates) {
            const auto& name = standard.name;
            const int physical_line_size = name == "columns" ? model.height : model.width;
            // Do not spend 2^p preprocessing time on the long-axis sweep, or
            // on an orientation whose ordinary estimate is already clearly
            // inferior.  Explicit rows-smart/columns-smart remains available.
            if (physical_line_size > 20 ||
                std::get<0>(standard.estimate) > standard_best + 2) continue;
            auto smart_order = smart_line_order_indices(model, name, standard.reverse);
            auto smart_estimate = width_estimate(model, smart_order);
            const std::string label = name + (standard.reverse ? "-reverse" : "") + "-smart";
            choices.push_back({smart_estimate, label, std::move(smart_order)});
        }
    }
    struct BandCandidate {
        std::string name;
        int size = 1;
        bool reverse = false;
    };
    std::vector<BandCandidate> candidates;
    if (band_size) {
        candidates.push_back({"columns", *band_size, false});
        candidates.push_back({"columns", *band_size, true});
        candidates.push_back({"rows", *band_size, false});
        candidates.push_back({"rows", *band_size, true});
    } else {
        auto row_sizes = candidate_band_sizes(model.height);
        auto column_sizes = candidate_band_sizes(model.width);
        for (size_t i = 1; i < row_sizes.size(); ++i) {
            candidates.push_back({"rows", row_sizes[i], false});
            candidates.push_back({"rows", row_sizes[i], true});
        }
        for (size_t i = 1; i < column_sizes.size(); ++i) {
            candidates.push_back({"columns", column_sizes[i], false});
            candidates.push_back({"columns", column_sizes[i], true});
        }
    }
    for (const auto& candidate : candidates) {
        auto order = order_indices(model, candidate.name, candidate.size, candidate.reverse);
        auto estimate = width_estimate(model, order);
        if (band_size || std::get<0>(estimate) <= standard_best - 2) {
            const std::string label = candidate.name +
                (candidate.reverse ? "-reverse" : "") +
                "-band-" + std::to_string(candidate.size);
            choices.push_back({estimate, label, std::move(order)});
        }
    }
    if (!band_size) {
        for (const std::string orientation : {"columns", "rows"}) {
            for (bool reverse : {false, true}) {
                auto dynamic = dynamic_band_order(model, orientation, reverse);
                auto estimate = width_estimate(model, dynamic.order);
                std::string label = orientation + (reverse ? "-reverse" : "") + "-dynamic-";
                for (size_t i = 0; i < dynamic.widths.size(); ++i) {
                    if (i) label += '.';
                    label += std::to_string(dynamic.widths[i]);
                }
                choices.push_back({estimate, std::move(label), std::move(dynamic.order)});
            }
        }
    }
    auto best = std::min_element(choices.begin(), choices.end(), [](const auto& a, const auto& b) {
        return std::tuple<int, int, uint64_t, std::string>{
                   std::get<0>(a.estimate), std::get<1>(a.estimate),
                   std::get<3>(a.estimate), a.name} <
               std::tuple<int, int, uint64_t, std::string>{
                   std::get<0>(b.estimate), std::get<1>(b.estimate),
                   std::get<3>(b.estimate), b.name};
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

template <typename FactorBits>
struct FactorOps;

template <>
struct FactorOps<Bits> {
    static Bits zero(size_t words) { return Bits(words); }
    static void set(Bits& bits, int position) { set_bit(bits, position); }
    static int count(const Bits& bits) { return bit_count(bits); }
    static int count_new_and(const Bits& member, const Bits& old, const Bits& filter) {
        return bit_count_new_and(member, old, filter);
    }
    static void add(Bits& target, const Bits& member) {
        for (size_t word = 0; word < target.size(); ++word) target[word] |= member[word];
    }
    static void retain(Bits& target, const Bits& active) {
        for (size_t word = 0; word < target.size(); ++word) target[word] &= active[word];
    }
    static bool test(const Bits& bits, int position) { return test_bit(bits, position); }
    static void clear(Bits& bits, int position) {
        bits[position / 64] &= ~(uint64_t{1} << (position % 64));
    }
};

template <>
struct FactorOps<uint64_t> {
    static uint64_t zero(size_t) { return 0; }
    static void set(uint64_t& bits, int position) { bits |= uint64_t{1} << position; }
    static int count(uint64_t bits) { return __builtin_popcountll(bits); }
    static int count_new_and(uint64_t member, uint64_t old, uint64_t filter) {
        return __builtin_popcountll(member & ~old & filter);
    }
    static void add(uint64_t& target, uint64_t member) { target |= member; }
    static void retain(uint64_t& target, uint64_t active) { target &= active; }
    static bool test(uint64_t bits, int position) { return (bits >> position) & 1U; }
    static void clear(uint64_t& bits, int position) {
        bits &= ~(uint64_t{1} << position);
    }
};

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

static bool dominance_penalty_at_most(uint64_t candidate, uint64_t other,
                                      uint64_t base_mask, int allowance) {
    const uint64_t candidate_good = candidate ^ base_mask;
    const uint64_t other_good = other ^ base_mask;
    return __builtin_popcountll(other_good & ~candidate_good) <= allowance;
}

static std::pair<int, int> factor_hit_counts(const Bits& hits, const Bits& base_mask) {
    int base_hits = 0;
    int total_hits = 0;
    for (size_t word = 0; word < hits.size(); ++word) {
        base_hits += __builtin_popcountll(hits[word] & base_mask[word]);
        total_hits += __builtin_popcountll(hits[word]);
    }
    return {base_hits, total_hits};
}

static std::pair<int, int> factor_hit_counts(uint64_t hits, uint64_t base_mask) {
    return {__builtin_popcountll(hits & base_mask), __builtin_popcountll(hits)};
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

template <typename FactorBits>
struct FactorState {
    uint32_t connectivity_id = 0;
    FactorBits hits;
    bool operator==(const FactorState& other) const {
        return connectivity_id == other.connectivity_id && hits == other.hits;
    }
};

static size_t append_factor_hash(size_t hash, const Bits& bits) {
    for (size_t i = 0; i < bits.size(); ++i) hash = hash_combine(hash, bits[i]);
    return hash;
}

static size_t append_factor_hash(size_t hash, uint64_t bits) {
    return hash_combine(hash, bits);
}

template <typename FactorBits>
struct FactorStateHash {
    size_t operator()(const FactorState<FactorBits>& state) const {
        size_t hash = hash_combine(0, state.connectivity_id);
        return append_factor_hash(hash, state.hits);
    }
};

template <bool TrackChosen>
struct Record {
    int cost = 0;
    Bits chosen;
};

template <>
struct Record<false> {
    int cost = 0;
};

static void release_factor_storage(Bits& bits) {
    if (bits.size() > Bits::INLINE_WORDS) bits = Bits{};
}

static void release_factor_storage(uint64_t&) {}

// The DP creates a fresh table for each layer, fills it, then only erases from
// it during dominance pruning. Store values densely and keep a compact open-
// addressed index alongside them. This avoids one allocation and one pointer
// chase per state while preserving stable dense-entry indexes during pruning.
template <typename FactorBits, bool TrackChosen>
class FactorTable {
public:
    using State = FactorState<FactorBits>;
    using StateHash = FactorStateHash<FactorBits>;
    using value_type = std::pair<State, Record<TrackChosen>>;

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = FactorTable::value_type;
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
        friend class FactorTable;
        iterator(FactorTable* owner, size_t index) : owner_(owner), index_(index) { skip_dead(); }
        void skip_dead() {
            if (owner_ == nullptr) return;
            while (index_ < owner_->entries_.size() && !owner_->alive_[index_]) ++index_;
        }

        FactorTable* owner_ = nullptr;
        size_t index_ = 0;
    };

    FactorTable() = default;
    FactorTable(FactorTable&&) noexcept = default;
    FactorTable& operator=(FactorTable&&) noexcept = default;
    FactorTable(const FactorTable&) = delete;
    FactorTable& operator=(const FactorTable&) = delete;

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
    std::pair<iterator, bool> emplace(State state, Record<TrackChosen> record) {
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
                release_factor_storage(entries_[entry_index].first.hits);
                if constexpr (TrackChosen) {
                    if (entries_[entry_index].second.chosen.size() > Bits::INLINE_WORDS) {
                        entries_[entry_index].second.chosen = Bits{};
                    }
                }
                return;
            }
            slot = (slot + 1) & mask;
        }
        throw std::logic_error("flat DP table lost an indexed state");
    }

    void swap(FactorTable& other) noexcept {
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

template <typename FactorBits, bool TrackChosen>
static size_t prune_dominated(FactorTable<FactorBits, TrackChosen>& table,
                              uint64_t comparison_limit,
                              const FactorBits& base_mask,
                              const ConnectivityPool& connectivity_pool,
                              size_t signature_words) {
    if (comparison_limit == 0 || table.size() < 2) return 0;
    using Item = typename FactorTable<FactorBits, TrackChosen>::iterator;
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
        const auto [base_hits, total_hits] =
            factor_hit_counts(item->first.hits, base_mask);
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
    const bool reverse = order_name.find("-reverse") != std::string::npos;
    const size_t dynamic_marker = order_name.find("-dynamic-");
    std::vector<int> dynamic_group;
    std::vector<int> dynamic_position;
    if (dynamic_marker != std::string::npos) {
        const int line_count = rows ? model.height : model.width;
        dynamic_group.resize(line_count);
        dynamic_position.resize(line_count);
        const std::string widths = order_name.substr(dynamic_marker + 9);
        std::istringstream input(widths);
        std::string token;
        int line = 0, group = 0;
        while (std::getline(input, token, '.')) {
            const int width = std::stoi(token);
            for (int offset = 0; offset < width && line < line_count; ++offset, ++line) {
                dynamic_group[line] = group;
                dynamic_position[line] = offset;
            }
            ++group;
        }
        if (line != line_count) throw std::logic_error("invalid dynamic band widths");
    }
    int band = 1;
    const size_t marker = order_name.rfind("-band-");
    if (marker != std::string::npos) band = std::stoi(order_name.substr(marker + 6));
    std::vector<int> cells(model.height * model.width);
    std::iota(cells.begin(), cells.end(), 0);
    auto key = [&](int cell) {
        const int r = cell / model.width;
        const int c = cell % model.width;
        if (rows) {
            const int line = reverse ? model.height - 1 - r : r;
            if (!dynamic_group.empty()) {
                return std::tuple<int, int, int>{dynamic_group[line], c,
                                                  dynamic_position[line]};
            }
            return std::tuple<int, int, int>{line / band, c, line % band};
        }
        const int line = reverse ? model.width - 1 - c : c;
        if (!dynamic_group.empty()) {
            return std::tuple<int, int, int>{dynamic_group[line], r,
                                              dynamic_position[line]};
        }
        return std::tuple<int, int, int>{line / band, r, line % band};
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

struct FactorPlan {
    std::vector<std::vector<int>> scopes;
    std::vector<uint8_t> is_flag;
    std::vector<int> first;
    std::vector<int> last;
    std::vector<int> slot;
    std::vector<int> zero_factor;
    int slot_count = 0;
};

static FactorPlan make_factor_plan(const Model& model) {
    FactorPlan plan;
    plan.zero_factor.assign(model.zero_scopes.size(), -1);
    auto add_scopes = [&](const std::vector<std::vector<int>>& source, bool flag) {
        for (size_t source_index = 0; source_index < source.size(); ++source_index) {
            const auto& scope = source[source_index];
            if (scope.empty()) continue;
            const int factor = static_cast<int>(plan.scopes.size());
            plan.scopes.push_back(scope);
            plan.is_flag.push_back(flag);
            if (!flag && source_index < model.zero_scopes.size()) {
                plan.zero_factor[source_index] = factor;
            }
        }
    };
    add_scopes(model.mine_scopes, true);
    add_scopes(model.base_scopes, false);
    const int factor_count = static_cast<int>(plan.scopes.size());
    plan.first.resize(factor_count);
    plan.last.resize(factor_count);
    plan.slot.resize(factor_count);
    std::vector<int> order(factor_count);
    std::iota(order.begin(), order.end(), 0);
    for (int f = 0; f < factor_count; ++f) {
        plan.first[f] = plan.scopes[f].front();
        plan.last[f] = plan.scopes[f].back();
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return std::tuple<int, int, int>{plan.first[a], plan.last[a], a} <
               std::tuple<int, int, int>{plan.first[b], plan.last[b], b};
    });
    std::vector<int> slot_last;
    for (int f : order) {
        int chosen = -1;
        for (int slot = 0; slot < static_cast<int>(slot_last.size()); ++slot) {
            if (slot_last[slot] < plan.first[f]) {
                chosen = slot;
                break;
            }
        }
        if (chosen < 0) {
            chosen = static_cast<int>(slot_last.size());
            slot_last.push_back(plan.last[f]);
        } else {
            slot_last[chosen] = plan.last[f];
        }
        plan.slot[f] = chosen;
    }
    plan.slot_count = static_cast<int>(slot_last.size());
    return plan;
}

template <typename FactorBits, bool TrackChosen>
static Solution solve_frontier_core(const Model& original_model,
                                    const Model& model,
                                    const std::string& order_name,
                                    const FactorPlan& factors,
                                    bool progress) {
    using Ops = FactorOps<FactorBits>;
    using StateType = FactorState<FactorBits>;
    using TableType = FactorTable<FactorBits, TrackChosen>;
    const int q = static_cast<int>(model.candidates.size());
    const size_t factor_words = (factors.slot_count + 63) / 64;
    const size_t chosen_words = (q + 63) / 64;
    const FactorBits zero_factors = Ops::zero(factor_words);
    std::vector<FactorBits> factor_member(q, zero_factors);
    std::vector<FactorBits> flag_member(q, zero_factors);
    std::vector<FactorBits> base_member(q, zero_factors);
    std::vector<FactorBits> active_after(q, zero_factors);
    std::vector<FactorBits> base_after(q, zero_factors);
    for (int f = 0; f < static_cast<int>(factors.scopes.size()); ++f) {
        const int slot = factors.slot[f];
        for (int variable : factors.scopes[f]) {
            Ops::set(factor_member[variable], slot);
            Ops::set(factors.is_flag[f] ? flag_member[variable] : base_member[variable], slot);
        }
        for (int i = factors.first[f]; i < factors.last[f]; ++i) {
            Ops::set(active_after[i], slot);
            if (!factors.is_flag[f]) Ops::set(base_after[i], slot);
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
    // Give each candidate a stable slot for the cuts where it can be reached
    // from the processed region. A slot can be reused at the transition that
    // processes its previous owner because that owner's bit is explicitly
    // removed before the outgoing signature is interned.
    std::vector<int> first_contact(q, q);
    for (int i = 0; i < q; ++i) {
        for (int candidate = i + 1; candidate < q; ++candidate) {
            if (test_bit(future_neighbors[i], candidate)) {
                first_contact[candidate] = std::min(first_contact[candidate], i);
            }
        }
    }
    std::vector<int> contact_order;
    for (int candidate = 0; candidate < q; ++candidate) {
        if (first_contact[candidate] < q) contact_order.push_back(candidate);
    }
    std::sort(contact_order.begin(), contact_order.end(), [&](int a, int b) {
        return std::pair<int, int>{first_contact[a], a} <
               std::pair<int, int>{first_contact[b], b};
    });
    std::vector<int> connectivity_slot(q, -1);
    std::vector<int> connectivity_slot_last;
    for (int candidate : contact_order) {
        int chosen = -1;
        for (int slot = 0; slot < static_cast<int>(connectivity_slot_last.size()); ++slot) {
            if (connectivity_slot_last[slot] <= first_contact[candidate]) {
                chosen = slot;
                break;
            }
        }
        if (chosen < 0) {
            chosen = static_cast<int>(connectivity_slot_last.size());
            connectivity_slot_last.push_back(candidate);
        } else {
            connectivity_slot_last[chosen] = candidate;
        }
        connectivity_slot[candidate] = chosen;
    }
    const bool compact_connectivity = connectivity_slot_last.size() <= 128;
    const size_t compact_signature_words =
        std::max<size_t>(1, (connectivity_slot_last.size() + 63) / 64);
    std::vector<std::vector<uint64_t>> compact_future_neighbors;
    if (compact_connectivity) {
        compact_future_neighbors.assign(q, std::vector<uint64_t>(compact_signature_words, 0));
        for (int i = 0; i < q; ++i) {
            for (int candidate = i + 1; candidate < q; ++candidate) {
                if (!test_bit(future_neighbors[i], candidate)) continue;
                const int slot = connectivity_slot[candidate];
                compact_future_neighbors[i][slot / 64] |= uint64_t{1} << (slot % 64);
            }
        }
    }
    struct OpeningPattern {
        int factor_slot = -1;
        std::vector<uint64_t> remaining_border;
    };
    std::vector<std::vector<OpeningPattern>> opening_patterns(q);
    // Exact normalization: a pending component whose entire future reach is
    // one opening's undecided border can be replaced by "opening still
    // uncovered, +1 click". With no later border chord, +1 is its seed click;
    // with one, that chord's new seed and newly saved base click cancel.
    for (int z = 0; z < zero_count; ++z) {
        const int factor = factors.zero_factor[z];
        if (factor < 0 || model.zero_scopes[z].size() < 2) continue;
        const int first = model.zero_scopes[z].front();
        const int last = model.zero_scopes[z].back();
        for (int i = first; i < last; ++i) {
            std::vector<uint64_t> border(
                compact_connectivity ? compact_signature_words : chosen_words, 0);
            for (int candidate : model.zero_scopes[z]) {
                if (candidate <= i) continue;
                const int position = compact_connectivity
                    ? connectivity_slot[candidate] : candidate;
                border[position / 64] |= uint64_t{1} << (position % 64);
            }
            opening_patterns[i].push_back(
                OpeningPattern{factors.slot[factor], std::move(border)});
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
    const auto region_ranks = progress ? ordered_region_ranks(model, order_name) : std::vector<int>{};

    TableType table;
    table.reserve(16);
    if constexpr (TrackChosen) {
        table.emplace(StateType{0, zero_factors},
                      Record<true>{model.three_bv(), Bits(chosen_words)});
    } else {
        table.emplace(StateType{0, zero_factors}, Record<false>{model.three_bv()});
    }
    ConnectivityPool connectivity_pool;
    ConnectivityPool next_connectivity_pool;
    size_t peak_states = 1;
    int max_boundary = 0;
    int max_active = 0;
    size_t dominated_total = 0;
    size_t opening_absorptions = 0;

    for (int i = 0; i < q; ++i) {
        const auto& new_boundary = boundaries[i + 1];
        const size_t signature_words = compact_connectivity
            ? compact_signature_words : chosen_words;
        const int current_connectivity_slot = compact_connectivity
            ? connectivity_slot[i] : i;

        TableType next;
        const size_t desired_capacity = table.size() > (std::numeric_limits<size_t>::max() - 16) / 2
            ? std::numeric_limits<size_t>::max() : table.size() * 2 + 16;
        next.reserve(desired_capacity);
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
            std::vector<uint64_t> merged(signature_words);
            std::vector<uint64_t> projected(signature_words);
            std::vector<uint64_t> canonical;
            std::vector<uint8_t> absorption_removed;
            for (const auto& [old_state, old_record] : table) {
                if (!connectivity_ready[old_state.connectivity_id]) {
                    const auto& old_signature = connectivity_pool[old_state.connectivity_id];
                    std::array<ConnectivityTransition, 2> computed;
                    for (int selected = 0; selected <= 1; ++selected) {
                        int closed = 0;
                        outgoing_words.clear();
                        outgoing_offsets.clear();
                        std::fill(merged.begin(), merged.end(), 0);
                        const size_t old_component_count =
                            old_signature.size() / signature_words;
                        outgoing_words.reserve(
                            (old_component_count + selected) * signature_words);
                        outgoing_offsets.reserve(old_component_count + selected);
                        if (selected) {
                            if (compact_connectivity) {
                                merged = compact_future_neighbors[i];
                            } else {
                                for (size_t word = 0; word < chosen_words; ++word) {
                                    merged[word] = future_neighbors[i][word];
                                }
                            }
                        }
                        for (size_t offset = 0; offset < old_signature.size();
                             offset += signature_words) {
                            const bool touches = current_connectivity_slot >= 0 &&
                                ((old_signature[offset + current_connectivity_slot / 64] >>
                                  (current_connectivity_slot % 64)) & 1U);
                            for (size_t word = 0; word < signature_words; ++word) {
                                projected[word] = old_signature[offset + word];
                            }
                            if (current_connectivity_slot >= 0) {
                                projected[current_connectivity_slot / 64] &=
                                    ~(uint64_t{1} << (current_connectivity_slot % 64));
                            }
                            if (selected && touches) {
                                for (size_t word = 0; word < signature_words; ++word) {
                                    merged[word] |= projected[word];
                                }
                                continue;
                            }
                            const size_t start = outgoing_words.size();
                            bool nonempty = false;
                            for (uint64_t value : projected) {
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
                                      for (size_t word = 0; word < signature_words; ++word) {
                                          if (outgoing_words[a + word] !=
                                              outgoing_words[b + word]) {
                                              return outgoing_words[a + word] <
                                                     outgoing_words[b + word];
                                          }
                                      }
                                      return false;
                                  });
                        canonical.clear();
                        canonical.reserve(outgoing_offsets.size() * signature_words);
                        for (size_t offset : outgoing_offsets) {
                            canonical.insert(canonical.end(),
                                             outgoing_words.begin() +
                                                 static_cast<std::ptrdiff_t>(offset),
                                             outgoing_words.begin() +
                                                 static_cast<std::ptrdiff_t>(
                                                     offset + signature_words));
                        }
                        computed[selected] = {next_connectivity_pool.intern(canonical),
                                              closed};
                    }
                    connectivity_cache[old_state.connectivity_id] = std::move(computed);
                    connectivity_ready[old_state.connectivity_id] = 1;
                }

                for (int selected = 0; selected <= 1; ++selected) {
                    FactorBits new_hits = old_state.hits;
                    int factor_cost = 0;
                    if (selected) {
                        factor_cost += Ops::count_new_and(
                            factor_member[i], old_state.hits, flag_member[i]);
                        factor_cost -= Ops::count_new_and(
                            factor_member[i], old_state.hits, base_member[i]);
                        Ops::add(new_hits, factor_member[i]);
                    }
                    Ops::retain(new_hits, active_after[i]);
                    const auto& connection =
                        connectivity_cache[old_state.connectivity_id][selected];
                    int new_cost = old_record.cost + selected + connection.closed + factor_cost;
                    uint32_t connectivity_id = connection.connectivity_id;
                    if (connectivity_id != 0 && !opening_patterns[i].empty()) {
                        const auto& signature = next_connectivity_pool[connectivity_id];
                        const size_t component_count = signature.size() / signature_words;
                        absorption_removed.assign(component_count, 0);
                        bool changed = false;
                        for (const OpeningPattern& opening : opening_patterns[i]) {
                            if (!Ops::test(new_hits, opening.factor_slot)) continue;
                            for (size_t component = 0; component < component_count;
                                 ++component) {
                                if (absorption_removed[component]) continue;
                                const size_t offset = component * signature_words;
                                bool equal = true;
                                for (size_t word = 0; word < signature_words; ++word) {
                                    if (signature[offset + word] !=
                                        opening.remaining_border[word]) {
                                        equal = false;
                                        break;
                                    }
                                }
                                if (!equal) continue;
                                absorption_removed[component] = 1;
                                Ops::clear(new_hits, opening.factor_slot);
                                ++new_cost;
                                ++opening_absorptions;
                                changed = true;
                                break;
                            }
                        }
                        if (changed) {
                            canonical.clear();
                            canonical.reserve(signature.size());
                            for (size_t component = 0; component < component_count;
                                 ++component) {
                                if (absorption_removed[component]) continue;
                                const size_t offset = component * signature_words;
                                canonical.insert(canonical.end(),
                                    signature.begin() + static_cast<std::ptrdiff_t>(offset),
                                    signature.begin() + static_cast<std::ptrdiff_t>(
                                        offset + signature_words));
                            }
                            connectivity_id = next_connectivity_pool.intern(canonical);
                        }
                    }
                    StateType state{connectivity_id, std::move(new_hits)};
                    auto found = next.find(state);
                    if (found == next.end()) {
                        if constexpr (TrackChosen) {
                            Bits new_chosen = old_record.chosen;
                            if (selected) set_bit(new_chosen, i);
                            next.emplace(std::move(state),
                                         Record<true>{new_cost, std::move(new_chosen)});
                        } else {
                            next.emplace(std::move(state), Record<false>{new_cost});
                        }
                    } else if (new_cost < found->second.cost) {
                        found->second.cost = new_cost;
                        if constexpr (TrackChosen) {
                            found->second.chosen = old_record.chosen;
                            if (selected) set_bit(found->second.chosen, i);
                        }
                    }
                }
            }
        }

        // The previous layer and connectivity pool are no longer needed. Release
        // them before dominance pruning so they do not overlap the largest
        // temporary structures of the new layer.
        {
            TableType released;
            table.swap(released);
        }
        {
            ConnectivityPool released;
            connectivity_pool.swap(released);
        }

        const size_t removed = prune_dominated(
            next, DOMINANCE_COMPARISONS, base_after[i],
            next_connectivity_pool, signature_words);
        table = std::move(next);
        connectivity_pool.swap(next_connectivity_pool);
        dominated_total += removed;
        peak_states = std::max(peak_states, table.size());
        max_boundary = std::max(max_boundary, static_cast<int>(new_boundary.size()));
        const int active_count = Ops::count(active_after[i]);
        max_active = std::max(max_active, active_count);
        if (progress && ((i + 1) % PROGRESS_INTERVAL == 0 || i + 1 == q)) {
            std::cerr << "DP: region contains " << region_ranks[i] << "/"
                      << model.height * model.width << " tiles; processed " << i + 1
                      << "/" << q << " chord candidates; " << comma_number(table.size())
                      << " valid boundary states; boundary " << new_boundary.size()
                      << " connectivity items + " << active_count << " factor bits; pruned "
                      << comma_number(dominated_total) << " dominated states; absorbed "
                      << comma_number(opening_absorptions) << " opening chains\n";
        }
    }

    StateType final_state{0, zero_factors};
    auto final = table.find(final_state);
    if (final == table.end()) throw std::logic_error("frontier DP did not reach an empty final state");
    Solution solution;
    if constexpr (TrackChosen) {
        std::vector<int> selected;
        selected.reserve(q);
        for (int i = 0; i < q; ++i) {
            if (!test_bit(final->second.chosen, i)) continue;
            const int cell = model.candidates[i];
            selected.push_back(original_model.candidate_index[cell]);
        }
        std::sort(selected.begin(), selected.end());
        solution = construct_solution(original_model, selected, final->second.cost);
    } else {
        solution.clicks = final->second.cost;
        solution.count_only = true;
    }
    solution.peak_states = peak_states;
    solution.max_boundary_vertices = max_boundary;
    solution.max_active_factors = max_active;
    solution.opening_chain_absorptions = opening_absorptions;
    solution.order_name = order_name;
    return solution;
}

static Solution solve_frontier(const Model& original_model,
                               const std::string& requested_order,
                               std::optional<int> band_size,
                               bool progress,
                               bool count_only = false) {
    CandidateReduction reduction = reduce_candidates(original_model, progress);
    auto [model, order_name] = choose_order(
        reduction.model, requested_order, band_size);
    const FactorPlan factors = make_factor_plan(model);
    Solution solution;
    if (factors.slot_count <= 64) {
        if (count_only) solution = solve_frontier_core<uint64_t, false>(
            original_model, model, order_name, factors, progress);
        else solution = solve_frontier_core<uint64_t, true>(
            original_model, model, order_name, factors, progress);
    } else {
        if (count_only) solution = solve_frontier_core<Bits, false>(
            original_model, model, order_name, factors, progress);
        else solution = solve_frontier_core<Bits, true>(
            original_model, model, order_name, factors, progress);
    }
    solution.candidate_chords_before_reduction =
        static_cast<int>(original_model.candidates.size());
    solution.candidate_chords_after_reduction =
        static_cast<int>(reduction.model.candidates.size());
    solution.swap_dominated_chords = reduction.swap_dominated;
    solution.left_click_dominated_chords = reduction.left_click_dominated;
    solution.reduction_seconds = reduction.seconds;
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
    solution.actions.reserve(solution.clicks);
    std::vector<uint8_t> flagged(model.height * model.width, 0);
    int flags_placed = 0;
    for (const auto& component : solution.components) {
        const auto order = component_chord_order(model, component);
        solution.actions.push_back({ActionType::Left, model.candidates[order.front()]});
        for (int candidate : order) {
            const int chord_cell = model.candidates[candidate];
            // Place each flag only when the first chord that needs it is about
            // to happen. The chosen chord order and click count stay intact.
            for (int adjacent : neighbors(chord_cell, model.height, model.width)) {
                if (model.mines[adjacent] && !flagged[adjacent]) {
                    flagged[adjacent] = 1;
                    ++flags_placed;
                    solution.actions.push_back({ActionType::Flag, adjacent});
                }
            }
            solution.actions.push_back({ActionType::Chord, chord_cell});
        }
    }
    if (flags_placed != static_cast<int>(solution.flags.size())) {
        throw std::logic_error("replay flag count disagrees with chord set");
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

static void print_coord_json(std::ostream& out, int cell, const Model& model) {
    out << '[' << cell / model.width + 1 << ',' << cell % model.width + 1 << ']';
}

static void print_json(const Model& model, const Solution& solution,
                       const std::optional<std::string>& generated_difficulty,
                       const std::optional<uint64_t>& generated_seed,
                       const std::optional<std::string>& generated_url) {
    std::cout << "{\n  \"optimal_clicks\": " << solution.clicks
              << ",\n  \"three_bv\": " << model.three_bv() << ",\n";
    if (!solution.count_only) {
    auto print_candidate_coords = [&](const std::vector<int>& candidates) {
        std::cout << '[';
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (i) std::cout << ',';
            print_coord_json(std::cout, model.candidates[candidates[i]], model);
        }
        std::cout << ']';
    };
    std::cout << "  \"chord_squares\": "; print_candidate_coords(solution.selected); std::cout << ",\n";
    std::cout << "  \"flags\": [";
    for (size_t i = 0; i < solution.flags.size(); ++i) {
        if (i) std::cout << ',';
        print_coord_json(std::cout, solution.flags[i], model);
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
                         model);
    }
    std::cout << "],\n  \"actions\": [";
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ',';
        const auto& action = solution.actions[i];
        std::cout << "{\"action\":\"" << action_word(action.type) << "\",\"row\":"
                  << action.cell / model.width + 1 << ",\"column\":"
                  << action.cell % model.width + 1 << '}';
    }
    std::cout << "],\n  \"clicks\": [";
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ',';
        const auto& action = solution.actions[i];
        std::cout << "[\"" << click_word(action.type) << "\"," << action.cell % model.width + 1
                  << ',' << action.cell / model.width + 1 << ']';
    }
    std::cout << "],\n  \"statistics\": {";
    } else {
        std::cout << "  \"statistics\": {";
    }
    std::cout << "\"candidate_chords\":" << model.candidates.size()
              << ",\"candidate_chords_after_reduction\":"
              << solution.candidate_chords_after_reduction
              << ",\"swap_dominated_chords\":" << solution.swap_dominated_chords
              << ",\"left_click_dominated_chords\":"
              << solution.left_click_dominated_chords
              << ",\"opening_chain_absorptions\":"
              << solution.opening_chain_absorptions;
    if (!solution.count_only) {
        std::cout << ",\"selected_chords\":" << solution.selected.size()
                  << ",\"flag_clicks\":" << solution.flags.size()
                  << ",\"component_seed_clicks\":" << solution.components.size()
                  << ",\"remaining_3bv_clicks\":" << solution.uncovered_units.size();
    }
    std::cout << ",\"order\":\"" << json_escape(solution.order_name) << "\""
              << ",\"peak_dp_states\":" << solution.peak_states
              << ",\"max_boundary_vertices\":" << solution.max_boundary_vertices
              << ",\"max_active_factors\":" << solution.max_active_factors
              << ",\"candidate_reduction_seconds\":" << std::fixed
              << std::setprecision(6) << solution.reduction_seconds
              << ",\"solve_seconds\":" << std::fixed << std::setprecision(6)
              << solution.solve_seconds << '}';
    if (generated_url) {
        std::cout << ",\n  \"generated_board\": {\"difficulty\":\""
                  << json_escape(*generated_difficulty) << "\",\"seed\":" << *generated_seed
                  << ",\"url\":\"" << json_escape(*generated_url) << "\"}";
    }
    std::cout << "\n}\n";
}

static void print_click_tuples(const Model& model, const Solution& solution) {
    std::cout << '[';
    for (size_t i = 0; i < solution.actions.size(); ++i) {
        if (i) std::cout << ", ";
        const auto& action = solution.actions[i];
        std::cout << "('" << click_word(action.type) << "', "
                  << action.cell % model.width + 1 << ", "
                  << action.cell / model.width + 1 << ')';
    }
    std::cout << "]\n";
}

struct Options {
    std::optional<std::string> board;
    std::optional<std::string> generate;
    std::optional<std::string> bulk;
    size_t bulk_count = 0;
    std::optional<size_t> threads;
    std::optional<bool> count_only;
    std::optional<uint64_t> seed;
    std::string order = "auto";
    std::optional<int> band_size;
    bool progress = false;
    bool json = false;
    bool click_tuples = false;
};

static void print_help(const char* program) {
    std::cout
        << "Deterministically Optimal Minesweeper Solver (DOMS, C++17)\n\n"
        << "Usage: " << program << " [BOARD] [options]\n\n"
        << "BOARD may be a PTTACG string or compatible LlamaSweeper URL,\n"
        << "MBF filename, or quoted MBF hex.\n\n"
        << "Options:\n"
        << "  --generate beginner|intermediate|expert\n"
        << "                                 Generate, print, and solve one random board\n"
        << "  --bulk beginner|intermediate|expert COUNT\n"
        << "                                 Generate and solve COUNT boards; write CSV\n"
        << "  --seed N                       Reproduce generated or bulk boards\n"
        << "  --threads N                    Bulk workers (default: one per available core)\n"
        << "  --count-only                   Compute click count without a move replay\n"
        << "  --no-count-only                Include replay (default for single boards)\n"
        << "  --order auto|rows|columns|rows-smart|columns-smart\n"
        << "  --band-size N\n"
        << "  --progress\n"
        << "  --json\n"
        << "  --click-tuples\n"
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

static size_t parse_positive_size(const std::string& value, const std::string& option) {
    const uint64_t parsed = parse_u64(value, option);
    if (parsed == 0 || parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw UserError(option + " requires a positive integer");
    }
    return static_cast<size_t>(parsed);
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
        else if (arg == "--bulk") {
            options.bulk = value_after(i, arg);
            options.bulk_count = parse_positive_size(value_after(i, arg), arg);
        }
        else if (arg == "--seed") options.seed = parse_u64(value_after(i, arg), arg);
        else if (arg == "--threads") options.threads = parse_positive_size(value_after(i, arg), arg);
        else if (arg == "--count-only") options.count_only = true;
        else if (arg == "--no-count-only") options.count_only = false;
        else if (arg == "--order") options.order = value_after(i, arg);
        else if (arg == "--band-size") options.band_size = parse_positive_int(value_after(i, arg), arg);
        else if (arg == "--progress") options.progress = true;
        else if (arg == "--json") options.json = true;
        else if (arg == "--click-tuples") options.click_tuples = true;
        else if (!arg.empty() && arg.front() == '-') throw UserError("unknown option: " + arg);
        else if (options.board) throw UserError("only one board argument is allowed");
        else options.board = arg;
    }
    const auto valid_difficulty = [](const std::string& difficulty) {
        return difficulty == "beginner" || difficulty == "intermediate" ||
               difficulty == "expert";
    };
    if (options.generate && options.bulk) {
        throw UserError("choose only one of --generate and --bulk");
    }
    if (options.bulk) {
        if (!valid_difficulty(*options.bulk)) {
            throw UserError("--bulk must be beginner, intermediate, or expert");
        }
        if (options.board) throw UserError("do not supply a board argument with --bulk");
        if (options.json || options.click_tuples) {
            throw UserError("--bulk already writes CSV; do not combine it with --json or --click-tuples");
        }
    } else if (options.generate) {
        if (!valid_difficulty(*options.generate)) {
            throw UserError("--generate must be beginner, intermediate, or expert");
        }
        if (options.board) throw UserError("do not supply a board argument with --generate");
    } else {
        if (!options.board) throw UserError("a board argument, --generate, or --bulk is required");
        if (options.seed) throw UserError("--seed requires --generate or --bulk");
    }
    if (options.order != "auto" && options.order != "rows" && options.order != "columns" &&
        options.order != "rows-smart" && options.order != "columns-smart") {
        throw UserError("invalid --order value");
    }
    if (options.json && options.click_tuples) throw UserError("choose only one of --json and --click-tuples");
    if (options.threads && !options.bulk) throw UserError("--threads requires --bulk");
    if (options.click_tuples && options.count_only.value_or(false)) {
        throw UserError("--click-tuples requires --no-count-only");
    }
    return options;
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.bulk) {
            const uint64_t master_seed = options.seed.value_or(random_seed());
            std::mt19937_64 rng(master_seed);
            const unsigned detected = std::thread::hardware_concurrency();
            const size_t workers = std::min(options.bulk_count,
                options.threads.value_or(detected ? detected : 1));
            const bool count_only = options.count_only.value_or(true);
            std::cerr << "Bulk RNG: std::mt19937_64; seed: " << master_seed << "\n";
            std::cerr << "Bulk workers: " << workers << "; "
                      << (count_only ? "count-only" : "full replay") << " mode\n";
            std::cout << "board,3bv,optimal_clicks\n";
            const auto bulk_start = std::chrono::steady_clock::now();
            // Bound outstanding boards/results while generating every board on the
            // main thread. This preserves the seeded sequence and CSV row order.
            for (size_t begin = 0; begin < options.bulk_count;) {
                const size_t batch = std::min(options.bulk_count - begin,
                                              workers * std::min<size_t>(8, 64 / workers + 1));
                struct Job {
                    Board board;
                    std::string encoded;
                    int three_bv = 0;
                    int clicks = 0;
                    std::string error;
                };
                std::vector<Job> jobs(batch);
                for (size_t j = 0; j < batch; ++j) {
                    jobs[j].board = generate_standard_board(*options.bulk, rng);
                    jobs[j].encoded = format_pttacg_string(jobs[j].board);
                }
                std::atomic<size_t> next{0};
                auto work = [&] {
                    for (;;) {
                        const size_t j = next.fetch_add(1, std::memory_order_relaxed);
                        if (j >= batch) break;
                        try {
                            Model model = build_model(jobs[j].board);
                            Solution solution = solve_frontier(
                                model, options.order, options.band_size, false, count_only);
                            jobs[j].three_bv = model.three_bv();
                            jobs[j].clicks = solution.clicks;
                        } catch (const std::exception& error) {
                            jobs[j].error = error.what();
                        }
                    }
                };
                if (workers == 1) work();
                else {
                    std::vector<std::thread> pool;
                    pool.reserve(workers);
                    for (size_t t = 0; t < workers; ++t) pool.emplace_back(work);
                    for (auto& thread : pool) thread.join();
                }
                for (size_t j = 0; j < batch; ++j) {
                    const size_t index = begin + j;
                    const auto& job = jobs[j];
                    if (!job.error.empty()) {
                        throw UserError("bulk board " + std::to_string(index + 1) + "/" +
                                        std::to_string(options.bulk_count) + " failed (" +
                                        job.encoded + "): " + job.error);
                    }
                    if (options.progress) {
                        std::cerr << "Bulk board " << index + 1 << '/' << options.bulk_count
                                  << ": " << job.encoded << " => " << job.clicks << " clicks\n";
                    }
                    std::cout << '"' << job.encoded << "\"," << job.three_bv << ','
                              << job.clicks << '\n';
                }
                begin += batch;
            }
            const auto bulk_end = std::chrono::steady_clock::now();
            const double seconds =
                std::chrono::duration<double>(bulk_end - bulk_start).count();
            std::cerr << "Bulk solve time: " << std::fixed << std::setprecision(3)
                      << seconds << " seconds\n";
            return 0;
        }
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
            board = load_board(*options.board);
        }

        const auto start = std::chrono::steady_clock::now();
        Model model = build_model(board);
        Solution solution = solve_frontier(
            model, options.order, options.band_size, options.progress,
            options.count_only.value_or(false));
        const auto end = std::chrono::steady_clock::now();
        solution.solve_seconds = std::chrono::duration<double>(end - start).count();
        std::cerr << "Solve time: " << std::fixed << std::setprecision(3)
                  << solution.solve_seconds << " seconds\n";

        if (options.click_tuples) {
            print_click_tuples(model, solution);
            return 0;
        }
        if (options.json) {
            print_json(model, solution, options.generate, generated_seed, generated_url);
            return 0;
        }
        std::cout << "Optimal clicks: " << solution.clicks << " (3BV without chording: "
                  << model.three_bv() << ")\n";
        if (!solution.count_only) std::cout
                  << "Breakdown: " << solution.flags.size() << " flags + "
                  << solution.components.size() << " seed left-clicks + "
                  << solution.selected.size() << " chords + "
                  << solution.uncovered_units.size() << " remaining 3BV clicks\n";
        std::cout << "DP: " << solution.order_name << " order, "
                  << solution.candidate_chords_after_reduction << '/'
                  << solution.candidate_chords_before_reduction
                  << " chord candidates after proven reductions, "
                  << comma_number(solution.peak_states) << " peak states, boundary "
                  << solution.max_boundary_vertices << " connectivity items + "
                  << solution.max_active_factors << " factor bits\n";
        if (!solution.count_only) std::cout << "Actions:\n";
        for (size_t i = 0; i < solution.actions.size(); ++i) {
            const auto& action = solution.actions[i];
            const char* name = action.type == ActionType::Flag ? "FLAG " :
                               (action.type == ActionType::Left ? "LEFT " : "CHORD");
            std::cout << std::setw(4) << i + 1 << ". " << name << " ("
                      << action.cell / model.width + 1 << ','
                      << action.cell % model.width + 1 << ")\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}

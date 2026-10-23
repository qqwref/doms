// Exact Minesweeper click optimizer using a frontier connectivity DP.
// C++17 port of minesweeper_chord_dp.py.

#include <algorithm>
#include <type_traits>
#include <atomic>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
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

#include "benchmarks/expert_benchmark_reference.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

constexpr std::string_view LLAMA_ALPHABET = "0123456789abcdefghijklmnopqrstuv";
constexpr int PROGRESS_INTERVAL = 10;

// Portable builds otherwise call libgcc for every 64-bit population count.
// Sum bit pairs, nibbles, then bytes in parallel and collect the byte totals.
// Keep the intrinsic when the compilation target has a hardware instruction.
static inline int population_count(uint64_t bits) {
#if defined(__POPCNT__) || defined(__aarch64__)
    return __builtin_popcountll(bits);
#else
    bits -= (bits >> 1) & UINT64_C(0x5555555555555555);
    bits = (bits & UINT64_C(0x3333333333333333)) +
           ((bits >> 2) & UINT64_C(0x3333333333333333));
    bits = (bits + (bits >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return static_cast<int>((bits * UINT64_C(0x0101010101010101)) >> 56);
#endif
}

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

struct ProfileStats {
    double model = 0.0;
    double ordering = 0.0;
    double factor_plan = 0.0;
    double dp_setup = 0.0;
    double transitions = 0.0;
    double dominance = 0.0;
    double replay = 0.0;
    double dominance_setup = 0.0;
    double dominance_exact = 0.0;
    double dominance_cross = 0.0;
    double dominance_forward = 0.0;
    double dominance_reverse = 0.0;
    double dominance_erase = 0.0;
    uint64_t processed_states = 0;
    uint64_t generated_states = 0;
    uint64_t exact_pruned = 0;
    uint64_t forward_pruned = 0;
    uint64_t reverse_pruned = 0;
    uint64_t conditional_pruned = 0;
    uint64_t dominance_passes = 0;
    uint64_t deferred_layers = 0;
    uint64_t peak_generated_states = 0;
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
    ProfileStats profile;
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
    else {
        code = std::to_string(board.width) +
               std::to_string(board.height).insert(0,
                   std::max(0, static_cast<int>(std::to_string(board.width).size()) -
                                  static_cast<int>(std::to_string(board.height).size())), '0');
        const auto dimensions = llama_dimensions(code, (board.mines.size() + 4) / 5);
        if (dimensions != std::pair<int, int>{board.height, board.width}) {
            throw UserError("board dimensions cannot be encoded unambiguously as PTTACG");
        }
    }
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

static bool is_world_game_url(const std::string& source) {
    static const std::regex url(R"(^https://(?:www\.)?minesweeper\.online/game/[0-9]+/?$)",
                                std::regex::icase);
    return std::regex_match(source, url);
}

static fs::path world_browser_path() {
    if (const char* specified = std::getenv("DOMS_BROWSER")) {
        if (fs::is_regular_file(specified)) return fs::absolute(specified);
        throw UserError("DOMS_BROWSER must name an installed Chrome, Edge, or Chromium executable");
    }
    std::vector<fs::path> candidates;
#ifdef _WIN32
    for (const char* variable : {"PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"}) {
        if (const char* root = std::getenv(variable)) {
            candidates.push_back(fs::path(root) / "Google/Chrome/Application/chrome.exe");
            candidates.push_back(fs::path(root) / "Microsoft/Edge/Application/msedge.exe");
        }
    }
    const std::vector<std::string> names = {"chrome.exe", "msedge.exe"};
    constexpr char separator = ';';
#elif defined(__APPLE__)
    candidates.push_back("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    candidates.push_back("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge");
    const std::vector<std::string> names = {"google-chrome", "chromium", "microsoft-edge"};
    constexpr char separator = ':';
#else
    const std::vector<std::string> names = {"google-chrome", "chromium", "chromium-browser",
                                            "microsoft-edge"};
    constexpr char separator = ':';
#endif
    if (const char* env_path = std::getenv("PATH")) {
        std::istringstream directories(env_path);
        std::string directory;
        while (std::getline(directories, directory, separator)) {
            for (const auto& name : names) candidates.push_back(fs::path(directory) / name);
        }
    }
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate)) return fs::absolute(candidate);
    }
    throw UserError("World of Minesweeper import needs Chrome, Edge, or Chromium; "
                    "set DOMS_BROWSER to its executable path if it is not found");
}

static std::string world_game_dom(const std::string& url) {
    const fs::path browser = world_browser_path();
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path temporary = fs::temp_directory_path() / ("doms-world-" + nonce);
    if (!fs::create_directory(temporary)) throw UserError("cannot create browser work directory");
    struct RemoveDirectory {
        fs::path path;
        ~RemoveDirectory() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{temporary};
    const fs::path output = temporary / "dom.html";
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw UserError("cannot capture browser output");
    const HANDLE errors = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inheritable,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inheritable,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errors == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        throw UserError("cannot configure headless browser output");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = file;
    startup.hStdError = errors;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + browser.wstring() + L"\" --headless=new --dump-dom "
        L"--timeout=45000 --virtual-time-budget=45000 --no-first-run "
        L"\"--user-data-dir=" + temporary.wstring() + L"\" \"" +
        std::wstring(url.begin(), url.end()) + L"\"";
    const BOOL started = CreateProcessW(browser.c_str(), command.data(), nullptr,
                                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(file);
    CloseHandle(errors);
    CloseHandle(input);
    if (!started) throw UserError("could not launch headless browser");
    const DWORD wait = WaitForSingleObject(process.hProcess, 65000);
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        throw UserError("headless browser did not finish loading the World of Minesweeper game");
    }
#else
    const std::string profile = "--user-data-dir=" + temporary.string();
    const pid_t process = fork();
    if (process < 0) throw UserError("could not start headless browser");
    if (process == 0) {
        const int file = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int errors = open("/dev/null", O_WRONLY);
        if (file < 0 || errors < 0) _exit(127);
        dup2(file, STDOUT_FILENO);
        dup2(errors, STDERR_FILENO);
        close(file);
        close(errors);
        execl(browser.c_str(), browser.c_str(), "--headless=new", "--dump-dom",
              "--timeout=45000", "--virtual-time-budget=45000", "--no-first-run",
              profile.c_str(), url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    bool finished = false;
    for (int i = 0; i < 650; ++i) {
        if (waitpid(process, &status, WNOHANG) == process) { finished = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!finished) { kill(process, SIGKILL); waitpid(process, &status, 0); }
    if (!finished || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw UserError("headless browser did not finish loading the World of Minesweeper game");
    }
#endif
    std::error_code error;
    if (!fs::is_regular_file(output, error) || fs::file_size(output, error) > 20'000'000) {
        throw UserError("headless browser returned no usable game DOM");
    }
    return read_file_text(output);
}

static Board parse_world_game_dom(const std::string& html) {
    static const std::regex tag_re(R"(<div\b[^>]*>)", std::regex::icase);
    static const std::regex id_re(R"re(\bid="cell_([0-9]+)_([0-9]+)")re");
    static const std::regex face_re(R"re(\bid="top_area_face")re");
    static const std::regex class_re(R"re(\bclass="([^"]*)")re");
    static const std::regex number_re(R"((?:hd|hdd)_type([0-8])(?:\s|$))");
    struct Cell { int x, y, number; bool mine; };
    std::vector<Cell> cells;
    int max_x = -1, max_y = -1;
    bool won = false;
    bool lost = false;
    for (auto it = std::sregex_iterator(html.begin(), html.end(), tag_re);
         it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::smatch id, classes;
        if (std::regex_search(tag, face_re) && std::regex_search(tag, classes, class_re)) {
            won = classes[1].str().find("top-area-face-win") != std::string::npos;
            lost = classes[1].str().find("top-area-face-lose") != std::string::npos;
        }
        if (!std::regex_search(tag, id, id_re)) continue;
        if (!std::regex_search(tag, classes, class_re)) {
            throw UserError("World of Minesweeper cell is missing its state");
        }
        const std::string style = classes[1];
        const bool opened = style.find("_opened") != std::string::npos;
        const bool closed = style.find("_closed") != std::string::npos;
        if (opened == closed) throw UserError("unrecognized World of Minesweeper cell state");
        std::smatch number_match;
        int number = -1;
        if (opened && std::regex_search(style, number_match, number_re)) {
            number = std::stoi(number_match[1]);
        }
        const int x = std::stoi(id[1]), y = std::stoi(id[2]);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        cells.push_back({x, y, number, closed});
    }
    if (!won) {
        throw UserError(lost ? "World of Minesweeper import needs a completed, won game" :
                        "World of Minesweeper board was not fully loaded by the browser");
    }
    if (cells.empty() || max_x > 1000 || max_y > 1000 ||
        static_cast<size_t>(max_x + 1) * (max_y + 1) != cells.size()) {
        throw UserError("World of Minesweeper board was not fully rendered");
    }
    Board board{max_y + 1, max_x + 1,
                std::vector<uint8_t>(static_cast<size_t>(max_x + 1) * (max_y + 1), 0)};
    std::vector<uint8_t> seen(board.mines.size(), 0);
    for (const Cell& cell : cells) {
        const size_t index = static_cast<size_t>(cell.y) * board.width + cell.x;
        if (seen[index]++) throw UserError("World of Minesweeper board repeats a cell");
        board.mines[index] = cell.mine;
    }
    for (const Cell& cell : cells) {
        if (cell.number < 0) continue;
        int adjacent = 0;
        for (int other : neighbors(cell.y * board.width + cell.x,
                                   board.height, board.width)) adjacent += board.mines[other];
        if (adjacent != cell.number) {
            throw UserError("World of Minesweeper displayed numbers disagree with mine positions");
        }
    }
    return board;
}

static Board load_world_game(const std::string& url) {
    return parse_world_game_dom(world_game_dom(url));
}

static Board load_board(const std::string& source) {
    if (is_world_game_url(trim(source))) return load_world_game(trim(source));
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
    throw UserError("input must be a PTTACG string, compatible LlamaSweeper URL, "
                    "World of Minesweeper game URL, MBF file, or quoted MBF hexadecimal");
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

static std::tuple<int, int, int> bulk_board_spec(const std::string& spec) {
    if (spec == "evil") return {20, 30, 130};
    if (spec == "beginner" || spec == "intermediate" || spec == "expert") {
        return standard_board_spec(spec);
    }
    std::smatch match;
    static const std::regex custom_size(R"(([0-9]+)[xX]([0-9]+)/([0-9]+))");
    if (!std::regex_match(spec, match, custom_size)) {
        throw UserError("--bulk must be beginner, intermediate, expert, evil, or NxM/K "
                        "(width x height / mines)");
    }
    uint64_t width, height, mine_count;
    try {
        width = std::stoull(match[1].str());
        height = std::stoull(match[2].str());
        mine_count = std::stoull(match[3].str());
    } catch (const std::exception&) {
        throw UserError("--bulk board dimensions or mine count are too large");
    }
    const auto max_cells = static_cast<uint64_t>(std::numeric_limits<int>::max());
    if (width == 0 || height == 0) {
        throw UserError("--bulk board width and height must be positive");
    }
    // Cell indices and counts throughout the solver use int.
    if (width > max_cells || height > max_cells || width * height > max_cells) {
        throw UserError("--bulk board has too many cells for integer cell indices");
    }
    if (mine_count > width * height) {
        throw UserError("--bulk mine count cannot exceed width * height");
    }
    // PTTACG lists width first; Board and the generator take height first.
    return {static_cast<int>(height), static_cast<int>(width), static_cast<int>(mine_count)};
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

static Board generate_random_board(const std::tuple<int, int, int>& spec,
                                   std::mt19937_64& rng) {
    const auto [height, width, mine_count] = spec;
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
                                     std::mt19937_64& rng) {
    return generate_random_board(standard_board_spec(difficulty), rng);
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
    Model result;
    result.height = model.height;
    result.width = model.width;
    const int q = static_cast<int>(order.size());
    std::vector<int> inverse(q);
    result.candidates.clear();
    result.candidates.reserve(q);
    for (int next = 0; next < q; ++next) {
        inverse[order[next]] = next;
        result.candidates.push_back(model.candidates[order[next]]);
    }
    // Only the original model needs cell-to-candidate lookup or board arrays.
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

    // The reduced model only feeds ordering and the DP. Replay and board
    // validation use the untouched original model, so copying those arrays
    // (and then replacing most scopes) on every deletion is wasted work.
    Model result;
    result.height = model.height;
    result.width = model.width;
    result.candidates.reserve(retained.size());
    for (int old : retained) result.candidates.push_back(model.candidates[old]);
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
    // Each opening induces a clique. Union its candidate mask into each
    // member's neighbors instead of connecting every pair separately.
    std::vector<uint64_t> opening_members(candidate_words, 0);
    for (const auto& scope : model.zero_scopes) {
        std::fill(opening_members.begin(), opening_members.end(), 0);
        for (int candidate : scope)
            opening_members[candidate / 64] |= uint64_t{1} << (candidate % 64);
        for (int candidate : scope) {
            for (size_t word = 0; word < candidate_words; ++word)
                relations.propagation[candidate][word] |= opening_members[word];
        }
    }
    for (int candidate = 0; candidate < q; ++candidate)
        relations.propagation[candidate][candidate / 64] &=
            ~(uint64_t{1} << (candidate % 64));
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
                                           const std::vector<int>& private_mine_counts,
                                           std::vector<int>& adjacent,
                                           std::vector<int>& base_ids) {
    adjacent.clear();
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

    base_ids.clear();
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
        best = std::max(best, chosen - population_count(covered));
        if (best > threshold) {
            exceeds_threshold = true;
            return;
        }
        const int optimistic = chosen + population_count(available) -
                               population_count(covered);
        if (optimistic <= best || !available) return;

        int pivot = __builtin_ctzll(available);
        int pivot_degree = -1;
        uint64_t scan = available;
        while (scan) {
            const int vertex = __builtin_ctzll(scan);
            scan &= scan - 1;
            const int current_degree =
                population_count(local_edges[vertex] & available);
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
            std::vector<int> adjacent;
            std::vector<int> base_ids;
            for (const auto& scope : result.model.mine_scopes) {
                if (scope.size() == 1) ++private_mine_counts[scope.front()];
            }
            int removed = 0;
            for (int candidate = 0; candidate < q; ++candidate) {
                if (left_click_dominated_candidate(
                        relations, candidate, private_mine_counts, adjacent, base_ids)) {
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

struct WidthEstimateSelection {
    std::vector<int> vertices;
    std::vector<const std::vector<int>*> openings, factors;
};

// Score a permutation through its inverse, reusing endpoint buffers across calls.
static WidthEstimate width_estimate_from_inverse(const Model& model,
                                    const std::vector<int>& inverse,
                                    int first_cut, int end_cut,
                                    std::vector<int>& graph_delta,
                                    std::vector<int>& factor_delta,
                                    const WidthEstimateSelection* selection = nullptr) {
    const int q = static_cast<int>(inverse.size());
    // Each interval contributes on [first, last).
    std::fill(graph_delta.begin(), graph_delta.end(), 0);
    std::fill(factor_delta.begin(), factor_delta.end(), 0);
    auto add_vertex = [&](int old) {
        const int position = inverse[old];
        int last = position;
        for (int other : model.graph[old]) {
            if (inverse[other] > position) last = std::max(last, inverse[other]);
        }
        if (last > position) { ++graph_delta[position]; --graph_delta[last]; }
    };
    auto add_scope = [&](const std::vector<int>& scope, std::vector<int>& delta) {
        if (scope.empty()) return;
        int first = q;
        int last = -1;
        for (int item : scope) {
            first = std::min(first, inverse[item]);
            last = std::max(last, inverse[item]);
        }
        if (first < last) { ++delta[first]; --delta[last]; }
    };
    if (selection) {
        for (int old : selection->vertices) add_vertex(old);
        for (const auto* scope : selection->openings) add_scope(*scope, graph_delta);
        for (const auto* scope : selection->factors) add_scope(*scope, factor_delta);
    } else {
        for (int old = 0; old < q; ++old) add_vertex(old);
        for (const auto& scope : model.zero_scopes) add_scope(scope, graph_delta);
        for (const auto& scope : model.mine_scopes) add_scope(scope, factor_delta);
        for (const auto& scope : model.base_scopes) add_scope(scope, factor_delta);
    }
    int max_graph = 0;
    int max_factors = 0;
    int max_total = 0;
    uint64_t work = 0;
    int graph = 0, factors = 0;
    for (int cut = 0; cut < end_cut; ++cut) {
        graph += graph_delta[cut];
        factors += factor_delta[cut];
        if (cut < first_cut) continue;
        max_graph = std::max(max_graph, graph);
        max_factors = std::max(max_factors, factors);
        max_total = std::max(max_total, graph + factors);
        work = saturated_add(work, estimated_cut_work(graph + factors));
    }
    return {max_total, max_graph, max_factors, work};
}

static WidthEstimate width_estimate(const Model& model, const std::vector<int>& order,
                                    int first_cut = 0, int end_cut = -1) {
    const int q = static_cast<int>(order.size());
    if (end_cut < 0) end_cut = q - 1;
    std::vector<int> inverse(q);
    for (int i = 0; i < q; ++i) inverse[order[i]] = i;
    std::vector<int> graph_delta(q + 1), factor_delta(q + 1);
    return width_estimate_from_inverse(model, inverse, first_cut, end_cut,
                                       graph_delta, factor_delta);
}

// For each bit, add the lower half of each block into its upper half.
// This visits exactly the masks containing that bit, in ascending order,
// without a conditional per mask. The contiguous inner loop can vectorize.
static void subset_zeta(std::vector<int>& counts) {
    const size_t size = counts.size(); // Callers supply a power-of-two size.
    for (size_t half = 1; half < size; half *= 2) {
        for (size_t block = 0; block < size; block += 2 * half) {
            for (size_t offset = 0; offset < half; ++offset) {
                counts[block + half + offset] += counts[block + offset];
            }
        }
    }
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
    // Scope and neighbor scans revisit each candidate for every sweep line.
    // Classify once, avoiding repeated coordinate division in those scans.
    std::vector<int> sweep_lines(q);
    for (int candidate = 0; candidate < q; ++candidate) {
        const int line = primary(candidate);
        sweep_lines[candidate] = reverse_lines ? line_count - 1 - line : line;
    }
    auto sweep_line = [&](int candidate) { return sweep_lines[candidate]; };

    // Only scopes whose first or last line is the current line need a
    // detailed scan. Interior crossings are always live; exterior scopes
    // cannot cross any cut within this line.
    using ScopeExtent = std::pair<int, int>;
    auto scope_extents = [&](const std::vector<std::vector<int>>& scopes) {
        std::vector<ScopeExtent> extents;
        extents.reserve(scopes.size());
        for (const auto& scope : scopes) {
            int first = line_count, last = -1;
            for (int candidate : scope) {
                const int line = sweep_line(candidate);
                first = std::min(first, line);
                last = std::max(last, line);
            }
            extents.emplace_back(first, last);
        }
        return extents;
    };
    const auto zero_extents = scope_extents(model.zero_scopes);
    const auto mine_extents = scope_extents(model.mine_scopes);
    const auto base_extents = scope_extents(model.base_scopes);

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
                                   const std::vector<ScopeExtent>& first_extents,
                                   const std::vector<std::vector<int>>* second = nullptr,
                                   const std::vector<ScopeExtent>* second_extents = nullptr) {
            std::vector<int> before_only(state_count, 0);
            std::vector<int> after_only(state_count, 0);
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
                    ++before_only[mask];
                    ++after_only[mask];
                    ++current_total;
                }
            };
            auto add_scopes = [&](const std::vector<std::vector<int>>& scopes,
                                  const std::vector<ScopeExtent>& extents) {
                for (size_t i = 0; i < scopes.size(); ++i) {
                    const auto [begin, end] = extents[i];
                    if (begin > line_number || end < line_number) continue;
                    if (begin < line_number && end > line_number) ++always;
                    else add_scope(scopes[i]);
                }
            };
            add_scopes(first, first_extents);
            if (second) add_scopes(*second, *second_extents);
            subset_zeta(before_only);
            subset_zeta(after_only);
            // A current-line scope contributes to both subtracted subset
            // sums. Linearity lets us fold it into the before/after arrays
            // before transforming, saving a third transform and its buffer.
            std::vector<int> answer(state_count, always);
            for (uint32_t mask = 0; mask < state_count; ++mask) {
                answer[mask] += before_total - before_only[mask];
                answer[mask] += after_total - after_only[all ^ mask];
                answer[mask] += current_total;
            }
            return answer;
        };

        std::vector<int> graph_cost = crossing_counts(model.zero_scopes, zero_extents);
        std::vector<int> factor_cost = crossing_counts(model.mine_scopes, mine_extents,
                                                      &model.base_scopes, &base_extents);

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
        // A selected current-line vertex closes exactly when it has no later
        // neighbor and its same-line neighbors have all been selected. Add
        // its closed-neighborhood mask to the old-vertex histogram: one subset
        // transform then counts both closed old and closed current vertices.
        for (int bit = 0; bit < p; ++bit) {
            uint32_t closed_mask = uint32_t{1} << bit;
            bool has_later_neighbor = false;
            for (int neighbor : model.graph[line[bit]]) {
                const int neighbor_line = sweep_line(neighbor);
                if (neighbor_line > line_number) has_later_neighbor = true;
                else if (neighbor_line == line_number) {
                    closed_mask |= uint32_t{1} << local_bit[neighbor];
                }
            }
            if (!has_later_neighbor) ++old_masks[closed_mask];
        }
        subset_zeta(old_masks);
        for (uint32_t mask = 0; mask < state_count; ++mask) {
            graph_cost[mask] += old_always + old_total + __builtin_popcount(mask) -
                                old_masks[mask];
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
            // Every predecessor pays the same cut cost. Keep the winning
            // score local and publish it once, after considering every bit.
            const int graph = graph_cost[mask];
            const int factors = factor_cost[mask];
            const int total = graph + factors;
            const uint64_t cut_work = estimated_cut_work(total);
            PathScore best_score;
            int8_t best_bit = -1;
            uint32_t choices = mask;
            while (choices) {
                const int bit = __builtin_ctz(choices);
                choices &= choices - 1;
                const uint32_t previous = mask ^ (uint32_t{1} << bit);
                PathScore score = best[previous];
                score.max_total = std::max(score.max_total, total);
                score.max_graph = std::max(score.max_graph, graph);
                score.max_factors = std::max(score.max_factors, factors);
                score.work = saturated_add(score.work, cut_work);
                if (best_bit < 0 || less_score(score, best_score)) {
                    best_score = score;
                    best_bit = static_cast<int8_t>(bit);
                }
            }
            best[mask] = best_score;
            predecessor[mask] = best_bit;
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
    // Positions outside the current band stay in plain sweep order.
    std::vector<int> plain_inverse(q), inverse(q);
    for (int line = 0; line < lines; ++line) {
        int position = prefix[line];
        for (int candidate : by_line[line]) plain_inverse[candidate] = position++;
    }
    std::vector<int> graph_delta(q + 1), factor_delta(q + 1);
    // Only contributions intersecting this start's largest band can affect
    // any of its cut scores. Extents use sweep lines, independent of the
    // changing order inside a band; nonlocal scopes remain fully supported.
    auto sweep_line = [&](int candidate) {
        const int cell = model.candidates[candidate];
        const int physical = columns ? cell % model.width : cell / model.width;
        return reverse ? lines - 1 - physical : physical;
    };
    std::vector<std::pair<int, int>> vertex_extents(q);
    for (int candidate = 0; candidate < q; ++candidate) {
        const int first = sweep_line(candidate);
        int last = first;
        for (int other : model.graph[candidate]) last = std::max(last, sweep_line(other));
        vertex_extents[candidate] = {first, last};
    }
    struct ScopeExtent {
        const std::vector<int>* scope;
        int first, last;
    };
    auto scope_extents = [&](const std::vector<std::vector<int>>& scopes) {
        std::vector<ScopeExtent> result;
        result.reserve(scopes.size());
        for (const auto& scope : scopes) {
            int first = lines, last = -1;
            for (int item : scope) {
                const int line = sweep_line(item);
                first = std::min(first, line);
                last = std::max(last, line);
            }
            if (last >= 0) result.push_back({&scope, first, last});
        }
        return result;
    };
    const auto openings = scope_extents(model.zero_scopes);
    auto factors = scope_extents(model.mine_scopes);
    const auto bases = scope_extents(model.base_scopes);
    factors.insert(factors.end(), bases.begin(), bases.end());
    WidthEstimateSelection selection;
    std::vector<std::vector<Edge>> edge(lines);
    for (int start = 0; start < lines; ++start) {
        const int limit = std::min(lines, start + max_band);
        selection.vertices.clear();
        selection.openings.clear();
        selection.factors.clear();
        for (int candidate = 0; candidate < q; ++candidate) {
            const auto [first, last] = vertex_extents[candidate];
            if (first < limit && last >= start) selection.vertices.push_back(candidate);
        }
        for (const auto& extent : openings)
            if (extent.first < limit && extent.last >= start)
                selection.openings.push_back(extent.scope);
        for (const auto& extent : factors)
            if (extent.first < limit && extent.last >= start)
                selection.factors.push_back(extent.scope);
        inverse = plain_inverse;
        for (int end = start + 1; end <= std::min(lines, start + max_band); ++end) {
            auto& current = edge[start].emplace_back();
            const auto& added = by_line[end - 1];
            if (end == start + 1) current.inside = added;
            else {
                const auto& previous_inside = edge[start][end - start - 2].inside;
                current.inside.reserve(previous_inside.size() + added.size());
                // Both inputs are secondary-sorted. Stable merging puts the
                // earlier sweep lines first on ties, including reverse sweeps,
                // exactly matching the full band's original sort order.
                std::merge(previous_inside.begin(), previous_inside.end(),
                           added.begin(), added.end(),
                           std::back_inserter(current.inside),
                           [&](int a, int b) { return secondary(a) < secondary(b); });
            }
            int position = prefix[start];
            for (int candidate : current.inside) inverse[candidate] = position++;
            current.estimate = width_estimate_from_inverse(model, inverse, prefix[start],
                std::min(prefix[end], q - 1), graph_delta, factor_delta, &selection);
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

struct OrderRequest {
    enum class Kind { Auto, Plain, Smart, Dynamic, FixedBand, ExactBands } kind;
    std::string orientation;
    bool reverse = false;
    int band_size = 1;
    std::vector<int> widths;
};

static OrderRequest parse_order_request(const std::string& name) {
    if (name.size() > 8 && name.compare(name.size() - 8, 8, "-windows") == 0) {
        const auto base = name.substr(0, name.size() - 8);
        if (base == "auto" || base.find("-windows") != std::string::npos)
            throw UserError("invalid --order value: " + name);
        return parse_order_request(base);
    }
    if (name == "auto") return {OrderRequest::Kind::Auto, "", false, 1, {}};
    OrderRequest result{OrderRequest::Kind::Plain, "", false, 1, {}};
    size_t offset;
    if (name.rfind("columns", 0) == 0) {
        result.orientation = "columns";
        offset = 7;
    } else if (name.rfind("rows", 0) == 0) {
        result.orientation = "rows";
        offset = 4;
    } else throw UserError("invalid --order value: " + name);
    if (name.compare(offset, 8, "-reverse") == 0) {
        result.reverse = true;
        offset += 8;
    }
    const std::string suffix = name.substr(offset);
    if (suffix.empty()) return result;
    if (suffix == "-smart") {
        result.kind = OrderRequest::Kind::Smart;
        return result;
    }
    if (suffix == "-dynamic") {
        result.kind = OrderRequest::Kind::Dynamic;
        return result;
    }
    auto positive_number = [&](const std::string& value) {
        if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                return ch >= '0' && ch <= '9';
            })) throw UserError("invalid --order value: " + name);
        size_t used = 0;
        unsigned long long number;
        try { number = std::stoull(value, &used); }
        catch (...) { throw UserError("invalid --order value: " + name); }
        if (used != value.size() || number == 0 ||
            number > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
            throw UserError("invalid --order value: " + name);
        }
        return static_cast<int>(number);
    };
    if (suffix.rfind("-band-", 0) == 0) {
        result.kind = OrderRequest::Kind::FixedBand;
        result.band_size = positive_number(suffix.substr(6));
        return result;
    }
    if (suffix.rfind("-dynamic-", 0) == 0) {
        result.kind = OrderRequest::Kind::ExactBands;
        std::istringstream parts(suffix.substr(9));
        std::string width;
        while (std::getline(parts, width, '.')) {
            result.widths.push_back(positive_number(width));
        }
        if (result.widths.empty() || suffix.back() == '.') {
            throw UserError("invalid --order value: " + name);
        }
        return result;
    }
    throw UserError("invalid --order value: " + name);
}

static std::vector<int> explicit_band_order_indices(const Model& model,
                                                     const OrderRequest& request) {
    const bool columns = request.orientation == "columns";
    const int line_count = columns ? model.width : model.height;
    std::vector<int> group(line_count), position(line_count);
    int line = 0;
    for (size_t band = 0; band < request.widths.size(); ++band) {
        if (request.widths[band] > line_count - line) {
            throw UserError("--order dynamic band widths must sum to " +
                            std::to_string(line_count));
        }
        for (int within = 0; within < request.widths[band]; ++within, ++line) {
            group[line] = static_cast<int>(band);
            position[line] = within;
        }
    }
    if (line != line_count) {
        throw UserError("--order dynamic band widths must sum to " +
                        std::to_string(line_count));
    }
    std::vector<int> order(model.candidates.size());
    std::iota(order.begin(), order.end(), 0);
    auto key = [&](int candidate) {
        const int cell = model.candidates[candidate];
        const int r = cell / model.width, c = cell % model.width;
        const int physical = columns ? c : r;
        const int sweep = request.reverse ? line_count - 1 - physical : physical;
        return std::tuple<int, int, int>{group[sweep], columns ? r : c, position[sweep]};
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    return order;
}

// Refine a spatial sweep in bounded windows. Every permutation is a valid exact
// DP order. The guard limits planning work only, never the solution search.
static std::pair<int, double> window_order_score(
        const Model& model, const std::vector<int>& order) {
    const int q = static_cast<int>(order.size());
    std::vector<int> inverse(q), graph_delta(q + 1), factor_delta(q + 1);
    for (int i = 0; i < q; ++i) inverse[order[i]] = i;
    width_estimate_from_inverse(model, inverse, 0, q, graph_delta, factor_delta);
    int graph = 0, factors = 0, peak = 0;
    double work = 0;
    for (int i = 0; i < q; ++i) {
        graph += graph_delta[i]; factors += factor_delta[i];
        const int width = 3 * graph + 2 * factors;
        peak = std::max(peak, width);
        work += std::exp2(width / 8.0);
    }
    return {peak, work};
}

static std::vector<int> refine_window_order(const Model& model,
                                          std::vector<int> order,
                                          int window = 16) {
    const int q = static_cast<int>(order.size());
    auto score = window_order_score(model, order);
    // Wider windows reduce frontier states on the fixed large boards; include
    // their extra planning cost when benchmarking changes to this bound.
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<int> inverse(q);
        for (int i = 0; i < q; ++i) inverse[order[i]] = i;
        struct Block { int begin, end; std::vector<int> width; };
        std::vector<Block> blocks;
        int global_peak = 0;
        for (int begin = 0; begin < q;) {
            const int end = std::min(q, begin +
                (begin == 0 && pass % 2 ? window / 2 : window));
            const int size = 1 << (end - begin), full = size - 1;
            std::vector<int> direct(size), complement(size);
            int constant = 0;
            auto classify = [&](const std::vector<int>& scope) {
                int mask = 0; bool before = false, after = false;
                for (int v : scope) {
                    const int p = inverse[v];
                    if (p < begin) before = true;
                    else if (p >= end) after = true;
                    else mask |= 1 << (p - begin);
                }
                return std::tuple<int, bool, bool>{mask, before, after};
            };
            auto hyperedge = [&](const auto& scope, int weight) {
                const auto [mask, before, after] = classify(scope);
                if (!mask && !before && !after) return;
                constant += weight;
                if (!before) complement[mask] -= weight;
                if (!after) direct[mask] -= weight;
            };
            for (int v = 0; v < q; ++v) {
                if (inverse[v] >= end) continue;
                const auto [mask, before, after] = classify(model.graph[v]);
                (void)before;
                if (inverse[v] < begin) {
                    constant += 3;
                    if (!after) direct[mask] -= 3;
                } else {
                    const int self = 1 << (inverse[v] - begin);
                    direct[self] += 3;
                    if (!after) direct[mask | self] -= 3;
                }
            }
            for (const auto& scope : model.zero_scopes) hyperedge(scope, 3);
            for (const auto& scope : model.mine_scopes) hyperedge(scope, 2);
            for (const auto& scope : model.base_scopes) hyperedge(scope, 2);
            subset_zeta(direct); subset_zeta(complement);
            Block block{begin, end, std::vector<int>(size)};
            std::vector<int> peak(size, std::numeric_limits<int>::max());
            for (int mask = 0; mask < size; ++mask)
                block.width[mask] = constant + direct[mask] + complement[full ^ mask];
            peak[0] = block.width[0];
            for (int mask = 1; mask < size; ++mask) {
                for (unsigned bits = mask; bits; bits &= bits - 1) {
                    const unsigned bit = bits & -bits;
                    peak[mask] = std::min(peak[mask],
                        std::max(peak[mask ^ bit], block.width[mask]));
                }
            }
            global_peak = std::max(global_peak, peak.back());
            blocks.push_back(std::move(block));
            begin = end;
        }
        std::vector<double> increments(global_peak + 1);
        for (int w = 0; w <= global_peak; ++w) increments[w] = std::exp2(w / 8.0);
        std::vector<int> optimized;
        optimized.reserve(q);
        for (const auto& block : blocks) {
            const int size = static_cast<int>(block.width.size());
            std::vector<double> cost(size, std::numeric_limits<double>::infinity());
            std::vector<unsigned> parent(size);
            cost[0] = 0;
            for (int mask = 1; mask < size; ++mask) {
                if (block.width[mask] > global_peak) continue;
                const double increment = increments[block.width[mask]];
                for (unsigned bits = mask; bits; bits &= bits - 1) {
                    const unsigned bit = bits & -bits;
                    const double next = cost[mask ^ bit] + increment;
                    if (next < cost[mask]) { cost[mask] = next; parent[mask] = bit; }
                }
            }
            std::vector<int> reversed;
            for (unsigned mask = size - 1; mask;) {
                const unsigned bit = parent[mask];
                if (!bit) throw std::logic_error("window order reconstruction failed");
                reversed.push_back(order[block.begin + __builtin_ctz(bit)]);
                mask ^= bit;
            }
            optimized.insert(optimized.end(), reversed.rbegin(), reversed.rend());
        }
        const auto next_score = window_order_score(model, optimized);
        if (next_score < score) { order = std::move(optimized); score = next_score; }
    }
    return order;
}

// Budget subset planning against the same frontier-work estimate used to
// assess its result. This is a deterministic effort heuristic, not a time
// limit: explicit order names must reproduce their permutations.
static int automatic_window_size(const Model& model, const std::vector<int>& order) {
    const size_t q = order.size();
    if (q < 8) return 0;
    const double budget = window_order_score(model, order).second / 4;
    size_t scope_items = q;
    for (const auto& neighbors : model.graph) scope_items += neighbors.size();
    for (const auto* scopes : {&model.zero_scopes, &model.mine_scopes, &model.base_scopes})
        for (const auto& scope : *scopes) scope_items += scope.size();
    int chosen = 0;
    for (int window = 8; window <= 16 && size_t(window) <= q; ++window) {
        const double effort = 8.0 * q * (uint64_t{1} << window) +
            4.0 * ((q + window - 1) / window + 1) * scope_items;
        if (effort <= budget) chosen = window;
    }
    return chosen;
}

static std::pair<Model, std::string> choose_order_from_plan(const Model& model,
                                                   const std::string& requested,
                                                   std::optional<int> band_size) {
    if (requested.size() > 8 &&
        requested.compare(requested.size() - 8, 8, "-windows") == 0) {
        parse_order_request(requested);
        auto [base, name] = choose_order_from_plan(model, requested.substr(0, requested.size() - 8), band_size);
        std::vector<int> identity(base.candidates.size());
        std::iota(identity.begin(), identity.end(), 0);
        const int window = automatic_window_size(base, identity);
        if (window) identity = refine_window_order(base, std::move(identity), window);
        return {ordered_model(base, identity), name + "-windows"};
    }
    if (requested != "auto") {
        const OrderRequest order = parse_order_request(requested);
        if (band_size && order.kind != OrderRequest::Kind::Plain) {
            throw UserError("--band-size requires a plain row or column --order (or auto)");
        }
        if (order.kind == OrderRequest::Kind::Smart) {
            auto indices = smart_line_order_indices(model, order.orientation, order.reverse);
            return {ordered_model(model, indices), requested};
        }
        if (order.kind == OrderRequest::Kind::Dynamic) {
            auto dynamic = dynamic_band_order(model, order.orientation, order.reverse);
            std::string label = requested + '-';
            for (size_t i = 0; i < dynamic.widths.size(); ++i) {
                if (i) label += '.';
                label += std::to_string(dynamic.widths[i]);
            }
            return {ordered_model(model, dynamic.order), label};
        }
        if (order.kind == OrderRequest::Kind::ExactBands) {
            return {ordered_model(model, explicit_band_order_indices(model, order)), requested};
        }
        const int size = order.kind == OrderRequest::Kind::FixedBand
            ? order.band_size : band_size.value_or(1);
        auto indices = order_indices(model, order.orientation, size, order.reverse);
        const std::string name = order.kind == OrderRequest::Kind::FixedBand || size == 1
            ? requested : requested + "-band-" + std::to_string(size);
        return {ordered_model(model, indices), name};
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
    // Connectivity signatures grow substantially faster than factor-hit masks.
    // A one-item increase in total width can therefore be a worthwhile trade
    // for a narrower connectivity frontier; use both widths to rank sweeps.
    auto best = std::min_element(choices.begin(), choices.end(), [](const auto& a, const auto& b) {
        return std::tuple<int, int, uint64_t, std::string>{
                   std::get<0>(a.estimate) + std::get<1>(a.estimate),
                   std::get<0>(a.estimate),
                   std::get<3>(a.estimate), a.name} <
               std::tuple<int, int, uint64_t, std::string>{
                   std::get<0>(b.estimate) + std::get<1>(b.estimate),
                   std::get<0>(b.estimate),
                   std::get<3>(b.estimate), b.name};
    });
    const int window = band_size ? 0 : automatic_window_size(model, best->order);
    if (window) {
        auto refined = refine_window_order(model, best->order, window);
        const bool changed = refined != best->order;
        return {ordered_model(model, refined), best->name + (changed ? "-windows" : "")};
    }
    return {ordered_model(model, best->order), best->name};
}

static std::pair<Model, std::string> choose_order(const Model& model,
                                                const std::string& requested,
                                                std::optional<int> band_size) {
    // Planning counts zero regions as connectivity, and only numbered 3BV
    // targets as coverage factors, matching the mandatory-zero DP. Keep the
    // physical objective intact in the model returned to the solver.
    Model planning_model = model;
    for (size_t z = 0; z < planning_model.zero_scopes.size(); ++z)
        planning_model.base_scopes[z].clear();
    auto [planned, name] = choose_order_from_plan(planning_model, requested, band_size);
    std::vector<int> candidate_index(model.width * model.height, -1);
    for (int i = 0; i < static_cast<int>(model.candidates.size()); ++i)
        candidate_index[model.candidates[i]] = i;
    std::vector<int> order;
    order.reserve(planned.candidates.size());
    for (int cell : planned.candidates) order.push_back(candidate_index[cell]);
    return {ordered_model(model, order), name};
}

// Six words cover chosen-chord histories on standard boards and most 30x20
// boards. Keeping them inline avoids millions of allocations in replay mode.
class Bits {
public:
    static constexpr size_t INLINE_WORDS = 6;

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
    Bits(Bits&& other) noexcept
        : size_(std::exchange(other.size_, 0)), inline_(other.inline_),
          heap_(std::move(other.heap_)) {}
    Bits& operator=(Bits&& other) noexcept {
        if (this == &other) return *this;
        size_ = std::exchange(other.size_, 0);
        inline_ = other.inline_;
        heap_ = std::move(other.heap_);
        return *this;
    }

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
    for (size_t i = 0; i < bits.size(); ++i) count += population_count(bits[i]);
    return count;
}

static int bit_count_new_and(const Bits& member, const Bits& old, const Bits& filter) {
    int count = 0;
    for (size_t i = 0; i < member.size(); ++i) {
        count += population_count(member[i] & ~old[i] & filter[i]);
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
    static int count(uint64_t bits) { return population_count(bits); }
    static int count_new_and(uint64_t member, uint64_t old, uint64_t filter) {
        return population_count(member & ~old & filter);
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
        penalty += population_count(other_good & ~candidate_good);
        if (penalty > allowance) return false;
    }
    return true;
}

static bool dominance_penalty_at_most(uint64_t candidate, uint64_t other,
                                      uint64_t base_mask, int allowance) {
    const uint64_t candidate_good = candidate ^ base_mask;
    const uint64_t other_good = other ^ base_mask;
    return population_count(other_good & ~candidate_good) <= allowance;
}

static std::pair<int, int> factor_hit_counts(const Bits& hits, const Bits& base_mask) {
    int base_hits = 0;
    int total_hits = 0;
    for (size_t word = 0; word < hits.size(); ++word) {
        base_hits += population_count(hits[word] & base_mask[word]);
        total_hits += population_count(hits[word]);
    }
    return {base_hits, total_hits};
}

static std::pair<int, int> factor_hit_counts(uint64_t hits, uint64_t base_mask) {
    return {population_count(hits & base_mask), population_count(hits)};
}

static size_t hash_combine(size_t seed, uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

// Non-owning access to an interned signature. Views are temporary: never keep
// one across a mutating operation on its pool, or beyond the pool's lifetime.
class SignatureView {
public:
    SignatureView(const uint64_t* data, size_t count) : data_(data), size_(count) {}
    SignatureView(const std::vector<uint64_t>& words)
        : data_(words.empty() ? &empty_word : words.data()), size_(words.size()) {}
    const uint64_t* begin() const { return data_; }
    const uint64_t* end() const { return data_ + size_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint64_t operator[](size_t index) const { return data_[index]; }
    friend bool operator==(SignatureView a, SignatureView b) {
        return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
    }

private:
    inline static constexpr uint64_t empty_word = 0;
    const uint64_t* data_;
    size_t size_;
};

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
    ConnectivityPool() { reset(); }

    uint32_t intern(const std::vector<uint64_t>& signature) {
        const size_t hash = WordsHash{}(signature);
        size_t slot = hash & (slots_.size() - 1);
        while (slots_[slot] != empty_slot) {
            const auto& entry = entries_[slots_[slot]];
            if (entry.hash == hash && entry.length == signature.size() &&
                std::equal(signature.begin(), signature.end(), words_.begin() + entry.offset))
                return slots_[slot];
            slot = (slot + 1) & (slots_.size() - 1);
        }
        // Store offsets rather than pointers so arena growth preserves IDs.
        // The input is an external vector, never a view into this arena.
        if (entries_.size() >= empty_slot)
            throw std::length_error("too many connectivity signatures");
        if (entries_.size() + 1 > slots_.size() / 2) {
            rehash(capacity_for(entries_.size() + 1));
            slot = hash & (slots_.size() - 1);
            while (slots_[slot] != empty_slot) slot = (slot + 1) & (slots_.size() - 1);
        }
        const uint32_t id = static_cast<uint32_t>(entries_.size());
        const size_t offset = words_.size();
        words_.insert(words_.end(), signature.begin(), signature.end());
        entries_.push_back(Entry{offset, signature.size(), hash});
        slots_[slot] = id;
        return id;
    }

    SignatureView operator[](uint32_t id) const {
        const auto& entry = entries_[id];
        return SignatureView(words_.data() + entry.offset, entry.length);
    }
    size_t size() const { return entries_.size(); }
    void reserve(size_t count) {
        const size_t capacity = capacity_for(count);
        if (capacity > slots_.size()) rehash(capacity);
        entries_.reserve(count);
    }
    void reset(size_t reserve_count = 0) {
        entries_.clear();
        entries_.reserve(reserve_count);
        words_.clear();
        // Keep empty-signature views non-null, including before the first
        // nonempty signature is inserted.
        words_.push_back(0);
        slots_.assign(capacity_for(reserve_count), empty_slot);
        intern(std::vector<uint64_t>{});
    }
    void swap(ConnectivityPool& other) noexcept {
        slots_.swap(other.slots_);
        words_.swap(other.words_);
        entries_.swap(other.entries_);
    }

private:
    struct Entry {
        size_t offset;
        size_t length;
        size_t hash;
    };
    static constexpr uint32_t empty_slot = std::numeric_limits<uint32_t>::max();
    // Contiguous metadata and word storage replace per-signature allocations.
    std::vector<uint32_t> slots_;
    std::vector<Entry> entries_;
    std::vector<uint64_t> words_;

    static size_t capacity_for(size_t count) {
        size_t capacity = 16;
        while (capacity / 2 < count) {
            if (capacity > std::numeric_limits<size_t>::max() / 2)
                throw std::length_error("connectivity pool capacity overflow");
            capacity *= 2;
        }
        return capacity;
    }
    void rehash(size_t capacity) {
        std::vector<uint32_t> slots(capacity, empty_slot);
        for (uint32_t id = 0; id < entries_.size(); ++id) {
            size_t slot = entries_[id].hash & (capacity - 1);
            while (slots[slot] != empty_slot) slot = (slot + 1) & (capacity - 1);
            slots[slot] = id;
        }
        slots_.swap(slots);
    }
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

    // Completed layers only erase and iterate. Dense entry positions remain
    // stable, so pruning no longer needs the duplicate-lookup index.
    void release_lookup_index() {
        std::vector<uint32_t>().swap(slots_);
        used_slots_ = 0;
        deleted_slots_ = 0;
    }

    // Reuse dense layer capacity, but destroy records now so wide factor and
    // chosen-history allocations do not survive into the dominance pass.
    // Rebuild the index with the next layer's normal reserve calculation.
    void clear_for_reuse() {
        entries_.clear();
        alive_.clear();
        live_size_ = 0;
        release_lookup_index();
    }

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

    // state_hash must equal StateHash{}(state); transitions cache its
    // connectivity prefix, shared by all states with that connectivity.
    // A transition needs to create a record only when its state is new. Probe
    // once, then construct the record lazily in the vacant slot; replay-mode
    // chosen histories can be large enough that eager copying is expensive.
    template <typename MakeRecord>
    std::pair<iterator, bool> find_or_emplace(State state, size_t state_hash,
                                           MakeRecord&& make_record) {
        if (slots_.empty()) reserve(live_size_ + 1);
        ensure_insert_capacity();
        const size_t mask = slots_.size() - 1;
        size_t slot = state_hash & mask;
        size_t first_deleted = std::numeric_limits<size_t>::max();
        for (;;) {
            const uint32_t entry = slots_[slot];
            if (entry == EMPTY) {
                if (first_deleted != std::numeric_limits<size_t>::max()) {
                    slot = first_deleted;
                    --deleted_slots_;
                } else {
                    ++used_slots_;
                }
                const size_t index = entries_.size();
                entries_.emplace_back(std::move(state), make_record());
                alive_.push_back(1);
                slots_[slot] = static_cast<uint32_t>(index);
                ++live_size_;
                return {iterator(this, index), true};
            }
            if (entry == DELETED) {
                if (first_deleted == std::numeric_limits<size_t>::max()) {
                    first_deleted = slot;
                }
            } else if (alive_[entry] && entries_[entry].first == state) {
                return {iterator(this, entry), false};
            }
            slot = (slot + 1) & mask;
        }
    }

    // Pruning holds stable entry pointers: it never inserts into this table.
    void erase(value_type* where) {
        const size_t index = static_cast<size_t>(where - entries_.data());
        if (index >= entries_.size() || !alive_[index]) return;
        erase(iterator(this, index));
    }

    void erase(iterator where) {
        const size_t entry_index = where.index_;
        if (entry_index >= entries_.size() || !alive_[entry_index]) return;
        if (!slots_.empty()) {
            const size_t mask = slots_.size() - 1;
            size_t slot = StateHash{}(entries_[entry_index].first) & mask;
            while (slots_[slot] != EMPTY) {
                if (slots_[slot] == entry_index) {
                    slots_[slot] = DELETED;
                    ++deleted_slots_;
                    break;
                }
                slot = (slot + 1) & mask;
            }
            if (slots_[slot] == EMPTY) {
                throw std::logic_error("flat DP table lost an indexed state");
            }
        }
        alive_[entry_index] = 0;
        --live_size_;
        // Inline words occupy the dense slot regardless, but release any
        // wide-board overflow allocations immediately.
        release_factor_storage(entries_[entry_index].first.hits);
        if constexpr (TrackChosen) {
            if (entries_[entry_index].second.chosen.size() > Bits::INLINE_WORDS) {
                entries_[entry_index].second.chosen = Bits{};
            }
        }
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
            reserve(live_size_ + 1);
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
    size_t hash_seed = 0;
};

static uint64_t dominance_primary_key(int cost, int base_hits) {
    // Pack the signed primary fields in their original lexicographic order.
    return (uint64_t{static_cast<uint32_t>(cost + base_hits) ^ UINT32_C(0x80000000)} << 32) |
           (static_cast<uint32_t>(cost) ^ UINT32_C(0x80000000));
}

// A coarser signature can represent several finer components as one, provided
// every finer component's future reachability is contained in its representative.
// Requiring every coarse component to represent at least one fine component
// prevents an unmatched component from adding a future seed-click liability.
template <size_t FixedWords>
static bool connectivity_coarsens_impl(SignatureView coarse,
                                  SignatureView fine,
                                  size_t runtime_words) {
    const size_t signature_words = FixedWords ? FixedWords : runtime_words;
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

// Standard frontiers fit in one word. A fixed word count lets the compiler
// remove word loops and index multiplications without changing the matching.
static bool connectivity_coarsens(SignatureView coarse,
                                  SignatureView fine,
                                  size_t signature_words) {
    if (signature_words == 1) return connectivity_coarsens_impl<1>(coarse, fine, 1);
    return connectivity_coarsens_impl<0>(coarse, fine, signature_words);
}

// For reverse dominance, require an exact quotient: each fine component has
// one unambiguous coarse parent, and every coarse reach is precisely the union
// of its assigned fine reaches. Merely being contained is insufficient: extra
// coarse reach can join two future components without a corresponding join on
// the fine side, exceeding the simple seed-count bound.
static bool connectivity_exact_quotient(SignatureView coarse,
                                        SignatureView fine,
                                        size_t signature_words) {
    const size_t coarse_count = coarse.size() / signature_words;
    const size_t fine_count = fine.size() / signature_words;
    if (coarse_count == 0 || coarse_count >= fine_count) return false;
    if (coarse_count == 1) return true; // The enclosing union buckets agree.
    if (signature_words == 1 && coarse_count <= 64) {
        std::array<uint64_t, 64> recovered{};
        for (uint64_t component : fine) {
            size_t parent = coarse_count;
            for (size_t c = 0; c < coarse_count; ++c) {
                if ((component & ~coarse[c]) != 0) continue;
                if (parent != coarse_count) return false;
                parent = c;
            }
            if (parent == coarse_count) return false;
            recovered[parent] |= component;
        }
        for (size_t c = 0; c < coarse_count; ++c) {
            if (recovered[c] != coarse[c]) return false;
        }
        return true;
    }
    std::vector<uint64_t> recovered(coarse.size(), 0);
    for (size_t f = 0; f < fine_count; ++f) {
        size_t parent = coarse_count;
        for (size_t c = 0; c < coarse_count; ++c) {
            bool contained = true;
            for (size_t word = 0; word < signature_words; ++word) {
                if ((fine[f * signature_words + word] &
                     ~coarse[c * signature_words + word]) != 0) {
                    contained = false;
                    break;
                }
            }
            if (!contained) continue;
            if (parent != coarse_count) return false;
            parent = c;
        }
        if (parent == coarse_count) return false;
        for (size_t word = 0; word < signature_words; ++word) {
            recovered[parent * signature_words + word] |=
                fine[f * signature_words + word];
        }
    }
    return recovered == coarse;
}

// Both signatures have the same union of future reaches. Merge components
// along their bipartite overlap graph to obtain a common coarsening J.
// A needs at most count(A)-count(J) more future seeds than B: merging that
// many components takes A to J, and J can only improve on B.
template <size_t FixedWords>
static int connectivity_merge_penalty_impl(SignatureView a,
                                      SignatureView b,
                                      size_t runtime_words, int maximum_penalty) {
    const size_t words = FixedWords ? FixedWords : runtime_words;
    const int na = static_cast<int>(a.size() / words);
    const int nb = static_cast<int>(b.size() / words);
    if (na + nb > 128) return std::numeric_limits<int>::max();
    // Eliminate A vertices from the bipartite graph: each nonisolated A
    // vertex joins all its B neighbors. Isolated A vertices remain separate
    // groups. This preserves connected components with fewer root lookups.
    std::array<int, 128> parent;
    std::iota(parent.begin(), parent.begin() + nb, 0);
    auto root = [&](int v) {
        while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
        return v;
    };
    int groups = nb;
    // Equal reach unions and nonempty components exclude isolated A vertices.
    // Each union increases the final penalty's lower bound by exactly one.
    if (na - groups > maximum_penalty) return std::numeric_limits<int>::max();
    for (int x = 0; x < na; ++x) {
        int anchor = -1;
        for (int y = 0; y < nb; ++y) {
            bool overlap = false;
            for (size_t w = 0; w < words; ++w) {
                if (a[x * words + w] & b[y * words + w]) { overlap = true; break; }
            }
            if (!overlap) continue;
            const int ry = root(y);
            if (anchor < 0) anchor = ry;
            else if (anchor != ry) {
                parent[ry] = anchor;
                --groups;
                if (na - groups > maximum_penalty)
                    return std::numeric_limits<int>::max();
                // The caller groups signatures by equal reach union. Every
                // remaining nonempty A component therefore touches B. Once
                // all B components are connected, none can add another group.
                if (groups == 1) return na - 1;
            }
        }
        if (anchor < 0) ++groups;
    }
    return na - groups;
}

static int connectivity_merge_penalty(SignatureView a,
                                      SignatureView b,
                                      size_t signature_words,
                                      int maximum_penalty = std::numeric_limits<int>::max()) {
    if (signature_words == 1)
        return connectivity_merge_penalty_impl<1>(a, b, 1, maximum_penalty);
    return connectivity_merge_penalty_impl<0>(a, b, signature_words, maximum_penalty);
}

template <typename FactorBits, bool TrackChosen>
static size_t prune_dominated(FactorTable<FactorBits, TrackChosen>& table,
                              const FactorBits& base_mask,
                              const ConnectivityPool& connectivity_pool,
                              size_t signature_words,
                              ProfileStats* profile = nullptr) {
    if (table.size() < 2) return 0;
    const auto profile_start = profile ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    using Item = typename FactorTable<FactorBits, TrackChosen>::value_type*;
    // Entries cannot move during pruning. Keep one direct pointer and derive
    // the final sort field from the hit counts instead of storing it again.
    // Scalar factor states have at most 64 hits. Narrow only their counts,
    // placing them after the score so scalar records fit in 24 bytes on
    // 64-bit builds. Wide factor states retain full integer counts.
    using HitCount = std::conditional_t<std::is_same_v<FactorBits, uint64_t>, uint8_t, int>;
    struct CachedItem {
        Item item;
        uint64_t key;
        int quasi_score;
        HitCount base_hits;
        HitCount flag_hits;
        bool exact_dominated;
        bool cross_dominated;

        bool key_before(const CachedItem& other) const {
            if (key != other.key) return key < other.key;
            // The last key field is -total_hits, already available here.
            return base_hits + flag_hits > other.base_hits + other.flag_hits;
        }
    };
    struct CachedList {
        CachedItem* data = nullptr;
        size_t count = 0;
        CachedItem* begin() { return data; }
        CachedItem* end() { return data + count; }
        void push_back(CachedItem item) { data[count++] = item; }
    };
    struct IdList {
        uint32_t* data = nullptr;
        size_t count = 0;
        uint32_t* begin() const { return data; }
        uint32_t* end() const { return data + count; }
        size_t size() const { return count; }
    };
    struct Bucket {
        IdList connectivity_ids;
        std::conditional_t<TrackChosen, std::vector<CachedItem>, CachedList> entries;
        size_t state_count = 0;
        // If two future boundary positions occur together in a fine component,
        // a coarsening must contain them together in some component too.
        // Pair keys are (a << 6) | b for boundary bits a < b < 64.
        // Allocate the direct index only for buckets that use postings;
        // keep the posting vectors dense rather than constructing 4096 of them.
        std::vector<int> pair_index;
        std::vector<std::vector<uint32_t>> pair_postings;
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
    // Scalar unions need only a temporary lookup index. Store bucket IDs in
    // flat slots instead of allocating a hash node for every distinct union.
    // At most connectivity_count keys can be inserted; keep load at most 1/2.
    constexpr size_t no_bucket = std::numeric_limits<size_t>::max();
    std::vector<size_t> word_slots;
    std::vector<uint64_t> bucket_words;
    if (signature_words == 1) {
        size_t capacity = 8;
        while (capacity / 2 < connectivity_count) {
            if (capacity > std::numeric_limits<size_t>::max() / 2)
                throw std::length_error("dominance union index capacity overflow");
            capacity *= 2;
        }
        word_slots.assign(capacity, no_bucket);
        bucket_words.reserve(connectivity_count);
    } else bucket_by_union.reserve(connectivity_count);
    std::vector<Bucket> buckets;
    std::vector<size_t> connectivity_bucket(connectivity_count,
                                             std::numeric_limits<size_t>::max());
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (state_counts[id] == 0) continue;
        const auto& signature = connectivity_pool[id];
        size_t bucket;
        bool new_bucket;
        if (signature_words == 1) {
            // A scalar key represents the same union without allocating a
            // one-element vector for each signature. Assign bucket IDs in
            // signature order, just as on the multiword path.
            uint64_t combined = 0;
            for (uint64_t component : signature) combined |= component;
            const size_t mask = word_slots.size() - 1;
            size_t slot = hash_combine(0, combined) & mask;
            while (word_slots[slot] != no_bucket &&
                   bucket_words[word_slots[slot]] != combined) {
                slot = (slot + 1) & mask;
            }
            new_bucket = word_slots[slot] == no_bucket;
            if (new_bucket) {
                bucket = buckets.size();
                bucket_words.push_back(combined);
                word_slots[slot] = bucket;
            } else bucket = word_slots[slot];
        } else {
            std::vector<uint64_t> combined(signature_words, 0);
            for (size_t offset = 0; offset < signature.size(); offset += signature_words) {
                for (size_t word = 0; word < signature_words; ++word) {
                    combined[word] |= signature[offset + word];
                }
            }
            const auto result = bucket_by_union.emplace(std::move(combined), buckets.size());
            bucket = result.first->second;
            new_bucket = result.second;
        }
        if (new_bucket) buckets.emplace_back();
        connectivity_bucket[id] = bucket;
        ++buckets[bucket].connectivity_ids.count;
        buckets[bucket].state_count += state_counts[id];
    }
    // Assign disjoint slices before filling IDs in their original order.
    // No per-bucket growth or allocation is needed, and slices stay stable.
    std::vector<uint32_t> bucket_id_storage(connectivity_count);
    size_t bucket_id_offset = 0;
    for (auto& bucket : buckets) {
        bucket.connectivity_ids.data = bucket_id_storage.data() + bucket_id_offset;
        bucket_id_offset += bucket.connectivity_ids.count;
        bucket.connectivity_ids.count = 0;
    }
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (state_counts[id] == 0) continue;
        auto& ids = buckets[connectivity_bucket[id]].connectivity_ids;
        ids.data[ids.count++] = id;
    }
    if (signature_words == 1) {
        for (auto& bucket : buckets) {
            if (bucket.connectivity_ids.size() < 64) continue;
            bucket.pair_index.assign(4096, -1);
            for (uint32_t id : bucket.connectivity_ids) {
                for (uint64_t component : connectivity_pool[id]) {
                    for (uint64_t outer = component; outer; outer &= outer - 1) {
                        const int a = __builtin_ctzll(outer);
                        uint64_t inner = outer & (outer - 1);
                        while (inner) {
                            const int b = __builtin_ctzll(inner);
                            inner &= inner - 1;
                            int& index = bucket.pair_index[(a << 6) | b];
                            if (index < 0) {
                                index = static_cast<int>(bucket.pair_postings.size());
                                bucket.pair_postings.emplace_back();
                            }
                            bucket.pair_postings[index].push_back(id);
                        }
                    }
                }
            }
            for (auto& posting : bucket.pair_postings) {
                auto& ids = posting;
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            }
        }
    }
    // Bucket sizes are known: count-only mode can use one uninitialized POD
    // array, with every slot assigned before sorting. Per-bucket vectors had
    // lower peak RSS in replay benchmarks with heap-backed chord histories,
    // so keep that allocation strategy when histories are present.
    std::unique_ptr<CachedItem[]> cached_storage;
    if constexpr (!TrackChosen) cached_storage.reset(new CachedItem[table.size()]);
    size_t cached_offset = 0;
    for (auto& bucket : buckets) {
        if constexpr (TrackChosen) {
            bucket.entries.reserve(bucket.state_count);
        } else {
            bucket.entries.data = cached_storage.get() + cached_offset;
            cached_offset += bucket.state_count;
        }
    }
    for (auto item = table.begin(); item != table.end(); ++item) {
        const size_t bucket = connectivity_bucket[item->first.connectivity_id];
        const auto [base_hits, total_hits] =
            factor_hit_counts(item->first.hits, base_mask);
        const int flag_hits = total_hits - base_hits;
        const uint64_t key = dominance_primary_key(item->second.cost, base_hits);
        const int quasi_score = item->second.cost + base_hits - flag_hits;
        buckets[bucket].entries.push_back(
            CachedItem{&*item, key, quasi_score, static_cast<HitCount>(base_hits),
                       static_cast<HitCount>(flag_hits), false, false});
    }
    for (auto& bucket : buckets) {
        std::sort(bucket.entries.begin(), bucket.entries.end(),
                  [](const CachedItem& a, const CachedItem& b) {
                      return a.key_before(b);
                  });
    }

    // Each signature can retain at most its incoming state count. Give its
    // survivor list a disjoint slice of one allocation instead of allocating
    // a separate vector for every signature on every dominance pass. Appends
    // preserve the same sorted witness order and pointers stay stable.
    struct SurvivorList {
        CachedItem** data = nullptr;
        size_t count = 0;
        bool empty() const { return count == 0; }
        CachedItem** begin() const { return data; }
        CachedItem** end() const { return data + count; }
        CachedItem* front() const { return data[0]; }
        CachedItem* back() const { return data[count - 1]; }
        void push_back(CachedItem* item) { data[count++] = item; }
    };
    std::vector<CachedItem*> survivor_storage(table.size());
    std::vector<SurvivorList> survivors(connectivity_count);
    std::vector<int> minimum_quasi(connectivity_count, std::numeric_limits<int>::max());
    std::vector<int> maximum_quasi(connectivity_count, std::numeric_limits<int>::min());
    std::vector<int> minimum_cost(connectivity_count, std::numeric_limits<int>::max());
    size_t survivor_offset = 0;
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        survivors[id].data = survivor_storage.data() + survivor_offset;
        survivor_offset += state_counts[id];
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
    const auto setup_end = profile ? std::chrono::steady_clock::now() : profile_start;
    for (auto& bucket : buckets) {
        for (CachedItem& cached : bucket.entries) {
            const uint32_t connectivity_id = cached.item->first.connectivity_id;
            auto& kept = survivors[connectivity_id];
            bool is_dominated = false;
            for (CachedItem* prior : kept) {
                if (dominates(prior, &cached)) {
                    is_dominated = true;
                    break;
                }
            }
            cached.exact_dominated = is_dominated;
            if (is_dominated && profile) ++profile->exact_pruned;
            if (!is_dominated) {
                kept.push_back(&cached);
                minimum_quasi[connectivity_id] =
                    std::min(minimum_quasi[connectivity_id], cached.quasi_score);
                maximum_quasi[connectivity_id] =
                    std::max(maximum_quasi[connectivity_id], cached.quasi_score);
                minimum_cost[connectivity_id] =
                    std::min(minimum_cost[connectivity_id], cached.item->second.cost);
            }
        }
    }
    const auto exact_end = profile ? std::chrono::steady_clock::now() : setup_end;

    std::vector<size_t> component_counts(connectivity_count, 0);
    // Prefix offsets give each live signature a disjoint slice. Build the
    // populations in one allocation rather than growing one vector per ID.
    std::vector<size_t> population_offsets(connectivity_count + 1, 0);
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (!survivors[id].empty()) {
            component_counts[id] = connectivity_pool[id].size() / signature_words;
        }
        population_offsets[id + 1] = population_offsets[id] + component_counts[id];
    }
    std::vector<int> component_populations(population_offsets.back());
    std::vector<int> minimum_component_population(connectivity_count,
                                                   std::numeric_limits<int>::max());
    std::vector<int> maximum_component_population(connectivity_count, 0);
    for (uint32_t id = 0; id < connectivity_count; ++id) {
        if (!survivors[id].empty()) {
            const auto& signature = connectivity_pool[id];
            size_t population_index = population_offsets[id];
            for (size_t offset = 0; offset < signature.size(); offset += signature_words) {
                int population = 0;
                for (size_t word = 0; word < signature_words; ++word) {
                    population += population_count(signature[offset + word]);
                }
                minimum_component_population[id] =
                    std::min(minimum_component_population[id], population);
                maximum_component_population[id] =
                    std::max(maximum_component_population[id], population);
                component_populations[population_index++] = population;
            }
            std::sort(component_populations.begin() + population_offsets[id],
                      component_populations.begin() + population_offsets[id + 1]);
        }
    }

    // Then apply connectivity-coarsening dominance to exact survivors. Keep
    // cross-dominated states available as witnesses until the scan finishes,
    // matching the previous deferred-erasure behavior. Discover coarsening
    // relationships lazily: many signature pairs cannot have even one
    // scalar-feasible dominance comparison.
    std::vector<uint32_t> coarser;
    for (uint32_t fine_id = 0; fine_id < connectivity_count; ++fine_id) {
        if (survivors[fine_id].empty()) continue;
        coarser.clear();
        const auto& bucket = buckets[connectivity_bucket[fine_id]];
        const uint32_t* candidate_begin = bucket.connectivity_ids.begin();
        size_t candidate_count = bucket.connectivity_ids.size();
        if (!bucket.pair_postings.empty()) {
            for (uint64_t component : connectivity_pool[fine_id]) {
                for (uint64_t outer = component; outer; outer &= outer - 1) {
                    const int a = __builtin_ctzll(outer);
                    uint64_t inner = outer & (outer - 1);
                    while (inner) {
                        const int b = __builtin_ctzll(inner);
                        inner &= inner - 1;
                        const int index = bucket.pair_index[(a << 6) | b];
                        if (index >= 0 &&
                            bucket.pair_postings[index].size() < candidate_count) {
                            const auto& posting = bucket.pair_postings[index];
                            candidate_begin = posting.data();
                            candidate_count = posting.size();
                        }
                    }
                }
            }
        }
        coarser.reserve(candidate_count);
        for (size_t candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
            const uint32_t coarse_id = candidate_begin[candidate_index];
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
                if (component_populations[population_offsets[fine_id] + component] >
                    component_populations[population_offsets[coarse_id] + component]) {
                    population_matching_possible = false;
                    break;
                }
            }
            if (!population_matching_possible) continue;
            if (survivors[fine_id].back()->key_before(*survivors[coarse_id].front())) continue;
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
        auto best_coarse = std::min_element(coarser.begin(), coarser.end(),
            [&](uint32_t a, uint32_t b) {
                return std::pair<int, int>{minimum_quasi[a], minimum_cost[a]} <
                       std::pair<int, int>{minimum_quasi[b], minimum_cost[b]};
            });
        std::iter_swap(coarser.begin(), best_coarse);
        for (CachedItem* cached : survivors[fine_id]) {
            bool is_dominated = false;
            for (uint32_t coarse_id : coarser) {
                for (CachedItem* prior : survivors[coarse_id]) {
                    if (cached->key_before(*prior)) break;
                    if (dominates(prior, cached)) {
                        is_dominated = true;
                        break;
                    }
                }
                if (is_dominated) break;
            }
            cached->cross_dominated = is_dominated;
            if (is_dominated && profile) ++profile->forward_pruned;
        }
    }

    const auto forward_end = profile ? std::chrono::steady_clock::now() : exact_end;

    // Cost-compensated connectivity dominance extends the reverse pass.
    // Exact quotients pay the difference in live component counts. Otherwise,
    // a common merge bounds the extra seeds even for incomparable signatures.
    // Run this after coarsening dominance so already-dominated targets can
    // skip these more expensive comparisons.
    std::vector<CachedItem*> live_targets;
    for (uint32_t target_id = 0; target_id < connectivity_count; ++target_id) {
        if (survivors[target_id].empty()) continue;
        live_targets.assign(survivors[target_id].begin(), survivors[target_id].end());
        int live_maximum_cost;
        int live_maximum_quasi;
        auto refresh_targets = [&]() {
            live_maximum_cost = std::numeric_limits<int>::min();
            live_maximum_quasi = std::numeric_limits<int>::min();
            size_t kept = 0;
            for (CachedItem* target : live_targets) {
                if (target->cross_dominated) continue;
                live_targets[kept++] = target;
                live_maximum_cost = std::max(live_maximum_cost, target->item->second.cost);
                live_maximum_quasi = std::max(live_maximum_quasi, target->quasi_score);
            }
            live_targets.resize(kept);
        };
        // Forward dominance and earlier compensated witnesses can exhaust a
        // target signature. Keep witnesses in survivors, but bound and scan
        // only the remaining targets. Stable compaction preserves key order.
        refresh_targets();
        if (live_targets.empty()) continue;
        const auto& bucket = buckets[connectivity_bucket[target_id]];
        for (uint32_t candidate_id : bucket.connectivity_ids) {
            if (candidate_id == target_id || survivors[candidate_id].empty()) continue;
            // A positive compensation requires a strictly cheaper witness.
            // This also prevents cycles of mutually deleted new witnesses.
            int extra_seeds = std::max(1,
                static_cast<int>(component_counts[candidate_id]) -
                static_cast<int>(component_counts[target_id]));
            if (minimum_cost[candidate_id] + extra_seeds > live_maximum_cost ||
                minimum_quasi[candidate_id] + extra_seeds > live_maximum_quasi) {
                continue;
            }
            // The high half of the key orders cost + active 3BV hits.
            // The factor penalty is at least base_hits(A)-base_hits(B),
            // so adding the seed charge still cannot exceed B's primary key.
            // Use 64-bit arithmetic on the biased primary to avoid carry
            // between the two packed fields of DominanceKey.
            const uint64_t minimum_primary = survivors[candidate_id].front()->key >> 32;
            const uint64_t maximum_primary = live_targets.back()->key >> 32;
            if (minimum_primary + static_cast<uint64_t>(extra_seeds) > maximum_primary)
                continue;
            if (!connectivity_exact_quotient(connectivity_pool[target_id],
                                             connectivity_pool[candidate_id],
                                             signature_words)) {
                // These are the same necessary bounds checked below. Stop
                // merging as soon as no remaining target can afford the charge.
                const int maximum_penalty = static_cast<int>(std::min<uint64_t>(
                    std::min(live_maximum_cost - minimum_cost[candidate_id],
                             live_maximum_quasi - minimum_quasi[candidate_id]),
                    maximum_primary - minimum_primary));
                extra_seeds = connectivity_merge_penalty(
                    connectivity_pool[candidate_id], connectivity_pool[target_id],
                    signature_words, maximum_penalty);
                if (extra_seeds <= 0 ||
                    extra_seeds == std::numeric_limits<int>::max() ||
                    minimum_cost[candidate_id] + extra_seeds > live_maximum_cost ||
                    minimum_quasi[candidate_id] + extra_seeds > live_maximum_quasi) {
                    continue;
                }
                if (minimum_primary + static_cast<uint64_t>(extra_seeds) > maximum_primary)
                    continue;
            }
            const auto& candidates = survivors[candidate_id];
            auto eligible_end = candidates.begin();
            bool removed_target = false;
            for (CachedItem* target : live_targets) {
                const uint64_t target_primary = target->key >> 32;
                // Both lists have ascending primary keys. Advance the
                // affordable prefix once across all targets for this pair
                // of signatures, keeping the cutoff test out of the inner loop.
                while (eligible_end != candidates.end() &&
                       ((*eligible_end)->key >> 32) +
                           static_cast<uint64_t>(extra_seeds) <= target_primary) {
                    ++eligible_end;
                }
                for (auto candidate_it = candidates.begin(); candidate_it != eligible_end;
                     ++candidate_it) {
                    CachedItem* candidate = *candidate_it;
                    if (candidate->item->second.cost + extra_seeds >
                        target->item->second.cost) continue;
                    if (candidate->quasi_score + extra_seeds > target->quasi_score)
                        continue;
                    const int allowance = target->item->second.cost -
                        candidate->item->second.cost - extra_seeds;
                    const int lower_bound =
                        std::max(0, target->flag_hits - candidate->flag_hits) +
                        std::max(0, candidate->base_hits - target->base_hits);
                    if (lower_bound <= allowance && dominance_penalty_at_most(
                            candidate->item->first.hits, target->item->first.hits,
                            base_mask, allowance)) {
                        target->cross_dominated = true;
                        removed_target = true;
                        if (profile) ++profile->reverse_pruned;
                        break;
                    }
                }
            }
            if (removed_target) {
                refresh_targets();
                if (live_targets.empty()) break;
            }
        }
    }
    const auto cross_end = profile ? std::chrono::steady_clock::now() : exact_end;

    size_t removed = 0;
    for (auto& bucket : buckets) {
        for (CachedItem& cached : bucket.entries) {
            if (!cached.exact_dominated && !cached.cross_dominated) continue;
            table.erase(cached.item);
            ++removed;
        }
    }
    if (profile) {
        const auto erase_end = std::chrono::steady_clock::now();
        profile->dominance_setup += std::chrono::duration<double>(setup_end - profile_start).count();
        profile->dominance_exact += std::chrono::duration<double>(exact_end - setup_end).count();
        profile->dominance_cross += std::chrono::duration<double>(cross_end - exact_end).count();
        profile->dominance_forward += std::chrono::duration<double>(forward_end - exact_end).count();
        profile->dominance_reverse += std::chrono::duration<double>(cross_end - forward_end).count();
        profile->dominance_erase += std::chrono::duration<double>(erase_end - cross_end).count();
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
    bool mandatory_zeros = false;
    std::vector<std::vector<int>> scopes;
    std::vector<uint8_t> is_flag;
    std::vector<int> first;
    std::vector<int> last;
    std::vector<int> slot;
    std::vector<int> zero_factor;
    int slot_count = 0;
};

static FactorPlan make_factor_plan(const Model& model, bool mandatory_zeros = false) {
    FactorPlan plan;
    plan.mandatory_zeros = mandatory_zeros;
    plan.zero_factor.assign(model.zero_scopes.size(), -1);
    auto add_scopes = [&](const std::vector<std::vector<int>>& source, bool flag) {
        for (size_t source_index = 0; source_index < source.size(); ++source_index) {
            const auto& scope = source[source_index];
            if (mandatory_zeros && !flag && source_index < model.zero_scopes.size())
                continue;
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
                                    bool progress,
                                    bool profile_enabled) {
    const auto core_start = profile_enabled ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    ProfileStats profile;
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
    for (int z = 0; z < zero_count; ++z) {
        for (int variable : model.zero_scopes[z]) zero_memberships[variable].push_back(z);
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
    // Mandatory openings enter before their first neighboring decision, so
    // their first candidate also needs a contact slot even without a past chord.
    if (factors.mandatory_zeros) {
        for (const auto& scope : model.zero_scopes) {
            if (scope.empty()) continue;
            for (int candidate : scope)
                first_contact[candidate] = std::min(first_contact[candidate], scope.front());
        }
    }
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
            // Entering zero components need both the current and future bits
            // before projection, so they cannot share a slot at this cut.
            if (connectivity_slot_last[slot] < first_contact[candidate] ||
                (!factors.mandatory_zeros &&
                 connectivity_slot_last[slot] == first_contact[candidate])) {
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
    std::vector<uint64_t> compact_future_neighbors;
    if (compact_connectivity) {
        compact_future_neighbors.assign(
            static_cast<size_t>(q) * compact_signature_words, 0);
        for (int i = 0; i < q; ++i) {
            for (int candidate = i + 1; candidate < q; ++candidate) {
                if (!test_bit(future_neighbors[i], candidate)) continue;
                const int slot = connectivity_slot[candidate];
                compact_future_neighbors[static_cast<size_t>(i) * compact_signature_words +
                                         slot / 64] |= uint64_t{1} << (slot % 64);
            }
        }
    }
    std::vector<FactorBits> pending_flags(q, zero_factors);
    std::vector<FactorBits> pending_bases(q, zero_factors);
    for (int i = 0; i < q; ++i) {
        pending_flags[i] = flag_member[i];
        pending_bases[i] = base_member[i];
        Ops::retain(pending_flags[i], active_after[i]);
        Ops::retain(pending_bases[i], active_after[i]);
    }
    struct OpeningPattern {
        int factor_slot = -1;
        std::vector<uint64_t> remaining_border;
    };
    std::vector<std::vector<OpeningPattern>> opening_patterns(q);
    // An opening always owns a component, whether or not any neighboring
    // chord has been selected.
    // This removes its coverage bit and the need for opening absorption.
    std::vector<std::vector<uint64_t>> entering_zeros(q);
    std::vector<int> entering_zero_count(q, 0);
    if (factors.mandatory_zeros) {
        const size_t words = compact_connectivity ? compact_signature_words : chosen_words;
        for (const auto& scope : model.zero_scopes) {
            if (scope.empty()) continue;
            auto& entering = entering_zeros[scope.front()];
            const size_t offset = entering.size();
            entering.resize(offset + words, 0);
            for (int candidate : scope) {
                const int position = compact_connectivity
                    ? connectivity_slot[candidate] : candidate;
                entering[offset + position / 64] |= uint64_t{1} << (position % 64);
            }
            ++entering_zero_count[scope.front()];
        }
    }
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
    // Only boundary sizes are used for statistics. Each graph vertex or zero
    // scope contributes over an interval of cuts; count those with a prefix
    // sum instead of retaining every boundary's vertex list.
    std::vector<int> boundary_delta(q + 1, 0);
    for (int v = 0; v < q; ++v) {
        int last = v;
        for (int other : model.graph[v]) last = std::max(last, other);
        ++boundary_delta[v];
        --boundary_delta[last];
    }
    for (const auto& scope : model.zero_scopes) {
        if (scope.empty()) continue;
        ++boundary_delta[scope.front()];
        --boundary_delta[scope.back()];
    }
    int boundary_size = 0;
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

    const auto loop_start = profile_enabled ? std::chrono::steady_clock::now() : core_start;
    TableType next;
    for (int i = 0; i < q; ++i) {
        if (profile_enabled) profile.processed_states += table.size();
        boundary_size += boundary_delta[i];
        const int new_boundary_size = boundary_size;
        const size_t signature_words = compact_connectivity
            ? compact_signature_words : chosen_words;
        const int current_connectivity_slot = compact_connectivity
            ? connectivity_slot[i] : i;

        const size_t desired_capacity = table.size() > (std::numeric_limits<size_t>::max() - 16) / 2
            ? std::numeric_limits<size_t>::max() : table.size() * 2 + 16;
        next.reserve(desired_capacity);
        next_connectivity_pool.reset(connectivity_pool.size() * 2 + 16);
        {
            std::vector<std::array<ConnectivityTransition, 2>> connectivity_cache(
                connectivity_pool.size());
            std::vector<uint8_t> connectivity_ready(connectivity_pool.size(), 0);
            // For each incoming connectivity signature, prove how much more
            // future component-seeding the skip/take branch can require than
            // its sibling. A finite bound compiles a whole family of factor
            // patterns into one cheap conditional-dominance inequality.
            constexpr int no_bound = std::numeric_limits<int>::max();
            std::vector<std::array<int, 2>> sibling_penalties(
                connectivity_pool.size(), std::array<int, 2>{no_bound, no_bound});
            // Reuse flat scratch buffers for every connectivity transition in
            // this layer. Component offsets remain valid if the word buffer
            // grows, unlike pointers into a vector.
            std::vector<uint64_t> outgoing_words;
            std::vector<size_t> outgoing_offsets;
            std::vector<uint64_t> merged(signature_words);
            std::vector<uint64_t> projected(signature_words);
            std::vector<uint64_t> canonical;
            // Reuse component markers, clearing them for each successor.
            std::vector<uint8_t> absorption_marks;
            for (const auto& [old_state, old_record] : table) {
                if (!connectivity_ready[old_state.connectivity_id]) {
                    const auto previous_signature = connectivity_pool[old_state.connectivity_id];
                    std::vector<uint64_t> with_zeros;
                    if (!entering_zeros[i].empty()) {
                        with_zeros.assign(previous_signature.begin(), previous_signature.end());
                        with_zeros.insert(with_zeros.end(),
                            entering_zeros[i].begin(), entering_zeros[i].end());
                    }
                    const SignatureView old_signature = with_zeros.empty()
                        ? previous_signature : SignatureView(with_zeros);
                    std::array<ConnectivityTransition, 2> computed;
                    for (int selected = 0; selected <= 1; ++selected) {
                        int closed = 0;
                        outgoing_words.clear();
                        outgoing_offsets.clear();
                        if (signature_words == 1) {
                            // Work directly on component words: no projection
                            // buffer, offset list, or per-word loops are needed.
                            const uint64_t current_bit = current_connectivity_slot >= 0
                                ? uint64_t{1} << current_connectivity_slot : 0;
                            uint64_t merged_word = selected
                                ? (compact_connectivity
                                       ? compact_future_neighbors[
                                             static_cast<size_t>(i) * compact_signature_words]
                                                        : future_neighbors[i][0])
                                : 0;
                            outgoing_words.reserve(old_signature.size() + selected);
                            for (uint64_t component : old_signature) {
                                const uint64_t remaining = component & ~current_bit;
                                if (selected && (component & current_bit)) {
                                    merged_word |= remaining;
                                } else if (remaining) {
                                    outgoing_words.push_back(remaining);
                                } else {
                                    ++closed;
                                }
                            }
                            if (selected) {
                                if (merged_word) outgoing_words.push_back(merged_word);
                                else ++closed;
                            }
                            std::sort(outgoing_words.begin(), outgoing_words.end());
                            computed[selected] = {
                                next_connectivity_pool.intern(outgoing_words), closed};
                            continue;
                        }
                        std::fill(merged.begin(), merged.end(), 0);
                        const size_t old_component_count =
                            old_signature.size() / signature_words;
                        outgoing_words.reserve(
                            (old_component_count + selected) * signature_words);
                        outgoing_offsets.reserve(old_component_count + selected);
                        if (selected) {
                            if (compact_connectivity) {
                                std::copy_n(
                                    compact_future_neighbors.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            static_cast<size_t>(i) * compact_signature_words),
                                    compact_signature_words, merged.begin());
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
                    // These certificates use exactly the same connectivity
                    // relations as the later dominance pass. Factor hit bits
                    // do not affect them. Opening absorption below is an exact
                    // normalization, so it need not be performed first.
                    auto& penalty = sibling_penalties[old_state.connectivity_id];
                    if (computed[0].connectivity_id == computed[1].connectivity_id) {
                        penalty = {0, 0};
                    } else {
                        const auto& skip = next_connectivity_pool[computed[0].connectivity_id];
                        const auto& take = next_connectivity_pool[computed[1].connectivity_id];
                        bool same_union = true;
                        for (size_t word = 0; word < signature_words; ++word) {
                            uint64_t skip_union = 0, take_union = 0;
                            for (size_t offset = word; offset < skip.size(); offset += signature_words)
                                skip_union |= skip[offset];
                            for (size_t offset = word; offset < take.size(); offset += signature_words)
                                take_union |= take[offset];
                            if (skip_union != take_union) { same_union = false; break; }
                        }
                        if (same_union) {
                            if (connectivity_coarsens(skip, take, signature_words)) penalty[0] = 0;
                            else if (connectivity_exact_quotient(take, skip, signature_words))
                                penalty[0] = static_cast<int>((skip.size() - take.size()) / signature_words);
                            if (connectivity_coarsens(take, skip, signature_words)) penalty[1] = 0;
                            else if (connectivity_exact_quotient(skip, take, signature_words))
                                penalty[1] = static_cast<int>((take.size() - skip.size()) / signature_words);
                        }
                    }
                    for (auto& branch : computed)
                        branch.hash_seed = hash_combine(0, branch.connectivity_id);
                    connectivity_cache[old_state.connectivity_id] = std::move(computed);
                    connectivity_ready[old_state.connectivity_id] = 1;
                }

                const int new_flags = Ops::count_new_and(
                    factor_member[i], old_state.hits, flag_member[i]);
                const int new_bases = Ops::count_new_and(
                    factor_member[i], old_state.hits, base_member[i]);
                const auto& branch = connectivity_cache[old_state.connectivity_id];
                const auto& penalty = sibling_penalties[old_state.connectivity_id];
                // The take-minus-skip cost is known without constructing a
                // successor. A skipped flag can still help a later chord, so
                // compensate for every newly hit flag that remains active.
                // Conversely, take must compensate for active base rewards
                // already claimed. Expired factors need no compensation.
                const int delta = 1 + branch[1].closed - branch[0].closed + new_flags - new_bases;
                // Prefer skip on a tie, retaining at least one branch even
                // when both certificates apply.
                int first_selected = 0, last_selected = 1;
                if (penalty[0] != no_bound && delta >= penalty[0] &&
                    delta >= penalty[0] + Ops::count_new_and(
                        factor_member[i], old_state.hits, pending_flags[i])) {
                    last_selected = 0;
                } else if (penalty[1] != no_bound && -delta >= penalty[1] &&
                    -delta >= penalty[1] + Ops::count_new_and(
                        factor_member[i], old_state.hits, pending_bases[i])) {
                    first_selected = 1;
                }
                if (profile_enabled && first_selected == last_selected) ++profile.conditional_pruned;
                for (int selected = first_selected; selected <= last_selected; ++selected) {
                    FactorBits new_hits = old_state.hits;
                    const int factor_cost = selected ? new_flags - new_bases : 0;
                    if (selected) {
                        Ops::add(new_hits, factor_member[i]);
                    }
                    Ops::retain(new_hits, active_after[i]);
                    const auto& connection =
                        connectivity_cache[old_state.connectivity_id][selected];
                    // Baseline 3BV already pays for each opening. Its mandatory
                    // component now pays through the usual component-closure cost.
                    int new_cost = old_record.cost + selected + connection.closed
                        + factor_cost - entering_zero_count[i];
                    uint32_t connectivity_id = connection.connectivity_id;
                    if (connectivity_id != 0 && !opening_patterns[i].empty()) {
                        const auto& signature = next_connectivity_pool[connectivity_id];
                        const size_t component_count = signature.size() / signature_words;
                        if (absorption_marks.size() < component_count)
                            absorption_marks.resize(component_count, 0);
                        std::fill(absorption_marks.begin(), absorption_marks.end(), 0);
                        bool changed = false;
                        for (const OpeningPattern& opening : opening_patterns[i]) {
                            if (!Ops::test(new_hits, opening.factor_slot)) continue;
                            size_t matched_component = component_count;
                            if (signature_words == 1) {
                                // One-word signatures are sorted by component
                                // value. Find the first matching component
                                // directly instead of rescanning the frontier.
                                const auto match = std::lower_bound(
                                    signature.begin(), signature.end(),
                                    opening.remaining_border[0]);
                                if (match != signature.end() &&
                                    *match == opening.remaining_border[0]) {
                                    matched_component = static_cast<size_t>(
                                        match - signature.begin());
                                    while (matched_component < component_count &&
                                           absorption_marks[matched_component] != 0 &&
                                           signature[matched_component] ==
                                               opening.remaining_border[0]) {
                                        ++matched_component;
                                    }
                                    if (matched_component == component_count ||
                                        signature[matched_component] !=
                                            opening.remaining_border[0]) {
                                        matched_component = component_count;
                                    }
                                }
                            } else {
                                for (size_t component = 0; component < component_count;
                                     ++component) {
                                    if (absorption_marks[component] != 0) continue;
                                    const size_t offset = component * signature_words;
                                    bool equal = true;
                                    for (size_t word = 0; word < signature_words; ++word) {
                                        if (signature[offset + word] !=
                                            opening.remaining_border[word]) {
                                            equal = false;
                                            break;
                                        }
                                    }
                                    if (equal) {
                                        matched_component = component;
                                        break;
                                    }
                                }
                            }
                            if (matched_component == component_count) continue;
                            absorption_marks[matched_component] = 1;
                            Ops::clear(new_hits, opening.factor_slot);
                            ++new_cost;
                            ++opening_absorptions;
                            changed = true;
                        }
                        if (changed) {
                            canonical.clear();
                            canonical.reserve(signature.size());
                            for (size_t component = 0; component < component_count;
                                 ++component) {
                                if (absorption_marks[component] != 0) continue;
                                const size_t offset = component * signature_words;
                                canonical.insert(canonical.end(),
                                    signature.begin() + static_cast<std::ptrdiff_t>(offset),
                                    signature.begin() + static_cast<std::ptrdiff_t>(
                                        offset + signature_words));
                            }
                            connectivity_id = next_connectivity_pool.intern(canonical);
                        }
                    }
                    // Reuse the connectivity part of the state hash across
                    // all factor states taking the same cached transition.
                    const size_t hash_seed = connectivity_id == connection.connectivity_id
                        ? connection.hash_seed : hash_combine(0, connectivity_id);
                    const size_t state_hash = append_factor_hash(hash_seed, new_hits);
                    StateType state{connectivity_id, std::move(new_hits)};
                    Bits old_chosen{};
                    if constexpr (TrackChosen) old_chosen = old_record.chosen;
                    auto [found, inserted] = next.find_or_emplace(
                        std::move(state), state_hash, [&]() {
                        if constexpr (TrackChosen) {
                            Bits new_chosen = old_chosen;
                            if (selected) set_bit(new_chosen, i);
                            return Record<true>{new_cost, std::move(new_chosen)};
                        } else {
                            return Record<false>{new_cost};
                        }
                    });
                    if (!inserted && new_cost < found->second.cost) {
                        found->second.cost = new_cost;
                        if constexpr (TrackChosen) {
                            found->second.chosen = old_chosen;
                            if (selected) set_bit(found->second.chosen, i);
                        }
                    }
                }
            }
        }

        // Destroy previous records and release the connectivity pool before
        // dominance pruning. Retain only the dense table capacity for reuse
        // when constructing the following layer.
        table.clear_for_reuse();
        {
            ConnectivityPool released;
            connectivity_pool.swap(released);
        }

        if (profile_enabled) {
            profile.generated_states += next.size();
            profile.peak_generated_states = std::max<uint64_t>(
                profile.peak_generated_states, next.size());
        }
        // Keep duplicate merging, opening absorption, and conditional branch
        // pruning at every candidate. Apply the full dominance passes after
        // each pair, when both candidates' effects can be compared together.
        // The final candidate is always pruned, including an unpaired tail.
        const bool prune_now = (i % 2 == 1) || i + 1 == q;
        if (prune_now) next.release_lookup_index();
        const auto prune_start = profile_enabled && prune_now
            ? std::chrono::steady_clock::now() : loop_start;
        const size_t removed = prune_now ? prune_dominated(
            next, base_after[i],
            next_connectivity_pool, signature_words,
            profile_enabled ? &profile : nullptr) : 0;
        if (profile_enabled) {
            if (prune_now) {
                ++profile.dominance_passes;
                profile.dominance += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - prune_start).count();
            } else ++profile.deferred_layers;
        }
        table.swap(next);
        connectivity_pool.swap(next_connectivity_pool);
        dominated_total += removed;
        peak_states = std::max(peak_states, table.size());
        max_boundary = std::max(max_boundary, new_boundary_size);
        const int active_count = Ops::count(active_after[i]);
        max_active = std::max(max_active, active_count);
        if (progress && ((i + 1) % PROGRESS_INTERVAL == 0 || i + 1 == q)) {
            std::cerr << "DP: region contains " << region_ranks[i] << "/"
                      << model.height * model.width << " tiles; processed " << i + 1
                      << "/" << q << " chord candidates; " << comma_number(table.size())
                      << " valid boundary states; boundary " << new_boundary_size
                      << " connectivity items + " << active_count << " factor bits; pruned "
                      << comma_number(dominated_total) << " dominated states; absorbed "
                      << comma_number(opening_absorptions) << " opening chains\n";
        }
    }

    const auto loop_end = profile_enabled ? std::chrono::steady_clock::now() : loop_start;
    if (profile_enabled) {
        profile.dp_setup = std::chrono::duration<double>(loop_start - core_start).count();
        profile.transitions = std::chrono::duration<double>(loop_end - loop_start).count()
            - profile.dominance;
    }
    StateType final_state{0, zero_factors};
    auto final = table.find(final_state);
    if (final == table.end()) throw std::logic_error("frontier DP did not reach an empty final state");
    Solution solution;
    const auto replay_start = profile_enabled ? std::chrono::steady_clock::now() : loop_end;
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
    if (profile_enabled) {
        profile.replay = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - replay_start).count();
        solution.profile = profile;
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
                               bool count_only = false,
                               bool profile_enabled = false) {
    CandidateReduction reduction = reduce_candidates(original_model, progress);
    const auto ordering_start = profile_enabled ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    auto [model, order_name] = choose_order(
        reduction.model, requested_order, band_size);
    const auto factor_start = profile_enabled ? std::chrono::steady_clock::now() : ordering_start;
    // Zero openings and selected chords share one connectivity objective at
    // every board size; no separate zero-coverage bits are needed.
    const FactorPlan factors = make_factor_plan(model, true);
    const auto dp_start = profile_enabled ? std::chrono::steady_clock::now() : factor_start;
    Solution solution;
    if (factors.slot_count <= 64) {
        if (count_only) solution = solve_frontier_core<uint64_t, false>(
            original_model, model, order_name, factors, progress, profile_enabled);
        else solution = solve_frontier_core<uint64_t, true>(
            original_model, model, order_name, factors, progress, profile_enabled);
    } else {
        if (count_only) solution = solve_frontier_core<Bits, false>(
            original_model, model, order_name, factors, progress, profile_enabled);
        else solution = solve_frontier_core<Bits, true>(
            original_model, model, order_name, factors, progress, profile_enabled);
    }
    if (profile_enabled) {
        solution.profile.ordering = std::chrono::duration<double>(factor_start - ordering_start).count();
        solution.profile.factor_plan = std::chrono::duration<double>(dp_start - factor_start).count();
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
                       const std::optional<std::string>& generated_url,
                       const std::optional<std::string>& imported_url) {
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
    if (imported_url) {
        std::cout << ",\n  \"imported_board_url\":\"" << json_escape(*imported_url) << '"';
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
    bool benchmark = false;
    bool benchmark_multi = false;
    std::optional<size_t> benchmark_warmup;
    std::optional<size_t> benchmark_repeat;
    std::optional<size_t> benchmark_cpu;
    std::optional<std::string> board;
    std::optional<std::string> generate;
    std::optional<std::tuple<int, int, int>> bulk;
    size_t bulk_count = 0;
    std::optional<size_t> threads;
    std::optional<bool> count_only;
    std::optional<uint64_t> seed;
    std::string order = "auto";
    std::optional<int> band_size;
    bool progress = false;
    bool profile = false;
    bool json = false;
    bool click_tuples = false;
};

static void print_help(const char* program) {
    std::cout
        << "Deterministically Optimal Minesweeper Solver (DOMS, C++17)\n\n"
        << "Usage: " << program << " [BOARD] [options]\n\n"
        << "BOARD may be a PTTACG string, compatible LlamaSweeper URL,\n"
        << "World of Minesweeper game URL, MBF filename, or quoted MBF hex.\n\n"
        << "Options:\n"
        << "  --benchmark                    Fixed Expert timing: warm-up, high priority, three repeats; validate counts\n"
        << "  --benchmark-multi              Same fixed benchmark using multiple threads\n"
        << "  --benchmark-warmup N           Warm-up boards per set (default: 100; 0 disables)\n"
        << "  --benchmark-repeat N           Timed repeats per fixed set (default: 3)\n"
        << "  --benchmark-cpu N              Pin the single benchmark thread to logical CPU N\n"
        << "  --generate beginner|intermediate|expert\n"
        << "                                 Generate, print, and solve one random board\n"
        << "  --bulk beginner|intermediate|expert|evil|NxM/K COUNT\n"
        << "                                 Generate and solve COUNT boards; write CSV\n"
        << "                                 evil = 30x20/130; NxM/K = width x height / mines\n"
        << "  --seed N                       Reproduce generated or bulk boards\n"
        << "  --threads N                    Bulk workers (default: one per available core)\n"
        << "  --count-only                   Compute click count without a move replay\n"
        << "  --no-count-only                Include replay (default for single boards)\n"
        << "  --order NAME                  Choose any printed sweep order (or auto)\n"
        << "  --band-size N\n"
        << "  --progress\n"
        << "  --profile                    Print a timing breakdown on single boards\n"
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

static size_t parse_nonnegative_size(const std::string& value, const std::string& option) {
    const uint64_t parsed = parse_u64(value, option);
    if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw UserError(option + " is too large");
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
        } else if (arg == "--benchmark") options.benchmark = true;
        else if (arg == "--benchmark-multi") {
            options.benchmark = true;
            options.benchmark_multi = true;
        }
        else if (arg == "--benchmark-warmup")
            options.benchmark_warmup = parse_nonnegative_size(value_after(i, arg), arg);
        else if (arg == "--benchmark-repeat")
            options.benchmark_repeat = parse_positive_size(value_after(i, arg), arg);
        else if (arg == "--benchmark-cpu")
            options.benchmark_cpu = parse_nonnegative_size(value_after(i, arg), arg);
        else if (arg == "--generate") options.generate = value_after(i, arg);
        else if (arg == "--bulk") {
            options.bulk = bulk_board_spec(value_after(i, arg));
            options.bulk_count = parse_positive_size(value_after(i, arg), arg);
        }
        else if (arg == "--seed") options.seed = parse_u64(value_after(i, arg), arg);
        else if (arg == "--threads") options.threads = parse_positive_size(value_after(i, arg), arg);
        else if (arg == "--count-only") options.count_only = true;
        else if (arg == "--no-count-only") options.count_only = false;
        else if (arg == "--order") options.order = value_after(i, arg);
        else if (arg == "--band-size") options.band_size = parse_positive_int(value_after(i, arg), arg);
        else if (arg == "--progress") options.progress = true;
        else if (arg == "--profile") options.profile = true;
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
    if (options.benchmark) {
        if (options.board || options.generate || options.bulk || options.seed ||
            (options.threads && !options.benchmark_multi) || options.json || options.click_tuples || options.profile ||
            (options.count_only && !*options.count_only)) {
            throw UserError("benchmark modes use fixed seeds, Expert boards, and count-only output; "
                            "cannot combine with BOARD, --generate, --bulk, --seed, incompatible --threads, "
                            "--json, --click-tuples, --profile, or --no-count-only");
        }
    }
    if (!options.benchmark &&
        (options.benchmark_warmup || options.benchmark_repeat || options.benchmark_cpu)) {
        throw UserError("--benchmark-warmup, --benchmark-repeat, and --benchmark-cpu "
                        "require --benchmark");
    }
    if (options.benchmark_multi &&
        (options.benchmark_warmup || options.benchmark_repeat || options.benchmark_cpu)) {
        throw UserError("--benchmark-warmup, --benchmark-repeat, and --benchmark-cpu "
                        "are not supported with --benchmark-multi");
    }
    if (options.generate && options.bulk) {
        throw UserError("choose only one of --generate and --bulk");
    }
    if (options.bulk) {
        if (options.profile) throw UserError("--profile is for single boards only");
        if (options.board) throw UserError("do not supply a board argument with --bulk");
        if (options.json || options.click_tuples) {
            throw UserError("--bulk already writes CSV; do not combine it with --json or --click-tuples");
        }
    } else if (options.generate) {
        if (!valid_difficulty(*options.generate)) {
            throw UserError("--generate must be beginner, intermediate, or expert");
        }
        if (options.board) throw UserError("do not supply a board argument with --generate");
    } else if (!options.benchmark) {
        if (!options.board) throw UserError("a board argument, --generate, or --bulk is required");
        if (options.seed) throw UserError("--seed requires --generate or --bulk");
    }
    const OrderRequest parsed_order = parse_order_request(options.order);
    if (options.band_size && parsed_order.kind != OrderRequest::Kind::Auto &&
        parsed_order.kind != OrderRequest::Kind::Plain) {
        throw UserError("--band-size requires a plain row or column --order (or auto)");
    }
    if (options.json && options.click_tuples) throw UserError("choose only one of --json and --click-tuples");
    if (options.threads && !options.bulk && !options.benchmark_multi)
        throw UserError("--threads requires --bulk or --benchmark-multi");
    if (options.click_tuples && options.count_only.value_or(false)) {
        throw UserError("--click-tuples requires --no-count-only");
    }
    return options;
}

// Input must already be sorted. Match the bulk runner's interpolated percentiles.
static double benchmark_percentile(const std::vector<double>& sorted, double p) {
    const double rank = p * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(rank);
    const size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double fraction = rank - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

struct BenchmarkReference {
    std::string board;
    int three_bv;
    int clicks;
};

static std::array<std::vector<BenchmarkReference>, 3> load_benchmark_references() {
    std::ifstream input(expert_benchmark::reference_csv);
    if (!input) throw UserError("cannot open benchmark references: " +
                                std::string(expert_benchmark::reference_csv));
    std::string line;
    if (!std::getline(input, line) || line != "run,seed,board,three_bv,clicks")
        throw UserError("invalid benchmark reference CSV header");

    std::array<std::vector<BenchmarkReference>, 3> references;
    for (auto& run : references) run.reserve(expert_benchmark::board_count);
    size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        std::array<std::string, 5> fields;
        size_t start = 0;
        for (size_t field = 0; field < fields.size(); ++field) {
            const size_t comma = line.find(',', start);
            if (field + 1 < fields.size() && comma == std::string::npos)
                throw UserError("invalid benchmark reference CSV row " + std::to_string(line_number));
            if (field + 1 == fields.size() && comma != std::string::npos)
                throw UserError("invalid benchmark reference CSV row " + std::to_string(line_number));
            fields[field] = line.substr(start, comma == std::string::npos ? comma : comma - start);
            start = comma == std::string::npos ? line.size() : comma + 1;
        }
        try {
            const size_t run = static_cast<size_t>(std::stoul(fields[0]));
            const uint64_t seed = std::stoull(fields[1]);
            const int three_bv = std::stoi(fields[3]);
            const int clicks = std::stoi(fields[4]);
            if (run < 1 || run > references.size() ||
                seed != expert_benchmark::seeds[run - 1] || fields[2].empty() ||
                fields[2].find('"') != std::string::npos)
                throw std::runtime_error("invalid values");
            references[run - 1].push_back({fields[2], three_bv, clicks});
        } catch (const std::exception&) {
            throw UserError("invalid benchmark reference CSV row " + std::to_string(line_number));
        }
        if (references[0].size() > expert_benchmark::board_count ||
            references[1].size() > expert_benchmark::board_count ||
            references[2].size() > expert_benchmark::board_count)
            throw UserError("too many rows in benchmark reference CSV");
    }
    for (const auto& run : references)
        if (run.size() != expert_benchmark::board_count)
            throw UserError("benchmark reference CSV must contain 1000 boards for each seed");
    return references;
}

static void validate_benchmark_result(size_t run, size_t index, const std::string& encoded,
                                      int three_bv, int clicks,
                                      const std::array<std::vector<BenchmarkReference>, 3>& references) {
    const auto& expected = references[run][index];
    if (encoded != expected.board || three_bv != expected.three_bv || clicks != expected.clicks) {
        throw UserError("benchmark validation failed: run " + std::to_string(run + 1) +
                        ", seed " + std::to_string(expert_benchmark::seeds[run]) +
                        ", board " + std::to_string(index + 1) +
                        "; expected " + expected.board + " (3BV " +
                        std::to_string(expected.three_bv) + ", clicks " +
                        std::to_string(expected.clicks) + "), got " + encoded +
                        " (3BV " + std::to_string(three_bv) + ", clicks " +
                        std::to_string(clicks) + ")");
    }
}

struct BenchmarkSetResult {
    double wall_seconds = 0.0;
    double solve_seconds = 0.0;
    double solve_cpu_seconds = 0.0;
    std::vector<double> solve_times;
};

static bool benchmark_pin_to_cpu(size_t cpu) {
#ifdef _WIN32
    if (cpu >= sizeof(DWORD_PTR) * 8) return false;
    return SetThreadAffinityMask(GetCurrentThread(),
                                 static_cast<DWORD_PTR>(1) << cpu) != 0;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

static bool benchmark_raise_priority() {
#ifdef _WIN32
    const bool process = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    const bool thread = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    return process && thread;
#elif defined(__linux__) || defined(__APPLE__)
    return setpriority(PRIO_PROCESS, 0, -10) == 0;
#else
    return false;
#endif
}

static double benchmark_thread_cpu_seconds() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0.0;
    ULARGE_INTEGER kernel_time, user_time;
    kernel_time.LowPart = kernel.dwLowDateTime;
    kernel_time.HighPart = kernel.dwHighDateTime;
    user_time.LowPart = user.dwLowDateTime;
    user_time.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernel_time.QuadPart + user_time.QuadPart) / 10000000.0;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0.0;
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
#else
    return 0.0;
#endif
}

static BenchmarkSetResult run_benchmark_set(
    size_t run, const Options& options,
    const std::array<std::vector<BenchmarkReference>, 3>& references,
    size_t board_limit, bool collect_times, bool report_progress,
    const std::string& progress_label) {
    BenchmarkSetResult result;
    if (collect_times) result.solve_times.reserve(board_limit);
    std::mt19937_64 rng(expert_benchmark::seeds[run]);
    const auto start = std::chrono::steady_clock::now();
    // Intentionally execute directly on this thread, without a worker pool.
    for (size_t index = 0; index < board_limit; ++index) {
        Board board = generate_standard_board("expert", rng);
        const std::string encoded = format_pttacg_string(board);
        const auto solve_start = std::chrono::steady_clock::now();
        const double cpu_start = benchmark_thread_cpu_seconds();
        Model model = build_model(board);
        Solution solution = solve_frontier(model, options.order, options.band_size, false, true);
        const auto solve_end = std::chrono::steady_clock::now();
        const double cpu_end = benchmark_thread_cpu_seconds();
        validate_benchmark_result(run, index, encoded, model.three_bv(), solution.clicks, references);
        const double solve_seconds =
            std::chrono::duration<double>(solve_end - solve_start).count();
        result.solve_seconds += solve_seconds;
        result.solve_cpu_seconds += cpu_end - cpu_start;
        if (collect_times) result.solve_times.push_back(solve_seconds);
        if (report_progress && ((index + 1) % 100 == 0)) {
            std::cerr << progress_label << ": " << index + 1 << '/' << board_limit
                      << " boards validated\n";
        }
    }
    result.wall_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    if (collect_times) std::sort(result.solve_times.begin(), result.solve_times.end());
    return result;
}

static void warm_up_benchmark(
    const Options& options,
    const std::array<std::vector<BenchmarkReference>, 3>& references,
    size_t boards_per_set) {
    if (boards_per_set == 0) return;
    const size_t boards = std::min(boards_per_set,
        static_cast<size_t>(expert_benchmark::board_count));
    std::cerr << "Benchmark warm-up: " << boards << " boards per fixed set...\n";
    for (size_t run = 0; run < expert_benchmark::seeds.size(); ++run) {
        run_benchmark_set(run, options, references, boards, false, false, "");
    }
    std::cerr << "Benchmark warm-up complete.\n";
}

static double benchmark_mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

static double benchmark_cv_percent(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    const double mean = benchmark_mean(values);
    if (mean == 0.0) return 0.0;
    double sum = 0.0;
    for (double value : values) {
        const double difference = value - mean;
        sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(values.size())) / mean * 100.0;
}

static void print_benchmark_aggregates(
    const std::array<std::vector<BenchmarkSetResult>, 3>& results) {
    std::cout << "\naggregate_run,seed,repeats,median_wall_seconds,min_wall_seconds,"
                 "max_wall_seconds,median_solve_seconds,min_solve_seconds,"
                 "max_solve_seconds,median_cpu_seconds,min_cpu_seconds,"
                 "max_cpu_seconds,cv_solve_percent\n";
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return benchmark_percentile(values, 0.5);
    };
    for (size_t run = 0; run < results.size(); ++run) {
        std::vector<double> wall;
        std::vector<double> solve;
        std::vector<double> cpu;
        for (const auto& result : results[run]) {
            wall.push_back(result.wall_seconds);
            solve.push_back(result.solve_seconds);
            cpu.push_back(result.solve_cpu_seconds);
        }
        if (solve.empty()) continue;
        std::cout << run + 1 << ',' << expert_benchmark::seeds[run] << ','
                  << solve.size() << ',' << std::fixed << std::setprecision(6)
                  << median(wall) << ','
                  << *std::min_element(wall.begin(), wall.end()) << ','
                  << *std::max_element(wall.begin(), wall.end()) << ','
                  << median(solve) << ','
                  << *std::min_element(solve.begin(), solve.end()) << ','
                  << *std::max_element(solve.begin(), solve.end()) << ','
                  << median(cpu) << ','
                  << *std::min_element(cpu.begin(), cpu.end()) << ','
                  << *std::max_element(cpu.begin(), cpu.end()) << ','
                  << std::setprecision(3) << benchmark_cv_percent(solve) << '\n';
    }
}

static void run_benchmark(const Options& options) {
    const auto references = load_benchmark_references();
    if (options.benchmark_multi) {
        const unsigned detected = std::thread::hardware_concurrency();
        const size_t workers = std::min(expert_benchmark::board_count,
            options.threads.value_or(detected ? detected : 1));
        std::cout << "Expert benchmark: 3 runs x 1000 boards; " << workers
                  << " worker thread(s); count-only\n"
                  << "Per-board times include model construction and solving; "
                     "percentiles use linear interpolation.\n"
                  << "run,seed,boards,threads,total_seconds,median_seconds,p5_seconds,p95_seconds,validation\n";
        for (size_t run = 0; run < expert_benchmark::seeds.size(); ++run) {
            struct Job {
                Board board;
                std::string encoded;
                double seconds = 0.0;
                std::string error;
            };
            const auto start = std::chrono::steady_clock::now();
            std::mt19937_64 rng(expert_benchmark::seeds[run]);
            std::vector<Job> jobs(expert_benchmark::board_count);
            for (auto& job : jobs) {
                job.board = generate_standard_board("expert", rng);
                job.encoded = format_pttacg_string(job.board);
            }
            std::atomic<size_t> next{0};
            auto work = [&] {
                for (;;) {
                    const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= jobs.size()) break;
                    auto& job = jobs[index];
                    try {
                        const auto solve_start = std::chrono::steady_clock::now();
                        Model model = build_model(job.board);
                        Solution solution = solve_frontier(
                            model, options.order, options.band_size, false, true);
                        const auto solve_end = std::chrono::steady_clock::now();
                        validate_benchmark_result(run, index, job.encoded, model.three_bv(),
                                                  solution.clicks, references);
                        job.seconds = std::chrono::duration<double>(solve_end - solve_start).count();
                    } catch (const std::exception& error) {
                        job.error = error.what();
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
            std::vector<double> solve_times;
            solve_times.reserve(jobs.size());
            for (size_t index = 0; index < jobs.size(); ++index) {
                if (!jobs[index].error.empty())
                    throw UserError("benchmark run " + std::to_string(run + 1) + ", board " +
                                    std::to_string(index + 1) + " failed: " + jobs[index].error);
                solve_times.push_back(jobs[index].seconds);
                if (options.progress && ((index + 1) % 100 == 0))
                    std::cerr << "Benchmark run " << run + 1 << "/3, seed "
                              << expert_benchmark::seeds[run] << ": " << index + 1
                              << "/1000 boards validated\n";
            }
            const double total = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            std::sort(solve_times.begin(), solve_times.end());
            std::cout << run + 1 << ',' << expert_benchmark::seeds[run] << ','
                      << solve_times.size() << ',' << workers << ','
                      << std::fixed << std::setprecision(6) << total << ','
                      << benchmark_percentile(solve_times, 0.50) << ','
                      << benchmark_percentile(solve_times, 0.05) << ','
                      << benchmark_percentile(solve_times, 0.95) << ",PASS\n" << std::flush;
        }
        std::cout << "Validated all 3000 boards against fixed board, 3BV, and optimal-click references.\n";
        return;
    }
    const size_t warmup_boards = options.benchmark_warmup.value_or(100);
    const size_t repeats = options.benchmark_repeat.value_or(3);
    const bool high_priority = benchmark_raise_priority();
    if (!high_priority) {
        std::cerr << "Warning: could not raise benchmark priority.\n";
    }
    if (options.benchmark_cpu) {
        const size_t cpu = *options.benchmark_cpu;
        if (!benchmark_pin_to_cpu(cpu)) {
            throw UserError("cannot pin benchmark thread to logical CPU " +
                            std::to_string(cpu));
        }
        std::cerr << "Benchmark CPU affinity: logical CPU " << cpu << " (pinned)\n";
    }
    std::cerr << "Benchmark: single thread; warm-up " << warmup_boards
              << " boards/set; repeats " << repeats << "; priority "
              << (high_priority ? "high" : "default") << "\n";
    warm_up_benchmark(options, references, warmup_boards);

    std::cout << "Expert benchmark: 3 runs x 1000 boards; single thread; count-only\n"
              << "Per-board times include model construction and solving; "
                 "percentiles use linear interpolation.\n"
              << "repeat,run,seed,boards,wall_seconds,solve_seconds,cpu_seconds,"
                 "median_seconds,p5_seconds,p95_seconds,validation\n";
    std::array<std::vector<BenchmarkSetResult>, 3> results;
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
        for (size_t run = 0; run < expert_benchmark::seeds.size(); ++run) {
            const uint64_t seed = expert_benchmark::seeds[run];
            const std::string label = "Benchmark repeat " + std::to_string(repeat + 1) +
                "/" + std::to_string(repeats) + ", run " +
                std::to_string(run + 1) + "/3, seed " + std::to_string(seed);
            BenchmarkSetResult result = run_benchmark_set(
                run, options, references, expert_benchmark::board_count,
                true, options.progress, label);
            results[run].push_back(result);
            std::cout << repeat + 1 << ',' << run + 1 << ',' << seed << ','
                      << result.solve_times.size() << ',' << std::fixed
                      << std::setprecision(6) << result.wall_seconds << ','
                      << result.solve_seconds << ',' << result.solve_cpu_seconds << ','
                      << benchmark_percentile(result.solve_times, 0.50) << ','
                      << benchmark_percentile(result.solve_times, 0.05) << ','
                      << benchmark_percentile(result.solve_times, 0.95)
                      << ",PASS\n" << std::flush;
        }
    }
    print_benchmark_aggregates(results);
    std::cout << "Validated all "
              << repeats * expert_benchmark::seeds.size() * expert_benchmark::board_count
              << " boards against fixed board, 3BV, and optimal-click references.\n";
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.benchmark) {
            run_benchmark(options);
            return 0;
        }
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
            std::cout << "board,3bv,optimal_clicks,solve_seconds\n";
            const auto bulk_start = std::chrono::steady_clock::now();
            std::vector<double> solve_times;
            solve_times.reserve(options.bulk_count);
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
                    double solve_seconds = 0.0;
                    std::string error;
                };
                std::vector<Job> jobs(batch);
                for (size_t j = 0; j < batch; ++j) {
                    jobs[j].board = generate_random_board(*options.bulk, rng);
                    jobs[j].encoded = format_pttacg_string(jobs[j].board);
                }
                std::atomic<size_t> next{0};
                auto work = [&] {
                    for (;;) {
                        const size_t j = next.fetch_add(1, std::memory_order_relaxed);
                        if (j >= batch) break;
                        try {
                            const auto solve_start = std::chrono::steady_clock::now();
                            Model model = build_model(jobs[j].board);
                            Solution solution = solve_frontier(
                                model, options.order, options.band_size, false, count_only);
                            const auto solve_end = std::chrono::steady_clock::now();
                            jobs[j].three_bv = model.three_bv();
                            jobs[j].clicks = solution.clicks;
                            jobs[j].solve_seconds = std::chrono::duration<double>(
                                solve_end - solve_start).count();
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
                              << job.clicks << ',' << std::fixed << std::setprecision(6)
                              << job.solve_seconds << '\n';
                    solve_times.push_back(job.solve_seconds);
                }
                begin += batch;
            }
            const auto bulk_end = std::chrono::steady_clock::now();
            const double seconds =
                std::chrono::duration<double>(bulk_end - bulk_start).count();
            std::cerr << "Bulk solve time: " << std::fixed << std::setprecision(3)
                      << seconds << " seconds\n";
            std::sort(solve_times.begin(), solve_times.end());
            const auto percentile = [&](double p) {
                const double rank = p * static_cast<double>(solve_times.size() - 1);
                const size_t lower = static_cast<size_t>(rank);
                const size_t upper = std::min(lower + 1, solve_times.size() - 1);
                const double fraction = rank - static_cast<double>(lower);
                return solve_times[lower] * (1.0 - fraction) + solve_times[upper] * fraction;
            };
            std::cerr << "Per-board solve time (seconds, linear interpolation): median "
                      << std::fixed << std::setprecision(6) << percentile(0.50)
                      << ", p5 " << percentile(0.05)
                      << ", p95 " << percentile(0.95) << "\n";
            return 0;
        }
        Board board;
        std::optional<std::string> generated_url;
        std::optional<std::string> imported_url;
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
            if (is_world_game_url(trim(*options.board))) {
                imported_url = format_llamasweeper_url(board);
                std::cerr << "Imported board: " << *imported_url << "\n";
            }
        }

        const auto start = std::chrono::steady_clock::now();
        Model model = build_model(board);
        const auto model_end = options.profile ? std::chrono::steady_clock::now() : start;
        Solution solution = solve_frontier(
            model, options.order, options.band_size, options.progress,
            options.count_only.value_or(false), options.profile);
        const auto end = std::chrono::steady_clock::now();
        solution.solve_seconds = std::chrono::duration<double>(end - start).count();
        if (options.profile) {
            auto& p = solution.profile;
            p.model = std::chrono::duration<double>(model_end - start).count();
            std::cerr << std::fixed << std::setprecision(3)
                      << "Profile: model " << p.model << " s; candidate reduction "
                      << solution.reduction_seconds << " s; ordering " << p.ordering
                      << " s; factor plan " << p.factor_plan << " s\n"
                      << "Profile: DP setup " << p.dp_setup << " s; transitions "
                      << p.transitions << " s; dominance " << p.dominance
                      << " s; move reconstruction " << p.replay << " s\n"
                      << "Profile: dominance detail: setup " << p.dominance_setup
                      << " s; same-connectivity " << p.dominance_exact
                      << " s; cross-connectivity " << p.dominance_cross
                      << " s (forward " << p.dominance_forward
                      << " s, reverse " << p.dominance_reverse << " s)"
                      << "; erasure " << p.dominance_erase
                      << " s; cleanup/other " << std::max(0.0, p.dominance -
                          p.dominance_setup - p.dominance_exact -
                          p.dominance_cross - p.dominance_erase) << " s\n"
                      << "Profile: processed " << comma_number(p.processed_states)
                      << " states; generated " << comma_number(p.generated_states)
                      << " pre-pruning states\n";
            std::cerr << "Profile: conditional branches skipped " << comma_number(p.conditional_pruned) << "\n";
            std::cerr << "Profile: dominance passes " << p.dominance_passes
                      << "; deferred intermediate cuts " << p.deferred_layers
                      << "; peak generated states " << comma_number(p.peak_generated_states) << "\n";
            std::cerr << "Profile: dominance removed " << comma_number(p.exact_pruned)
                      << " exact, " << comma_number(p.forward_pruned)
                      << " forward, " << comma_number(p.reverse_pruned)
                      << " reverse\n";
        }
        std::cerr << "Solve time: " << std::fixed << std::setprecision(3)
                  << solution.solve_seconds << " seconds\n";

        if (options.click_tuples) {
            print_click_tuples(model, solution);
            return 0;
        }
        if (options.json) {
            print_json(model, solution, options.generate, generated_seed,
                       generated_url, imported_url);
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

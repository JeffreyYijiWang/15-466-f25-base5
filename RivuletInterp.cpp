// rivulet_interpreter.cpp
// C++17 port of the Rivulet Interpreter core (single file).
// Build: g++ -std=c++17 -O2 -o rivulet rivulet_interpreter.cpp
//help with ChatGPT-4
//reference from https://github.com/rottytooth/Rivulet/blob/main/rivulet/riv_interpreter.py

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// -------------------- Exceptions --------------------
struct RivuletSyntaxError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// -------------------- Token / Glyph model --------------------
struct ActionInfo {
    // Mirrors token["action"]
    std::string command;              // e.g., "addition_assignment", "append", etc.
    std::optional<std::string> subtype; // e.g., "list", "list2list"
};

struct Token {
    // Minimal mirror of Python dict-based tokens.
    std::string type;                 // "question_marker" or value/ref marker
    std::string subtype;              // "value" or "ref" (for non-question), else unused
    std::optional<ActionInfo> action;

    // Assignment target
    std::optional<int> list;          // token["list"]
    std::optional<int> assign_to_cell;// token["assign_to_cell"]

    // Source references
    std::optional<std::pair<int,int>> ref_cell; // (list_id, cell_idx)
    std::optional<int> ref_list;      // list id for question markers on lists
    std::optional<double> value;      // literal value

    // Question/loop
    std::optional<std::string> block_type; // e.g., "while"
    std::optional<std::string> applies_to; // "cell" | "list"
};

struct Glyph {
    int id = -1;                 // assigned by interpreter
    int level = 0;               // indentation level (set/overwritten during decorate)
    int list_size = 0;           // used to determine # of lists (as in Python)
    std::u32string glyph;        // original glyph (for pretty/debug; optional to fill)
    std::vector<Token> tokens;

    // Decorations:
    std::optional<int> first;        // id of first glyph in this block
    std::optional<int> following;    // id of next glyph after this block
};

// Node is either a Glyph or a Block (= vector<Node>)
struct Node;
using Block = std::vector<Node>;
struct Node {
    std::variant<Glyph, Block> v;
    Node() = default;
    Node(const Glyph& g) : v(g) {}
    Node(const Block& b) : v(b) {}
    bool isGlyph() const { return std::holds_alternative<Glyph>(v); }
    bool isBlock() const { return std::holds_alternative<Block>(v); }
    Glyph& asGlyph() { return std::get<Glyph>(v); }
    const Glyph& asGlyph() const { return std::get<Glyph>(v); }
    Block& asBlock() { return std::get<Block>(v); }
    const Block& asBlock() const { return std::get<Block>(v); }
};

// -------------------- Stubs you can replace --------------------
struct PythonTranspiler {
    std::string glyph_drawn(const std::u32string&) { return "[glyph drawn]"; }
    std::string glyph_pseudo(const Glyph&) { return "[pseudo-code]"; }
    std::string print_program(const std::vector<Glyph>&) { return "[program pseudo]"; }
};

struct SvgGenerator {
    explicit SvgGenerator(const std::string&) {}
    void generate(const std::vector<Glyph>&) {
        std::cout << "[SVG generation stub]\n";
    }
};

struct Parser {
    // Replace this with your actual C++ parser that fills glyphs/tokens.
    std::vector<Glyph> parse_program(const std::string& program) {
        (void)program;
        // Stub: throw so it’s obvious you need to plug in the real thing.
        throw RivuletSyntaxError("Parser::parse_program not implemented (stub).");
    }
};

// -------------------- Interpreter --------------------
class Interpreter {
public:
    enum class OutputOption { None, Unicode, Numeric };
    enum class Action { Rollback = 1, Cont = 2, Repeat = 3 };

    Interpreter() = default;

    void interpret_file(const std::string& progfile,
                        bool verbose,
                        const std::optional<std::vector<int>>& start_state,
                        OutputOption output) {
        verbose_ = verbose;
        output_ = output;

        std::ifstream ifs(progfile);
        if (!ifs) throw std::runtime_error("Could not open file: " + progfile);
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        interpret_program(buffer.str(), verbose, start_state, nullptr);
    }

    void interpret_program(const std::string& program,
                           bool verbose,
                           const std::optional<std::vector<int>>& start_state,
                           std::function<void(const std::unordered_map<int, std::vector<double>>&)> debug) {
        verbose_ = verbose;

        Parser parser;
        auto glyphs = parser.parse_program(program);

        // assign IDs
        for (size_t i=0;i<glyphs.size();++i) glyphs[i].id = static_cast<int>(i);

        interpret(glyphs, start_state, std::move(debug));
    }

    void print_and_exit(const std::string& progfile) {
        std::ifstream ifs(progfile);
        if (!ifs) throw std::runtime_error("Could not open file: " + progfile);
        std::stringstream buffer; buffer << ifs.rdbuf();

        Parser parser;
        auto glyphs = parser.parse_program(buffer.str());

        debug_ = PythonTranspiler();
        std::cout << debug_.print_program(glyphs) << "\n";
    }

    void draw_svg(const std::string& progfile, const std::string& theme) {
        std::ifstream ifs(progfile);
        if (!ifs) throw std::runtime_error("Could not open file: " + progfile);
        std::stringstream buffer; buffer << ifs.rdbuf();

        Parser parser;
        auto glyphs = parser.parse_program(buffer.str());
        SvgGenerator svg(theme);
        svg.generate(glyphs);
    }

private:
    using State = std::unordered_map<int, std::vector<double>>;

    void interpret(std::vector<Glyph> glyphs,
                   const std::optional<std::vector<int>>& start_state,
                   std::function<void(const State&)> debug) {
        // Initialize state with list 1
        State state;
        state.emplace(1, std::vector<double>{});

        // Determine prime_size = max list_size
        if (glyphs.empty()) return;
        int prime_size = 0;
        for (const auto& g : glyphs) prime_size = std::max(prime_size, g.list_size);

        // Initialize lists with primes starting from 2
        for (int num = 2; static_cast<int>(state.size()) < prime_size; ++num) {
            bool isPrime = (num >= 2);
            for (int i=2; i*i<=num; ++i) {
                if (num % i == 0) { isPrime = false; break; }
            }
            if (isPrime) state.emplace(num, std::vector<double>{});
        }

        // Input goes to list 2 if provided
        if (start_state.has_value()) {
            auto& v = state[2];
            v.clear();
            for (int x : *start_state) v.push_back(static_cast<double>(x));
        }

        if (verbose_) debug_ = PythonTranspiler();

        // Build block tree from flat glyphs by their original "level"
        // First, we need a copy because treeify will pop from front
        auto glyphCopy = glyphs;
        Node rootBlock = Node(Block{});
        treeify_glyphs(glyphCopy, /*curr_level=*/1, std::get<Block>(rootBlock.v));

        // Decorate with first/following and re-level (starting at 0)
        decorate_blocks(rootBlock, /*level=*/0, /*following=*/std::nullopt);

        // Interpret
        state = interpret_block(rootBlock, state, debug);

        // Output
        auto convert_num = [](double n) -> double {
            if (std::floor(n) == n) return static_cast<double>(static_cast<long long>(n));
            return n;
        };

        auto it = state.find(1);
        if (it != state.end()) {
            if (output_ == OutputOption::Unicode) {
                std::u32string out;
                for (double num : it->second) {
                    long long v = static_cast<long long>(std::floor(num));
                    if (0 <= v && v <= 0x10FFFF) out.push_back(static_cast<char32_t>(v));
                }
                // naive UTF-32 -> UTF-8 for BMP only (for brevity); extend as needed
                for (char32_t c : out) {
                    if (c < 0x80) std::cout << static_cast<char>(c);
                    else if (c < 0x800) {
                        std::cout << static_cast<char>(0xC0 | (c >> 6));
                        std::cout << static_cast<char>(0x80 | (c & 0x3F));
                    } else if (c < 0x10000) {
                        std::cout << static_cast<char>(0xE0 | (c >> 12));
                        std::cout << static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        std::cout << static_cast<char>(0x80 | (c & 0x3F));
                    } else {
                        std::cout << '?'; // keep simple
                    }
                }
                std::cout << "\n";
            } else if (output_ == OutputOption::Numeric) {
                bool first = true;
                for (double num : it->second) {
                    if (!first) std::cout << ' ';
                    first = false;
                    double n = convert_num(num);
                    // Print as integer if integral, else as double
                    if (std::floor(n) == n) std::cout << static_cast<long long>(n);
                    else std::cout << n;
                }
                std::cout << "\n";
            }
        }

        if (debug) debug(state);
    }

    // Reorganize a flat vector<Glyph> into a nested block tree by "level".
    void treeify_glyphs(std::vector<Glyph>& glyphs, int curr_level, Block& out) {
        if (glyphs.empty()) return;

        if (glyphs.front().level == curr_level) {
            out.emplace_back(glyphs.front());
            glyphs.erase(glyphs.begin());
        } else if (glyphs.front().level > curr_level) {
            // descend: create a nested block
            Block levelBlock;
            out.emplace_back(levelBlock);
            Block& child = std::get<Block>(out.back().v);
            treeify_glyphs(glyphs, curr_level + 1, child);
        } else {
            // ascend (end of this block)
            return;
        }

        if (!glyphs.empty()) {
            treeify_glyphs(glyphs, curr_level, out);
        }
    }

    // Decorate blocks so each glyph knows the first glyph of its block and the following glyph after block.
    void decorate_blocks(Node& n, int level, std::optional<Glyph> following) {
        if (n.isGlyph()) {
            auto& g = n.asGlyph();
            g.level = level;
            if (following.has_value()) g.following = following->id;
            else g.following.reset();
            // 'first' is set by parent block logic below
            return;
        }
        Block& block = n.asBlock();
        if (block.empty()) return;

        // Find first glyph in this block (even if nested)
        Glyph firstGlyph = find_first_descendant_glyph(block.front());
        // Walk the block
        for (size_t idx=0; idx<block.size(); ++idx) {
            // Compute "following" for nested sub-blocks: next glyph or its first descendant
            std::optional<Glyph> nextFollowing = following;
            if (idx + 1 < block.size()) {
                Glyph f = find_first_descendant_glyph(block[idx+1]);
                nextFollowing = f;
            }

            if (block[idx].isGlyph()) {
                auto& g = block[idx].asGlyph();
                g.first = firstGlyph.id;
                g.level = level;
                g.following = nextFollowing ? std::optional<int>(nextFollowing->id) : std::optional<int>{};
            } else {
                // Nested block inherits or updates 'following' based on the next item
                decorate_blocks(block[idx], level+1, nextFollowing);
            }
        }
    }

    Glyph find_first_descendant_glyph(const Node& n) const {
        if (n.isGlyph()) return n.asGlyph();
        const Block& b = n.asBlock();
        if (b.empty()) throw RivuletSyntaxError("Empty block encountered");
        return find_first_descendant_glyph(b.front());
    }

    State interpret_block(const Node& n, State state,
                          const std::function<void(const State&)>& debug) {
        if (n.isGlyph()) {
            interpret_glyph(n.asGlyph(), state, debug);
            return state;
        }

        // Block execution with rollback checkpoint
        State rollback_state = state;
        Block const& block = n.asBlock();

        for (const auto& item : block) {
            if (item.isBlock()) {
                state = interpret_block(item, state, debug);
            } else {
                auto action = interpret_glyph(item.asGlyph(), state, debug);
                if (action == Action::Rollback) {
                    state = rollback_state;
                    return state; // exit block on rollback
                } else if (action == Action::Cont) {
                    // continue
                } else if (action == Action::Repeat) {
                    state = interpret_block(n, state, debug); // repeat the whole block
                }
            }
        }
        return state;
    }

    Action interpret_glyph(const Glyph& glyph, State& state,
                           const std::function<void(const State&)>& debug) {
        Action retval = Action::Cont;

        for (const auto& token : glyph.tokens) {
            if (token.type == "question_marker") {
                retval = resolve_question(token, state);
            } else {
                // Ensure cell exists for assignment (append 0) for certain paths
                auto needs_cell_init = [&]() -> bool {
                    if (!token.assign_to_cell.has_value() || !token.list.has_value()) return false;
                    int L = *token.list;
                    int idx = *token.assign_to_cell;
                    const bool is_pop_or_append =
                        token.action.has_value() &&
                        (token.action->command == "pop_and_append" || token.action->command == "append");
                    if (is_pop_or_append) return false;
                    auto& vec = state[L];
                    return static_cast<int>(vec.size()) == idx;
                };
                if (needs_cell_init()) {
                    state[*token.list].push_back(0.0);
                }

                // Detect list2list
                bool list2list = token.action.has_value() &&
                                 token.action->subtype.has_value() &&
                                 *token.action->subtype == "list2list";

                // Resolve source
                std::vector<double> sourceList; // for list2list as array
                double sourceScalar = 0.0;
                bool sourceIsList = false;

                if (list2list) {
                    if (!token.ref_cell.has_value())
                        throw RivuletSyntaxError("list2list requires ref_cell list id");
                    int srcList = token.ref_cell->first;
                    sourceList = state[srcList];
                    sourceIsList = true;
                } else if (token.subtype == "value") {
                    sourceScalar = token.value.value_or(0.0);
                } else if (token.subtype == "ref") {
                    // exclude list2list handled above
                    if (!token.ref_cell.has_value())
                        throw RivuletSyntaxError("ref token missing ref_cell");
                    int L = token.ref_cell->first;
                    int idx = token.ref_cell->second;
                    if (!state.count(L)) throw RivuletSyntaxError("List reference out of bounds");
                    auto& vec = state[L];
                    if (idx >= static_cast<int>(vec.size())) sourceScalar = 0.0;
                    else sourceScalar = vec[idx];
                }

                // Apply to destination
                if (list2list) {
                    const std::string cmd = token.action->command;
                    int dstList = token.list.value_or(-1);
                    if (dstList < 0) throw RivuletSyntaxError("Missing destination list for list2list");

                    if (cmd == "pop_and_append") {
                        int src = token.ref_cell->first;
                        if (state[src].empty()) state[dstList].push_back(0.0);
                        else {
                            double v = state[src].back();
                            state[src].pop_back();
                            state[dstList].push_back(v);
                        }
                    } else if (cmd == "append") {
                        int src = token.ref_cell->first;
                        if (state[src].empty()) throw RivuletSyntaxError("append from empty source list");
                        state[dstList].push_back(state[src].back());
                    } else {
                        // elementwise with resize via zeros
                        auto& dst = state[dstList];
                        if (static_cast<int>(dst.size()) < static_cast<int>(sourceList.size())) {
                            dst.resize(sourceList.size(), 0.0);
                        }
                        for (size_t i=0;i<sourceList.size();++i) {
                            dst[i] = resolve_cmd(token, dst[i], sourceList[i]);
                        }
                    }
                } else if (!token.action.has_value() || token.action->command.empty()) {
                    // default: add-assign
                    int L = token.list.value_or(-1);
                    int idx = token.assign_to_cell.value_or(-1);
                    if (L < 0 || idx < 0) throw RivuletSyntaxError("Missing list/cell for default assignment");
                    state[L][idx] += sourceScalar;
                } else {
                    const auto& cmd = token.action->command;
                    if (cmd == "insert") {
                        int L = token.list.value_or(-1);
                        int idx = token.assign_to_cell.value_or(-1);
                        if (L < 0 || idx < 0) throw RivuletSyntaxError("insert missing list/cell");
                        auto& v = state[L];
                        if (idx < 0 || idx > static_cast<int>(v.size())) idx = static_cast<int>(v.size());
                        v.insert(v.begin() + idx, sourceScalar);
                    } else if (cmd == "append") {
                        int L = token.list.value_or(-1);
                        if (L < 0) throw RivuletSyntaxError("append missing list");
                        state[L].push_back(sourceScalar);
                    } else if (cmd == "pop") {
                        int L = token.list.value_or(-1);
                        int idx = token.assign_to_cell.value_or(-1);
                        if (L < 0 || idx < 0) throw RivuletSyntaxError("pop assignment missing dst");
                        state[L][idx] += sourceScalar;
                        if (token.subtype == "ref" && token.ref_cell.has_value()) {
                            int srcL = token.ref_cell->first;
                            int srcIdx = token.ref_cell->second;
                            auto& srcV = state[srcL];
                            if (srcIdx >= 0 && srcIdx < static_cast<int>(srcV.size()))
                                srcV.erase(srcV.begin() + srcIdx);
                        }
                    } else if (cmd == "pop_and_append") {
                        if (!token.ref_cell.has_value() || !token.list.has_value())
                            throw RivuletSyntaxError("pop_and_append missing refs");
                        int srcL = token.ref_cell->first;
                        int srcIdx = token.ref_cell->second;
                        auto& srcV = state[srcL];
                        if (srcIdx < 0 || srcIdx >= static_cast<int>(srcV.size()))
                            throw RivuletSyntaxError("pop_and_append out of bounds");
                        double v = srcV[srcIdx];
                        srcV.erase(srcV.begin() + srcIdx);
                        state[*token.list].push_back(v);
                    } else if (token.action->subtype.has_value() && *token.action->subtype == "list") {
                        int L = token.list.value_or(-1);
                        if (L < 0) throw RivuletSyntaxError("list-subtype action missing list");
                        auto& dst = state[L];
                        for (double& d : dst) d = resolve_cmd(token, d, sourceScalar);
                    } else {
                        int L = token.list.value_or(-1);
                        int idx = token.assign_to_cell.value_or(-1);
                        if (L < 0 || idx < 0) throw RivuletSyntaxError("assignment missing list/cell");
                        state[L][idx] = resolve_cmd(token, state[L][idx], sourceScalar);
                    }
                }
            }
        }

        if (debug) debug(state);
        if (verbose_) {
            std::cout << debug_.glyph_drawn(glyph.glyph) << "\n";
            std::cout << debug_.glyph_pseudo(glyph) << "\n";
            print_state(state);
            std::cout << "\n";
        } else if (output_ == OutputOption::None) {
            std::cout << " \n";
            std::cout << "glyph: " << glyph.id << "\n";
            for (auto& [k, v] : state) {
                if (!v.empty()) {
                    std::cout << k << ": [";
                    for (size_t i=0;i<v.size();++i) {
                        if (i) std::cout << ", ";
                        std::cout << v[i];
                    }
                    std::cout << "]\n";
                }
            }
        }
        return retval;
    }

    double resolve_cmd(const Token& token, double initial_value, double assign_value) {
        if (!token.action.has_value())
            throw RivuletSyntaxError("No command found in token");
        const std::string& cmd = token.action->command;

        if (cmd == "addition_assignment") return initial_value + assign_value;
        if (cmd == "subtraction_assignment") return initial_value - assign_value;
        if (cmd == "reverse_subtraction_assignment") return assign_value - initial_value;
        if (cmd == "overwrite") return assign_value;
        if (cmd == "multiplication_assignment") return initial_value * assign_value;
        if (cmd == "division_assignment") return initial_value / assign_value;
        if (cmd == "reverse_division_assignment") return assign_value / initial_value;
        if (cmd == "mod_assignment") return std::fmod(initial_value, assign_value);
        if (cmd == "reverse_mod_assignment") {
            if (initial_value == 0.0) return 0.0;
            return std::fmod(assign_value, initial_value);
        }
        if (cmd == "exponent_assignment") return std::pow(initial_value, assign_value);
        if (cmd == "root_assignment") return std::pow(initial_value, 1.0 / assign_value);

        throw RivuletSyntaxError("Unknown command: " + cmd);
    }

    Action resolve_question(const Token& token, const State& state) {
        bool succeeds = false;

        if (!token.applies_to.has_value())
            throw RivuletSyntaxError("Question marker missing applies_to");
        const std::string& applies = *token.applies_to;

        if (applies == "cell") {
            if (!token.ref_cell.has_value())
                throw RivuletSyntaxError("Question on cell missing ref_cell");
            int L = token.ref_cell->first;
            int idx = token.ref_cell->second;
            auto it = state.find(L);
            if (it == state.end()) succeeds = false;
            else {
                const auto& v = it->second;
                if (idx >= static_cast<int>(v.size())) succeeds = false;
                else succeeds = v[idx] > 0.0;
            }
        } else if (applies == "list") {
            if (!token.ref_list.has_value())
                throw RivuletSyntaxError("Question on list missing ref_list");
            int L = *token.ref_list;
            auto it = state.find(L);
            if (it == state.end() || it->second.empty()) succeeds = false;
            else {
                const auto& v = it->second;
                bool allZero = std::all_of(v.begin(), v.end(), [](double x){ return x == 0.0; });
                bool anyNeg = std::any_of(v.begin(), v.end(), [](double x){ return x < 0.0; });
                succeeds = (!allZero) && (!anyNeg);
            }
        } else {
            throw RivuletSyntaxError("Could not determine what question marker applies to");
        }

        if (succeeds) {
            if (token.block_type.has_value() && *token.block_type == "while")
                return Action::Repeat;
            return Action::Cont;
        } else {
            return Action::Rollback;
        }
    }

    void print_state(const State& state) {
        // Find highest key with non-empty list
        int lastKey = -1;
        for (const auto& kv : state) {
            if (!kv.second.empty()) {
                if (kv.first > lastKey) lastKey = kv.first;
            }
        }
        if (lastKey < 0) {
            // All empty
            std::cout << "{ }";
            return;
        }
        // Print truncated map (keys <= lastKey)
        std::vector<int> keys;
        keys.reserve(state.size());
        for (const auto& kv : state) if (kv.first <= lastKey) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        std::cout << "{\n";
        for (size_t i=0;i<keys.size();++i) {
            int k = keys[i];
            const auto& v = state.at(k);
            std::cout << "  " << k << ": [";
            for (size_t j=0;j<v.size();++j) {
                if (j) std::cout << ", ";
                std::cout << v[j];
            }
            std::cout << "]";
            if (i+1<keys.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "}\n";
    }

private:
    bool verbose_ = false;
    OutputOption output_ = OutputOption::None;
    PythonTranspiler debug_;
};

// -------------------- CLI helpers --------------------
static std::vector<int> parse_list_of_ints(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(std::stoi(item));
    }
    return out;
}

static Interpreter::OutputOption parse_output(const std::string& s) {
    if (s == "none") return Interpreter::OutputOption::None;
    if (s == "unicode") return Interpreter::OutputOption::Unicode;
    if (s == "numeric") return Interpreter::OutputOption::Numeric;
    throw std::runtime_error("Invalid -o value (use: none|unicode|numeric)");
}

// -------------------- main --------------------
int main(int argc, char** argv) {
    auto usage = [](){
        std::cerr <<
        "Rivulet Interpreter (C++ port)\n"
        "Usage: rivulet progfile [-i 1,2,3] [-p] [-v] [-o none|unicode|numeric] [--svg] [--theme NAME]\n";
    };

    if (argc < 2) { usage(); return 1; }

    std::string progfile;
    std::optional<std::vector<int>> input;
    bool print_only = false;
    bool verbose = false;
    bool svg = false;
    std::string theme = "default";
    Interpreter::OutputOption output = Interpreter::OutputOption::None;

    progfile = argv[1];
    for (int i=2;i<argc;++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i+1 < argc) {
            input = parse_list_of_ints(argv[++i]);
        } else if (arg == "-p") {
            print_only = true;
        } else if (arg == "-v") {
            verbose = true;
        } else if (arg == "-o" && i+1 < argc) {
            output = parse_output(argv[++i]);
        } else if (arg == "--svg") {
            svg = true;
        } else if (arg == "--theme" && i+1 < argc) {
            theme = argv[++i];
        } else {
            // ignore unknown
        }
    }

    try {
        Interpreter intr;
        if (print_only) {
            intr.print_and_exit(progfile);
            return 0;
        }
        if (svg) {
            intr.draw_svg(progfile, theme);
            return 0;
        }
        intr.interpret_file(progfile, verbose, input, output);
    } catch (const RivuletSyntaxError& e) {
        std::cerr << "RivuletSyntaxError: " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 3;
    }
    return 0;
}

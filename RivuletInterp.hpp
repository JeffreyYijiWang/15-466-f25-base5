//help with ChatGPT-4
//reference from https://github.com/rottytooth/Rivulet/blob/main/rivulet/riv_interpreter.py


#ifndef RIVULET_INTERPRETER_HPP
#define RIVULET_INTERPRETER_HPP

// rivulet_interpreter.hpp
// Public header for the C++17 Rivulet interpreter core.
// Provide definitions in a corresponding .cpp.

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rivulet {

// -------------------- Exceptions --------------------
struct RivuletSyntaxError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// -------------------- Token / Glyph model --------------------
struct ActionInfo {
    // Mirrors token["action"] in the Python version.
    std::string command;                       // e.g., "addition_assignment", "append", ...
    std::optional<std::string> subtype;        // e.g., "list", "list2list"
};

struct Token {
    // Minimal mirror of Python dict-based tokens.
    std::string type;                          // "question_marker" or non-question
    std::string subtype;                       // "value" | "ref" (for non-question)
    std::optional<ActionInfo> action;

    // Destination
    std::optional<int> list;                   // target list id
    std::optional<int> assign_to_cell;         // target cell index

    // Sources
    std::optional<std::pair<int,int>> ref_cell;// (list_id, cell_idx)
    std::optional<int> ref_list;               // list id for list-question markers
    std::optional<double> value;               // literal numeric value

    // Question/loop metadata
    std::optional<std::string> block_type;     // e.g., "while"
    std::optional<std::string> applies_to;     // "cell" | "list"
};

struct Glyph {
    int id = -1;                               // assigned by interpreter
    int level = 0;                             // indentation/structure level
    int list_size = 0;                         // used to determine # of lists
    std::u32string glyph;                      // optional pretty/debug data
    std::vector<Token> tokens;

    // Decorations (filled during decoration pass)
    std::optional<int> first;                  // id of first glyph in this block
    std::optional<int> following;              // id of glyph following this block
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
    Glyph&       asGlyph()       { return std::get<Glyph>(v); }
    const Glyph& asGlyph() const { return std::get<Glyph>(v); }
    Block&       asBlock()       { return std::get<Block>(v); }
    const Block& asBlock() const { return std::get<Block>(v); }
};

// -------------------- Public stubs (replace with real impls if you have them) --------------------
struct PythonTranspiler {
    std::string glyph_drawn(const std::u32string& /*g*/);
    std::string glyph_pseudo(const Glyph& /*g*/);
    std::string print_program(const std::vector<Glyph>& /*glyphs*/);
};

struct SvgGenerator {
    explicit SvgGenerator(const std::string& /*theme*/);
    void generate(const std::vector<Glyph>& /*glyphs*/);
};

struct Parser {
    // Must return a flat list of Glyphs with their original "level", "list_size", and "tokens".
    std::vector<Glyph> parse_program(const std::string& program);
};

// -------------------- Interpreter --------------------
class Interpreter {
public:
    enum class OutputOption { None, Unicode, Numeric };
    enum class Action { Rollback = 1, Cont = 2, Repeat = 3 };

    using State = std::unordered_map<int, std::vector<double>>;

    Interpreter();

    // Interpret program from file path.
    void interpret_file(const std::string& progfile,
                        bool verbose,
                        const std::optional<std::vector<int>>& start_state,
                        OutputOption output);

    // Interpret program from string.
    // 'debug' (optional) is called with the state after each glyph.
    void interpret_program(const std::string& program,
                           bool verbose,
                           const std::optional<std::vector<int>>& start_state,
                           std::function<void(const State&)> debug);

    // Print pseudo-code and exit (uses PythonTranspiler-like interface).
    void print_and_exit(const std::string& progfile);

    // Generate an SVG for the program (requires a real SvgGenerator if used).
    void draw_svg(const std::string& progfile, const std::string& theme);

private:
    // Core driver over parsed glyphs.
    void interpret(std::vector<Glyph> glyphs,
                   const std::optional<std::vector<int>>& start_state,
                   std::function<void(const State&)> debug);

    // Reorganize flat glyphs (by level) into a nested block tree.
    static void treeify_glyphs(std::vector<Glyph>& glyphs,
                               int curr_level,
                               Block& out);

    // Decorate blocks so each glyph knows the first glyph of its block and the following glyph.
    void decorate_blocks(Node& n, int level, std::optional<Glyph> following);

    // Find the first glyph in a (possibly nested) node.
    Glyph find_first_descendant_glyph(const Node& n) const;

    // Execute one block (or single glyph node) with rollback semantics.
    State interpret_block(const Node& n,
                          State state,
                          const std::function<void(const State&)>& debug);

    // Execute a single glyph.
    Action interpret_glyph(const Glyph& glyph,
                           State& state,
                           const std::function<void(const State&)>& debug);

    // Arithmetic/assignment helpers.
    static double resolve_cmd(const Token& token,
                              double initial_value,
                              double assign_value);

    // Evaluate a question marker (condition).
    Action resolve_question(const Token& token,
                            const State& state);

    // Pretty-print state (truncated to last non-empty list id).
    static void print_state(const State& state);

private:
    bool verbose_ = false;
    OutputOption output_ = OutputOption::None;
    PythonTranspiler debug_; // used when verbose_ is true
};

} // namespace rivulet

#endif // RIVULET_INTERPRETER_HPP

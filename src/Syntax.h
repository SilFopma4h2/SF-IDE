// Syntax - a fast single-line tokenizer for highlighting, driven by the
// language table in Languages.h.
//
// Lines are tokenised on demand (only the visible ones). The only state
// carried from one line to the next is whether it starts inside a block
// comment or a multi-line string; Document precomputes that once per edit.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct Language;

namespace syntax {

enum class Kind : uint8_t { Default, Keyword, Type, String, Number, Comment, Preproc, Function, Punct };

struct Token {
    uint32_t begin, end;  // byte offsets inside the line
    Kind     kind;
};

// Line states: what a line starts inside of.
enum : uint8_t { StateNormal = 0, StateBlockComment, StateTripleDouble, StateTripleSingle, StateBacktick };

// Fills `out` (when not null) with tokens covering the whole line, in order,
// and returns the state the next line starts in.
uint8_t tokenize(const Language& lang, std::string_view line, uint8_t startState, std::vector<Token>* out);

} // namespace syntax

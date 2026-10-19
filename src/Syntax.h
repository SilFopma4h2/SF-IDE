// Syntax - a fast single-line tokenizer for highlighting.
//
// Lines are tokenised on demand (only the visible ones); the only state
// carried between lines is "inside a block comment", which Document
// precomputes once per edit.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "Document.h"

namespace syntax {

enum class Kind : uint8_t { Default, Keyword, Type, String, Number, Comment, Preproc, Function, Punct };

struct Token {
    uint32_t begin, end;  // byte offsets inside the line
    Kind     kind;
};

// Fills `out` with tokens covering the whole line, in order.
void tokenize(Document::Lang lang, std::string_view line, bool startsInBlockComment, std::vector<Token>& out);

} // namespace syntax

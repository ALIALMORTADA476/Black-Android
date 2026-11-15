#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arabic {

// Unicode codepoint ranges
inline constexpr char32_t kArabicStart = 0x0600;
inline constexpr char32_t kArabicEnd = 0x06FF;

// Glyph form table: (base, isolated, final, initial, medial, right_joining_only)
struct GlyphData {
    char32_t base;
    char32_t iso;
    char32_t fin;
    char32_t ini;
    char32_t med;
    bool right_only;
};

inline constexpr GlyphData kGlyphs[] = {
    {0x0621, 0xFE80, 0xFE80, 0xFE80, 0xFE80, true},   // Hamza (non-joining)
    {0x0622, 0xFE81, 0xFE82, 0xFE81, 0xFE82, true},   // Alef Madda
    {0x0623, 0xFE83, 0xFE84, 0xFE83, 0xFE84, true},   // Alef Hamza Above
    {0x0624, 0xFE85, 0xFE86, 0xFE85, 0xFE86, true},   // Waw Hamza
    {0x0625, 0xFE87, 0xFE88, 0xFE87, 0xFE88, true},   // Alef Hamza Below
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, false},  // Yeh Hamza
    {0x0627, 0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E, true},   // Alef
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, false},  // Beh
    {0x0629, 0xFE93, 0xFE94, 0xFE93, 0xFE94, true},   // Teh Marbuta
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, false},  // Teh
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, false},  // Theh
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, false},  // Jeem
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, false},  // Hah
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, false},  // Khah
    {0x062F, 0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA, true},   // Dal
    {0x0630, 0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC, true},   // Thal
    {0x0631, 0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE, true},   // Reh
    {0x0632, 0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0, true},   // Zain
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, false},  // Seen
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, false},  // Sheen
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, false},  // Sad
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, false},  // Dad
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, false},  // Tah
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, false},  // Zah
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, false},  // Ain
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, false},  // Ghain
    {0x0640, 0x0640, 0x0640, 0x0640, 0x0640, false},  // Tatweel (Kashida)
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, false},  // Feh
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, false},  // Qaf
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, false},  // Kaf
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, false},  // Lam
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, false},  // Meem
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, false},  // Noon
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, false},  // Heh
    {0x0648, 0xFEED, 0xFEEE, 0xFEED, 0xFEEE, true},   // Waw
    {0x0649, 0xFEEF, 0xFEF0, 0xFEEF, 0xFEF0, true},   // Alef Maksura
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, false},  // Yeh
};

// Lam-Alef ligatures: (alef_base, isolated, final)
struct LamAlefPair {
    char32_t alef;
    char32_t iso;
    char32_t fin;
};

inline constexpr LamAlefPair kLamAlef[] = {
    {0x0622, 0xFEF5, 0xFEF6}, // Lam + Alef Madda
    {0x0623, 0xFEF7, 0xFEF8}, // Lam + Alef Hamza Above
    {0x0625, 0xFEF9, 0xFEFA}, // Lam + Alef Hamza Below
    {0x0627, 0xFEFB, 0xFEFC}, // Lam + Plain Alef
};

inline const GlyphData* FindGlyph(char32_t c) {
    for (const auto& g : kGlyphs) {
        if (g.base == c) return &g;
    }
    return nullptr;
}

inline const LamAlefPair* FindLamAlef(char32_t alef) {
    for (const auto& p : kLamAlef) {
        if (p.alef == alef) return &p;
    }
    return nullptr;
}

inline bool IsTashkeel(char32_t c) {
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

inline bool IsArabicPunctuation(char32_t c) {
    return c == 0x060C || c == 0x061B || c == 0x061F || c == 0x066A || c == 0x066B || c == 0x066C || c == 0x066D;
}

inline bool IsArabicChar(char32_t c) {
    if (IsArabicPunctuation(c)) return false;
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

// UTF-8 Decoders and Encoders
inline std::vector<char32_t> Utf8ToCodepoints(std::string_view utf8) {
    std::vector<char32_t> result;
    size_t i = 0;
    size_t n = utf8.size();
    while (i < n) {
        uint8_t b1 = uint8_t(utf8[i++]);
        if (b1 < 0x80) {
            result.push_back(b1);
        } else if ((b1 >> 5) == 0x6 && i < n) {
            uint8_t b2 = uint8_t(utf8[i++]);
            result.push_back(((b1 & 0x1F) << 6) | (b2 & 0x3F));
        } else if ((b1 >> 4) == 0xE && i + 1 < n) {
            uint8_t b2 = uint8_t(utf8[i++]);
            uint8_t b3 = uint8_t(utf8[i++]);
            result.push_back(((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F));
        } else if ((b1 >> 3) == 0x1E && i + 2 < n) {
            uint8_t b2 = uint8_t(utf8[i++]);
            uint8_t b3 = uint8_t(utf8[i++]);
            uint8_t b4 = uint8_t(utf8[i++]);
            result.push_back(((b1 & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F));
        }
    }
    return result;
}

inline void AppendCodepointToUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

inline std::string CodepointsToUtf8(const std::vector<char32_t>& cps) {
    std::string out;
    out.reserve(cps.size() * 3);
    for (char32_t c : cps) {
        AppendCodepointToUtf8(out, c);
    }
    return out;
}

// Contextual Arabic shaper: maps raw Arabic codepoints to Presentation Forms-B
inline std::vector<char32_t> ShapeWord(const std::vector<char32_t>& word) {
    std::vector<char32_t> out;
    size_t n = word.size();
    size_t i = 0;
    while (i < n) {
        char32_t c = word[i];
        if (IsTashkeel(c)) {
            // Keep tashkeel transparently
            out.push_back(c);
            i++;
            continue;
        }

        // Check Lam-Alef ligature (ل + ا/أ/إ/آ)
        if (c == 0x0644 && i + 1 < n) {
            char32_t next = word[i + 1];
            if (const LamAlefPair* pair = FindLamAlef(next)) {
                bool prev_connects = false;
                if (i > 0) {
                    size_t p = i - 1;
                    while (p > 0 && IsTashkeel(word[p])) p--;
                    if (const GlyphData* pg = FindGlyph(word[p])) {
                        prev_connects = !pg->right_only;
                    }
                }
                out.push_back(prev_connects ? pair->fin : pair->iso);
                i += 2;
                continue;
            }
        }

        const GlyphData* g = FindGlyph(c);
        if (!g) {
            out.push_back(c);
            i++;
            continue;
        }

        bool prev_connects = false;
        if (i > 0) {
            size_t p = i - 1;
            while (p > 0 && IsTashkeel(word[p])) p--;
            if (const GlyphData* pg = FindGlyph(word[p])) {
                prev_connects = !pg->right_only;
            }
        }

        bool next_connects = false;
        if (i + 1 < n) {
            size_t nx = i + 1;
            while (nx + 1 < n && IsTashkeel(word[nx])) nx++;
            if (FindGlyph(word[nx])) {
                next_connects = true;
            }
        }

        if (prev_connects && next_connects && !g->right_only) {
            out.push_back(g->med);
        } else if (prev_connects) {
            out.push_back(g->fin);
        } else if (next_connects && !g->right_only) {
            out.push_back(g->ini);
        } else {
            out.push_back(g->iso);
        }
        i++;
    }
    return out;
}

// Token classifications for bidirectional processing
enum class TokenType {
    Arabic,
    Number,
    Latin,
    Punctuation,
    Space
};

struct Token {
    TokenType type;
    std::vector<char32_t> chars;
};

inline bool IsAsciiDigit(char32_t c) {
    return c >= '0' && c <= '9';
}

inline bool IsAsciiAlpha(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline char32_t MirrorBracket(char32_t c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        default: return c;
    }
}

// BiDi and Contextual Shaping pipeline: produces proper glyph sequence for Dear ImGui LTR rendering
inline std::string ShapeAndBidi(std::string_view utf8_input) {
    std::vector<char32_t> codepoints = Utf8ToCodepoints(utf8_input);
    if (codepoints.empty()) return "";

    // Idempotency check: If string already contains Presentation Forms (0xFB50-0xFDFF or 0xFE70-0xFEFF),
    // it was already shaped and BiDi-reordered. Returning immediately prevents double-reversal!
    for (char32_t c : codepoints) {
        if ((c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) {
            return std::string(utf8_input);
        }
    }

    bool has_arabic = false;
    for (char32_t c : codepoints) {
        if (IsArabicChar(c)) {
            has_arabic = true;
            break;
        }
    }

    // Pure Latin / ASCII strings bypass BiDi overhead completely
    if (!has_arabic) {
        return std::string(utf8_input);
    }

    // Tokenize codepoint stream
    std::vector<Token> tokens;
    size_t n = codepoints.size();
    size_t i = 0;
    while (i < n) {
        char32_t c = codepoints[i];
        if (c == ' ' || c == '\t') {
            Token t{TokenType::Space, {c}};
            while (i + 1 < n && (codepoints[i + 1] == ' ' || codepoints[i + 1] == '\t')) {
                t.chars.push_back(codepoints[++i]);
            }
            tokens.push_back(std::move(t));
        } else if (IsArabicChar(c)) {
            Token t{TokenType::Arabic, {c}};
            while (i + 1 < n && (IsArabicChar(codepoints[i + 1]) || IsTashkeel(codepoints[i + 1]))) {
                t.chars.push_back(codepoints[++i]);
            }
            tokens.push_back(std::move(t));
        } else if (IsAsciiDigit(c) || c == '%' || c == '.' || c == 'x' || c == 'X') {
            // Keep numbers, percentages, and adjacent alphanumeric model/caliber tags cohesive (e.g. 7.62x54R, 1.85x)
            Token t{TokenType::Number, {c}};
            while (i + 1 < n && (IsAsciiDigit(codepoints[i + 1]) || IsAsciiAlpha(codepoints[i + 1]) ||
                                 codepoints[i + 1] == '%' || codepoints[i + 1] == '.' ||
                                 codepoints[i + 1] == '-' || codepoints[i + 1] == '/')) {
                t.chars.push_back(codepoints[++i]);
            }
            tokens.push_back(std::move(t));
        } else if (IsAsciiAlpha(c)) {
            // Keep alphanumeric Latin tokens (AK-47, M16, MP5, G36C, C96, M16A2) cohesive in LTR order
            Token t{TokenType::Latin, {c}};
            while (i + 1 < n && (IsAsciiAlpha(codepoints[i + 1]) || IsAsciiDigit(codepoints[i + 1]) ||
                                 codepoints[i + 1] == '-' || codepoints[i + 1] == '.' ||
                                 codepoints[i + 1] == '/')) {
                t.chars.push_back(codepoints[++i]);
            }
            tokens.push_back(std::move(t));
        } else {
            // Punctuation and symbols
            tokens.push_back(Token{TokenType::Punctuation, {MirrorBracket(c)}});
        }
        i++;
    }

    // Contextually shape each Arabic token and reverse its internal glyphs for LTR drawing
    for (auto& tok : tokens) {
        if (tok.type == TokenType::Arabic) {
            std::vector<char32_t> shaped = ShapeWord(tok.chars);
            std::reverse(shaped.begin(), shaped.end());
            tok.chars = std::move(shaped);
        }
    }

    // Reorder tokens from right-to-left for full RTL sentence layout
    std::vector<char32_t> output_codepoints;
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        output_codepoints.insert(output_codepoints.end(), it->chars.begin(), it->chars.end());
    }

    return CodepointsToUtf8(output_codepoints);
}

// Formats multiline Arabic text by processing each line independently to preserve vertical order
inline std::string ShapeAndBidiMultiline(std::string_view utf8_input) {
    if (utf8_input.find('\n') == std::string_view::npos) {
        return ShapeAndBidi(utf8_input);
    }
    std::string result;
    size_t start = 0;
    while (start < utf8_input.size()) {
        size_t end = utf8_input.find('\n', start);
        if (end == std::string_view::npos) {
            result += ShapeAndBidi(utf8_input.substr(start));
            break;
        }
        result += ShapeAndBidi(utf8_input.substr(start, end - start));
        result += '\n';
        start = end + 1;
    }
    return result;
}

// Wraps long text at word boundaries into multiple lines before shaping
inline std::string WrapText(std::string_view utf8_input, size_t max_chars = 48) {
    if (utf8_input.size() <= max_chars) return std::string(utf8_input);
    std::string result;
    size_t line_len = 0;
    size_t i = 0;
    while (i < utf8_input.size()) {
        size_t next_space = utf8_input.find(' ', i);
        if (next_space == std::string_view::npos) next_space = utf8_input.size();
        std::string_view word = utf8_input.substr(i, next_space - i);
        if (line_len + word.size() > max_chars && line_len > 0) {
            result += '\n';
            line_len = 0;
        } else if (line_len > 0) {
            result += ' ';
            line_len++;
        }
        result += word;
        line_len += word.size();
        i = (next_space == utf8_input.size()) ? next_space : next_space + 1;
    }
    return result;
}

} // namespace arabic


#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace PersianShaper {

struct GlyphMap {
    uint32_t base;
    uint32_t isolated, final_form, initial, medial;
    bool connects_after;
};

inline const GlyphMap* find_glyph(uint32_t cp) {
    static const GlyphMap table[] = {
        {0x0621, 0xFE80, 0xFE80, 0xFE80, 0xFE80, false},
        {0x0622, 0xFE81, 0xFE82, 0xFE81, 0xFE82, false},
        {0x0623, 0xFE83, 0xFE84, 0xFE83, 0xFE84, false},
        {0x0624, 0xFE85, 0xFE86, 0xFE85, 0xFE86, false},
        {0x0625, 0xFE87, 0xFE88, 0xFE87, 0xFE88, false},
        {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, true},
        {0x0627, 0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E, false},
        {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, true},
        {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59, true},
        {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, true},
        {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, true},
        {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, true},
        {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, true},
        {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, true},
        {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, true},
        {0x062F, 0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA, false},
        {0x0630, 0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC, false},
        {0x0631, 0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE, false},
        {0x0632, 0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0, false},
        {0x0698, 0xFB8A, 0xFB8B, 0xFB8A, 0xFB8B, false},
        {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, true},
        {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, true},
        {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, true},
        {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, true},
        {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, true},
        {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, true},
        {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, true},
        {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, true},
        {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, true},
        {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, true},
        {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91, true},
        {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, true},
        {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95, true},
        {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, true},
        {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, true},
        {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, true},
        {0x0648, 0xFEED, 0xFEEE, 0xFEED, 0xFEEE, false},
        {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, true},
        {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF, true},
        {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, true},
        {0x0649, 0xFEEF, 0xFEF0, 0xFEEF, 0xFEF0, false}
    };
    for (const auto& g : table) {
        if (g.base == cp) return &g;
    }
    return nullptr;
}

inline std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = s[i];
        uint32_t cp = 0;
        size_t len = 1;
        if ((c & 0x80) == 0) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4; }
        else { cp = c; len = 1; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

inline std::string codepoints_to_utf8(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline bool is_rtl_char(uint32_t cp) {
    if ((cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9)) return false;
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFEFF) || cp == 0x200C;
}

inline bool is_ltr_anchor(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z')
        || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && !is_rtl_char(cp));
}

inline uint32_t mirror_bracket(uint32_t cp) {
    switch (cp) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return cp;
    }
}

inline std::string shape_line(const std::string& text) {
    auto cps = utf8_to_codepoints(text);
    bool has_rtl = false;
    for (uint32_t cp : cps) if (is_rtl_char(cp)) { has_rtl = true; break; }
    if (!has_rtl) return text;

    std::vector<uint32_t> shaped;
    for (size_t i = 0; i < cps.size(); ++i) {
        const GlyphMap* curr = find_glyph(cps[i]);
        if (!curr) {
            if (cps[i] != 0x200C) shaped.push_back(cps[i]);
            continue;
        }
        const GlyphMap* prev = (i > 0) ? find_glyph(cps[i - 1]) : nullptr;
        const GlyphMap* next = (i + 1 < cps.size()) ? find_glyph(cps[i + 1]) : nullptr;

        bool join_prev = prev && prev->connects_after && curr->base != 0x0621;
        bool join_next = curr->connects_after && next && next->base != 0x0621;

        if (cps[i] == 0x0644 && i + 1 < cps.size() && (cps[i + 1] == 0x0627 || cps[i + 1] == 0x0622)) {
            uint32_t la = (cps[i + 1] == 0x0622) ? (join_prev ? 0xFEF6 : 0xFEF5) : (join_prev ? 0xFEFC : 0xFEFB);
            shaped.push_back(la);
            ++i;
            continue;
        }

        if (join_prev && join_next) shaped.push_back(curr->medial);
        else if (join_prev && !join_next) shaped.push_back(curr->final_form);
        else if (!join_prev && join_next) shaped.push_back(curr->initial);
        else shaped.push_back(curr->isolated);
    }

    std::vector<uint32_t> visual(shaped.rbegin(), shaped.rend());
    for (auto begin = visual.begin(); begin != visual.end();) {
        if (is_rtl_char(*begin)) {
            ++begin;
            continue;
        }
        const auto end = std::find_if(begin, visual.end(), is_rtl_char);
        const auto text_begin = std::find_if(begin, end, [](uint32_t cp) { return cp != ' '; });
        auto text_end = end;
        while (text_end != text_begin && text_end[-1] == ' ') --text_end;
        if (std::any_of(text_begin, text_end, is_ltr_anchor)) {
            std::reverse(text_begin, text_end);
        } else {
            std::transform(text_begin, text_end, text_begin, mirror_bracket);
        }
        begin = end;
    }
    return codepoints_to_utf8(visual);
}

inline std::string shape_for_overlay(const std::string& text) {
    std::string output;
    size_t begin = 0;
    for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', begin)) {
        output += shape_line(text.substr(begin, end - begin));
        output.push_back('\n');
        begin = end + 1;
    }
    output += shape_line(text.substr(begin));
    return output;
}

}

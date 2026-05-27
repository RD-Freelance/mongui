#include "app.hpp"
#include "mongo.hpp"
#include "term.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace app {

namespace {

using term::Colors;

const std::string Q_FIELDS[5] = {
    "query", "q_project", "q_sort", "q_skip", "q_limit"
};

std::string* q_field_ptr(State& s, int focus) {
    switch (focus) {
        case 1: return &s.query;
        case 2: return &s.q_project;
        case 3: return &s.q_sort;
        case 4: return &s.q_skip;
        case 5: return &s.q_limit;
    }
    return nullptr;
}

// Operators offered by the aggregation autocomplete popup.
// Sorted / deduped once at startup.
const std::vector<std::string>& operators() {
    static std::vector<std::string> ops = [] {
        std::vector<std::string> raw = {
            "$match","$group","$project","$sort","$limit","$skip","$unwind",
            "$lookup","$addFields","$set","$unset","$count","$facet","$bucket",
            "$bucketAuto","$replaceRoot","$replaceWith","$sample","$sortByCount",
            "$graphLookup","$merge","$out","$redact","$geoNear","$unionWith",
            "$setWindowFields","$densify","$fill","$documents",
            "$eq","$ne","$gt","$gte","$lt","$lte","$in","$nin","$and","$or",
            "$nor","$not","$exists","$type","$expr","$mod","$regex","$text",
            "$where","$all","$elemMatch","$size","$jsonSchema",
            "$sum","$avg","$min","$max","$first","$last","$push","$addToSet",
            "$stdDevPop","$stdDevSamp","$mergeObjects","$top","$bottom","$topN",
            "$bottomN","$firstN","$lastN","$maxN","$minN",
            "$cond","$ifNull","$switch","$map","$filter","$reduce","$let",
            "$concat","$concatArrays","$arrayElemAt","$slice","$range","$zip",
            "$objectToArray","$arrayToObject","$getField","$setField","$literal",
            "$toString","$toInt","$toLong","$toDouble","$toDecimal","$toBool",
            "$toDate","$toObjectId","$convert","$dateToString","$dateFromString",
            "$dateToParts","$dateFromParts","$dateAdd","$dateDiff","$dateSubtract",
            "$dateTrunc","$year","$month","$dayOfMonth","$hour","$minute",
            "$second","$millisecond","$dayOfWeek","$dayOfYear","$week",
            "$add","$subtract","$multiply","$divide","$abs","$ceil","$floor",
            "$round","$trunc","$sqrt","$pow","$exp","$ln","$log","$log10",
            "$isArray","$isNumber","$regexMatch","$regexFind","$regexFindAll",
            "$split","$strLenCP","$substrCP","$toUpper","$toLower","$trim",
            "$ltrim","$rtrim","$replaceOne","$replaceAll","$indexOfCP",
            "$indexOfArray","$setUnion","$setIntersection","$setDifference",
            "$setEquals","$setIsSubset","$anyElementTrue","$allElementsTrue",
            "$function","$accumulator","$rand","$meta","$cmp"
        };
        std::set<std::string> dedup(raw.begin(), raw.end());
        std::vector<std::string> out(dedup.begin(), dedup.end());
        std::sort(out.begin(), out.end());
        return out;
    }();
    return ops;
}

constexpr int AC_MAX = 8;

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void ac_compute(State& s) {
    s.ac_active = false;
    if (s.mode != mongodb::ViewMode::Aggregation) return;
    if (s.cursor_y < 0 || s.cursor_y >= (int)s.agg_lines.size()) return;

    const std::string& cur = s.agg_lines[s.cursor_y];
    std::string before = cur.substr(0, std::min((int)cur.size(), s.cursor_x));

    // Find the trailing "$<word>" token.
    int i = (int)before.size() - 1;
    while (i >= 0 && (std::isalnum((unsigned char)before[i]) || before[i] == '_')) --i;
    if (i < 0 || before[i] != '$') return;
    std::string token = before.substr(i);

    std::string q = lower(token);
    std::vector<std::string> matches;
    for (const auto& op : operators()) {
        if (lower(op).rfind(q, 0) == 0) matches.push_back(op);
    }
    if (matches.empty()) return;
    if (matches.size() == 1 && matches[0] == token) return;

    s.ac_active  = true;
    s.ac_token   = token;
    s.ac_matches = std::move(matches);
    s.ac_i       = 0;
}

void ac_accept(State& s) {
    if (!s.ac_active) return;
    if (s.ac_matches.empty()) { s.ac_active = false; return; }
    if (s.cursor_y < 0 || s.cursor_y >= (int)s.agg_lines.size()) {
        s.ac_active = false; return;
    }
    int idx = std::min(std::max(s.ac_i, 0), (int)s.ac_matches.size() - 1);
    const std::string& op = s.ac_matches[idx];
    std::string& cur = s.agg_lines[s.cursor_y];
    int cx = std::min(std::max(s.cursor_x, 0), (int)cur.size());
    std::string before = cur.substr(0, cx);
    std::string after  = cur.substr(cx);

    size_t cut = std::min(before.size(), s.ac_token.size());
    std::string newbefore = before.substr(0, before.size() - cut) + op;
    cur = newbefore + after;
    s.cursor_x = (int)newbefore.size();
    s.ac_active = false;
}

void ac_render(const State& s, int left_w, int base_row, int base_col) {
    if (!s.ac_active || s.ac_matches.empty()) return;
    int n = std::min((int)s.ac_matches.size(), AC_MAX);
    int sel = std::min(std::max(s.ac_i, 0), (int)s.ac_matches.size() - 1);
    int w = 0;
    for (int i = 0; i < n; ++i) w = std::max(w, (int)s.ac_matches[i].size());
    w += 2;

    int px = base_col + std::max(0, s.cursor_x - (int)s.ac_token.size());
    if (px + w > left_w - 1) px = left_w - 1 - w;
    if (px < 2) px = 2;

    int cur_row = base_row + s.cursor_y + 1;
    int py = cur_row + 1;
    if (py + n - 1 > term::H + 1) py = cur_row - n;
    if (py < 4) py = 4;

    for (int i = 0; i < n; ++i) {
        const std::string& op = s.ac_matches[i];
        std::string row = " " + op + std::string(std::max(0, w - (int)op.size() - 1), ' ');
        std::string out;
        if (i == sel) {
            out.append(Colors::bg_green).append(Colors::black).append(row).append(Colors::reset);
        } else {
            out.append(Colors::bg_pop).append(Colors::white).append(row).append(Colors::reset);
        }
        term::line(px, py + i, out);
    }
    if ((int)s.ac_matches.size() > n) {
        char b[32]; std::snprintf(b, sizeof(b), " +%d more ", (int)s.ac_matches.size() - n);
        std::string out;
        out.append(Colors::bg_pop).append(Colors::gray).append(b).append(Colors::reset);
        term::line(px, py + n, out);
    }
}

// =========================================================
// AGG EDITOR — invariant + small editing helpers
// =========================================================
// agg_lines must never be empty; cursor must always sit on a valid cell.
void editor_normalize(State& s) {
    if (s.agg_lines.empty()) s.agg_lines.push_back("");
    int ny = (int)s.agg_lines.size();
    if (s.cursor_y < 0) s.cursor_y = 0;
    if (s.cursor_y >= ny) s.cursor_y = ny - 1;
    int len = (int)s.agg_lines[s.cursor_y].size();
    if (s.cursor_x < 0) s.cursor_x = 0;
    if (s.cursor_x > len) s.cursor_x = len;
}

inline char closer_for(char c) {
    switch (c) {
        case '{': return '}';
        case '[': return ']';
        case '(': return ')';
        case '"': return '"';
        default:  return 0;
    }
}
inline bool is_closer(char c) { return c == '}' || c == ']' || c == ')' || c == '"'; }
inline bool is_opener(char c) { return c == '{' || c == '[' || c == '('; }

// Pair-aware character insert. Returns true if it handled the keystroke.
bool editor_smart_insert(State& s, char c) {
    editor_normalize(s);
    std::string& cur = s.agg_lines[s.cursor_y];
    char next = (s.cursor_x < (int)cur.size()) ? cur[s.cursor_x] : 0;

    // Skip-over: typing a closer when one already sits under the cursor —
    // just step past it instead of doubling it up.
    if (is_closer(c) && next == c) {
        ++s.cursor_x;
        return true;
    }

    // Auto-pair: only insert the closer when the cursor is at a "safe" spot
    // (end of line or before whitespace/closer). Avoids garbage like {a}b on
    // mid-token edits.
    char close = closer_for(c);
    if (close) {
        bool safe = (s.cursor_x == (int)cur.size())
                 || next == ' ' || next == '\t'
                 || next == ',' || next == ')' || next == ']' || next == '}';
        if (safe) {
            cur.insert(s.cursor_x, std::string(1, c) + std::string(1, close));
            ++s.cursor_x;
            return true;
        }
    }

    cur.insert(s.cursor_x, 1, c);
    ++s.cursor_x;
    return true;
}

// Pair-aware backspace. If the cursor is between an opener and its matching
// closer with nothing between them, delete both.
void editor_smart_backspace(State& s) {
    editor_normalize(s);
    std::string& cur = s.agg_lines[s.cursor_y];
    if (s.cursor_x > 0) {
        char prev = cur[s.cursor_x - 1];
        char next = (s.cursor_x < (int)cur.size()) ? cur[s.cursor_x] : 0;
        if (closer_for(prev) == next && next != 0) {
            cur.erase(s.cursor_x - 1, 2);
            --s.cursor_x;
            return;
        }
        cur.erase(s.cursor_x - 1, 1);
        --s.cursor_x;
        return;
    }
    // Join with previous line.
    if (s.cursor_y > 0) {
        std::string& prev = s.agg_lines[s.cursor_y - 1];
        int prev_len = (int)prev.size();
        prev += cur;
        s.agg_lines.erase(s.agg_lines.begin() + s.cursor_y);
        --s.cursor_y;
        s.cursor_x = prev_len;
    }
}

// Enter: copy leading whitespace from current line; add a one-step indent if
// the cursor sits right after an opener; if the next char is the matching
// closer, drop it onto its own dedented line (classic "smart-return" feel).
void editor_smart_enter(State& s) {
    editor_normalize(s);
    std::string& cur = s.agg_lines[s.cursor_y];
    int cx = s.cursor_x;
    std::string left  = cur.substr(0, cx);
    std::string right = cur.substr(cx);

    std::string lead;
    for (char c : cur) {
        if (c == ' ' || c == '\t') lead.push_back(c);
        else break;
    }

    char prev = cx > 0 ? cur[cx - 1] : 0;
    char next = cx < (int)cur.size() ? cur[cx] : 0;
    bool split_pair =
        (prev == '{' && next == '}') ||
        (prev == '[' && next == ']') ||
        (prev == '(' && next == ')');

    cur = left;
    if (split_pair) {
        s.agg_lines.insert(s.agg_lines.begin() + s.cursor_y + 1, lead + "  ");
        s.agg_lines.insert(s.agg_lines.begin() + s.cursor_y + 2, lead + right);
        ++s.cursor_y;
        s.cursor_x = (int)(lead.size() + 2);
    } else {
        std::string indent = lead;
        if (is_opener(prev)) indent += "  ";
        s.agg_lines.insert(s.agg_lines.begin() + s.cursor_y + 1, indent + right);
        ++s.cursor_y;
        s.cursor_x = (int)indent.size();
    }
}

// Ctrl+L: reformat the whole pipeline by depth, preserving string contents.
void editor_reformat(State& s) {
    std::string src;
    for (size_t i = 0; i < s.agg_lines.size(); ++i) {
        if (i) src.push_back('\n');
        src += s.agg_lines[i];
    }

    std::vector<std::string> out;
    std::string line;
    int depth = 0;
    bool need_indent = true;
    bool in_str = false;
    char quote = 0;

    auto push_line = [&]() {
        // Strip trailing space; keep blank intent.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        out.push_back(line);
        line.clear();
        need_indent = true;
    };
    auto indent_to = [&](int d) {
        if (need_indent) {
            for (int i = 0; i < d * 2; ++i) line.push_back(' ');
            need_indent = false;
        }
    };

    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (in_str) {
            line.push_back(c);
            if (c == '\\' && i + 1 < src.size()) { line.push_back(src[++i]); continue; }
            if (c == quote) in_str = false;
            continue;
        }
        if (c == '"' || c == '\'') { indent_to(depth); in_str = true; quote = c; line.push_back(c); continue; }
        if (c == '\n' || c == '\r') { if (!line.empty()) push_line(); continue; }
        if (c == ' ' || c == '\t') {
            if (!line.empty() && line.back() != ' ') line.push_back(' ');
            continue;
        }
        if (c == '{' || c == '[' || c == '(') {
            indent_to(depth);
            line.push_back(c);
            ++depth;
            // Soft break only on top-level openers in pipelines (commonly '[').
            continue;
        }
        if (c == '}' || c == ']' || c == ')') {
            if (depth > 0) --depth;
            // Dedent before drawing the closer.
            if (!line.empty() && line != std::string(line.size(), ' ')) {
                push_line();
            } else {
                line.clear(); need_indent = true;
            }
            indent_to(depth);
            line.push_back(c);
            continue;
        }
        if (c == ',') {
            line.push_back(',');
            push_line();
            continue;
        }
        indent_to(depth);
        line.push_back(c);
    }
    if (!line.empty() || out.empty()) push_line();

    if (out.empty()) out.push_back("[]");
    s.agg_lines = std::move(out);
    s.cursor_y = std::min(s.cursor_y, (int)s.agg_lines.size() - 1);
    if (s.cursor_y < 0) s.cursor_y = 0;
    s.cursor_x = std::min(s.cursor_x, (int)s.agg_lines[s.cursor_y].size());
    if (s.cursor_x < 0) s.cursor_x = 0;
}


// =========================================================
// BUSY WRAPPER
// =========================================================
void load_cols(State& s) {
    if (s.dbs.empty()) return;
    const std::string& db = s.dbs[s.db_i];
    term::busy("Loading collections…");
    std::string e;
    s.cols = mongodb::list_collections(db, e);
    if (s.cols.empty()) s.cols = { "empty" };
    s.col_i = 0;
    s.col_scroll = 0;
    s.col_search.clear();
    s.col_searching = false;
    refresh_filtered(s);
}

// =========================================================
// HELPERS — wrapping + layout
// =========================================================

// Two breakpoints. Wide = full three-pane; Stack = vertical pills + body.
// Picked off the live terminal width so a resize from 200→80 cols just
// switches layout mid-frame.
enum class Layout { Wide, Stack };
Layout pick_layout() { return term::W >= 110 ? Layout::Wide : Layout::Stack; }

// Wrap a single source line into one or more display rows of `width` cols.
// Continuation rows pick up the line's leading indent plus two spaces, so
// wrapped BSON reads as belonging to the same logical line.
std::vector<std::string> wrap_line(const std::string& src, int width) {
    std::vector<std::string> out;
    if (width <= 0)                    { out.push_back(src); return out; }
    if ((int)src.size() <= width)      { out.push_back(src); return out; }

    int indent = 0;
    while (indent < (int)src.size() && src[indent] == ' ') ++indent;
    indent = std::min(indent, std::max(0, width - 12));
    std::string cont(indent + 2, ' ');

    size_t pos = 0;
    bool first = true;
    while (pos < src.size()) {
        int avail = first ? width : std::max(8, width - (int)cont.size());
        size_t take = std::min((size_t)avail, src.size() - pos);
        if (pos + take < src.size()) {
            // Prefer a space break in the last ~30% of the chunk.
            size_t lo = pos + (size_t)(avail * 7 / 10);
            size_t bp = src.find_last_of(' ', pos + take - 1);
            if (bp != std::string::npos && bp >= lo) take = bp - pos + 1;
        }
        out.push_back((first ? std::string() : cont) + src.substr(pos, take));
        pos += take;
        first = false;
    }
    return out;
}

// Flatten s.docs through wrap_line; nothing gets hidden — long values wrap
// onto continuation rows and scroll naturally.
std::vector<std::string> wrap_docs(const std::vector<std::string>& src, int width) {
    std::vector<std::string> out;
    out.reserve(src.size() * 2);
    for (auto& l : src) {
        if (l.empty()) { out.push_back(""); continue; }
        for (auto& c : wrap_line(l, width)) out.push_back(std::move(c));
    }
    return out;
}

// Horizontal pill selector — db / collection picker in Stack layout. Shows
// scroll glyphs when the list overflows; keeps the selected item visible.
void pill_bar(int x, int y, int w,
              const std::vector<std::string>& items, int sel,
              const std::string& label, bool focused) {
    int lw = (int)label.size();
    {
        std::string head;
        head.append(focused ? Colors::green_b : Colors::dgray)
            .append(label).append(Colors::reset);
        term::line(x, y, head);
    }
    int body_x = x + lw + 1;
    int body_w = w - lw - 1;
    if (items.empty()) {
        std::string s;
        s.append(Colors::dgray).append("(empty)").append(Colors::reset);
        term::line(body_x, y, s);
        return;
    }

    auto pw = [](const std::string& s){ return (int)s.size() + 2; };

    // Pick a window of items that keeps `sel` visible.
    int total = 0;
    for (auto& s : items) total += pw(s);

    int start = 0;
    int end = (int)items.size();
    if (total > body_w) {
        int used = 0;
        // Greedy: include sel; expand both sides until we run out of room.
        std::vector<int> left, right;
        used = pw(items[sel]);
        int li = sel - 1, ri = sel + 1;
        while (li >= 0 || ri < (int)items.size()) {
            if (li >= 0 && used + pw(items[li]) <= body_w - 2) {
                used += pw(items[li]); left.push_back(li--); }
            else if (ri < (int)items.size() && used + pw(items[ri]) <= body_w - 2) {
                used += pw(items[ri]); right.push_back(ri++); }
            else break;
        }
        start = left.empty() ? sel : left.back();
        end   = right.empty() ? sel + 1 : right.back() + 1;
    }

    int cx = body_x;
    if (start > 0) {
        std::string ind;
        ind.append(Colors::dgray).append("‹ ").append(Colors::reset);
        term::line(cx, y, ind);
        cx += 2;
    }
    for (int i = start; i < end; ++i) {
        std::string pill;
        if (i == sel) {
            pill.append(Colors::bg_sel).append(Colors::bold).append(Colors::white)
                .append(" ").append(items[i]).append(" ").append(Colors::reset);
        } else {
            pill.append(Colors::gray)
                .append(" ").append(items[i]).append(" ").append(Colors::reset);
        }
        term::line(cx, y, pill);
        cx += pw(items[i]);
    }
    if (end < (int)items.size()) {
        std::string ind;
        ind.append(Colors::dgray).append(" ›").append(Colors::reset);
        term::line(x + w - 2, y, ind);
    }
}

// Solid horizontal rule used as a soft separator between chrome and body.
void rule(int x, int y, int w, const char* color = Colors::dgray) {
    std::string s;
    s.append(color);
    for (int i = 0; i < w; ++i) s.append("─");
    s.append(Colors::reset);
    term::line(x, y, s);
}

} // namespace

void refresh_filtered(State& s) {
    s.filtered.clear();
    std::string q = lower(s.col_search);
    for (const auto& c : s.cols) {
        if (q.empty() || lower(c).find(q) != std::string::npos) {
            s.filtered.push_back(c);
        }
    }
    int maxi = std::max(0, (int)s.filtered.size() - 1);
    s.col_i = std::min(std::max(s.col_i, 0), maxi);
}

void select_default_db(State& s) {
    if (s.default_db.empty()) return;
    for (size_t i = 0; i < s.dbs.size(); ++i) {
        if (s.dbs[i] == s.default_db) { s.db_i = (int)i; return; }
    }
    s.dbs.insert(s.dbs.begin(), s.default_db);
    s.db_i = 0;
}

namespace {

// Count display columns in a string, skipping ANSI CSI escapes and
// counting UTF-8 multibyte chars as one cell. Used to right-pad lines
// inside the bordered card.
int visible_cols(const std::string& s) {
    int w = 0;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && !(s[i] >= '@' && s[i] <= '~')) ++i;
            if (i < n) ++i;
            continue;
        }
        if      (c < 0x80)            { ++w; i += 1; }
        else if ((c & 0xE0) == 0xC0)  { ++w; i += 2; }
        else if ((c & 0xF0) == 0xE0)  { ++w; i += 3; }
        else if ((c & 0xF8) == 0xF0)  { ++w; i += 4; }
        else                          {       i += 1; }
    }
    return w;
}

struct Card {
    int pad;     // left-margin spaces to center the card
    int inner;   // usable text width inside the border
    int outer;   // full card width including borders
    std::string lead;  // "<padding>│ "
};

Card make_card() {
    term::update_dims();
    int W = term::W;
    int outer = std::min(78, std::max(48, W - 4));
    int pad   = std::max(2, (W - outer) / 2);
    Card c;
    c.outer = outer;
    c.inner = outer - 4;        // 2 border chars + 2 spaces of inset
    c.pad   = pad;
    c.lead.assign(pad, ' ');
    return c;
}

void card_top(const Card& c) {
    std::cout << std::string(c.pad, ' ') << Colors::dgray << "╭";
    for (int i = 0; i < c.outer - 2; ++i) std::cout << "─";
    std::cout << "╮" << Colors::reset << "\n";
}
void card_bot(const Card& c) {
    std::cout << std::string(c.pad, ' ') << Colors::dgray << "╰";
    for (int i = 0; i < c.outer - 2; ++i) std::cout << "─";
    std::cout << "╯" << Colors::reset << "\n";
}
void card_rule(const Card& c) {
    std::cout << std::string(c.pad, ' ') << Colors::dgray << "├";
    for (int i = 0; i < c.outer - 2; ++i) std::cout << "─";
    std::cout << "┤" << Colors::reset << "\n";
}
void card_row(const Card& c, const std::string& body = "") {
    int vis  = visible_cols(body);
    int fill = std::max(0, c.inner - vis);
    std::cout << std::string(c.pad, ' ')
              << Colors::dgray << "│" << Colors::reset
              << " " << body << std::string(fill, ' ') << " "
              << Colors::dgray << "│" << Colors::reset << "\n";
}

std::string parse_default_db(const std::string& uri) {
    std::string def;
    size_t scheme = uri.find("://");
    if (scheme == std::string::npos) return def;
    size_t slash = uri.find('/', scheme + 3);
    if (slash == std::string::npos) return def;
    size_t end = uri.find_first_of("/?", slash + 1);
    def = uri.substr(slash + 1, end == std::string::npos
                                ? std::string::npos
                                : end - slash - 1);
    return def;
}

} // anonymous

void connect_screen(State& s) {
    term::clear();
    term::flush_out();
    std::cout << "\n";

    Card c = make_card();

    card_top(c);
    card_row(c);
    {
        std::string brand;
        brand.append(Colors::bg_green).append(Colors::black).append(Colors::bold)
             .append(" MONGUI ").append(Colors::reset)
             .append("   ").append(Colors::dgray)
             .append("the mongodb terminal client").append(Colors::reset);
        card_row(c, brand);
    }
    card_row(c);
    card_rule(c);
    card_row(c);
    {
        std::string s1;
        s1.append(Colors::green_b).append("Connection").append(Colors::reset)
          .append("   ").append(Colors::dgray)
          .append("press ").append(Colors::reset).append(Colors::gray).append("↵")
          .append(Colors::reset).append(Colors::dgray).append(" for default, ")
          .append(Colors::reset).append(Colors::gray).append("^c")
          .append(Colors::reset).append(Colors::dgray).append(" to quit")
          .append(Colors::reset);
        card_row(c, s1);
    }
    card_row(c);
    {
        std::string s2;
        s2.append(Colors::gray).append("URI").append(Colors::reset)
          .append("        ").append(Colors::cyan)
          .append("mongodb://localhost:27017").append(Colors::reset)
          .append("  ").append(Colors::dgray).append("(default)").append(Colors::reset);
        card_row(c, s2);
    }
    card_row(c);
    {
        std::string e;
        e.append(Colors::dgray).append("examples").append(Colors::reset);
        card_row(c, e);
    }
    auto example = [&](const char* ex) {
        std::string e;
        e.append("  ").append(Colors::dgray).append(ex).append(Colors::reset);
        card_row(c, e);
    };
    example("mongodb://localhost:27017");
    example("mongodb+srv://user:pass@cluster.mongodb.net");
    example("mongodb://user:pass@host:27017/db?authSource=admin");
    card_row(c);
    card_bot(c);

    std::cout << "\n" << c.lead << Colors::green_b << "▸ " << Colors::reset
              << std::flush;

    std::string uri;
    if (!std::getline(std::cin, uri)) {
        // Ctrl+D / EOF on the connect prompt — bail out cleanly. We're on
        // the alt screen at this point (entered in main), so leave it
        // before exiting so the user lands back on their shell prompt.
        term::show_cursor();
        term::leave_alt_screen();
        std::exit(0);
    }
    if (uri.empty()) uri = "mongodb://localhost:27017";
    s.uri = uri;

    std::string def = parse_default_db(uri);

    std::cout << "\n";
    card_top(c);
    card_row(c);
    {
        std::string s2;
        s2.append(Colors::gray).append("Database").append(Colors::reset)
          .append("   ");
        if (!def.empty()) {
            s2.append(Colors::cyan).append(def).append(Colors::reset)
              .append("  ").append(Colors::dgray)
              .append("(from URI · ↵ to use)").append(Colors::reset);
        } else {
            s2.append(Colors::dgray).append("(optional · ↵ to skip)")
              .append(Colors::reset);
        }
        card_row(c, s2);
    }
    card_row(c);
    card_bot(c);

    std::cout << "\n" << c.lead << Colors::green_b << "▸ " << Colors::reset
              << std::flush;

    std::string typed;
    if (std::getline(std::cin, typed) && !typed.empty()) {
        def = typed;
    }
    s.default_db = def;
}

namespace {

const char* mode_label(mongodb::ViewMode m) {
    switch (m) {
        case mongodb::ViewMode::Aggregation: return "AGGREGATE";
        case mongodb::ViewMode::Shell:       return "SHELL";
        default:                             return "QUERY";
    }
}

// Render the breadcrumb + mode badge at row 1, separator at row 2.
void render_header(State& s) {
    int W = term::W;
    rule(1, 2, W);

    std::string brand;
    brand.append(Colors::green_b).append("mongui").append(Colors::reset);
    term::line(2, 1, brand);

    std::string db  = !s.dbs.empty()     ? s.dbs[s.db_i] : std::string("—");
    std::string col = !s.filtered.empty() ? s.filtered[s.col_i] : std::string("—");
    std::string crumb;
    crumb.append(Colors::dgray).append("  ·  ").append(Colors::reset)
         .append(Colors::white).append(db).append(Colors::reset)
         .append(Colors::dgray).append(" › ").append(Colors::reset)
         .append(Colors::cyan).append(col).append(Colors::reset);
    term::line(10, 1, crumb);

    const char* lbl = mode_label(s.mode);
    int badge_w = (int)std::strlen(lbl) + 2;
    std::string badge;
    badge.append(Colors::bg_green).append(Colors::black).append(Colors::bold)
         .append(" ").append(lbl).append(" ").append(Colors::reset);
    term::line(W - badge_w - 1, 1, badge);

    if (s.confirm_exit) {
        std::string out;
        out.append(Colors::yellow).append("press ^c again to quit").append(Colors::reset);
        int x = std::max(2, W - badge_w - 30);
        term::line(x, 1, out);
    }
    if (!s.status_msg.empty()) {
        std::string m = s.status_msg;
        if ((int)m.size() > W - 4) m = m.substr(0, W - 5) + "…";
        bool warn = m.size() >= 3 && (unsigned char)m[0] == 0xE2;
        std::string out;
        out.append(warn ? Colors::yellow : Colors::green_b).append(m).append(Colors::reset);
        term::line(2, 2, out);
    }
}

void render_footer(State& s) {
    int W = term::W;
    int H = term::H;
    rule(1, H + 3, W);

    std::string mid;
    if (s.mode == mongodb::ViewMode::Aggregation) {
        const char* np_label = s.ac_active ? "suggest" : "page";
        mid = term::fkey("^g", "run") + "  " +
              term::fkey("^l", "format") + "  " +
              term::fkey("^n/^p", np_label) + "  " +
              term::fkey("tab", "complete") + "  ";
    } else if (s.mode == mongodb::ViewMode::Shell) {
        mid = term::fkey("↵", "run") + "  ";
    } else {
        mid = term::fkey("tab", "field") + "  " +
              term::fkey("^f", "search") + "  " +
              term::fkey("↵", "find") + "  " +
              term::fkey("^n/^p", "page") + "  ";
    }

    std::string footer =
        term::fkey("^e", "query") + "  " +
        term::fkey("^a", "agg") + "  " +
        term::fkey("^s", "shell") + "  " +
        mid +
        term::fkey("^j/^k", "scroll") + "  " +
        term::fkey("^o", "export") + "  " +
        term::fkey("^q", "quit");
    term::line(2, H + 4, footer);

    char status[96];
    if (s.mode == mongodb::ViewMode::Query && s.doc_count > 0) {
        // Find pagination: total is known (count_documents).
        long long total = (long long)s.doc_count;
        long long start = (long long)s.page * s.page_size + 1;
        long long end   = start + s.doc_shown - 1;
        long long pages = (total + s.page_size - 1) / s.page_size;
        if (pages < 1) pages = 1;
        std::snprintf(status, sizeof(status),
            "page %d/%lld · %lld–%lld of %lld",
            s.page + 1, pages, start, end, total);
    } else if (s.mode == mongodb::ViewMode::Aggregation && s.doc_shown > 0) {
        long long start = (long long)s.page * s.page_size + 1;
        long long end   = start + s.doc_shown - 1;
        if (s.doc_count > 0) {
            long long total = (long long)s.doc_count;
            long long pages = (total + s.page_size - 1) / s.page_size;
            if (pages < 1) pages = 1;
            std::snprintf(status, sizeof(status),
                "page %d/%lld · %lld–%lld of %lld",
                s.page + 1, pages, start, end, total);
        } else {
            std::snprintf(status, sizeof(status),
                "page %d · %lld–%lld", s.page + 1, start, end);
        }
    } else if (s.doc_count > s.doc_shown) {
        std::snprintf(status, sizeof(status), "%d / %lld docs",
            s.doc_shown, (long long)s.doc_count);
    } else if (s.doc_count > 0) {
        std::snprintf(status, sizeof(status), "%lld docs", (long long)s.doc_count);
    } else {
        std::snprintf(status, sizeof(status), "ready");
    }
    std::string st;
    st.append(Colors::dim).append(Colors::gray).append(status).append(Colors::reset);
    term::line(W - (int)std::strlen(status) - 2, H + 3, st);
}

void render_doc_pane(const std::vector<std::string>& docs,
                     int x, int y, int w, int h,
                     int& scroll_state) {
    int avail = std::max(8, w - 4);
    auto rows = wrap_docs(docs, avail);

    int max_scroll = std::max(0, (int)rows.size() - h);
    if (scroll_state > max_scroll) scroll_state = max_scroll;
    if (scroll_state < 0)          scroll_state = 0;

    if (rows.empty()) {
        std::string hint;
        hint.append(Colors::dgray).append("(no results — press ↵ to run)").append(Colors::reset);
        term::line(x, y, hint);
        return;
    }
    for (int i = 0; i < h; ++i) {
        int idx = scroll_state + i;
        if (idx >= (int)rows.size()) break;
        std::string out;
        out.append(Colors::dgray).append("│ ").append(Colors::reset)
           .append(term::highlight(rows[idx]));
        term::line(x, y + i, out);
    }

    if (max_scroll > 0) {
        int gh = std::max(1, h * h / std::max(1, (int)rows.size()));
        int gy = (h - gh) * scroll_state / max_scroll;
        for (int i = 0; i < h; ++i) {
            const char* glyph = (i >= gy && i < gy + gh) ? "▐" : " ";
            const char* col   = (i >= gy && i < gy + gh) ? Colors::green : Colors::dgray;
            std::string g; g.append(col).append(glyph).append(Colors::reset);
            term::line(x + w - 2, y + i, g);
        }
    }
}

// Block-style "I'm here" cursor. Reverse-video swaps the cell's foreground
// and background using whatever the terminal's current theme provides, so
// it's always visible — no dependence on 256-color palette mapping.
void render_block_cursor(int cx, int cy, const std::string& line, int col) {
    std::string ch = " ";
    if (col >= 0 && col < (int)line.size()) {
        ch = line.substr(col, 1);
        if (ch.empty()) ch = " ";
    }
    std::string blk;
    blk.append("\x1b[0m\x1b[7m\x1b[1m").append(ch).append("\x1b[0m");
    term::line(cx, cy, blk);
}

// ---------- QUERY ----------
void render_query_wide(State& s) {
    int W = term::W, H = term::H;
    int DB_W  = std::min(28, std::max(18, W / 6));
    int COL_W = std::min(36, std::max(24, W / 5));
    int DOC_X = DB_W + COL_W;
    int DOC_W = W - DOC_X;

    term::box(1,        3, DB_W,  H, "databases",  s.q_focus == 0);
    term::box(DB_W,     3, COL_W, H, "collections", s.q_focus == 0 || s.col_searching);
    term::box(DOC_X,    3, DOC_W, H, "documents",  s.q_focus > 0);

    // Databases
    int dvis = H - 4;
    int dstart = std::max(0, std::min(s.db_i - dvis / 2, (int)s.dbs.size() - dvis));
    if (dstart < 0) dstart = 0;
    for (int i = 0; i < dvis; ++i) {
        int idx = dstart + i;
        if (idx >= (int)s.dbs.size()) break;
        term::line(2, 6 + i, term::item(s.dbs[idx], idx == s.db_i, DB_W - 4));
    }

    // Collection search bar
    std::string sc;
    sc.append(s.col_searching ? Colors::yellow : Colors::gray)
      .append("search ").append(Colors::dgray).append("▸ ").append(Colors::reset);
    sc.append(s.col_searching ? Colors::white : Colors::dgray);
    sc.append(s.col_search.empty() ? "type to filter" : s.col_search);
    sc.append(Colors::reset);
    term::line(DB_W + 1, 4, sc);

    int CVIS = H - 4;
    if (s.col_i < s.col_scroll) s.col_scroll = s.col_i;
    else if (s.col_i > s.col_scroll + CVIS - 1) s.col_scroll = s.col_i - CVIS + 1;
    if (s.col_scroll < 0) s.col_scroll = 0;

    for (int i = 0; i < CVIS; ++i) {
        int idx = s.col_scroll + i;
        if (idx >= (int)s.filtered.size()) break;
        term::line(DB_W + 1, 6 + i,
            term::item(s.filtered[idx], idx == s.col_i, COL_W - 4));
    }

    int QX = DOC_X + 2;
    term::qfield(QX, 4, "filter",  s.query,     s.q_focus == 1);
    term::qfield(QX, 5, "project", s.q_project, s.q_focus == 2);
    term::qfield(QX, 6, "sort",    s.q_sort,    s.q_focus == 3);
    term::qfield(QX, 7, "skip",    s.q_skip,    s.q_focus == 4);
    term::qfield(QX, 8, "limit",   s.q_limit,   s.q_focus == 5);
    rule(QX, 9, DOC_W - 4);

    render_doc_pane(s.docs, QX, 10, DOC_W - 2, H - 8, s.doc_scroll);
}

void render_query_stack(State& s) {
    int W = term::W, H = term::H;
    // Compact pill bars for db / collection selection.
    pill_bar(2, 3, W - 4, s.dbs, s.db_i, "db", s.q_focus == 0);
    pill_bar(2, 4, W - 4, s.filtered, s.col_i, "col",
             s.q_focus == 0 || s.col_searching);

    if (s.col_searching || !s.col_search.empty()) {
        std::string sc;
        sc.append(s.col_searching ? Colors::yellow : Colors::dgray)
          .append("/ ").append(Colors::reset);
        sc.append(s.col_searching ? Colors::white : Colors::dgray);
        sc.append(s.col_search.empty() ? "type to filter" : s.col_search);
        sc.append(Colors::reset);
        term::line(2, 5, sc);
    }

    // Compact query fields — single-line layout if width allows.
    int qy = 6;
    rule(2, qy, W - 4);
    term::qfield(3, qy + 1, "filter",  s.query,     s.q_focus == 1);
    term::qfield(3, qy + 2, "project", s.q_project, s.q_focus == 2);
    term::qfield(3, qy + 3, "sort",    s.q_sort,    s.q_focus == 3);
    if (W >= 70) {
        term::qfield(3 + W / 2, qy + 1, "skip",  s.q_skip,  s.q_focus == 4);
        term::qfield(3 + W / 2, qy + 2, "limit", s.q_limit, s.q_focus == 5);
    } else {
        term::qfield(3, qy + 4, "skip",  s.q_skip,  s.q_focus == 4);
        term::qfield(3, qy + 5, "limit", s.q_limit, s.q_focus == 5);
        qy += 2;
    }

    int doc_y = qy + 5;
    rule(2, doc_y - 1, W - 4);
    std::string lbl;
    lbl.append(Colors::dgray).append("documents").append(Colors::reset);
    term::line(3, doc_y - 1, lbl);
    render_doc_pane(s.docs, 3, doc_y, W - 4, H + 2 - doc_y, s.doc_scroll);
}

// ---------- AGGREGATION ----------
// Per-line horizontal scroll offset that keeps cursor_x visible inside the
// `avail` columns of editor content. Other (non-cursor) lines render from 0.
int compute_hscroll(int line_len, int cursor_x, int avail) {
    if (avail <= 2) return 0;
    int hscroll = 0;
    if (cursor_x >= avail - 1) hscroll = cursor_x - (avail - 2);
    int max_scroll = std::max(0, line_len - (avail - 1));
    if (hscroll > max_scroll) hscroll = max_scroll;
    if (hscroll < 0) hscroll = 0;
    return hscroll;
}

void render_agg_editor(const State& s, int x, int y, int w, int h) {
    const int line_no_w = 3;
    int avail = std::max(8, w - line_no_w);

    for (int i = 0; i < (int)s.agg_lines.size() && i < h; ++i) {
        const std::string& body = s.agg_lines[i];
        int hscroll = (i == s.cursor_y)
            ? compute_hscroll((int)body.size(), s.cursor_x, avail) : 0;

        int start = std::min((int)body.size(), hscroll);
        int show  = std::min(avail, (int)body.size() - start);
        std::string visible = body.substr(start, std::max(0, show));

        bool more_left  = hscroll > 0;
        bool more_right = hscroll + avail < (int)body.size();

        // Reserve one column for each present indicator so highlighted text
        // never spills over them.
        if (more_left && !visible.empty())  visible.erase(0, 1);
        if (more_right && !visible.empty()) visible.pop_back();

        char buf[8]; std::snprintf(buf, sizeof(buf), "%2d ", i + 1);
        std::string out;
        out.append(Colors::dgray).append(buf).append(Colors::reset);

        if (more_left)  out.append(Colors::dgray).append("‹").append(Colors::reset);
        out += term::highlight(visible);
        if (more_right) out.append(Colors::dgray).append("›").append(Colors::reset);

        term::line(x, y + i, out);
    }
}

// Draw the block cursor at its screen column, applying the same hscroll
// the editor used. Returns the effective base column for ac_render.
int draw_agg_cursor(const State& s, int base_col_at_zero, int editor_y0, int avail) {
    if (s.agg_lines.empty()) return base_col_at_zero;
    int cy_idx = std::min(std::max(s.cursor_y, 0), (int)s.agg_lines.size() - 1);
    const std::string& ln = s.agg_lines[cy_idx];
    int cx_off = std::min(std::max(s.cursor_x, 0), (int)ln.size());
    int hscroll = compute_hscroll((int)ln.size(), cx_off, avail);

    // Indicator "‹" at column base_col_at_zero shifts text right by 1 when
    // hscroll>0; account for that so the cursor lands on the right cell.
    int extra = (hscroll > 0) ? 1 : 0;
    int screen_x = base_col_at_zero + extra + (cx_off - hscroll);
    render_block_cursor(screen_x, editor_y0 + cy_idx, ln, cx_off);
    return base_col_at_zero + extra - hscroll;
}

void render_agg_wide(State& s) {
    int H = term::H;
    int LEFT  = term::LEFT_W;
    int RIGHT = term::W - LEFT;

    term::box(1,    3, LEFT,  H, "pipeline", true);
    term::box(LEFT, 3, RIGHT, H, "preview",  false);

    render_agg_editor(s, 3, 5, LEFT - 4, H - 4);
    render_doc_pane(s.docs, LEFT + 2, 5, RIGHT - 3, H - 4, s.doc_scroll);

    int avail = std::max(8, (LEFT - 4) - 3);
    int ac_base = draw_agg_cursor(s, /*base_col_at_zero=*/6, /*editor_y0=*/5, avail);
    ac_render(s, LEFT, 4, ac_base);
}

void render_agg_stack(State& s) {
    int W = term::W, H = term::H;
    int top_h = std::max(8, (H + 2) / 2);
    int bot_h = H + 2 - top_h;

    term::box(1,         3, W, top_h, "pipeline", true);
    term::box(1, 3 + top_h, W, bot_h, "preview", false);

    render_agg_editor(s, 3, 5, W - 4, top_h - 4);
    render_doc_pane(s.docs, 3, 5 + top_h, W - 4, bot_h - 4, s.doc_scroll);

    int avail = std::max(8, (W - 4) - 3);
    int ac_base = draw_agg_cursor(s, /*base_col_at_zero=*/6, /*editor_y0=*/5, avail);
    ac_render(s, W, 4, ac_base);
}

// ---------- SHELL ----------
void render_shell(State& s) {
    int W = term::W, H = term::H;
    term::box(1, 3, W, 5,         "shell",  true);
    term::box(1, 8, W, H - 4,     "result", false);

    std::string db = !s.dbs.empty() ? s.dbs[s.db_i]
                   : !s.default_db.empty() ? s.default_db : std::string("?");
    std::string ctx;
    ctx.append(Colors::gray).append("using ").append(Colors::reset)
       .append(Colors::cyan).append(db).append(Colors::reset)
       .append(Colors::dgray)
       .append("   db.coll.find · updateMany · insertOne · deleteMany")
       .append(Colors::reset);
    term::line(4, 4, ctx);

    const std::string prompt   = "▸ ";
    const int         prompt_w = 2;  // display columns, not bytes (▸ is UTF-8 3B)
    int avail = W - 6;
    int cx_in = s.shell_cx;
    int off = s.shell_scroll;
    if (cx_in < off) off = cx_in;
    if (cx_in > off + avail) off = cx_in - avail;
    if (off < 0) off = 0;
    s.shell_scroll = off;

    std::string visible = s.shell_cmd.substr(
        std::min((size_t)off, s.shell_cmd.size()), (size_t)avail);
    std::string l;
    l.append(Colors::green_b).append(prompt).append(Colors::reset)
     .append(term::highlight(visible));
    term::line(3, 6, l);

    render_doc_pane(s.docs, 3, 9, W - 4, H - 6, s.doc_scroll);

    int ccx = 3 + prompt_w + (cx_in - off);
    render_block_cursor(ccx, 6, s.shell_cmd, cx_in);
}

void render_export_modal() {
    int W = term::W, H = term::H;
    std::string msg = "  export results   ·   [j] json   [c] csv   [esc] cancel  ";
    int x = std::max(1, (W - (int)msg.size()) / 2);
    int y = std::max(3, H / 2);
    std::string top, mid, bot;
    top.append(Colors::green).append("┌");
    for (size_t i = 0; i < msg.size(); ++i) top.append("─");
    top.append("┐").append(Colors::reset);
    mid.append(Colors::green).append("│").append(Colors::reset)
       .append(Colors::bg_sel).append(Colors::bold).append(Colors::white)
       .append(msg).append(Colors::reset)
       .append(Colors::green).append("│").append(Colors::reset);
    bot.append(Colors::green).append("└");
    for (size_t i = 0; i < msg.size(); ++i) bot.append("─");
    bot.append("┘").append(Colors::reset);
    term::line(x, y - 1, top);
    term::line(x, y,     mid);
    term::line(x, y + 1, bot);
}

} // anonymous

void render(State& s) {
    term::update_dims();
    term::clear();

    Layout L = pick_layout();
    render_header(s);

    if (s.mode == mongodb::ViewMode::Aggregation) {
        if (L == Layout::Wide) render_agg_wide(s);
        else                   render_agg_stack(s);
    } else if (s.mode == mongodb::ViewMode::Shell) {
        render_shell(s);
    } else {
        if (L == Layout::Wide) render_query_wide(s);
        else                   render_query_stack(s);
    }

    render_footer(s);
    if (s.export_menu) render_export_modal();
    term::flush_out();
}

namespace {

// Run the current find with pagination applied. Page offset stacks on top of
// the user's q_skip; we always ask the server for page_size docs and let the
// renderer wrap them. Stores results back into State.
void run_paginated_find(State& s) {
    long long base_skip = 0;
    if (!s.q_skip.empty()) {
        char* end = nullptr;
        long long v = std::strtoll(s.q_skip.c_str(), &end, 10);
        if (end != s.q_skip.c_str()) base_skip = v;
    }
    long long eff_skip = base_skip + (long long)s.page * s.page_size;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", eff_skip);
    std::string skip_arg = buf;

    mongodb::QueryResult r = mongodb::run_find(
        s.dbs.empty()      ? std::string() : s.dbs[s.db_i],
        s.filtered.empty() ? std::string() : s.filtered[s.col_i],
        s.query, s.q_project, s.q_sort, skip_arg, s.q_limit, s.page_size);
    s.docs       = std::move(r.lines);
    s.doc_count  = r.total;
    s.doc_shown  = r.shown;
    s.doc_scroll = 0;

    // Clamp the page if we walked off the end (e.g. doc_count shrank since
    // the previous fetch). Re-fetches page 0 silently if needed.
    if (s.doc_shown == 0 && s.page > 0) {
        s.page = 0;
        run_paginated_find(s);
    }
}

// Append { $skip: N }, { $limit: M } stages to the user's pipeline text so
// the server only ships the current page's documents. Returns the modified
// pipeline; falls back to the original if it can't find a closing bracket.
std::string pipeline_with_pagination(const std::string& src,
                                     long long skip, int limit) {
    size_t r = src.rfind(']');
    if (r == std::string::npos) return src;
    std::string before = src.substr(0, r);
    std::string after  = src.substr(r);  // "]" + any trailing whitespace

    // Trim trailing whitespace inside the array so the comma sits cleanly.
    std::string trimmed = before;
    while (!trimmed.empty()
        && (trimmed.back() == ' ' || trimmed.back() == '\n'
         || trimmed.back() == '\t' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "%s{ $skip: %lld }, { $limit: %d } %s",
        trimmed.empty() || trimmed.back() == '[' ? "" : ", ",
        skip, limit, "");

    return trimmed + buf + after;
}

// Run the pipeline for the current page. `recount` triggers a parallel
// $count aggregation to refresh the total — only do this on a fresh ^G,
// not on cheap ^N/^P paging.
void run_paginated_agg(State& s, bool recount) {
    std::string pipeline;
    for (size_t i = 0; i < s.agg_lines.size(); ++i) {
        if (i) pipeline.push_back('\n');
        pipeline += s.agg_lines[i];
    }
    std::string db  = s.dbs.empty()      ? std::string() : s.dbs[s.db_i];
    std::string col = s.filtered.empty() ? std::string() : s.filtered[s.col_i];

    if (recount) {
        int64_t total = mongodb::count_aggregation(db, col, pipeline);
        s.doc_count = total > 0 ? total : 0;
    }

    long long skip = (long long)s.page * s.page_size;
    std::string paged = pipeline_with_pagination(pipeline, skip, s.page_size);

    mongodb::QueryResult r = mongodb::run_aggregation(db, col, paged, s.page_size);
    s.docs       = std::move(r.lines);
    s.doc_shown  = r.shown;
    s.doc_scroll = 0;
    if (!recount && r.total > s.doc_count) s.doc_count = r.total;

    if (s.doc_shown == 0 && s.page > 0) {
        s.page = 0;
        run_paginated_agg(s, false);
    }
}

} // namespace

void run(State& s) {
    run_paginated_find(s);

    while (true) {
        render(s);

        std::string k = term::read_key();

        // Any key other than Ctrl+C disarms two-press quit.
        if (k != std::string("\x03")) s.confirm_exit = false;

        // Clear transient status on the next key (export picker has its
        // own surface and clears itself).
        if (!s.export_menu) s.status_msg.clear();

        // -------- EXPORT PICKER --------
        if (s.export_menu) {
            s.export_menu = false;
            if (k == "j" || k == "J" || k == "c" || k == "C") {
                std::string fmt = (k == "j" || k == "J") ? "json" : "csv";
                term::busy(std::string("Exporting ") + (fmt == "json" ? "JSON" : "CSV") + "…");
                mongodb::ExportResult r = mongodb::export_results(
                    s.dbs.empty() ? std::string() : s.dbs[s.db_i],
                    s.filtered.empty() ? std::string() : s.filtered[s.col_i],
                    fmt, s.mode,
                    s.query, s.q_project, s.q_sort, s.q_skip, s.q_limit,
                    [&]() {
                        std::string out;
                        for (size_t i = 0; i < s.agg_lines.size(); ++i) {
                            if (i) out.push_back('\n');
                            out += s.agg_lines[i];
                        }
                        return out;
                    }());
                s.status_msg = r.message;
            }
            continue;
        }

        // -------- EXIT (Ctrl+C twice) --------
        if (k == std::string("\x03", 1)) {
            if (s.confirm_exit) break;
            s.confirm_exit = true;
            continue;
        }
        if (k == std::string("\x11", 1)) break;   // ^q

        // -------- EXPORT (^o opens the picker) --------
        if (k == std::string("\x0f", 1)) { s.export_menu = true; continue; }

        // -------- ^f toggle collection search --------
        if (k == std::string("\x06", 1) && s.mode == mongodb::ViewMode::Query) {
            s.col_searching = !s.col_searching;
            continue;
        }

        // Backspace inside collection search.
        if ((k == "\x7f" || k == "\x08") && s.col_searching) {
            if (!s.col_search.empty()) s.col_search.pop_back();
            s.col_i = 0;
            refresh_filtered(s);
            continue;
        }

        // -------- Tab: cycle q_focus --------
        if (k == "\t" && s.mode == mongodb::ViewMode::Query) {
            s.q_focus = (s.q_focus + 1) % (5 + 1);
            continue;
        }

        // Backspace in focused query field.
        if ((k == "\x7f" || k == "\x08")
            && s.mode == mongodb::ViewMode::Query && s.q_focus > 0) {
            auto* p = q_field_ptr(s, s.q_focus);
            if (p && !p->empty()) p->pop_back();
            continue;
        }

        // -------- Database navigation (Up/Down, nav-only) --------
        if (k == "UP" && s.mode == mongodb::ViewMode::Query && s.q_focus == 0) {
            if (s.db_i > 0) { --s.db_i; load_cols(s); }
            continue;
        }
        if (k == "DOWN" && s.mode == mongodb::ViewMode::Query && s.q_focus == 0) {
            if (s.db_i + 1 < (int)s.dbs.size()) { ++s.db_i; load_cols(s); }
            continue;
        }

        // -------- Collection navigation (Left/Right) --------
        if (k == "LEFT" && s.mode == mongodb::ViewMode::Query && s.q_focus == 0) {
            if (s.col_i > 0) --s.col_i;
            continue;
        }
        if (k == "RIGHT" && s.mode == mongodb::ViewMode::Query && s.q_focus == 0) {
            if (s.col_i + 1 < (int)s.filtered.size()) ++s.col_i;
            continue;
        }

        // -------- Mode switches --------
        if (k == std::string("\x05", 1)) {           // ^e
            s.mode = mongodb::ViewMode::Query; s.ac_active = false; continue;
        }
        if (k == std::string("\x01", 1)) {           // ^a
            s.mode = mongodb::ViewMode::Aggregation; s.ac_active = false; continue;
        }
        if (k == std::string("\x13", 1)) {           // ^s
            s.mode = mongodb::ViewMode::Shell; s.ac_active = false;
            if (s.shell_cmd.empty()) {
                std::string c = s.filtered.empty() ? "users" : s.filtered[s.col_i];
                s.shell_cmd = "db." + c + ".find({})";
                s.shell_cx = (int)s.shell_cmd.size();
            }
            continue;
        }

        // -------- Enter: run (new query resets pagination) --------
        if (k == "\r" && s.mode == mongodb::ViewMode::Query) {
            s.col_searching = false;
            s.page = 0;
            term::busy("Running find…");
            run_paginated_find(s);
            continue;
        }

        // -------- Pagination: ^N next page, ^P prev page (Query mode) --------
        if (s.mode == mongodb::ViewMode::Query && k == std::string("\x0e", 1)) {
            long long pages = s.doc_count > 0
                ? ((long long)s.doc_count + s.page_size - 1) / s.page_size : 1;
            if (s.page + 1 < pages) {
                ++s.page;
                term::busy("Loading next page…");
                run_paginated_find(s);
            }
            continue;
        }
        if (s.mode == mongodb::ViewMode::Query && k == std::string("\x10", 1)) {
            if (s.page > 0) {
                --s.page;
                term::busy("Loading previous page…");
                run_paginated_find(s);
            }
            continue;
        }
        if (k == std::string("\x07", 1) && s.mode == mongodb::ViewMode::Aggregation) {
            s.page = 0;
            term::busy("Running aggregation…");
            run_paginated_agg(s, /*recount=*/true);
            continue;
        }

        // Aggregation preview pagination (only when AC popup is closed —
        // when it's open ^N/^P fall through to the editor's AC handler).
        if (s.mode == mongodb::ViewMode::Aggregation && !s.ac_active
            && k == std::string("\x0e", 1)) {
            long long pages = s.doc_count > 0
                ? ((long long)s.doc_count + s.page_size - 1) / s.page_size : 0;
            if (pages == 0 || s.page + 1 < pages) {
                ++s.page;
                term::busy("Loading next page…");
                run_paginated_agg(s, /*recount=*/false);
            }
            continue;
        }
        if (s.mode == mongodb::ViewMode::Aggregation && !s.ac_active
            && k == std::string("\x10", 1)) {
            if (s.page > 0) {
                --s.page;
                term::busy("Loading previous page…");
                run_paginated_agg(s, /*recount=*/false);
            }
            continue;
        }
        if (k == "\r" && s.mode == mongodb::ViewMode::Shell) {
            term::busy("Running shell command…");
            mongodb::QueryResult r = mongodb::run_shell(
                s.dbs.empty() ? std::string() : s.dbs[s.db_i], s.shell_cmd);
            s.docs = std::move(r.lines);
            s.doc_count = r.total;
            s.doc_shown = r.shown;
            s.doc_scroll = 0;
            continue;
        }

        // -------- Document scroll: ^j / ^k --------
        if (k == std::string("\x0a", 1)) {           // ^j
            if (s.doc_scroll + 1 < (int)s.docs.size()) ++s.doc_scroll;
            continue;
        }
        if (k == std::string("\x0b", 1)) {           // ^k
            if (s.doc_scroll > 0) --s.doc_scroll;
            continue;
        }

        // -------- Collection search input --------
        if (s.col_searching && k.size() == 1 && (unsigned char)k[0] >= 32) {
            s.col_search += k;
            s.col_i = 0;
            refresh_filtered(s);
            continue;
        }

        // -------- Query-field input --------
        if (s.mode == mongodb::ViewMode::Query && s.q_focus > 0
            && k.size() == 1 && (unsigned char)k[0] >= 32) {
            auto* p = q_field_ptr(s, s.q_focus);
            if (p) p->append(k);
            continue;
        }

        // -------- SHELL line editor --------
        if (s.mode == mongodb::ViewMode::Shell) {
            if (k == "LEFT") {
                if (s.shell_cx > 0) --s.shell_cx;
            } else if (k == "RIGHT") {
                if (s.shell_cx < (int)s.shell_cmd.size()) ++s.shell_cx;
            } else if (k == "\x7f" || k == "\x08") {
                if (s.shell_cx > 0) {
                    s.shell_cmd.erase(s.shell_cx - 1, 1);
                    --s.shell_cx;
                }
            } else if (k.size() == 1 && (unsigned char)k[0] >= 32) {
                s.shell_cmd.insert(s.shell_cx, k);
                ++s.shell_cx;
            }
            continue;
        }

        // -------- AGGREGATION editor --------
        if (s.mode == mongodb::ViewMode::Aggregation) {
            editor_normalize(s);
            bool ac_only_move = false;

            if (k == std::string("\x0e", 1)) {       // ^n
                if (s.ac_active && !s.ac_matches.empty()) {
                    s.ac_i = (s.ac_i + 1) % (int)s.ac_matches.size();
                    ac_only_move = true;
                }
            } else if (k == std::string("\x10", 1)) { // ^p
                if (s.ac_active && !s.ac_matches.empty()) {
                    s.ac_i = (s.ac_i - 1 + (int)s.ac_matches.size())
                             % (int)s.ac_matches.size();
                    ac_only_move = true;
                }
            } else if (k == "\t") {
                if (s.ac_active) ac_accept(s);
                else { s.agg_lines[s.cursor_y].insert(s.cursor_x, "  "); s.cursor_x += 2; }
            } else if (k == std::string("\x0c", 1)) {   // ^l  reformat
                editor_reformat(s);
            } else if (k == "LEFT") {
                if (s.cursor_x > 0) --s.cursor_x;
                else if (s.cursor_y > 0) {
                    --s.cursor_y;
                    s.cursor_x = (int)s.agg_lines[s.cursor_y].size();
                }
            } else if (k == "RIGHT") {
                int len = (int)s.agg_lines[s.cursor_y].size();
                if (s.cursor_x < len) ++s.cursor_x;
                else if (s.cursor_y + 1 < (int)s.agg_lines.size()) {
                    ++s.cursor_y;
                    s.cursor_x = 0;
                }
            } else if (k == "UP") {
                if (s.cursor_y > 0) --s.cursor_y;
                s.cursor_x = std::min(s.cursor_x, (int)s.agg_lines[s.cursor_y].size());
            } else if (k == "DOWN") {
                if (s.cursor_y + 1 < (int)s.agg_lines.size()) ++s.cursor_y;
                s.cursor_x = std::min(s.cursor_x, (int)s.agg_lines[s.cursor_y].size());
            } else if (k == "HOME") {
                s.cursor_x = 0;
            } else if (k == "END") {
                s.cursor_x = (int)s.agg_lines[s.cursor_y].size();
            } else if (k == "\x7f" || k == "\x08") {
                editor_smart_backspace(s);
            } else if (k == "\n" || k == "\r") {
                editor_smart_enter(s);
            } else if (k.size() == 1 && (unsigned char)k[0] >= 32) {
                editor_smart_insert(s, k[0]);
            }

            editor_normalize(s);
            if (!ac_only_move) ac_compute(s);
            continue;
        }
    }
}

} // namespace app

#include "mongo.hpp"
#include "term.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <bson/bson.h>
#include <mongoc/mongoc.h>

namespace mongodb {

namespace {

// Portable UTC breakdown: gmtime_r on POSIX, gmtime_s on the Windows CRT.
inline void gmtime_utc(const std::time_t* t, std::tm* out) {
#ifdef _WIN32
    gmtime_s(out, t);
#else
    gmtime_r(t, out);
#endif
}

// =========================================================
// DRIVER STATE
// =========================================================
// Pool-based: every call pops a client, runs, pushes it back. Sized to
// std::thread::hardware_concurrency() at connect time so future workers
// have headroom. With pool size 1 this still works for the current
// single-threaded main loop.
mongoc_client_pool_t* g_pool = nullptr;
mongoc_uri_t*         g_uri  = nullptr;
bson_error_t          g_err{};

constexpr int EXPORT_MAX = 100000;

std::string last_error() { return std::string(g_err.message); }

// RAII helper: pop a client from the pool, push it back on destruction.
// Safe to construct when g_pool is null — get() returns nullptr.
struct PooledClient {
    mongoc_client_t* c = nullptr;
    PooledClient() { if (g_pool) c = mongoc_client_pool_pop(g_pool); }
    ~PooledClient() { if (c && g_pool) mongoc_client_pool_push(g_pool, c); }
    PooledClient(const PooledClient&) = delete;
    PooledClient& operator=(const PooledClient&) = delete;
    mongoc_client_t* get() const { return c; }
    explicit operator bool() const { return c != nullptr; }
};

// =========================================================
// STRING HELPERS
// =========================================================
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_' || c == '$';
}
bool is_ident(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

// Wrap a value in double quotes unless it already is a string literal.
std::string as_quoted(std::string v) {
    v = trim(v);
    if (v.empty() || v[0] == '"') return v;
    return "\"" + v + "\"";
}

// JSON-escape a UTF-8 string for single-line display.
std::string jstr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// Format epoch milliseconds as ISODate("...Z").
std::string fmt_date(int64_t ms) {
    int64_t sec = ms / 1000;
    int     rem = (int)(ms - sec * 1000);
    if (rem < 0) { rem += 1000; sec -= 1; }
    std::time_t t = (std::time_t)sec;
    std::tm tm_utc{};
    gmtime_utc(&t, &tm_utc);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "ISODate(\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\")",
        tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
        tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, rem);
    return buf;
}

std::string pad(int n) { return std::string(n * 2, ' '); }

// =========================================================
// BSON -> DISPLAY TREE
// =========================================================
struct ValNode {
    bool        is_token = false;
    std::string raw;             // when is_token
    bool        is_array = false;
    // items have key only for documents (not arrays).
    struct Item { std::string k; std::shared_ptr<ValNode> v; };
    std::vector<Item> items;
};

// Cap recursion so pathological BSON (a degenerate doc nested thousands
// deep) can never blow the stack on us.
constexpr int MAX_BSON_DEPTH = 64;

std::shared_ptr<ValNode> read_value(bson_iter_t* it, int depth);

std::shared_ptr<ValNode> read_level(bson_iter_t* it, bool is_array, int depth) {
    auto node = std::make_shared<ValNode>();
    node->is_array = is_array;
    if (depth >= MAX_BSON_DEPTH) {
        node->is_token = true;
        node->raw = is_array ? "[ /* depth cap */ ]" : "{ /* depth cap */ }";
        return node;
    }
    while (bson_iter_next(it)) {
        ValNode::Item entry;
        if (!is_array) entry.k = bson_iter_key(it);
        entry.v = read_value(it, depth + 1);
        node->items.push_back(std::move(entry));
    }
    return node;
}

std::shared_ptr<ValNode> read_value(bson_iter_t* it, int depth) {
    auto node = std::make_shared<ValNode>();
    bson_type_t t = bson_iter_type(it);

    auto tok = [&](std::string s) {
        node->is_token = true;
        node->raw = std::move(s);
        return node;
    };

    switch (t) {
        case BSON_TYPE_UTF8: {
            uint32_t len = 0;
            const char* p = bson_iter_utf8(it, &len);
            return tok(jstr(std::string(p, len)));
        }
        case BSON_TYPE_INT32: {
            char b[16];
            std::snprintf(b, sizeof(b), "%d", bson_iter_int32(it));
            return tok(b);
        }
        case BSON_TYPE_INT64: {
            // Wrap in NumberLong(...) so the BSON type is visible and the
            // output round-trips through the relaxed-BSON parser. Same
            // convention mongosh and Compass use.
            char b[48];
            std::snprintf(b, sizeof(b), "NumberLong(\"%lld\")",
                (long long)bson_iter_int64(it));
            return tok(b);
        }
        case BSON_TYPE_DOUBLE: {
            // %.17g preserves full precision; falls back to scientific only
            // for huge/tiny magnitudes (avoids "1.23457e+09" for normal IDs).
            char b[40];
            std::snprintf(b, sizeof(b), "%.17g", bson_iter_double(it));
            return tok(b);
        }
        case BSON_TYPE_BOOL:
            return tok(bson_iter_bool(it) ? "true" : "false");
        case BSON_TYPE_NULL:
            return tok("null");
        case BSON_TYPE_OID: {
            char oid_str[25];
            bson_oid_to_string(bson_iter_oid(it), oid_str);
            return tok(std::string("ObjectId(\"") + oid_str + "\")");
        }
        case BSON_TYPE_DATE_TIME:
            return tok(fmt_date(bson_iter_date_time(it)));
        case BSON_TYPE_DECIMAL128: {
            bson_decimal128_t d;
            bson_iter_decimal128(it, &d);
            char b[BSON_DECIMAL128_STRING];
            bson_decimal128_to_string(&d, b);
            return tok(std::string("NumberDecimal(\"") + b + "\")");
        }
        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY: {
            bson_iter_t child;
            bool is_arr = (t == BSON_TYPE_ARRAY);
            if (bson_iter_recurse(it, &child)) {
                return read_level(&child, is_arr, depth);
            }
            node->is_array = is_arr;
            return node;
        }
        default: {
            const char* name = nullptr;
            switch (t) {
                case BSON_TYPE_BINARY:     name = "BinData"; break;
                case BSON_TYPE_UNDEFINED:  name = "undefined"; break;
                case BSON_TYPE_REGEX:      name = "Regex"; break;
                case BSON_TYPE_DBPOINTER:  name = "DBPointer"; break;
                case BSON_TYPE_CODE:       name = "Code"; break;
                case BSON_TYPE_SYMBOL:     name = "Symbol"; break;
                case BSON_TYPE_CODEWSCOPE: name = "CodeWScope"; break;
                case BSON_TYPE_TIMESTAMP:  name = "Timestamp"; break;
                case BSON_TYPE_MAXKEY:     name = "MaxKey"; break;
                case BSON_TYPE_MINKEY:     name = "MinKey"; break;
                default: break;
            }
            char buf[32];
            if (name) std::snprintf(buf, sizeof(buf), "<%s>", name);
            else      std::snprintf(buf, sizeof(buf), "<type %d>", (int)t);
            return tok(buf);
        }
    }
}

void emit(const ValNode& val, int indent,
          const std::string& prefix, const std::string& suffix,
          std::vector<std::string>& lines) {
    if (val.is_token) {
        lines.push_back(prefix + val.raw + suffix);
        return;
    }
    const char* open  = val.is_array ? "[" : "{";
    const char* close = val.is_array ? "]" : "}";
    size_t n = val.items.size();
    if (n == 0) {
        lines.push_back(prefix + open + close + suffix);
        return;
    }
    lines.push_back(prefix + open);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = val.items[i];
        std::string cprefix = pad(indent + 1);
        if (!val.is_array) cprefix += jstr(e.k) + ": ";
        emit(*e.v, indent + 1, cprefix, (i + 1 < n) ? "," : "", lines);
    }
    lines.push_back(pad(indent) + close + suffix);
}

std::vector<std::string> format_doc(const bson_t* doc) {
    bson_iter_t it;
    if (!bson_iter_init(&it, doc)) return { "{}" };
    auto root = read_level(&it, false, 0);
    std::vector<std::string> lines;
    emit(*root, 0, "", "", lines);
    return lines;
}

// =========================================================
// CSV CELL RENDERING
// =========================================================
std::string compact_value(const ValNode& v) {
    if (v.is_token) return v.raw;
    std::string out = v.is_array ? "[" : "{";
    bool first = true;
    for (const auto& e : v.items) {
        if (!first) out.push_back(',');
        first = false;
        if (v.is_array) out += compact_value(*e.v);
        else            out += jstr(e.k) + ":" + compact_value(*e.v);
    }
    out += v.is_array ? "]" : "}";
    return out;
}

std::string csv_value(bson_iter_t* it) {
    bson_type_t t = bson_iter_type(it);
    char buf[64];
    switch (t) {
        case BSON_TYPE_UTF8: {
            uint32_t len = 0;
            const char* p = bson_iter_utf8(it, &len);
            return std::string(p, len);
        }
        case BSON_TYPE_INT32:
            std::snprintf(buf, sizeof(buf), "%d", bson_iter_int32(it));
            return buf;
        case BSON_TYPE_INT64:
            std::snprintf(buf, sizeof(buf), "%lld", (long long)bson_iter_int64(it));
            return buf;
        case BSON_TYPE_DOUBLE:
            std::snprintf(buf, sizeof(buf), "%.17g", bson_iter_double(it));
            return buf;
        case BSON_TYPE_BOOL: return bson_iter_bool(it) ? "true" : "false";
        case BSON_TYPE_NULL: return "";
        case BSON_TYPE_OID: {
            char oid_str[25];
            bson_oid_to_string(bson_iter_oid(it), oid_str);
            return oid_str;
        }
        case BSON_TYPE_DATE_TIME: {
            int64_t ms = bson_iter_date_time(it);
            int64_t sec = ms / 1000;
            int rem = (int)(ms - sec * 1000);
            std::time_t t2 = (std::time_t)sec;
            std::tm tm_utc{};
            gmtime_utc(&t2, &tm_utc);
            std::snprintf(buf, sizeof(buf),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, rem);
            return buf;
        }
        case BSON_TYPE_DECIMAL128: {
            bson_decimal128_t d;
            bson_iter_decimal128(it, &d);
            char b[BSON_DECIMAL128_STRING];
            bson_decimal128_to_string(&d, b);
            return b;
        }
        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY:
            return compact_value(*read_value(it, 0));
        default:
            return "";
    }
}

struct CsvField { std::string k, v; };
std::vector<CsvField> csv_fields(const bson_t* doc) {
    std::vector<CsvField> out;
    bson_iter_t it;
    if (!bson_iter_init(&it, doc)) return out;
    while (bson_iter_next(&it)) {
        out.push_back({ bson_iter_key(&it), csv_value(&it) });
    }
    return out;
}

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

// =========================================================
// "RELAXER": Compass/shell BSON -> strict JSON
// =========================================================
// Walks the input character-by-character. String literals copy verbatim
// (so nothing inside "..." is ever rewritten). See the Lua source comments
// for the full grammar this accepts.

// Copies one "..."-string starting at i (s[i]=='"') into `out`, escapes
// preserved. Returns index just past the closing quote.
size_t copy_string(const std::string& s, size_t i, std::string& out) {
    out.push_back('"');
    ++i;
    while (i < s.size()) {
        char d = s[i];
        if (d == '\\') {
            if (i + 1 < s.size()) { out.push_back(d); out.push_back(s[i + 1]); i += 2; }
            else { out.push_back(d); ++i; }
        } else if (d == '"') {
            out.push_back('"');
            return i + 1;
        } else {
            out.push_back(d);
            ++i;
        }
    }
    return i;
}

// Pass 1: quote bare keys, normalize 'single' strings, strip trailing commas.
std::string relax(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];

        if (c == '"') {
            i = copy_string(s, i, out);
            continue;
        }
        if (c == '\'') {
            out.push_back('"');
            ++i;
            while (i < n) {
                char d = s[i];
                if (d == '\\' && i + 1 < n) {
                    char e = s[i + 1];
                    if (e == '\'') out.push_back('\'');
                    else { out.push_back('\\'); out.push_back(e); }
                    i += 2;
                } else if (d == '\'') {
                    ++i; break;
                } else if (d == '"') {
                    out += "\\\"";
                    ++i;
                } else {
                    out.push_back(d);
                    ++i;
                }
            }
            out.push_back('"');
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident(s[j])) ++j;
            std::string ident = s.substr(i, j - i);

            size_t k = j;
            while (k < n && std::isspace((unsigned char)s[k])) ++k;

            if (k < n && s[k] == ':') out += "\"" + ident + "\"";
            else out += ident;
            i = j;
            continue;
        }
        if (c == ',') {
            size_t k = i + 1;
            while (k < n && std::isspace((unsigned char)s[k])) ++k;
            if (k < n && (s[k] == '}' || s[k] == ']')) {
                ++i;
            } else {
                out.push_back(c);
                ++i;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// Parse a date/time string into epoch milliseconds (UTC). Accepts:
//   YYYY-MM-DD, YYYY-MM-DD HH:MM[:SS][.fff][Z|±HH:MM].
// Returns true on success.
bool date_to_millis(std::string s, int64_t& out_ms) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;

    int yn = std::atoi(s.substr(0, 4).c_str());
    int mn = std::atoi(s.substr(5, 2).c_str());
    int dn = std::atoi(s.substr(8, 2).c_str());

    std::string rest = s.substr(10);
    int hh = 0, mi = 0, ss = 0, frac = 0;

    if (rest.size() >= 6 && (rest[0] == 'T' || rest[0] == ' ')) {
        hh = std::atoi(rest.substr(1, 2).c_str());
        if (rest.size() >= 6 && rest[3] == ':') {
            mi = std::atoi(rest.substr(4, 2).c_str());
            if (rest.size() >= 9 && rest[6] == ':') {
                ss = std::atoi(rest.substr(7, 2).c_str());
            }
        }
    }
    size_t dot = rest.find('.');
    if (dot != std::string::npos) {
        std::string f;
        for (size_t i = dot + 1; i < rest.size() && std::isdigit((unsigned char)rest[i]); ++i) {
            f.push_back(rest[i]);
        }
        f += "000";
        f = f.substr(0, 3);
        frac = std::atoi(f.c_str());
    }

    // Civil-date algorithm: days since 1970-01-01, timezone-independent.
    int yy = (mn <= 2) ? (yn - 1) : yn;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    int mp  = (mn > 2) ? (mn - 3) : (mn + 9);
    int doy = (153 * mp + 2) / 5 + dn - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;

    int64_t secs = days * 86400LL + (int64_t)hh * 3600 + mi * 60 + ss;

    // ±HH:MM or ±HHMM offset.
    for (size_t i = 0; i + 4 < rest.size(); ++i) {
        char ch = rest[i];
        if ((ch == '+' || ch == '-') &&
            std::isdigit((unsigned char)rest[i + 1]) &&
            std::isdigit((unsigned char)rest[i + 2])) {
            int zh = std::atoi(rest.substr(i + 1, 2).c_str());
            int zm = 0;
            size_t j = i + 3;
            if (j < rest.size() && rest[j] == ':') ++j;
            if (j + 1 < rest.size() &&
                std::isdigit((unsigned char)rest[j]) &&
                std::isdigit((unsigned char)rest[j + 1])) {
                zm = std::atoi(rest.substr(j, 2).c_str());
            }
            int off = zh * 3600 + zm * 60;
            secs += (ch == '+' ? -off : off);
            break;
        }
    }

    out_ms = secs * 1000 + frac;
    return true;
}

std::string date_ext(int64_t ms) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "{\"$date\": {\"$numberLong\": \"%lld\"}}", (long long)ms);
    return buf;
}

// Pull a leading "..."-string out of s (escapes preserved); empty value
// returned if s doesn't start with a string.
bool take_string(std::string& rest, std::string& inner) {
    size_t i = 0;
    while (i < rest.size() && std::isspace((unsigned char)rest[i])) ++i;
    if (i >= rest.size() || rest[i] != '"') return false;
    ++i;
    inner.clear();
    while (i < rest.size()) {
        char c = rest[i];
        if (c == '\\' && i + 1 < rest.size()) {
            inner.push_back(c);
            inner.push_back(rest[i + 1]);
            i += 2;
        } else if (c == '"') {
            rest = rest.substr(i + 1);
            return true;
        } else {
            inner.push_back(c);
            ++i;
        }
    }
    rest.clear();
    return true;
}

// Translate one helper call -> Extended JSON; empty string = unknown.
std::string ctor_to_extjson(const std::string& name, std::string args) {
    args = trim(args);

    if (name == "ObjectId") {
        if (args.empty()) {
            bson_oid_t oid;
            bson_oid_init(&oid, nullptr);
            char buf[25];
            bson_oid_to_string(&oid, buf);
            return std::string("{\"$oid\": \"") + buf + "\"}";
        }
        return "{\"$oid\": " + as_quoted(args) + "}";
    }
    if (name == "ISODate" || name == "Date" || name == "new Date") {
        if (args.empty()) {
            int64_t ms = (int64_t)std::time(nullptr) * 1000;
            return date_ext(ms);
        }
        int64_t ms = 0;
        if (date_to_millis(args, ms)) return date_ext(ms);
        return "{\"$date\": " + as_quoted(args) + "}";
    }
    if (name == "NumberLong")    return "{\"$numberLong\": "    + as_quoted(args) + "}";
    if (name == "NumberInt")     return "{\"$numberInt\": "     + as_quoted(args) + "}";
    if (name == "NumberDecimal") return "{\"$numberDecimal\": " + as_quoted(args) + "}";

    if (name == "RegExp" || name == "Regex") {
        std::string rest = args, pat, opts;
        if (!take_string(rest, pat)) return {};
        // skip optional ", "
        size_t i = 0;
        while (i < rest.size() && std::isspace((unsigned char)rest[i])) ++i;
        if (i < rest.size() && rest[i] == ',') ++i;
        rest = rest.substr(i);
        take_string(rest, opts);
        return "{\"$regularExpression\": {\"pattern\": \"" + pat +
               "\", \"options\": \"" + opts + "\"}}";
    }
    return {};
}

// Pass 2: rewrite Helper(...) calls into Extended JSON.
std::string expand_ctors(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            i = copy_string(s, i, out);
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident(s[j])) ++j;
            std::string name = s.substr(i, j - i);

            size_t k = j;
            while (k < n && std::isspace((unsigned char)s[k])) ++k;

            if (k < n && s[k] == '(') {
                // Collect arguments up to the matching ')', honoring nested
                // brackets and string literals.
                std::string arg;
                int depth = 1;
                size_t p = k + 1;
                while (p < n && depth > 0) {
                    char d = s[p];
                    if (d == '"') {
                        p = copy_string(s, p, arg);
                    } else if (d == '(') {
                        ++depth; arg.push_back(d); ++p;
                    } else if (d == ')') {
                        --depth;
                        if (depth > 0) arg.push_back(d);
                        ++p;
                    } else {
                        arg.push_back(d); ++p;
                    }
                }
                std::string repl = ctor_to_extjson(name, arg);
                if (!repl.empty()) {
                    out += repl;
                } else {
                    out += s.substr(i, p - i);
                }
                i = p;
            } else {
                out += name;
                i = j;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// Build a canonical `{ "_id": <value> }` selector for `doc`. Canonical
// extended JSON preserves the exact BSON type so the selector round-trips
// back through bson_new_from_json. Returns "" if the doc has no _id.
std::string make_id_selector(const bson_t* doc) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, doc, "_id")) return std::string();
    bson_t sel;
    bson_init(&sel);
    bson_append_value(&sel, "_id", 3, bson_iter_value(&it));
    size_t len = 0;
    char* js = bson_as_canonical_extended_json(&sel, &len);
    std::string out = js ? std::string(js, len) : std::string();
    if (js) bson_free(js);
    bson_destroy(&sel);
    return out;
}

// =========================================================
// CURSOR DRAIN -> display lines
// =========================================================
// When `raw`/`ids` are non-null they receive one entry per *shown* document:
// the pretty JSON block (for copy/clone/edit) and the canonical _id selector.
void drain_cursor(mongoc_cursor_t* cursor, int display_limit, bool count_all,
                  std::vector<std::string>& docs, int64_t& total, int& shown,
                  std::vector<std::string>* raw = nullptr,
                  std::vector<std::string>* ids = nullptr) {
    total = 0; shown = 0;
    const bson_t* doc = nullptr;
    while (mongoc_cursor_next(cursor, &doc)) {
        ++total;
        if (shown < display_limit) {
            if (shown > 0) docs.push_back("");
            auto lines = format_doc(doc);
            if (raw) {
                std::string joined;
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (i) joined.push_back('\n');
                    joined += lines[i];
                }
                raw->push_back(std::move(joined));
            }
            if (ids) ids->push_back(make_id_selector(doc));
            for (auto& l : lines) docs.push_back(std::move(l));
            ++shown;
        } else if (!count_all) {
            break;
        }
    }
    if (mongoc_cursor_error(cursor, &g_err)) {
        docs.push_back(std::string("⚠ ") + last_error());
    }
}

// =========================================================
// FIND-OPTS BUILDER
// =========================================================
struct FindOpts {
    std::string json;      // empty when nothing was set
    bool        has_skip  = false;
    bool        has_limit = false;
    int64_t     skip      = 0;
    int64_t     limit     = 0;
};

FindOpts build_find_opts(const std::string& projection,
                         const std::string& sort,
                         const std::string& skip_str,
                         const std::string& limit_str) {
    FindOpts out;
    std::vector<std::string> parts;

    std::string proj = trim(projection);
    std::string srt  = trim(sort);
    std::string sk   = trim(skip_str);
    std::string lm   = trim(limit_str);

    if (!proj.empty()) parts.push_back("\"projection\": " + relax_bson(proj));
    if (!srt.empty())  parts.push_back("\"sort\": "       + relax_bson(srt));

    if (!sk.empty()) {
        char* end = nullptr;
        long long v = std::strtoll(sk.c_str(), &end, 10);
        if (end != sk.c_str()) {
            out.has_skip = true; out.skip = v;
            char b[32]; std::snprintf(b, sizeof(b), "%lld", v);
            parts.push_back(std::string("\"skip\": ") + b);
        }
    }
    if (!lm.empty()) {
        char* end = nullptr;
        long long v = std::strtoll(lm.c_str(), &end, 10);
        if (end != lm.c_str()) {
            out.has_limit = true; out.limit = v;
            char b[32]; std::snprintf(b, sizeof(b), "%lld", v);
            parts.push_back(std::string("\"limit\": ") + b);
        }
    }

    if (parts.empty()) return out;
    std::string js = "{ ";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) js += ", ";
        js += parts[i];
    }
    js += " }";
    out.json = std::move(js);
    return out;
}

// =========================================================
// SHELL ARG SPLITTER
// =========================================================
std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string buf;
    int depth = 0;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c;
            buf.push_back(c);
            ++i;
            while (i < n) {
                char d = s[i];
                buf.push_back(d);
                if (d == '\\' && i + 1 < n) {
                    buf.push_back(s[i + 1]);
                    i += 2;
                } else if (d == q) {
                    ++i; break;
                } else {
                    ++i;
                }
            }
        } else if (c == '{' || c == '[' || c == '(') {
            ++depth; buf.push_back(c); ++i;
        } else if (c == '}' || c == ']' || c == ')') {
            --depth; buf.push_back(c); ++i;
        } else if (c == ',' && depth == 0) {
            out.push_back(buf); buf.clear(); ++i;
        } else {
            buf.push_back(c); ++i;
        }
    }
    out.push_back(buf);
    return out;
}

// Parse one relaxed argument into BSON; default text used if arg is blank.
bson_t* shell_parse(const std::string& text, const std::string& default_text) {
    std::string t = trim(text);
    if (t.empty()) t = default_text;
    if (t.empty()) return nullptr;
    std::string strict = relax_bson(t);
    return bson_new_from_json(
        (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
}

void shell_show_reply(bool ok, const std::string& verb, bson_t* reply,
                      std::vector<std::string>& docs) {
    if (ok) {
        docs.push_back("✔ " + verb);
        for (auto& l : format_doc(reply)) docs.push_back(std::move(l));
    } else {
        docs.push_back(std::string("⚠ ") + last_error());
    }
    bson_destroy(reply);
}

// =========================================================
// EXPORT PATH
// =========================================================
std::string export_path(const std::string& db, const std::string& col,
                        ViewMode mode, const std::string& fmt) {
    auto safe = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') {
                r.push_back(c);
            } else {
                r.push_back('_');
            }
        }
        return r;
    };
    const char* tag = (mode == ViewMode::Aggregation) ? "agg" : "find";

    std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_utc(&t, &utc);
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d%02d%02dT%02d%02d%02dZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec);

    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : ".";
    return base + "/Downloads/" + safe(db) + "." + safe(col) + "." + tag +
           "." + ts + "." + fmt;
}

} // anonymous

// =========================================================
// PUBLIC: lifecycle
// =========================================================
// Swallow libmongoc's legacy log stream (connection-monitor DEBUG chatter,
// etc.). mongui surfaces its own errors via return values, and stray stderr
// writes would corrupt the TUI and clutter the --import output.
void quiet_log_handler(mongoc_log_level_t, const char*, const char*, void*) {}

bool init() {
    mongoc_init();
    mongoc_log_set_handler(quiet_log_handler, nullptr);
    return true;
}

void cleanup() {
    if (g_pool) {
        mongoc_client_pool_destroy(g_pool);
        g_pool = nullptr;
    }
    if (g_uri) {
        mongoc_uri_destroy(g_uri);
        g_uri = nullptr;
    }
    mongoc_cleanup();
}

bool connect(const std::string& uri) {
    bson_error_t err{};
    g_uri = mongoc_uri_new_with_error(uri.c_str(), &err);
    if (!g_uri) {
        g_err = err;
        return false;
    }
    g_pool = mongoc_client_pool_new(g_uri);
    if (!g_pool) {
        mongoc_uri_destroy(g_uri);
        g_uri = nullptr;
        return false;
    }

    // Pool size matches CPU cores so future worker threads have a client
    // each. Clamped to a sane range — pool min size is 1 client, no point
    // burning ten if you only have two cores; capped at 32 so we don't
    // chew driver memory on a 128-thread box.
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    if (cores < 2)  cores = 2;
    if (cores > 32) cores = 32;
    mongoc_client_pool_max_size(g_pool, cores);

    return true;
}

void disconnect() {
    if (g_pool) {
        mongoc_client_pool_destroy(g_pool);
        g_pool = nullptr;
    }
    if (g_uri) {
        mongoc_uri_destroy(g_uri);
        g_uri = nullptr;
    }
}

std::string relax_bson(const std::string& s) {
    return expand_ctors(relax(s));
}

// =========================================================
// PUBLIC: enumeration
// =========================================================
std::vector<std::string> list_databases(std::string& error_out) {
    std::vector<std::string> out;
    error_out.clear();
    if (!g_pool) { error_out = "not connected"; return out; }

    PooledClient pc;
    if (!pc) { error_out = "no client available"; return out; }

    char** arr = mongoc_client_get_database_names_with_opts(pc.get(), nullptr, &g_err);
    if (!arr) { error_out = last_error(); return out; }
    for (int i = 0; arr[i]; ++i) out.emplace_back(arr[i]);
    bson_strfreev(arr);
    return out;
}

std::vector<std::string> list_collections(const std::string& db,
                                          std::string& error_out) {
    std::vector<std::string> out;
    error_out.clear();
    if (!g_pool) { error_out = "not connected"; return out; }

    PooledClient pc;
    if (!pc) { error_out = "no client available"; return out; }

    mongoc_database_t* d = mongoc_client_get_database(pc.get(), db.c_str());
    char** arr = mongoc_database_get_collection_names_with_opts(d, nullptr, &g_err);
    if (arr) {
        for (int i = 0; arr[i]; ++i) out.emplace_back(arr[i]);
        bson_strfreev(arr);
    } else {
        error_out = last_error();
    }
    mongoc_database_destroy(d);
    return out;
}

// =========================================================
// PUBLIC: find
// =========================================================
QueryResult run_find(const std::string& db, const std::string& col,
                     const std::string& filter,
                     const std::string& projection,
                     const std::string& sort,
                     const std::string& skip_str,
                     const std::string& limit_str,
                     int display_limit) {
    QueryResult result;
    if (!g_pool) { result.lines = { "⚠ not connected" }; return result; }
    if (db.empty() || col.empty()) {
        result.lines = { "⚠ Select a database and collection first" };
        return result;
    }

    PooledClient pc;
    if (!pc) { result.lines = { "⚠ no client available" }; return result; }

    std::string strict_filter = relax_bson(filter);
    bson_t* bf = bson_new_from_json(
        (const uint8_t*)strict_filter.data(), (ssize_t)strict_filter.size(), &g_err);
    if (!bf) {
        result.lines = { std::string("⚠ Invalid filter: ") + last_error() };
        return result;
    }

    FindOpts opts = build_find_opts(projection, sort, skip_str, limit_str);
    bson_t* bopts = nullptr;
    if (!opts.json.empty()) {
        bopts = bson_new_from_json(
            (const uint8_t*)opts.json.data(), (ssize_t)opts.json.size(), &g_err);
        if (!bopts) {
            result.lines = { std::string("⚠ Invalid query options: ") + last_error() };
            bson_destroy(bf);
            return result;
        }
    }

    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());

    int64_t matched =
        mongoc_collection_count_documents(c, bf, nullptr, nullptr, nullptr, &g_err);

    mongoc_cursor_t* cur =
        mongoc_collection_find_with_opts(c, bf, bopts, nullptr);

    int64_t total = 0; int shown = 0;
    drain_cursor(cur, display_limit, opts.has_limit, result.lines, total, shown,
                 &result.raw, &result.ids);
    result.shown = shown;

    if (opts.has_limit) {
        result.total = total;
    } else if (matched >= 0) {
        int64_t skip = opts.has_skip ? opts.skip : 0;
        result.total = std::max((int64_t)0, matched - skip);
    } else {
        result.total = shown;
    }

    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(c);
    bson_destroy(bf);
    if (bopts) bson_destroy(bopts);
    return result;
}

// =========================================================
// PUBLIC: aggregate
// =========================================================
QueryResult run_aggregation(const std::string& db, const std::string& col,
                            const std::string& pipeline_text,
                            int display_limit) {
    QueryResult result;
    if (db.empty() || col.empty()) {
        result.lines = { "⚠ Select a database and collection first" };
        return result;
    }
    PooledClient pc;
    if (!pc) { result.lines = { "⚠ not connected" }; return result; }

    std::string wrapped = "{ \"pipeline\": " + relax_bson(pipeline_text) + " }";
    bson_t* pipeline = bson_new_from_json(
        (const uint8_t*)wrapped.data(), (ssize_t)wrapped.size(), &g_err);
    if (!pipeline) {
        result.lines = { std::string("⚠ Invalid pipeline: ") + last_error() };
        return result;
    }

    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());
    mongoc_cursor_t* cur =
        mongoc_collection_aggregate(c, MONGOC_QUERY_NONE, pipeline, nullptr, nullptr);

    int64_t total = 0; int shown = 0;
    drain_cursor(cur, display_limit, true, result.lines, total, shown);
    result.shown = shown;
    result.total = total;

    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(c);
    bson_destroy(pipeline);
    return result;
}

// =========================================================
// PUBLIC: shell (db.<coll>.<method>(...))
// =========================================================
QueryResult run_shell(const std::string& db, const std::string& cmd) {
    QueryResult result;
    if (db.empty()) { result.lines = { "⚠ No database selected" }; return result; }
    PooledClient pc;
    if (!pc) { result.lines = { "⚠ not connected" }; return result; }

    // Parse `db.<collection>.<method>(<args>);?` with a small state machine.
    std::string s = trim(cmd);
    if (s.size() < 4 || s.substr(0, 3) != "db.") {
        result.lines = {
            "⚠ Expected:  db.<collection>.<method>(...)",
            "  e.g. db.users.updateMany({ email: 'a' }, { $set: { email: 'b' } })" };
        return result;
    }
    size_t i = 3;
    size_t col_start = i;
    while (i < s.size() && s[i] != '.') ++i;
    if (i >= s.size()) {
        result.lines = { "⚠ Expected:  db.<collection>.<method>(...)" };
        return result;
    }
    std::string col = s.substr(col_start, i - col_start);
    ++i; // skip '.'

    size_t m_start = i;
    while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
    std::string method = s.substr(m_start, i - m_start);

    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    if (i >= s.size() || s[i] != '(') {
        result.lines = { "⚠ Expected:  db.<collection>.<method>(...)" };
        return result;
    }
    ++i;
    // Find matching ')'
    int depth = 1;
    size_t arg_start = i;
    while (i < s.size() && depth > 0) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c; ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else if (s[i] == q) { ++i; break; }
                else ++i;
            }
        } else if (c == '(') { ++depth; ++i; }
        else if (c == ')') { --depth; if (depth == 0) break; ++i; }
        else ++i;
    }
    if (depth != 0) {
        result.lines = { "⚠ Unbalanced parentheses" };
        return result;
    }
    std::string argstr = s.substr(arg_start, i - arg_start);

    std::vector<std::string> args = split_args(argstr);
    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());

    bson_t reply_storage;
    auto reply = [&]() -> bson_t* { return &reply_storage; };

    // -------- READS --------
    if (method == "find" || method == "findOne") {
        bson_t* filter = shell_parse(args.size() >= 1 ? args[0] : "", "{}");
        if (!filter) {
            result.lines = { std::string("⚠ filter: ") + last_error() };
        } else {
            std::vector<std::string> optparts;
            if (args.size() >= 2 && !trim(args[1]).empty()) {
                optparts.push_back("\"projection\": " + relax_bson(args[1]));
            }
            if (method == "findOne") optparts.push_back("\"limit\": 1");
            bson_t* opts = nullptr;
            if (!optparts.empty()) {
                std::string js = "{ ";
                for (size_t k = 0; k < optparts.size(); ++k) {
                    if (k) js += ", ";
                    js += optparts[k];
                }
                js += " }";
                opts = bson_new_from_json(
                    (const uint8_t*)js.data(), (ssize_t)js.size(), &g_err);
            }
            mongoc_cursor_t* cur =
                mongoc_collection_find_with_opts(c, filter, opts, nullptr);
            int64_t total = 0; int shown = 0;
            int lim = method == "findOne" ? 1 : 50;
            drain_cursor(cur, lim, false, result.lines, total, shown);
            result.shown = shown;
            result.total = shown;
            mongoc_cursor_destroy(cur);
            if (opts) bson_destroy(opts);
            bson_destroy(filter);
        }
    } else if (method == "aggregate") {
        std::string arr = trim(args.size() >= 1 ? args[0] : "[]");
        std::string js = "{ \"pipeline\": " + relax_bson(arr) + " }";
        bson_t* pipeline = bson_new_from_json(
            (const uint8_t*)js.data(), (ssize_t)js.size(), &g_err);
        if (!pipeline) {
            result.lines = { std::string("⚠ pipeline: ") + last_error() };
        } else {
            mongoc_cursor_t* cur = mongoc_collection_aggregate(
                c, MONGOC_QUERY_NONE, pipeline, nullptr, nullptr);
            int64_t total = 0; int shown = 0;
            drain_cursor(cur, 50, true, result.lines, total, shown);
            result.shown = shown;
            result.total = total;
            mongoc_cursor_destroy(cur);
            bson_destroy(pipeline);
        }
    } else if (method == "countDocuments" || method == "count"
            || method == "estimatedDocumentCount") {
        bson_t* filter = shell_parse(args.size() >= 1 ? args[0] : "", "{}");
        if (!filter) {
            result.lines = { std::string("⚠ filter: ") + last_error() };
        } else {
            int64_t nn = mongoc_collection_count_documents(
                c, filter, nullptr, nullptr, nullptr, &g_err);
            if (nn >= 0) {
                char b[32]; std::snprintf(b, sizeof(b), "%lld", (long long)nn);
                result.lines = { b };
            } else {
                result.lines = { std::string("⚠ ") + last_error() };
            }
            bson_destroy(filter);
        }
    }

    // -------- WRITES --------
    else if (method == "insertOne") {
        bson_t* doc = shell_parse(args.size() >= 1 ? args[0] : "", "");
        if (!doc) {
            result.lines = { std::string("⚠ document: ") + last_error() };
        } else {
            bool ok = mongoc_collection_insert_one(c, doc, nullptr, reply(), &g_err);
            shell_show_reply(ok, "insertOne", reply(), result.lines);
            bson_destroy(doc);
        }
    } else if (method == "insertMany") {
        std::string body = trim(args.size() >= 1 ? args[0] : "");
        if (!body.empty() && body.front() == '[') body.erase(body.begin());
        if (!body.empty() && body.back()  == ']') body.pop_back();
        auto elems = split_args(body);
        std::vector<bson_t*> docs;
        bool bad = false;
        for (auto& e : elems) {
            if (trim(e).empty()) continue;
            std::string strict = relax_bson(e);
            bson_t* d = bson_new_from_json(
                (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
            if (!d) {
                result.lines = { std::string("⚠ document ") +
                    std::to_string(docs.size() + 1) + ": " + last_error() };
                bad = true; break;
            }
            docs.push_back(d);
        }
        if (!bad && !docs.empty()) {
            std::vector<const bson_t*> arr(docs.begin(), docs.end());
            bool ok = mongoc_collection_insert_many(
                c, arr.data(), arr.size(), nullptr, reply(), &g_err);
            shell_show_reply(ok, "insertMany (" + std::to_string(docs.size()) + ")",
                             reply(), result.lines);
        } else if (!bad) {
            result.lines = { "⚠ insertMany needs an array of documents" };
        }
        for (bson_t* d : docs) bson_destroy(d);
    } else if (method == "updateOne" || method == "updateMany" || method == "replaceOne") {
        bson_t* selector = shell_parse(args.size() >= 1 ? args[0] : "", "");
        bson_t* change   = shell_parse(args.size() >= 2 ? args[1] : "", "");
        if (!selector || !change) {
            result.lines = { std::string("⚠ ") + method +
                " needs (filter, update[, opts]): " + last_error() };
        } else {
            bson_t* opts = nullptr;
            if (args.size() >= 3 && !trim(args[2]).empty()) {
                std::string strict = relax_bson(args[2]);
                opts = bson_new_from_json(
                    (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
            }
            bool ok;
            if      (method == "updateOne")
                ok = mongoc_collection_update_one (c, selector, change, opts, reply(), &g_err);
            else if (method == "updateMany")
                ok = mongoc_collection_update_many(c, selector, change, opts, reply(), &g_err);
            else
                ok = mongoc_collection_replace_one(c, selector, change, opts, reply(), &g_err);
            shell_show_reply(ok, method, reply(), result.lines);
            if (opts) bson_destroy(opts);
        }
        if (selector) bson_destroy(selector);
        if (change)   bson_destroy(change);
    } else if (method == "deleteOne" || method == "deleteMany") {
        bson_t* selector = shell_parse(args.size() >= 1 ? args[0] : "", "");
        if (!selector) {
            result.lines = { std::string("⚠ ") + method + " needs a filter: " + last_error() };
        } else {
            bool ok = (method == "deleteOne")
                ? mongoc_collection_delete_one (c, selector, nullptr, reply(), &g_err)
                : mongoc_collection_delete_many(c, selector, nullptr, reply(), &g_err);
            shell_show_reply(ok, method, reply(), result.lines);
            bson_destroy(selector);
        }
    }

    // -------- INDEX OPS --------
    else if (method == "createIndex" || method == "createIndexes") {
        if (args.empty() || trim(args[0]).empty()) {
            result.lines = {
                "⚠ Expected: createIndex({ field: 1 } [, { unique: true }])",
                "          or createIndexes([{ key: { f: 1 }, name: 'f_1' }, …])"
            };
        } else {
            std::string indexes_js;

            if (method == "createIndex") {
                // Single index — auto-name from keys when caller didn't provide one.
                std::string keys = relax_bson(trim(args[0]));
                std::string opts = (args.size() >= 2 && !trim(args[1]).empty())
                    ? relax_bson(trim(args[1])) : std::string("{}");

                bool has_name = opts.find("\"name\"") != std::string::npos;
                std::string auto_name;
                if (!has_name) {
                    bson_t* k = bson_new_from_json(
                        (const uint8_t*)keys.data(), (ssize_t)keys.size(), &g_err);
                    if (k) {
                        bson_iter_t it;
                        bson_iter_init(&it, k);
                        while (bson_iter_next(&it)) {
                            if (!auto_name.empty()) auto_name.push_back('_');
                            auto_name += bson_iter_key(&it);
                            auto_name.push_back('_');
                            bson_type_t t = bson_iter_type(&it);
                            if (t == BSON_TYPE_INT32) {
                                char b[16];
                                std::snprintf(b, sizeof(b), "%d", bson_iter_int32(&it));
                                auto_name += b;
                            } else if (t == BSON_TYPE_INT64) {
                                char b[24];
                                std::snprintf(b, sizeof(b), "%lld",
                                    (long long)bson_iter_int64(&it));
                                auto_name += b;
                            } else if (t == BSON_TYPE_DOUBLE) {
                                char b[24];
                                std::snprintf(b, sizeof(b), "%g", bson_iter_double(&it));
                                auto_name += b;
                            } else if (t == BSON_TYPE_UTF8) {
                                uint32_t l = 0;
                                const char* p = bson_iter_utf8(&it, &l);
                                auto_name.append(p, l);
                            } else {
                                auto_name += "x";
                            }
                        }
                        bson_destroy(k);
                    }
                    if (auto_name.empty()) auto_name = "idx";
                }

                indexes_js = "[ { \"key\": " + keys;
                std::string opts_trim = trim(opts);
                if (opts_trim.size() > 2) {  // not just "{}"
                    indexes_js += ", " + opts_trim.substr(1, opts_trim.size() - 2);
                }
                if (!has_name) indexes_js += ", \"name\": " + jstr(auto_name);
                indexes_js += " } ]";
            } else {
                // createIndexes([...]) — pass the array through as-is.
                indexes_js = relax_bson(trim(args[0]));
            }

            std::string cmd_js = "{ \"createIndexes\": " + jstr(col)
                + ", \"indexes\": " + indexes_js + " }";
            bson_t* cmd = bson_new_from_json(
                (const uint8_t*)cmd_js.data(), (ssize_t)cmd_js.size(), &g_err);
            if (!cmd) {
                result.lines = { std::string("⚠ ") + method + ": " + last_error() };
            } else {
                mongoc_database_t* d =
                    mongoc_client_get_database(pc.get(), db.c_str());
                bool ok = mongoc_database_write_command_with_opts(
                    d, cmd, nullptr, reply(), &g_err);
                shell_show_reply(ok, method, reply(), result.lines);
                mongoc_database_destroy(d);
                bson_destroy(cmd);
            }
        }
    } else if (method == "dropIndex") {
        if (args.empty() || trim(args[0]).empty()) {
            result.lines = {
                "⚠ Expected: dropIndex('name')  or  dropIndex({ field: 1 })"
            };
        } else {
            // Argument may be a string name or a key spec; relax_bson handles both.
            std::string idx_value = relax_bson(trim(args[0]));
            std::string cmd_js = "{ \"dropIndexes\": " + jstr(col)
                + ", \"index\": " + idx_value + " }";
            bson_t* cmd = bson_new_from_json(
                (const uint8_t*)cmd_js.data(), (ssize_t)cmd_js.size(), &g_err);
            if (!cmd) {
                result.lines = { std::string("⚠ dropIndex: ") + last_error() };
            } else {
                mongoc_database_t* d =
                    mongoc_client_get_database(pc.get(), db.c_str());
                bool ok = mongoc_database_write_command_with_opts(
                    d, cmd, nullptr, reply(), &g_err);
                shell_show_reply(ok, "dropIndex", reply(), result.lines);
                mongoc_database_destroy(d);
                bson_destroy(cmd);
            }
        }
    } else if (method == "dropIndexes") {
        // dropIndexes() drops every index except _id (server convention: "*").
        std::string cmd_js = "{ \"dropIndexes\": " + jstr(col)
            + ", \"index\": \"*\" }";
        bson_t* cmd = bson_new_from_json(
            (const uint8_t*)cmd_js.data(), (ssize_t)cmd_js.size(), &g_err);
        if (!cmd) {
            result.lines = { std::string("⚠ dropIndexes: ") + last_error() };
        } else {
            mongoc_database_t* d =
                mongoc_client_get_database(pc.get(), db.c_str());
            bool ok = mongoc_database_write_command_with_opts(
                d, cmd, nullptr, reply(), &g_err);
            shell_show_reply(ok, "dropIndexes", reply(), result.lines);
            mongoc_database_destroy(d);
            bson_destroy(cmd);
        }
    } else if (method == "getIndexes" || method == "indexes"
            || method == "listIndexes") {
        mongoc_cursor_t* cur =
            mongoc_collection_find_indexes_with_opts(c, nullptr);
        int64_t total = 0; int shown = 0;
        drain_cursor(cur, 100, true, result.lines, total, shown);
        result.shown = shown;
        result.total = total;
        mongoc_cursor_destroy(cur);
    }

    else {
        result.lines = {
            std::string("⚠ Unsupported method: ") + method,
            "  find findOne aggregate countDocuments",
            "  insertOne insertMany updateOne updateMany replaceOne",
            "  deleteOne deleteMany",
            "  createIndex createIndexes dropIndex dropIndexes getIndexes"
        };
    }

    mongoc_collection_destroy(c);
    return result;
}

// =========================================================
// PUBLIC: single-document writes (documents-pane actions)
// =========================================================
namespace {

// Read an integer field (deletedCount / modifiedCount …) out of a write reply.
int64_t reply_count(const bson_t* reply, const char* key) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, reply, key)) return bson_iter_as_int64(&it);
    return -1;
}

// Common preamble: open a pooled client and the target collection. Returns
// nullptr (with `err` set on the result) when unavailable.
mongoc_collection_t* open_coll(PooledClient& pc, const std::string& db,
                               const std::string& col, WriteResult& out) {
    if (db.empty() || col.empty()) { out.message = "⚠ No collection selected"; return nullptr; }
    if (!pc)                       { out.message = "⚠ not connected";          return nullptr; }
    return mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());
}

} // anonymous

WriteResult delete_doc(const std::string& db, const std::string& col,
                       const std::string& selector_json) {
    WriteResult out;
    PooledClient pc;
    mongoc_collection_t* c = open_coll(pc, db, col, out);
    if (!c) return out;

    bson_t* sel = bson_new_from_json(
        (const uint8_t*)selector_json.data(), (ssize_t)selector_json.size(), &g_err);
    if (!sel) { out.message = std::string("⚠ bad selector: ") + last_error();
                mongoc_collection_destroy(c); return out; }

    bson_t reply;
    out.ok = mongoc_collection_delete_one(c, sel, nullptr, &reply, &g_err);
    if (out.ok) {
        int64_t n = reply_count(&reply, "deletedCount");
        out.message = "✔ deleted " + std::to_string(n < 0 ? 1 : n) + " document";
    } else {
        out.message = std::string("⚠ ") + last_error();
    }
    bson_destroy(&reply);
    bson_destroy(sel);
    mongoc_collection_destroy(c);
    return out;
}

WriteResult replace_doc(const std::string& db, const std::string& col,
                        const std::string& selector_json,
                        const std::string& doc_text) {
    WriteResult out;
    PooledClient pc;
    mongoc_collection_t* c = open_coll(pc, db, col, out);
    if (!c) return out;

    bson_t* sel = bson_new_from_json(
        (const uint8_t*)selector_json.data(), (ssize_t)selector_json.size(), &g_err);
    std::string strict = relax_bson(doc_text);
    bson_t* doc = bson_new_from_json(
        (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
    if (!sel || !doc) {
        out.message = std::string("⚠ ") + last_error();
        if (sel) bson_destroy(sel);
        if (doc) bson_destroy(doc);
        mongoc_collection_destroy(c);
        return out;
    }

    bson_t reply;
    out.ok = mongoc_collection_replace_one(c, sel, doc, nullptr, &reply, &g_err);
    out.message = out.ok ? "✔ document updated"
                         : (std::string("⚠ ") + last_error());
    bson_destroy(&reply);
    bson_destroy(sel);
    bson_destroy(doc);
    mongoc_collection_destroy(c);
    return out;
}

WriteResult clone_doc(const std::string& db, const std::string& col,
                      const std::string& doc_text) {
    WriteResult out;
    PooledClient pc;
    mongoc_collection_t* c = open_coll(pc, db, col, out);
    if (!c) return out;

    std::string strict = relax_bson(doc_text);
    bson_t* src = bson_new_from_json(
        (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
    if (!src) { out.message = std::string("⚠ ") + last_error();
                mongoc_collection_destroy(c); return out; }

    // Copy every field except _id so the server mints a fresh id for the clone.
    bson_t doc;
    bson_init(&doc);
    bson_iter_t it;
    if (bson_iter_init(&it, src)) {
        while (bson_iter_next(&it)) {
            if (std::strcmp(bson_iter_key(&it), "_id") == 0) continue;
            bson_append_value(&doc, bson_iter_key(&it), -1, bson_iter_value(&it));
        }
    }

    bson_t reply;
    out.ok = mongoc_collection_insert_one(c, &doc, nullptr, &reply, &g_err);
    out.message = out.ok ? "✔ cloned document"
                         : (std::string("⚠ ") + last_error());
    bson_destroy(&reply);
    bson_destroy(&doc);
    bson_destroy(src);
    mongoc_collection_destroy(c);
    return out;
}

// =========================================================
// PUBLIC: bulk import from a file
// =========================================================
ImportResult import_documents(const std::string& db, const std::string& col,
                              const std::string& filepath) {
    ImportResult out;
    if (db.empty() || col.empty()) {
        out.message = "⚠ need both a database and a collection"; return out;
    }

    std::ifstream f(filepath, std::ios::binary);
    if (!f) { out.message = std::string("⚠ cannot open file: ") + filepath; return out; }
    std::ostringstream ss; ss << f.rdbuf();
    std::string content = ss.str();
    if (trim(content).empty()) { out.message = "⚠ file is empty"; return out; }

    PooledClient pc;
    if (!pc) { out.message = "⚠ not connected"; return out; }
    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());

    std::vector<bson_t*> batch;
    auto flush_batch = [&]() -> bool {
        if (batch.empty()) return true;
        std::vector<const bson_t*> arr(batch.begin(), batch.end());
        bson_t reply;
        bool ok = mongoc_collection_insert_many(
            c, arr.data(), arr.size(), nullptr, &reply, &g_err);
        bson_destroy(&reply);
        if (ok) out.inserted += (long long)batch.size();
        for (bson_t* b : batch) bson_destroy(b);
        batch.clear();
        return ok;
    };

    bool ok = true;
    std::string trimmed = trim(content);

    if (trimmed.front() == '[') {
        // JSON array: strip the outer brackets, split top-level elements.
        std::string body = trimmed.substr(1, trimmed.size() - 2);
        for (auto& e : split_args(body)) {
            if (trim(e).empty()) continue;
            std::string strict = relax_bson(e);
            bson_t* d = bson_new_from_json(
                (const uint8_t*)strict.data(), (ssize_t)strict.size(), &g_err);
            if (!d) { ok = false; out.message = std::string("⚠ parse error: ") + last_error(); break; }
            batch.push_back(d);
            if (batch.size() >= 1000 && !flush_batch()) { ok = false; break; }
        }
    } else {
        // NDJSON / concatenated / single object: stream documents out of the
        // relaxed text with libbson's JSON reader (handles multi-line objects).
        std::string strict = relax_bson(content);
        bson_json_reader_t* r = bson_json_data_reader_new(true, 4096);
        bson_json_data_reader_ingest(r, (const uint8_t*)strict.data(), strict.size());
        bson_t doc = BSON_INITIALIZER;
        int rc;
        while ((rc = bson_json_reader_read(r, &doc, &g_err)) == 1) {
            batch.push_back(bson_copy(&doc));
            bson_reinit(&doc);
            if (batch.size() >= 1000 && !flush_batch()) { ok = false; break; }
        }
        if (rc < 0) { ok = false; out.message = std::string("⚠ parse error: ") + last_error(); }
        bson_destroy(&doc);
        bson_json_reader_destroy(r);
    }

    // Flush whatever parsed cleanly (even on a later error, keep good docs).
    if (!flush_batch() && ok) { ok = false; out.message = std::string("⚠ insert failed: ") + last_error(); }

    mongoc_collection_destroy(c);
    out.ok = ok;
    if (out.message.empty())
        out.message = (ok ? "✔ imported " : "⚠ imported ") +
                      std::to_string(out.inserted) + " document(s)";
    return out;
}

int64_t count_documents(const std::string& db, const std::string& col,
                        const std::string& filter, std::string& error_out) {
    error_out.clear();
    if (!g_pool)     { error_out = "not connected";        return -1; }
    if (db.empty())  { error_out = "no database selected"; return -1; }
    if (col.empty()) { error_out = "no collection selected"; return -1; }

    PooledClient pc;
    if (!pc) { error_out = "no client available"; return -1; }

    std::string strict_filter =
        relax_bson(filter.empty() ? std::string("{}") : filter);
    bson_t* bf = bson_new_from_json(
        (const uint8_t*)strict_filter.data(), (ssize_t)strict_filter.size(), &g_err);
    if (!bf) { error_out = std::string("invalid filter: ") + last_error(); return -1; }

    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());
    int64_t n = mongoc_collection_count_documents(c, bf, nullptr, nullptr, nullptr, &g_err);
    if (n < 0) error_out = last_error();
    mongoc_collection_destroy(c);
    bson_destroy(bf);
    return n;
}

int64_t count_aggregation(const std::string& db, const std::string& col,
                          const std::string& pipeline_text) {
    if (!g_pool || db.empty() || col.empty()) return -1;
    PooledClient pc;
    if (!pc) return -1;

    // Insert `{ $count: "n" }` as the final stage before the closing `]`.
    std::string src = pipeline_text;
    size_t r = src.rfind(']');
    if (r == std::string::npos) return -1;
    std::string before = src.substr(0, r);
    while (!before.empty() && (before.back() == ' ' || before.back() == '\n'
                            || before.back() == '\t' || before.back() == '\r')) {
        before.pop_back();
    }
    const char* sep = (before.empty() || before.back() == '[') ? "" : ", ";
    std::string counted = before + sep + "{ $count: \"n\" } ]";

    std::string wrapped = "{ \"pipeline\": " + relax_bson(counted) + " }";
    bson_t* pipeline = bson_new_from_json(
        (const uint8_t*)wrapped.data(), (ssize_t)wrapped.size(), &g_err);
    if (!pipeline) return -1;

    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());
    mongoc_cursor_t* cur =
        mongoc_collection_aggregate(c, MONGOC_QUERY_NONE, pipeline, nullptr, nullptr);

    int64_t total = -1;
    const bson_t* doc = nullptr;
    if (mongoc_cursor_next(cur, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "n")) {
            if      (BSON_ITER_HOLDS_INT32 (&it)) total = bson_iter_int32(&it);
            else if (BSON_ITER_HOLDS_INT64 (&it)) total = bson_iter_int64(&it);
            else if (BSON_ITER_HOLDS_DOUBLE(&it)) total = (int64_t)bson_iter_double(&it);
        }
    }
    // Empty result = pipeline matched zero documents.
    if (total < 0 && !mongoc_cursor_error(cur, &g_err)) total = 0;

    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(c);
    bson_destroy(pipeline);
    return total;
}

// =========================================================
// PUBLIC: export
// =========================================================
ExportResult export_results(const std::string& db, const std::string& col,
                            const std::string& fmt,
                            ViewMode mode,
                            const std::string& filter,
                            const std::string& projection,
                            const std::string& sort,
                            const std::string& skip_str,
                            const std::string& limit_str,
                            const std::string& pipeline_text) {
    ExportResult out;
    if (!g_pool) { out.message = "⚠ not connected"; return out; }
    if (col.empty()) { out.message = "⚠ No collection selected"; return out; }

    PooledClient pc;
    if (!pc) { out.message = "⚠ no client available"; return out; }

    mongoc_collection_t* c =
        mongoc_client_get_collection(pc.get(), db.c_str(), col.c_str());

    // Open the matching cursor.
    mongoc_cursor_t* cur = nullptr;
    std::vector<bson_t*> owned;

    if (mode == ViewMode::Aggregation) {
        std::string js = "{ \"pipeline\": " + relax_bson(pipeline_text) + " }";
        bson_t* p = bson_new_from_json(
            (const uint8_t*)js.data(), (ssize_t)js.size(), &g_err);
        if (!p) {
            out.message = std::string("⚠ Invalid pipeline: ") + last_error();
            mongoc_collection_destroy(c);
            return out;
        }
        cur = mongoc_collection_aggregate(c, MONGOC_QUERY_NONE, p, nullptr, nullptr);
        owned.push_back(p);
    } else {
        std::string strict_filter = relax_bson(filter);
        bson_t* bf = bson_new_from_json(
            (const uint8_t*)strict_filter.data(), (ssize_t)strict_filter.size(), &g_err);
        if (!bf) {
            out.message = std::string("⚠ Invalid filter: ") + last_error();
            mongoc_collection_destroy(c);
            return out;
        }
        FindOpts opts = build_find_opts(projection, sort, skip_str, limit_str);
        bson_t* bopts = nullptr;
        if (!opts.json.empty()) {
            bopts = bson_new_from_json(
                (const uint8_t*)opts.json.data(), (ssize_t)opts.json.size(), &g_err);
            if (!bopts) {
                out.message = std::string("⚠ Invalid query options: ") + last_error();
                bson_destroy(bf);
                mongoc_collection_destroy(c);
                return out;
            }
        }
        cur = mongoc_collection_find_with_opts(c, bf, bopts, nullptr);
        owned.push_back(bf);
        if (bopts) owned.push_back(bopts);
    }

    std::string path = export_path(db, col, mode, fmt);
    std::ofstream f(path);
    if (!f) {
        out.message = "⚠ Cannot write " + path;
        mongoc_cursor_destroy(cur);
        mongoc_collection_destroy(c);
        for (auto* b : owned) if (b) bson_destroy(b);
        return out;
    }

    const bson_t* doc = nullptr;
    int count = 0;
    bool capped = false;

    if (fmt == "json") {
        f << "[\n";
        while (mongoc_cursor_next(cur, &doc)) {
            size_t len = 0;
            char* cstr = bson_as_relaxed_extended_json(doc, &len);
            if (cstr) {
                if (count > 0) f << ",\n";
                f << "  ";
                f.write(cstr, (std::streamsize)len);
                bson_free(cstr);
                ++count;
            }
            if (count >= EXPORT_MAX) { capped = true; break; }
        }
        f << "\n]\n";
    } else {
        // CSV: gather rows + union of headers, then write.
        std::vector<std::string> headers;
        std::unordered_set<std::string> seen;
        std::vector<std::unordered_map<std::string, std::string>> rows;

        while (mongoc_cursor_next(cur, &doc)) {
            std::unordered_map<std::string, std::string> map;
            for (auto& kv : csv_fields(doc)) {
                map[kv.k] = kv.v;
                if (seen.insert(kv.k).second) headers.push_back(kv.k);
            }
            rows.push_back(std::move(map));
            ++count;
            if (count >= EXPORT_MAX) { capped = true; break; }
        }

        for (size_t i = 0; i < headers.size(); ++i) {
            if (i) f << ',';
            f << csv_escape(headers[i]);
        }
        f << '\n';
        for (auto& row : rows) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i) f << ',';
                auto it = row.find(headers[i]);
                f << csv_escape(it == row.end() ? "" : it->second);
            }
            f << '\n';
        }
    }
    f.close();

    if (mongoc_cursor_error(cur, &g_err)) {
        out.message = std::string("⚠ Export error: ") + last_error();
    } else {
        std::ostringstream m;
        m << "✔ Exported " << count << " doc" << (count == 1 ? "" : "s");
        if (capped) m << " (capped at " << EXPORT_MAX << ")";
        m << " → " << path;
        out.message = m.str();
        out.ok = true;
    }

    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(c);
    for (auto* b : owned) if (b) bson_destroy(b);
    return out;
}

} // namespace mongodb

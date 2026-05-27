#include "term.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace term {

int W = 160;
int H = 46;
int LEFT_W = 70;
int RIGHT_W = 90;

namespace {

termios saved_termios{};
bool    termios_saved = false;

// Single-shot frame buffer: every draw call in a frame appends here, then
// flush_out() emits the whole frame in one syscall. Single atomic write =
// no tearing or partial-frame flicker even on slow terminals.
std::string frame_buf;

void buf_write(const char* p, size_t n) { frame_buf.append(p, n); }
void buf_write(const std::string& s)    { frame_buf.append(s); }
void buf_write(const char* s)           { frame_buf.append(s); }

// Direct-to-stdout helpers (used for terminal control before/after the main
// loop: alt-screen toggle, cursor visibility). These bypass the frame buffer.
void out_direct(const char* s, size_t n) {
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
}

} // namespace

void clear() {
    // Move home, then erase from cursor to end of screen. Issued at the very
    // top of each frame, inside the single buffered write.
    frame_buf.clear();
    if (frame_buf.capacity() < 32 * 1024) frame_buf.reserve(64 * 1024);
    buf_write("\x1b[H\x1b[2J");
}

void hide_cursor()      { out_direct("\x1b[?25l", 6); }
void show_cursor()      { out_direct("\x1b[?25h", 6); }
void enter_alt_screen() { out_direct("\x1b[?1049h", 8); }
void leave_alt_screen() { out_direct("\x1b[?1049l", 8); }

void flush_out() {
    if (!frame_buf.empty()) {
        std::fwrite(frame_buf.data(), 1, frame_buf.size(), stdout);
        frame_buf.clear();
    }
    std::fflush(stdout);
}

void raw_on() {
    if (!termios_saved && tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        termios_saved = true;
    }
    termios t = saved_termios;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

void raw_off() {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    }
}

void move_cursor(int x, int y) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y, x);
    if (n > 0) buf_write(buf, (size_t)n);
}

void line(int x, int y, const std::string& text) {
    // Clip: silently drop writes that would land outside the rendered area.
    // Prevents corrupted output when the terminal shrinks mid-render or when
    // layout math wanders off-screen for small windows.
    if (y < 1 || y > H + 6 || x < 1 || x > W) return;
    move_cursor(x, y);
    buf_write(text);
}

void update_dims() {
    winsize ws{};
    int cols = 0, rows = 0;
    for (int fd : {1, 0, 2}) {
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            cols = ws.ws_col;
            rows = ws.ws_row;
            break;
        }
    }
    if (cols == 0) { cols = 160; rows = 46; }

    // Use real terminal dims, with a tiny floor that keeps draw math sane.
    // Layout code reacts to W (see app::pick_layout) — narrow terminals get a
    // stacked layout instead of an unreadable cropped 3-column.
    W = std::max(40, cols);
    H = std::max(8, rows - 4);
    LEFT_W = (W * 7) / 16;
    RIGHT_W = W - LEFT_W;
}

void busy(const std::string& label) {
    std::string text = "  " + label + "  ";
    int x = std::max(1, (W - (int)text.size()) / 2);
    int y = H / 2 + 3;
    std::string out;
    out.append(Colors::bg_sel).append(Colors::green_b).append(text).append(Colors::reset);
    line(x, y, out);
    flush_out();
}

std::string read_key() {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return {};

    if (c != '\x1b') return std::string(1, c);

    char c2 = 0, c3 = 0;
    termios cur{};
    tcgetattr(STDIN_FILENO, &cur);
    termios tmp = cur;
    tmp.c_cc[VMIN] = 0;
    tmp.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &tmp);
    bool got2 = ::read(STDIN_FILENO, &c2, 1) > 0;
    bool got3 = got2 && ::read(STDIN_FILENO, &c3, 1) > 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &cur);

    if (got2 && c2 == '[' && got3) {
        switch (c3) {
            case 'A': return "UP";
            case 'B': return "DOWN";
            case 'C': return "RIGHT";
            case 'D': return "LEFT";
            case 'H': return "HOME";
            case 'F': return "END";
            default:  return {};
        }
    }
    if (got2 && c2 == 'O' && got3) {
        switch (c3) {
            case 'H': return "HOME";
            case 'F': return "END";
            default:  return {};
        }
    }
    return {};
}

namespace {

// Color routing for highlight(). One paint per token; no nested escape leaks.
inline void emit(std::string& out, const char* color, const std::string& tok) {
    out.append(color);
    out.append(tok);
    out.append(Colors::reset);
}
inline void emit_ch(std::string& out, const char* color, char c) {
    out.append(color);
    out.push_back(c);
    out.append(Colors::reset);
}

bool is_id_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '$'; }
bool is_id_cont (char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

// Returns index past closing quote (or n on unterminated).
size_t scan_string(const std::string& s, size_t i) {
    ++i;
    while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
        if (s[i] == '"') return i + 1;
        ++i;
    }
    return s.size();
}

} // namespace

// Tokenizing highlighter. Distinguishes:
//   green_b   keys      ("...":)
//   yellow    string values
//   cyan      numbers
//   magenta   booleans/null + leading-$ operators ($match, $group, …)
//   orange    extended-JSON helpers: ObjectId(…), ISODate(…), NumberLong(…)
//   dgray     structural punctuation { } [ ] , :
std::string highlight(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 32);
    size_t i = 0, n = s.size();

    while (i < n) {
        char c = s[i];

        if (c == ' ' || c == '\t') { out.push_back(c); ++i; continue; }

        if (c == '"') {
            size_t end = scan_string(s, i);
            std::string tok = s.substr(i, end - i);

            // Determine if this string is a key: next non-space is ':'.
            size_t j = end;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
            if (j < n && s[j] == ':') {
                emit(out, Colors::green_b, tok);
            } else {
                emit(out, Colors::yellow, tok);
            }
            i = end;
            continue;
        }

        if (is_id_start(c)) {
            size_t j = i;
            while (j < n && is_id_cont(s[j])) ++j;
            std::string ident = s.substr(i, j - i);

            // Look ahead for '(' to detect helper calls.
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            bool is_call = (k < n && s[k] == '(');

            if (is_call) {
                emit(out, Colors::orange, ident);
                i = j;
                continue;
            }
            if (ident == "true" || ident == "false" || ident == "null"
                || ident == "undefined" || ident == "NaN" || ident == "Infinity") {
                emit(out, Colors::magenta, ident);
                i = j;
                continue;
            }
            if (!ident.empty() && ident[0] == '$') {
                emit(out, Colors::magenta, ident);
                i = j;
                continue;
            }
            out.append(ident);
            i = j;
            continue;
        }

        if ((c >= '0' && c <= '9')
            || (c == '-' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            if (s[j] == '-') ++j;
            while (j < n && (std::isdigit((unsigned char)s[j]) || s[j] == '.'
                          || s[j] == 'e' || s[j] == 'E'
                          || ((s[j] == '+' || s[j] == '-') && j > i &&
                              (s[j - 1] == 'e' || s[j - 1] == 'E')))) {
                ++j;
            }
            emit(out, Colors::cyan, s.substr(i, j - i));
            i = j;
            continue;
        }

        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
            emit_ch(out, Colors::dgray, c);
            ++i;
            continue;
        }

        out.push_back(c);
        ++i;
    }
    return out;
}

std::string item(std::string text, bool selected, int w) {
    if ((int)text.size() > w) {
        text = text.substr(0, std::max(0, w - 1)) + "…";
    }
    int pad = std::max(0, w - (int)text.size());
    std::string padded = text + std::string(pad, ' ');
    std::string out;
    if (selected) {
        out.append(Colors::bg_sel).append(Colors::bold).append(Colors::white)
           .append("› ").append(padded).append(Colors::reset);
    } else {
        out.append("  ").append(Colors::gray).append(padded).append(Colors::reset);
    }
    return out;
}

std::string fkey(const std::string& k, const std::string& label) {
    std::string out;
    out.append(Colors::bg_sel).append(Colors::green_b)
       .append(" ").append(k).append(" ").append(Colors::reset)
       .append(Colors::gray).append(" ").append(label).append(Colors::reset);
    return out;
}

void box(int x, int y, int w, int h, const std::string& title, bool active) {
    if (w < 2 || h < 2) return;
    const char* bc = active ? Colors::green   : Colors::dgray;
    const char* tc = active ? Colors::green_b : Colors::gray;

    std::string top;
    top.append(bc).append("┌");
    for (int i = 0; i < w - 2; ++i) top.append("─");
    top.append("┐").append(Colors::reset);
    line(x, y, top);

    std::string ttl;
    ttl.append(tc).append(" ").append(title).append(" ").append(Colors::reset);
    line(x + 2, y, ttl);

    std::string side_l = std::string(bc) + "│" + Colors::reset;
    for (int i = 1; i < h - 1; ++i) {
        line(x, y + i, side_l);
        line(x + w - 1, y + i, side_l);
    }

    std::string bot;
    bot.append(bc).append("└");
    for (int i = 0; i < w - 2; ++i) bot.append("─");
    bot.append("┘").append(Colors::reset);
    line(x, y + h - 1, bot);
}

void qfield(int x, int y, const std::string& label,
            const std::string& val, bool focused) {
    const char* lc    = focused ? Colors::green_b : Colors::gray;
    std::string caret = focused ? (std::string(Colors::bg_green) + " " + Colors::reset)
                                : std::string();

    std::string body;
    if (val.empty()) {
        body = focused ? caret : (std::string(Colors::dgray) + "(optional)" + Colors::reset);
    } else {
        body = focused ? (highlight(val) + caret)
                       : (std::string(Colors::yellow) + val + Colors::reset);
    }

    char lab[16];
    std::snprintf(lab, sizeof(lab), "%-7s", label.c_str());

    std::string out;
    out.append(lc).append(lab).append(" ▸ ").append(Colors::reset).append(body);
    line(x, y, out);
}

} // namespace term

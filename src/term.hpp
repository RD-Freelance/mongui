// =========================================================
// Terminal: ANSI rendering, raw mode, dimensions, keys.
// =========================================================
#pragma once

#include <string>

namespace term {

// MongoDB leaf-green theme. ANSI escape sequences.
struct Colors {
    static constexpr const char* reset    = "\x1b[0m";
    static constexpr const char* bold     = "\x1b[1m";
    static constexpr const char* reverse  = "\x1b[7m";
    static constexpr const char* dim      = "\x1b[2m";
    static constexpr const char* black    = "\x1b[30m";
    static constexpr const char* white    = "\x1b[97m";
    static constexpr const char* gray     = "\x1b[38;5;245m";
    static constexpr const char* dgray    = "\x1b[38;5;240m";
    static constexpr const char* green    = "\x1b[38;5;48m";
    static constexpr const char* green_b  = "\x1b[1m\x1b[38;5;48m";
    static constexpr const char* cyan     = "\x1b[38;5;80m";
    static constexpr const char* yellow   = "\x1b[38;5;221m";
    static constexpr const char* magenta  = "\x1b[38;5;176m";
    static constexpr const char* orange   = "\x1b[38;5;208m";
    static constexpr const char* red      = "\x1b[38;5;203m";
    static constexpr const char* bg_sel   = "\x1b[48;5;22m";
    static constexpr const char* bg_green = "\x1b[48;5;48m";
    static constexpr const char* bg_pop   = "\x1b[48;5;238m";
};

// Live screen dimensions. Recomputed by update_dims() each frame.
extern int W;
extern int H;
extern int LEFT_W;
extern int RIGHT_W;

void clear();
void hide_cursor();
void show_cursor();
void enter_alt_screen();
void leave_alt_screen();
void raw_on();
void raw_off();
void flush_out();

void line(int x, int y, const std::string& text);
void move_cursor(int x, int y);
void update_dims();
void busy(const std::string& label);

// Reads one key — single char, or a named arrow ("UP"/"DOWN"/"LEFT"/"RIGHT").
// Returns empty string for unknown escape sequences.
std::string read_key();

// Inserts ANSI color around "quoted" substrings — JSON-ish highlight.
std::string highlight(const std::string& s);

// List item renderer; selected rows get the dark-green highlight.
std::string item(std::string text, bool selected, int w);

// Footer key hint pill, e.g. fkey("^q","Quit").
std::string fkey(const std::string& k, const std::string& label);

// Draws a titled box with bordered interior. Active = leaf-green; inactive = dim.
void box(int x, int y, int w, int h, const std::string& title, bool active);

// One query-bar input row (filter/project/sort/skip/limit). `w` is the
// total column width the field is allowed to occupy; the value is
// truncated (or scrolled, when focused) so it never exceeds it.
void qfield(int x, int y, int w, const std::string& label,
            const std::string& val, bool focused, int cursor = -1,
            const std::string& placeholder = "(optional)");

} // namespace term

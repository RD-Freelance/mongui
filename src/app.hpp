// =========================================================
// Application: state, render, main loop.
// =========================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mongo.hpp"

namespace app {

struct State {
    std::string uri;

    std::vector<std::string> dbs;
    std::vector<std::string> cols;
    std::vector<std::string> filtered;
    std::string              default_db;

    std::string col_search;
    bool        col_searching = false;

    // 0-based indices throughout.
    int db_i       = 0;
    int col_i      = 0;
    int col_scroll = 0;

    // Result pane.
    std::vector<std::string> docs;
    int     doc_scroll = 0;
    int64_t doc_count  = 0;     // total matching server-side
    int     doc_shown  = 0;     // docs rendered into `docs` this page

    // Pagination — server-side skip/limit, additive on top of user's q_skip.
    // Reset to 0 on every new Enter; ^N / ^P step.
    int     page       = 0;
    int     page_size  = 20;

    // Query bar (DOCUMENTS page).
    std::string query     = "{}";
    std::string q_project;
    std::string q_sort;
    std::string q_skip;
    std::string q_limit;
    int         q_focus = 0;   // 0 = nav, 1..5 = editing a field

    mongodb::ViewMode mode = mongodb::ViewMode::Query;

    bool        confirm_exit = false;
    bool        export_menu  = false;
    std::string status_msg;

    // mongosh-style shell line editor.
    std::string shell_cmd;
    int         shell_cx     = 0;
    int         shell_scroll = 0;

    // Aggregation editor (0-based cursor_y).
    std::vector<std::string> agg_lines = {
        "[",
        "  { $match: {} }",
        "]"
    };
    int cursor_x = 0;
    int cursor_y = 0;
    int agg_scroll = 0;

    // Autocomplete popup.
    bool                     ac_active = false;
    std::vector<std::string> ac_matches;
    int                      ac_i = 0;
    std::string              ac_token;
};

// Interactive connect screen (normal terminal mode).
void connect_screen(State& s);

// Apply the URI's parsed default_db: pre-select if present, prepend if not.
void select_default_db(State& s);

// Rebuild filtered collections from col_search + state.cols.
void refresh_filtered(State& s);

// Render one frame and flush.
void render(State& s);

// Blocking main loop: render → read_key → mutate → repeat until exit.
void run(State& s);

} // namespace app

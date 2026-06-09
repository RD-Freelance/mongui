// =========================================================
// Application: state, render, main loop.
// =========================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mongo.hpp"

namespace app {

// One workspace tab: a (collection + documents) view together with its own
// query bar, aggregation pipeline, shell, and view mode. The `State` below
// holds the *active* tab's fields live; inactive tabs are snapshotted here and
// swapped in/out on tab switch. The `dbs` list and connection are global, so
// they are NOT part of a Tab.
struct Tab {
    std::vector<std::string> cols;
    std::vector<std::string> filtered;
    std::string col_search;
    bool        col_searching = false;
    int db_i = 0, col_i = 0, col_scroll = 0;

    std::vector<std::string> docs;
    int     doc_scroll = 0;
    int64_t doc_count  = 0;
    int     doc_shown  = 0;
    std::vector<std::string> doc_raw;
    std::vector<std::string> doc_ids;
    int     doc_sel = 0;

    bool                     edit_active = false;
    std::vector<std::string> edit_lines;
    int                      edit_cx = 0, edit_cy = 0, edit_scroll = 0;
    std::string              edit_selector;
    std::string              edit_status;
    bool                     del_confirm = false;

    int page = 0, page_size = 20;

    std::string query = "{}";
    std::string q_project, q_sort, q_skip, q_limit;
    int q_focus = 0, q_cx = 0;

    mongodb::ViewMode mode = mongodb::ViewMode::Query;

    std::string shell_cmd;
    int shell_cx = 0, shell_scroll = 0;

    std::vector<std::string> agg_lines = { "[", "  { $match: {} }", "]" };
    int cursor_x = 0, cursor_y = 0, agg_scroll = 0;

    bool                     ac_active = false;
    std::vector<std::string> ac_matches;
    int                      ac_i = 0;
    std::string              ac_token;
};

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

    // Per-document data for the documents-pane actions (parallel, size==doc_shown).
    std::vector<std::string> doc_raw;   // pretty JSON per doc (copy/clone/edit)
    std::vector<std::string> doc_ids;   // canonical { _id: … } selector per doc
    int     doc_sel = 0;        // selected document index (into doc_raw/doc_ids)

    // Per-document edit modal (full-screen JSON editor).
    bool                     edit_active = false;
    std::vector<std::string> edit_lines;
    int                      edit_cx = 0, edit_cy = 0, edit_scroll = 0;
    std::string              edit_selector;  // _id selector of the doc being edited
    std::string              edit_status;    // last save error, shown in the modal

    // Delete confirmation popup.
    bool                     del_confirm = false;

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
    int         q_cx    = 0;   // cursor index within the focused field

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

    // Workspace tabs (max 5). The fields above mirror tabs[tab_i] while it is
    // active; switching saves them back and loads the target tab.
    std::vector<Tab> tabs;
    int              tab_i = 0;
};

static constexpr int MAX_TABS = 5;

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

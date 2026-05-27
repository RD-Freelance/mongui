// =========================================================
// Mongo Compass TUI — C++ port of mongo_compass.lua.
// Entry point: connect screen, signal handling, main loop.
// =========================================================
#include "app.hpp"
#include "mongo.hpp"
#include "term.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <mongoc/mongoc.h>

#ifndef MONGUI_VERSION
#define MONGUI_VERSION "0.1.0"
#endif

namespace {

void print_version() {
    std::printf("mongui %s\n", MONGUI_VERSION);
    std::printf("  libmongoc %s\n", MONGOC_VERSION_S);
    std::printf("  RD-Freelance · MIT License\n");
}

void print_help() {
    std::printf(
        "mongui — keyboard-driven MongoDB terminal client\n"
        "\n"
        "Usage:\n"
        "  mongui                    launch interactive TUI\n"
        "  mongui -v, --version      print version and exit\n"
        "  mongui -h, --help         print this help and exit\n"
        "\n"
        "Modes (inside the TUI):\n"
        "  ^E query · ^A aggregate · ^S shell · ^O export · ^Q quit\n"
        "\n"
        "Project: https://github.com/RD-Freelance/mongui\n");
}

void on_sigint(int) {
    // Only fires before raw_on() — once raw mode is on, ISIG is masked and
    // Ctrl+C arrives as 0x03 through the main loop instead.
    term::raw_off();
    term::show_cursor();
    term::leave_alt_screen();
    // Wipe whatever the connect screen printed on the normal buffer.
    std::fwrite("\x1b[2J\x1b[H", 1, 7, stdout);
    std::fflush(stdout);
    std::_Exit(0);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        std::fprintf(stderr, "mongui: unknown option '%s' (try --help)\n", argv[i]);
        return 2;
    }

    std::signal(SIGINT, on_sigint);

    if (!mongodb::init()) {
        std::cerr << "Could not initialize the MongoDB C driver\n";
        return 1;
    }

    // Switch to the alt screen FIRST so even the connect prompt renders
    // there — the user's real terminal stays untouched whether the app
    // exits cleanly or aborts at the connect step.
    term::enter_alt_screen();
    term::clear();

    app::State state;
    app::connect_screen(state);

    // Open the client (URI is parsed but no connection yet).
    if (!mongodb::connect(state.uri)) {
        term::leave_alt_screen();
        term::show_cursor();
        std::cerr << "Invalid MongoDB URI: " << state.uri << "\n";
        mongodb::cleanup();
        return 1;
    }

    term::hide_cursor();

    // Bulk stdio so each render is a single flushed write.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    {
        std::string e;
        state.dbs = mongodb::list_databases(e);
        if (state.dbs.empty()) {
            if (!state.default_db.empty()) {
                state.dbs = { state.default_db };
                state.db_i = 0;
            } else {
                term::leave_alt_screen();
                term::show_cursor();
                std::cerr << "\nCould not connect / list databases:\n  " << e << "\n";
                mongodb::cleanup();
                return 1;
            }
        }
    }
    app::select_default_db(state);

    // Load collections for the initially selected db.
    if (!state.dbs.empty()) {
        term::busy("Loading collections…");
        std::string e;
        state.cols = mongodb::list_collections(state.dbs[state.db_i], e);
        if (state.cols.empty()) state.cols = { "empty" };
        app::refresh_filtered(state);
    }

    term::raw_on();

    app::run(state);

    // Cleanup. Clear the normal screen so the user lands on a fresh prompt
    // instead of seeing the connect-screen card scrolled back into history.
    term::raw_off();
    term::show_cursor();
    term::leave_alt_screen();
    std::fwrite("\x1b[2J\x1b[H", 1, 7, stdout);
    std::fflush(stdout);
    mongodb::cleanup();
    return 0;
}

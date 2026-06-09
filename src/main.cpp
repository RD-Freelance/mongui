// =========================================================
// Mongo Compass TUI — C++ port of mongo_compass.lua.
// Entry point: connect screen, signal handling, main loop.
// =========================================================
#include "app.hpp"
#include "mongo.hpp"
#include "term.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

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
"USAGE\n"
"  mongui [options]            with no options, launch the interactive TUI\n"
"\n"
"OPTIONS\n"
"  -v, --version              print version, libmongoc version, and license\n"
"  -h, --help                 show this help and exit\n"
"  --uninstall [-y]           remove the installed mongui binary (-y: no prompt)\n"
"\n"
"IMPORT (non-interactive — import a file, then exit)\n"
"  --import                   run a bulk import (implied when --filepath is given)\n"
"  --uri <uri>                connection string (default: mongodb://localhost:27017)\n"
"  --database, --db <name>    target database              (required)\n"
"  --collection, --col <name> target collection            (required)\n"
"  --filepath, --file <path>  source file                  (required)\n"
"                             accepts a JSON array, NDJSON (one doc per line),\n"
"                             or a single JSON document; relaxed/mongosh JSON ok\n"
"  example:\n"
"    mongui --import --uri mongodb://localhost:27017 \\\n"
"           --db shop --collection orders --filepath orders.json\n"
"\n"
"KEYS (inside the TUI)\n"
"  Global\n"
"    ^E / ^A / ^S             switch to Query / Aggregate / Shell\n"
"    ^T / ^B / ^W             new / next / close workspace tab (max 5)\n"
"    ^O                       export current view to JSON / CSV\n"
"    ^J / ^K                  scroll the documents pane down / up\n"
"    ^Q  or  ^C ^C            quit\n"
"  Query mode\n"
"    Up / Down                navigate databases\n"
"    Left / Right             navigate collections\n"
"    ^F                       toggle collection search\n"
"    Tab                      cycle query fields, then the documents pane\n"
"    Enter                    run find (resets to page 1)\n"
"    ^N / ^P                  next / previous page\n"
"  Documents pane (press Tab until it is focused)\n"
"    j / k                    move the selection between documents\n"
"    y                        copy the document JSON to the clipboard\n"
"    c                        clone the document (re-inserts without _id)\n"
"    e  or  Enter             edit the document inline (^S save, Esc cancel)\n"
"    d                        delete the document (asks to confirm)\n"
"  Aggregate mode\n"
"    ^G                       run the pipeline\n"
"    ^L                       reformat / reindent the pipeline\n"
"    Tab                      accept autocomplete, or insert two spaces\n"
"  Shell mode\n"
"    Enter                    run a mongosh-style db.coll.method(...) command\n");
}

// Absolute path of the running executable, or "" if it can't be determined.
std::string self_path() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, out.data(), len, nullptr, nullptr);
    return out;
#elif defined(__APPLE__)
    char buf[8192];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    char resolved[8192];
    if (realpath(buf, resolved)) return resolved;
    return buf;
#else
    char buf[8192];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
#endif
}

// Remove the running binary itself. Returns a process exit code.
int do_uninstall(bool assume_yes) {
    std::string path = self_path();
    if (path.empty()) {
        std::fprintf(stderr, "mongui: could not determine my own path; "
                             "use scripts/uninstall instead.\n");
        return 1;
    }

    std::printf("This removes only the mongui binary:\n  %s\n", path.c_str());
    std::printf("Build tools and the MongoDB C driver are left untouched.\n");
    if (!assume_yes) {
        std::printf("Remove it? [y/N] ");
        std::fflush(stdout);
        int c = std::getchar();
        if (c != 'y' && c != 'Y') { std::printf("Cancelled.\n"); return 0; }
    }

#ifdef _WIN32
    // A running .exe can't delete itself, so hand the job to a detached cmd
    // that waits for us to exit, then deletes the file.
    std::wstring wpath;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
        wpath.resize((size_t)wlen);
        MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), wpath.data(), wlen);
    }
    std::wstring cmd = L"cmd.exe /C ping 127.0.0.1 -n 2 >nul & del /f /q \"" + wpath + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    else { std::fprintf(stderr, "mongui: failed to schedule self-delete\n"); return 1; }
    std::printf("mongui will be removed in a moment.\n");
    std::printf("(Remove it from your PATH too: %%LOCALAPPDATA%%\\Programs\\mongui)\n");
    return 0;
#else
    if (std::remove(path.c_str()) == 0) {
        std::printf("Removed %s\n", path.c_str());
        return 0;
    }
    std::fprintf(stderr, "mongui: could not remove %s: %s\n",
                 path.c_str(), std::strerror(errno));
    std::fprintf(stderr, "It may need elevated rights — try:\n  sudo rm -f \"%s\"\n",
                 path.c_str());
    return 1;
#endif
}

// Non-interactive bulk import. Connects, imports the file, prints a summary.
int do_import(const std::string& uri, const std::string& db,
              const std::string& col, const std::string& file) {
    if (db.empty() || col.empty() || file.empty()) {
        std::fprintf(stderr,
            "mongui --import: need --database, --collection and --filepath\n"
            "  e.g. mongui --import --db shop --collection orders --filepath orders.json\n");
        return 2;
    }
    std::string conn = uri.empty() ? "mongodb://localhost:27017" : uri;

    if (!mongodb::init()) {
        std::fprintf(stderr, "mongui: could not initialize the MongoDB C driver\n");
        return 1;
    }
    if (!mongodb::connect(conn)) {
        std::fprintf(stderr, "mongui: invalid MongoDB URI: %s\n", conn.c_str());
        mongodb::cleanup();
        return 1;
    }

    std::printf("Importing %s → %s.%s …\n", file.c_str(), db.c_str(), col.c_str());
    mongodb::ImportResult r = mongodb::import_documents(db, col, file);
    std::printf("%s\n", r.message.c_str());
    mongodb::cleanup();
    return r.ok ? 0 : 1;
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
    // Quiet libmongoc's default structured logger (it logs connection monitor
    // chatter at DEBUG to stderr, which clutters the import output and can
    // corrupt the TUI). Respect an explicit user setting if present.
#ifdef _WIN32
    if (!std::getenv("MONGODB_LOG_ALL")) _putenv_s("MONGODB_LOG_ALL", "warning");
#else
    if (!std::getenv("MONGODB_LOG_ALL")) setenv("MONGODB_LOG_ALL", "warning", 1);
#endif

    bool want_uninstall = false, assume_yes = false, want_import = false;
    std::string opt_uri, opt_db, opt_col, opt_file;

    // Match "--name value" or "--name=value"; stores into `out`, advances i.
    auto take = [&](const char* arg, const char* name, std::string& out, int& i) -> bool {
        size_t n = std::strlen(name);
        if (std::strncmp(arg, name, n) != 0) return false;
        if (arg[n] == '=') { out = arg + n + 1; return true; }
        if (arg[n] == '\0') { if (i + 1 < argc) out = argv[++i]; return true; }
        return false;   // e.g. "--db" must not swallow "--database"
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--version") == 0) {
            print_version(); return 0;
        }
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_help(); return 0;
        }
        if (std::strcmp(a, "--uninstall") == 0) { want_uninstall = true; continue; }
        if (std::strcmp(a, "-y") == 0 || std::strcmp(a, "--yes") == 0) { assume_yes = true; continue; }
        if (std::strcmp(a, "--import") == 0) { want_import = true; continue; }
        if (take(a, "--uri", opt_uri, i)) continue;
        if (take(a, "--database", opt_db, i) || take(a, "--db", opt_db, i)) continue;
        if (take(a, "--collection", opt_col, i) || take(a, "--col", opt_col, i)) continue;
        if (take(a, "--filepath", opt_file, i) || take(a, "--file", opt_file, i)) continue;
        std::fprintf(stderr, "mongui: unknown option '%s' (try --help)\n", a);
        return 2;
    }

    if (want_uninstall) return do_uninstall(assume_yes);
    if (want_import || !opt_file.empty())
        return do_import(opt_uri, opt_db, opt_col, opt_file);

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

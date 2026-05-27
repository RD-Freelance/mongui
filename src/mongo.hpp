// =========================================================
// MongoDB layer: driver lifecycle, BSON formatting, queries,
// aggregation, mongosh-style shell, and export.
// =========================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mongodb {

enum class ViewMode { Query, Aggregation, Shell };

struct QueryResult {
    std::vector<std::string> lines; // display lines (split per document, blank line separators)
    int64_t total = 0;              // total matching/produced
    int     shown = 0;              // documents rendered into `lines`
};

struct ExportResult {
    bool        ok = false;
    std::string message;            // human-readable status (✔ / ⚠)
};

// Driver lifecycle.
bool init();
void cleanup();

// Opens a client from a URI string. Returns false if the URI is malformed.
// Lazy: does not actually contact the server.
bool connect(const std::string& uri);
void disconnect();

// Enumerate databases on the server. Sets `error_out` on failure.
std::vector<std::string> list_databases(std::string& error_out);

// Enumerate collections in the named database. Tolerates auth failure
// (returns an empty list with `error_out` populated).
std::vector<std::string> list_collections(const std::string& db,
                                          std::string& error_out);

// Run a find against db.col with the relaxed-BSON filter and optional opts.
QueryResult run_find(const std::string& db, const std::string& col,
                     const std::string& filter,
                     const std::string& projection,
                     const std::string& sort,
                     const std::string& skip_str,
                     const std::string& limit_str,
                     int display_limit);

// Run an aggregation pipeline (an entire "[...]" text block).
QueryResult run_aggregation(const std::string& db, const std::string& col,
                            const std::string& pipeline_text,
                            int display_limit);

// Parse and run a mongosh-style `db.coll.method(...)` command.
QueryResult run_shell(const std::string& db, const std::string& cmd);

// Count documents matching `filter`. -1 on error (with `error_out` set).
int64_t count_documents(const std::string& db, const std::string& col,
                        const std::string& filter, std::string& error_out);

// Run the user's pipeline with `{ $count: "n" }` appended to get the total
// document count of the unpaginated result. -1 on error (eg. pipeline ends
// in `$out` / `$merge` which makes count meaningless).
int64_t count_aggregation(const std::string& db, const std::string& col,
                          const std::string& pipeline_text);

// Export the current view to a file in ~/Downloads. fmt = "json" or "csv".
ExportResult export_results(const std::string& db, const std::string& col,
                            const std::string& fmt,
                            ViewMode mode,
                            const std::string& filter,
                            const std::string& projection,
                            const std::string& sort,
                            const std::string& skip_str,
                            const std::string& limit_str,
                            const std::string& pipeline_text);

// Exposed for the editor / preview rendering when needed.
std::string relax_bson(const std::string& s);

} // namespace mongodb

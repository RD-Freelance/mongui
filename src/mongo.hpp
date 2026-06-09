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
    // Parallel per-document data (size == shown) for find results. `raw[i]` is
    // the i-th document's pretty JSON (round-trips through relax_bson); `ids[i]`
    // is a canonical `{ "_id": … }` selector for delete/replace. Empty for docs
    // without an _id (e.g. some aggregation output).
    std::vector<std::string> raw;
    std::vector<std::string> ids;
};

// Result of a single-document write (delete/replace/clone).
struct WriteResult {
    bool        ok = false;
    std::string message;            // human-readable status (✔ / ⚠)
};

// Result of a bulk import from a file.
struct ImportResult {
    bool        ok = false;
    long long   inserted = 0;       // documents successfully inserted
    std::string message;            // human-readable status (✔ / ⚠)
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

// Single-document writes used by the per-document actions in the documents
// pane. `selector_json` is a canonical `{ "_id": … }` produced by run_find;
// `doc_text` is relaxed/display JSON (passed through relax_bson internally).
WriteResult delete_doc (const std::string& db, const std::string& col,
                        const std::string& selector_json);
WriteResult replace_doc(const std::string& db, const std::string& col,
                        const std::string& selector_json,
                        const std::string& doc_text);
WriteResult clone_doc  (const std::string& db, const std::string& col,
                        const std::string& doc_text);  // inserts a copy, sans _id

// Bulk-import documents from a file into db.col. Accepts a JSON array
// (`[ {…}, {…} ]`), newline-delimited / concatenated JSON objects (NDJSON),
// or a single object. Relaxed/mongosh JSON (ObjectId(...), unquoted keys) is
// accepted. Inserts in batches; partial progress is reported in `inserted`.
ImportResult import_documents(const std::string& db, const std::string& col,
                              const std::string& filepath);

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

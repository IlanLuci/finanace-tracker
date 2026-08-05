#ifndef EXPENSE_TAGS_HPP
#define EXPENSE_TAGS_HPP

#include <ctime>
#include <map>
#include <string>
#include <vector>

// Sidecar store for 529 qualified-expense tags. Everything lives under
// data/529/ so Plaid re-syncs and portfolio rewrites can never touch it.
namespace ExpenseTags
{
    struct TagRecord
    {
        std::string key;                   // txn fingerprint, e.g. "9a3f...d2-0"
        std::string status;                // "qualified" | "dismissed"
        double qualified_amount = 0.0;     // meaningful only when qualified
        std::vector<std::string> receipts; // stored filenames under receipts/<key>/
        // Denormalized source-transaction fields (export + orphan display).
        std::string account;
        time_t date = 0;
        double amount = 0.0;               // positive charge amount
        std::string notes;
        std::string category;
        time_t created = 0;
    };

    // Deterministic fingerprint for one spend transaction. Amount is rounded
    // to cents and taken as absolute value before hashing, so the stored
    // (negative) withdrawal amount and the displayed (positive) spend amount
    // produce the same key.
    std::string computeTxnKey(const std::string& account, time_t date, double amount,
                              const std::string& notes, int occurrence);

    // Assigns occurrence indices while iterating a portfolio's FULL
    // transaction history in stored order, so identical same-day duplicates
    // get -0, -1, ... deterministically regardless of any date-range filter
    // applied afterwards.
    class KeyAssigner
    {
    public:
        std::string next(const std::string& account, time_t date, double amount,
                         const std::string& notes);

    private:
        std::map<std::string, int> occurrence_counts;
    };

    // tags.json round-trip. loadTags returns true with an empty vector when
    // the file does not exist; false only on read/parse failure. saveTags
    // writes atomically (temp file + rename).
    bool loadTags(const std::string& file_path, std::vector<TagRecord>& out);
    bool saveTags(const std::string& file_path, const std::vector<TagRecord>& tags);

    // Receipt filename helpers. sanitizeFilename maps path separators and
    // shell-hostile characters to '_' and returns "" when nothing safe
    // remains (so callers must reject that).
    std::string sanitizeFilename(const std::string& raw);
    bool isAllowedReceiptExtension(const std::string& filename);
    std::string receiptMimeType(const std::string& filename);
}

#endif

# 529 Qualified Expense Tracker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tag Plaid-synced credit-card charges as 529-qualified expenses, attach receipt files, sum them into a range-scoped total, and export CSV/ZIP — on the renamed "Spending" page.

**Architecture:** Sidecar store in `data/529/` (tags.json + receipt files) keyed by a deterministic transaction fingerprint; a new `ExpenseTags` C++ module holds all pure logic (keys, persistence, sanitization) so it is unit-testable; `web_server.cpp` gains `/api/529/*` endpoints; the vanilla-JS frontend gets sub-tabs on the Spending page.

**Tech Stack:** C++17 (g++, `-Wall -Wextra`), hand-rolled HTTP server in `src/web_server.cpp`, vanilla JS/HTML/CSS in `web/`, no frontend test framework (manual + curl verification), unit tests as standalone binaries like `test_persistence`.

**Spec:** `docs/superpowers/specs/2026-08-05-529-expense-tracker-design.md` — read it before starting.

## Global Constraints

- C++17 only, no new libraries. Follow existing style: 4-space indent, Allman braces, `lowerCopy`/`trim`-style helper naming.
- Frontend is vanilla JS, 2-space indent, double quotes, template literals for HTML. Reuse existing helpers (`currency`, `dateLabel`, `escapeHtml`, `showFlash`, `apiGet`, `apiPost`, `apiDelete`).
- `web_server.cpp` wraps almost everything in an anonymous namespace starting near line 37. New endpoint helpers go inside it; anything unit-tested must live in the new `expense_tags` module instead.
- The Makefile has **no header dependency tracking**: after ANY change to `include/*.hpp`, build with `make clean && make` or you get silent segfaults.
- Deploy = `pm2 restart 3` (backend is pm2 id 3). Never `pkill`.
- Upload limits: 25 MB per file. Receipt extension allowlist: jpg, jpeg, png, heic, webp, pdf.
- Existing element IDs (`spendView`, `spendBucketSelect`, …) must NOT be renamed — only user-visible copy changes from "Spend Analysis" to "Spending".
- All writes to `data/529/tags.json` are atomic (write `<path>.tmp`, then `std::rename`).
- Commit after every task with a short imperative message ending in the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer.

---

### Task 1: ExpenseTags module — keys, sanitization, MIME

**Files:**
- Create: `include/expense_tags.hpp`
- Create: `src/expense_tags.cpp`
- Create: `test_529.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces (used by Tasks 2–6):
  - `std::string ExpenseTags::computeTxnKey(const std::string& account, time_t date, double amount, const std::string& notes, int occurrence)`
  - `class ExpenseTags::KeyAssigner { std::string next(const std::string& account, time_t date, double amount, const std::string& notes); }`
  - `std::string ExpenseTags::sanitizeFilename(const std::string& raw)` — returns `""` when nothing safe remains
  - `bool ExpenseTags::isAllowedReceiptExtension(const std::string& filename)`
  - `std::string ExpenseTags::receiptMimeType(const std::string& filename)`

- [ ] **Step 1: Add build targets to the Makefile**

Modify `Makefile`:
- Line 10, append ` $(SRC_DIR)/expense_tags.cpp` to `SOURCES`.
- Line 11, append ` $(OBJ_DIR)/expense_tags.o` to `OBJECTS`.
- Line 30, append ` $(SRC_DIR)/expense_tags.cpp` to the `$(TEST_PERSISTENCE_BIN)` prerequisite/compile line (web_server.cpp will reference the module from Task 3 on; the test binary links web_server.cpp).
- After the `test-persistence` rule add:

```make
TEST_529_BIN = $(BIN_DIR)/test_529

$(TEST_529_BIN): test_529.cpp $(SRC_DIR)/expense_tags.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^

test-529: $(TEST_529_BIN)
	./$(TEST_529_BIN)
```

- Add `test-529` to `.PHONY` (line 15) and `$(TEST_529_BIN)` to the `clean` rule's `rm -rf` list (line 37).

- [ ] **Step 2: Write the failing test**

Create `test_529.cpp`. Same conventions as `test_persistence.cpp`: plain `main`, `std::cerr` + `return 1` on failure, `✓`/`✗` output.

```cpp
#include "expense_tags.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string& label)
{
    if (ok)
    {
        std::cout << "✓ " << label << std::endl;
    }
    else
    {
        std::cerr << "✗ " << label << std::endl;
        ++failures;
    }
}

int main()
{
    std::cout << "=== Testing 529 Expense Tags ===" << std::endl;

    // --- computeTxnKey ---
    const std::string k1 = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 0);
    const std::string k2 = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 0);
    check(!k1.empty() && k1 == k2, "computeTxnKey is deterministic");
    check(k1.size() >= 3 && k1.substr(k1.size() - 2) == "-0", "computeTxnKey appends occurrence suffix");

    const std::string k_other_amount = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.01, "Target", 0);
    const std::string k_other_notes = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Walmart", 0);
    const std::string k_other_occ = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 1);
    check(k1 != k_other_amount, "amount changes the key");
    check(k1 != k_other_notes, "notes change the key");
    check(k1 != k_other_occ, "occurrence changes the key");

    // Sign must not matter: spend txs are stored negative but keyed on abs value.
    const std::string k_neg = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, -120.00, "Target", 0);
    check(k1 == k_neg, "key uses absolute amount");

    // --- KeyAssigner ---
    ExpenseTags::KeyAssigner assigner;
    const std::string a1 = assigner.next("Capital One Balance", 1722400000, 120.00, "Target");
    const std::string a2 = assigner.next("Capital One Balance", 1722400000, 120.00, "Target");
    const std::string a3 = assigner.next("Capital One Balance", 1722400000, 55.00, "Costco");
    check(a1 == k1, "first duplicate gets occurrence 0");
    check(a2 == k_other_occ, "second identical tuple gets occurrence 1");
    check(a3.substr(a3.size() - 2) == "-0", "different tuple restarts at occurrence 0");

    // --- sanitizeFilename ---
    check(ExpenseTags::sanitizeFilename("receipt.jpg") == "receipt.jpg", "clean name passes through");
    check(ExpenseTags::sanitizeFilename("../../etc/passwd") == "etc_passwd", "path traversal stripped");
    check(ExpenseTags::sanitizeFilename("a b/c.pdf") == "a_b_c.pdf", "slashes and spaces become underscores");
    check(ExpenseTags::sanitizeFilename("...") == "", "dot-only name rejected");
    check(ExpenseTags::sanitizeFilename("") == "", "empty name rejected");

    // --- extension allowlist / mime ---
    check(ExpenseTags::isAllowedReceiptExtension("r.JPG"), "jpg allowed case-insensitively");
    check(ExpenseTags::isAllowedReceiptExtension("r.pdf"), "pdf allowed");
    check(ExpenseTags::isAllowedReceiptExtension("r.heic"), "heic allowed");
    check(!ExpenseTags::isAllowedReceiptExtension("r.exe"), "exe rejected");
    check(!ExpenseTags::isAllowedReceiptExtension("noext"), "extensionless rejected");
    check(ExpenseTags::receiptMimeType("r.jpg") == "image/jpeg", "jpg mime");
    check(ExpenseTags::receiptMimeType("r.png") == "image/png", "png mime");
    check(ExpenseTags::receiptMimeType("r.pdf") == "application/pdf", "pdf mime");
    check(ExpenseTags::receiptMimeType("r.bin") == "application/octet-stream", "unknown mime falls back");

    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All checks passed" << std::endl;
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails to compile**

Run: `make test-529`
Expected: compile error — `expense_tags.hpp` not found.

- [ ] **Step 4: Write the header**

Create `include/expense_tags.hpp`:

```cpp
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
```

- [ ] **Step 5: Implement everything except load/save (stub those to return false)**

Create `src/expense_tags.cpp`:

```cpp
#include "expense_tags.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace
{
    std::string keyBase(const std::string& account, time_t date, double amount,
                        const std::string& notes)
    {
        const long long cents = std::llround(std::abs(amount) * 100.0);
        std::ostringstream base;
        base << account << "|" << static_cast<long long>(date) << "|" << cents << "|" << notes;
        return base.str();
    }

    std::string lowerExtension(const std::string& filename)
    {
        const size_t dot = filename.find_last_of('.');
        if (dot == std::string::npos || dot + 1 >= filename.size())
        {
            return "";
        }
        std::string ext = filename.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }
}

namespace ExpenseTags
{
    std::string computeTxnKey(const std::string& account, time_t date, double amount,
                              const std::string& notes, int occurrence)
    {
        // FNV-1a 64-bit over the composite tuple.
        unsigned long long hash = 14695981039346656037ULL;
        for (unsigned char c : keyBase(account, date, amount, notes))
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        std::ostringstream out;
        out << std::hex << hash << "-" << std::dec << occurrence;
        return out.str();
    }

    std::string KeyAssigner::next(const std::string& account, time_t date, double amount,
                                  const std::string& notes)
    {
        const std::string base = keyBase(account, date, amount, notes);
        const int occurrence = occurrence_counts[base]++;
        return computeTxnKey(account, date, amount, notes, occurrence);
    }

    bool loadTags(const std::string& file_path, std::vector<TagRecord>& out)
    {
        (void)file_path;
        (void)out;
        return false; // implemented in Task 2
    }

    bool saveTags(const std::string& file_path, const std::vector<TagRecord>& tags)
    {
        (void)file_path;
        (void)tags;
        return false; // implemented in Task 2
    }

    std::string sanitizeFilename(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '.' || c == '-')
            {
                out += c;
            }
            else if (c == '/' || c == '\\' || c == ' ' || c == '_')
            {
                out += '_';
            }
            // every other character is dropped
        }
        // Collapse leading dots/underscores so ".." and dotfiles can't survive.
        size_t start = 0;
        while (start < out.size() && (out[start] == '.' || out[start] == '_'))
        {
            ++start;
        }
        out.erase(0, start);
        // Reject names that are empty or all dots after cleaning.
        if (out.empty() || out.find_first_not_of('.') == std::string::npos)
        {
            return "";
        }
        return out;
    }

    bool isAllowedReceiptExtension(const std::string& filename)
    {
        const std::string ext = lowerExtension(filename);
        return ext == "jpg" || ext == "jpeg" || ext == "png" ||
               ext == "heic" || ext == "webp" || ext == "pdf";
    }

    std::string receiptMimeType(const std::string& filename)
    {
        const std::string ext = lowerExtension(filename);
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "png") return "image/png";
        if (ext == "heic") return "image/heic";
        if (ext == "webp") return "image/webp";
        if (ext == "pdf") return "application/pdf";
        return "application/octet-stream";
    }
}
```

Note on the sanitize test expectations: `"../../etc/passwd"` → dots survive as `.`, slashes become `_`, then leading dots/underscores are stripped → `etc_passwd`. Walk through the code by hand if the test disagrees — fix the code, not the test.

- [ ] **Step 6: Run the test to verify it passes**

Run: `make test-529`
Expected: all `✓`, exit 0. (`loadTags`/`saveTags` are not exercised yet.)

- [ ] **Step 7: Verify the main build still compiles**

Run: `make clean && make`
Expected: clean build (header was added, so full rebuild).

- [ ] **Step 8: Commit**

```bash
git add include/expense_tags.hpp src/expense_tags.cpp test_529.cpp Makefile
git commit -m "Add ExpenseTags module: txn keys, filename sanitization, mime

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: ExpenseTags persistence — tags.json round-trip

**Files:**
- Modify: `src/expense_tags.cpp` (replace the `loadTags`/`saveTags` stubs)
- Modify: `test_529.cpp`

**Interfaces:**
- Consumes: `TagRecord` from Task 1.
- Produces (used by Tasks 4–6): working `loadTags(path, out)` / `saveTags(path, tags)`. File format: JSON array of flat objects — string fields `key/status/account/notes/category`, number fields `qualified_amount/date/amount/created`, string-array field `receipts`. Missing file → `loadTags` returns true with empty vector.

- [ ] **Step 1: Add failing round-trip tests**

In `test_529.cpp`, before the `if (failures > 0)` block, add:

```cpp
    // --- tags.json round-trip ---
    const std::string tags_path = "test_529_tags.json";
    std::remove(tags_path.c_str());

    std::vector<ExpenseTags::TagRecord> missing_result;
    check(ExpenseTags::loadTags(tags_path, missing_result) && missing_result.empty(),
          "loadTags on missing file returns empty success");

    ExpenseTags::TagRecord r1;
    r1.key = "abc123-0";
    r1.status = "qualified";
    r1.qualified_amount = 40.0;
    r1.receipts = {"target-receipt.jpg", "back-of-receipt.png"};
    r1.account = "Capital One Balance";
    r1.date = 1722400000;
    r1.amount = 120.0;
    r1.notes = "Target \"back to school\" run, aisle 5\\9";
    r1.category = "GENERAL_MERCHANDISE_SUPERSTORES";
    r1.created = 1722500000;

    ExpenseTags::TagRecord r2;
    r2.key = "def456-1";
    r2.status = "dismissed";
    r2.account = "Capital One Balance";
    r2.date = 1722300000;
    r2.amount = 15.5;
    r2.notes = "Chipotle";
    r2.category = "FOOD_AND_DRINK_RESTAURANT";
    r2.created = 1722500001;

    check(ExpenseTags::saveTags(tags_path, {r1, r2}), "saveTags succeeds");

    std::vector<ExpenseTags::TagRecord> loaded;
    check(ExpenseTags::loadTags(tags_path, loaded), "loadTags succeeds");
    check(loaded.size() == 2, "loadTags returns both records");
    if (loaded.size() == 2)
    {
        check(loaded[0].key == r1.key && loaded[0].status == "qualified", "record 1 identity round-trips");
        check(std::abs(loaded[0].qualified_amount - 40.0) < 1e-9, "qualified_amount round-trips");
        check(loaded[0].receipts.size() == 2 && loaded[0].receipts[1] == "back-of-receipt.png",
              "receipts array round-trips");
        check(loaded[0].notes == r1.notes, "quotes and backslashes in notes round-trip");
        check(loaded[0].date == r1.date && std::abs(loaded[0].amount - 120.0) < 1e-9,
              "denormalized fields round-trip");
        check(loaded[1].status == "dismissed" && loaded[1].receipts.empty(),
              "dismissed record round-trips with no receipts");
    }

    {
        std::ofstream corrupt(tags_path);
        corrupt << "{not json";
    }
    std::vector<ExpenseTags::TagRecord> corrupt_result;
    check(!ExpenseTags::loadTags(tags_path, corrupt_result), "loadTags fails on corrupt file");

    std::remove(tags_path.c_str());
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test-529`
Expected: FAIL on "loadTags on missing file returns empty success" (stub returns false).

- [ ] **Step 3: Implement save + load**

In `src/expense_tags.cpp`, add `#include <cstdio>`, `#include <cstdlib>`, and `#include <fstream>`, then replace the two stubs:

```cpp
    namespace
    {
        // (place inside the existing anonymous namespace at the top of the file)
        std::string jsonEscape(const std::string& value)
        {
            std::ostringstream out;
            for (char c : value)
            {
                switch (c)
                {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out << buf;
                    }
                    else
                    {
                        out << c;
                    }
                }
            }
            return out.str();
        }

        // Minimal scanner for the exact shape saveTags writes: an array of
        // flat objects with string / number / string-array values. Anything
        // else is a parse failure — this file is only ever written by us.
        struct TagScanner
        {
            const std::string& text;
            size_t pos = 0;

            explicit TagScanner(const std::string& t) : text(t) {}

            void skipWhitespace()
            {
                while (pos < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[pos])))
                {
                    ++pos;
                }
            }

            bool consume(char expected)
            {
                skipWhitespace();
                if (pos < text.size() && text[pos] == expected)
                {
                    ++pos;
                    return true;
                }
                return false;
            }

            bool peek(char expected)
            {
                skipWhitespace();
                return pos < text.size() && text[pos] == expected;
            }

            bool parseString(std::string& out)
            {
                if (!consume('"')) return false;
                out.clear();
                while (pos < text.size())
                {
                    const char c = text[pos++];
                    if (c == '"') return true;
                    if (c == '\\')
                    {
                        if (pos >= text.size()) return false;
                        const char esc = text[pos++];
                        switch (esc)
                        {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u':
                        {
                            if (pos + 4 > text.size()) return false;
                            const std::string hex = text.substr(pos, 4);
                            pos += 4;
                            const long code = std::strtol(hex.c_str(), nullptr, 16);
                            // We only ever emit \u00XX for control chars.
                            out += static_cast<char>(code & 0xFF);
                            break;
                        }
                        default: return false;
                        }
                    }
                    else
                    {
                        out += c;
                    }
                }
                return false;
            }

            bool parseNumber(double& out)
            {
                skipWhitespace();
                const size_t start = pos;
                while (pos < text.size() &&
                       (std::isdigit(static_cast<unsigned char>(text[pos])) ||
                        text[pos] == '-' || text[pos] == '+' ||
                        text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'))
                {
                    ++pos;
                }
                if (pos == start) return false;
                try
                {
                    out = std::stod(text.substr(start, pos - start));
                }
                catch (...)
                {
                    return false;
                }
                return true;
            }
        };
    }

    bool ExpenseTags::saveTags(const std::string& file_path,
                               const std::vector<TagRecord>& tags)
    {
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < tags.size(); ++i)
        {
            const TagRecord& tag = tags[i];
            if (i > 0) out << ",";
            out << "\n  {"
                << "\"key\":\"" << jsonEscape(tag.key) << "\","
                << "\"status\":\"" << jsonEscape(tag.status) << "\","
                << "\"qualified_amount\":" << tag.qualified_amount << ","
                << "\"receipts\":[";
            for (size_t r = 0; r < tag.receipts.size(); ++r)
            {
                if (r > 0) out << ",";
                out << "\"" << jsonEscape(tag.receipts[r]) << "\"";
            }
            out << "],"
                << "\"account\":\"" << jsonEscape(tag.account) << "\","
                << "\"date\":" << static_cast<long long>(tag.date) << ","
                << "\"amount\":" << tag.amount << ","
                << "\"notes\":\"" << jsonEscape(tag.notes) << "\","
                << "\"category\":\"" << jsonEscape(tag.category) << "\","
                << "\"created\":" << static_cast<long long>(tag.created)
                << "}";
        }
        out << "\n]\n";

        const std::string temp_path = file_path + ".tmp";
        {
            std::ofstream file(temp_path, std::ios::trunc);
            if (!file.is_open()) return false;
            file << out.str();
            if (!file.good()) return false;
        }
        return std::rename(temp_path.c_str(), file_path.c_str()) == 0;
    }

    bool ExpenseTags::loadTags(const std::string& file_path,
                               std::vector<TagRecord>& out)
    {
        out.clear();
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            return true; // missing file == empty store
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();

        TagScanner scanner(text);
        if (!scanner.consume('[')) return false;
        if (scanner.consume(']')) return true;

        while (true)
        {
            if (!scanner.consume('{')) return false;
            TagRecord tag;
            if (!scanner.peek('}'))
            {
                while (true)
                {
                    std::string field;
                    if (!scanner.parseString(field)) return false;
                    if (!scanner.consume(':')) return false;

                    if (field == "receipts")
                    {
                        if (!scanner.consume('[')) return false;
                        if (!scanner.consume(']'))
                        {
                            while (true)
                            {
                                std::string receipt;
                                if (!scanner.parseString(receipt)) return false;
                                tag.receipts.push_back(receipt);
                                if (scanner.consume(']')) break;
                                if (!scanner.consume(',')) return false;
                            }
                        }
                    }
                    else if (field == "qualified_amount" || field == "date" ||
                             field == "amount" || field == "created")
                    {
                        double value = 0.0;
                        if (!scanner.parseNumber(value)) return false;
                        if (field == "qualified_amount") tag.qualified_amount = value;
                        else if (field == "date") tag.date = static_cast<time_t>(value);
                        else if (field == "amount") tag.amount = value;
                        else tag.created = static_cast<time_t>(value);
                    }
                    else
                    {
                        std::string value;
                        if (!scanner.parseString(value)) return false;
                        if (field == "key") tag.key = value;
                        else if (field == "status") tag.status = value;
                        else if (field == "account") tag.account = value;
                        else if (field == "notes") tag.notes = value;
                        else if (field == "category") tag.category = value;
                        // unknown string fields are ignored for forward compat
                    }

                    if (scanner.consume('}')) break;
                    if (!scanner.consume(',')) return false;
                }
            }
            else
            {
                scanner.consume('}');
            }
            out.push_back(tag);
            if (scanner.consume(']')) break;
            if (!scanner.consume(',')) return false;
        }
        return true;
    }
```

(Adjust namespace placement to match the file: the helpers go in the anonymous namespace, the two functions inside the existing `namespace ExpenseTags` block without the `ExpenseTags::` qualifier.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `make test-529`
Expected: all `✓`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/expense_tags.cpp test_529.cpp
git commit -m "Add atomic tags.json persistence to ExpenseTags

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Stable transaction keys in /api/spend

**Files:**
- Modify: `src/web_server.cpp` — `buildSpendJson` (~line 3259) and its call site (~line 4163)

**Interfaces:**
- Consumes: `ExpenseTags::KeyAssigner` from Task 1.
- Produces (used by Tasks 4–6 and the frontend):
  - Anonymous-namespace struct + function in `web_server.cpp`:
    ```cpp
    struct SpendTxn
    {
        std::string key;
        time_t date;
        double amount;            // positive spend amount
        std::string category;     // effective category (after spend_overrides)
        std::string notes;
        std::string account;
        std::string account_type; // "CASH" | "DEBT"
    };
    std::vector<SpendTxn> collectSpendTransactions(PortfolioManager& manager, time_t from, time_t to);
    ```
  - `GET /api/spend` transactions each gain `"key":"<hex>-<n>"`.

- [ ] **Step 1: Refactor buildSpendJson into collect + serialize**

Add `#include "expense_tags.hpp"` to the includes at the top of `src/web_server.cpp`.

Directly above `buildSpendJson`, add `SpendTxn` and `collectSpendTransactions`. The collection logic is `buildSpendJson`'s current loop with ONE structural change — the date-range check moves BELOW key assignment, because occurrence indices must be computed over the account's full history or the same transaction would get a different key depending on the requested range:

```cpp
    std::vector<SpendTxn> collectSpendTransactions(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendOverride> overrides = loadSpendOverrides();
        std::vector<SpendTxn> result;
        ExpenseTags::KeyAssigner key_assigner;

        if (!manager.scanPortfolios())
        {
            return result;
        }

        const std::vector<std::string> names = manager.getPortfolioNames();
        for (const std::string& name : names)
        {
            Portfolio portfolio;
            if (!loadPortfolioCached(manager, name, portfolio)) continue;
            const PortfolioType pt = portfolio.getType();
            if (pt != PortfolioType::CASH && pt != PortfolioType::DEBT) continue;

            for (const Transaction& tx : portfolio.getTransactions())
            {
                if (tx.type != TransactionType::WITHDRAWAL) continue;
                if (notesIsPending(tx.notes)) continue;
                if (notesStartsWithTxfr(tx.notes)) continue;

                const std::string upper_cat = upperCopy(tx.category);
                if (upper_cat.rfind("TRANSFER_IN", 0) == 0) continue;
                if (upper_cat.rfind("TRANSFER_OUT", 0) == 0) continue;
                if (upper_cat.rfind("LOAN_PAYMENTS", 0) == 0) continue;

                // Key BEFORE the range filter: occurrence indices must be
                // stable across different requested ranges.
                const std::string key = key_assigner.next(name, tx.date, tx.amount, tx.notes);

                if (tx.date < from || tx.date > to) continue;

                std::string effective_category = tx.category;
                if (!overrides.empty())
                {
                    const std::string notes_lower = lowerCopy(tx.notes);
                    for (const auto& rule : overrides)
                    {
                        if (notes_lower.find(rule.match_lower) != std::string::npos)
                        {
                            effective_category = rule.category;
                            break;
                        }
                    }
                }

                SpendTxn spend_tx;
                spend_tx.key = key;
                spend_tx.date = tx.date;
                spend_tx.amount = std::abs(tx.amount);
                spend_tx.category = effective_category;
                spend_tx.notes = tx.notes;
                spend_tx.account = name;
                spend_tx.account_type = portfolioTypeToString(pt);
                result.push_back(spend_tx);
            }
        }
        return result;
    }
```

Then rewrite `buildSpendJson` as a thin serializer over it (preserving the existing response shape, adding `key`):

```cpp
    std::string buildSpendJson(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendTxn> txs = collectSpendTransactions(manager, from, to);
        std::ostringstream out;
        out << "{"
            << "\"from\":" << static_cast<long long>(from) << ","
            << "\"to\":" << static_cast<long long>(to) << ","
            << "\"transactions\":[";
        for (size_t i = 0; i < txs.size(); ++i)
        {
            if (i > 0) out << ",";
            out << "{"
                << "\"key\":" << jsonString(txs[i].key) << ","
                << "\"date\":" << static_cast<long long>(txs[i].date) << ","
                << "\"amount\":" << jsonNumber(txs[i].amount) << ","
                << "\"category\":" << jsonString(txs[i].category) << ","
                << "\"notes\":" << jsonString(txs[i].notes) << ","
                << "\"account\":" << jsonString(txs[i].account) << ","
                << "\"account_type\":" << jsonString(txs[i].account_type)
                << "}";
        }
        out << "]}";
        return out.str();
    }
```

Delete the old inline loop body (the comment block above the old function explaining CASH/DEBT filtering moves to `collectSpendTransactions`).

- [ ] **Step 2: Build**

Run: `make clean && make` (a header include was added — full rebuild).
Expected: clean build, no warnings from the new code.

- [ ] **Step 3: Verify keys via curl**

Restart: `pm2 restart 3`, wait 2 seconds. Then (adjust port if different — check `pm2 describe 3` / `.env` for the port; the frontend calls the same origin):

```bash
curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | head -c 600
```

Expected: each transaction object contains `"key":"<hex>-<n>"`.

Range stability check — the same transaction must have the same key in a wider range:

```bash
curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted((t['key'],t['notes']) for t in d['transactions'])[:5])"
curl -s "http://localhost:8080/api/spend?from=2026-01-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); ks={t['key'] for t in d['transactions']}; print(len(ks), len(d['transactions']))"
```

Expected: first command's keys all appear in the second range's key set; second command shows key count == transaction count (no collisions).

- [ ] **Step 4: Run existing regression test**

Run: `make test-persistence`
Expected: passes as before.

- [ ] **Step 5: Commit**

```bash
git add src/web_server.cpp Makefile
git commit -m "Expose stable transaction keys in /api/spend

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Tag endpoints — GET /api/529/tags, POST /api/529/tag

**Files:**
- Modify: `src/web_server.cpp` — new helpers in the anonymous namespace near `buildSpendJson`, new routes in the dispatch chain (after the `/api/spend` block, ~line 4164)

**Interfaces:**
- Consumes: `ExpenseTags::loadTags/saveTags/TagRecord` (Task 2), `collectSpendTransactions` (Task 3).
- Produces (used by Tasks 5–6 and the frontend):
  - `GET /api/529/tags` → `{"tags":[{key,status,qualified_amount,receipts:[...],account,date,amount,notes,category,created}, ...]}`
  - `POST /api/529/tag` body `{"key":"...","status":"qualified"|"dismissed"|"none","qualified_amount":40.0}` → `{"tag":{...}}` (or `{"removed":true}` for status "none")
  - Anonymous-namespace helpers: `const char* kTagsFile = "data/529/tags.json";` `std::mutex g_529_mutex;` `std::string serializeTagRecord(const ExpenseTags::TagRecord&)`

- [ ] **Step 1: Add shared helpers**

In the anonymous namespace (near the other 529 code), add (`#include <mutex>` and `#include <filesystem>` at the top of the file if not present):

```cpp
    const char* kTagsFile = "data/529/tags.json";
    const char* kReceiptsDir = "data/529/receipts";

    // Request handling is one-thread-per-connection; serialize every
    // load-modify-save of tags.json and every receipt-dir mutation.
    std::mutex g_529_mutex;

    std::string serializeTagRecord(const ExpenseTags::TagRecord& tag)
    {
        std::ostringstream out;
        out << "{"
            << "\"key\":" << jsonString(tag.key) << ","
            << "\"status\":" << jsonString(tag.status) << ","
            << "\"qualified_amount\":" << jsonNumber(tag.qualified_amount) << ","
            << "\"receipts\":[";
        for (size_t i = 0; i < tag.receipts.size(); ++i)
        {
            if (i > 0) out << ",";
            out << jsonString(tag.receipts[i]);
        }
        out << "],"
            << "\"account\":" << jsonString(tag.account) << ","
            << "\"date\":" << static_cast<long long>(tag.date) << ","
            << "\"amount\":" << jsonNumber(tag.amount) << ","
            << "\"notes\":" << jsonString(tag.notes) << ","
            << "\"category\":" << jsonString(tag.category) << ","
            << "\"created\":" << static_cast<long long>(tag.created)
            << "}";
        return out.str();
    }

    bool ensure529Dirs()
    {
        std::error_code ec;
        std::filesystem::create_directories("data/529", ec);
        std::filesystem::create_directories(kReceiptsDir, ec);
        return !ec;
    }
```

- [ ] **Step 2: Add the GET route**

In the dispatch chain, right after the `/api/spend` handler's closing brace:

```cpp
        if (request.method == "GET" && request.path == "/api/529/tags")
        {
            std::lock_guard<std::mutex> lock(g_529_mutex);
            std::vector<ExpenseTags::TagRecord> tags;
            if (!ExpenseTags::loadTags(kTagsFile, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            std::ostringstream out;
            out << "{\"tags\":[";
            for (size_t i = 0; i < tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(tags[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }
```

- [ ] **Step 3: Add the POST route**

```cpp
        if (request.method == "POST" && request.path == "/api/529/tag")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }

            const auto raw_key = getObjectString(body, "key");
            const auto raw_status = getObjectString(body, "status");
            if (!raw_key.has_value() || !raw_status.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("key and status are required"));
            }
            const std::string key = trim(raw_key.value());
            const std::string status = trim(raw_status.value());
            if (status != "qualified" && status != "dismissed" && status != "none")
            {
                return makeJsonResponse(400, makeErrorBody("status must be qualified, dismissed, or none"));
            }

            std::lock_guard<std::mutex> lock(g_529_mutex);
            if (!ensure529Dirs())
            {
                return makeJsonResponse(500, makeErrorBody("Failed to create data/529"));
            }
            std::vector<ExpenseTags::TagRecord> tags;
            if (!ExpenseTags::loadTags(kTagsFile, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }

            auto existing = std::find_if(tags.begin(), tags.end(),
                [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });

            if (status == "none")
            {
                if (existing == tags.end())
                {
                    return makeJsonResponse(404, makeErrorBody("No 529 tag for that key"));
                }
                // Receipt files stay on disk (spec: only explicit DELETE removes them).
                tags.erase(existing);
                if (!ExpenseTags::saveTags(kTagsFile, tags))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save 529 tags"));
                }
                return makeJsonResponse(200, "{\"removed\":true}");
            }

            ExpenseTags::TagRecord record;
            if (existing != tags.end())
            {
                record = *existing;
            }
            else
            {
                // New tag: capture denormalized source fields from spend data.
                const time_t now = std::time(nullptr);
                const std::vector<SpendTxn> all = collectSpendTransactions(manager, 0, now + 86400);
                auto txn = std::find_if(all.begin(), all.end(),
                    [&key](const SpendTxn& t) { return t.key == key; });
                if (txn == all.end())
                {
                    return makeJsonResponse(404, makeErrorBody("Transaction not found for that key"));
                }
                record.key = key;
                record.account = txn->account;
                record.date = txn->date;
                record.amount = txn->amount;
                record.notes = txn->notes;
                record.category = txn->category;
                record.created = now;
            }

            record.status = status;
            if (status == "qualified")
            {
                double qualified_amount = record.amount; // default: full charge
                const auto raw_amount = getObjectNumber(body, "qualified_amount");
                if (raw_amount.has_value())
                {
                    qualified_amount = raw_amount.value();
                }
                if (!(qualified_amount > 0.0) || qualified_amount > record.amount + 0.005)
                {
                    return makeJsonResponse(400, makeErrorBody("qualified_amount must be > 0 and <= the charge amount"));
                }
                record.qualified_amount = qualified_amount;
            }
            else
            {
                record.qualified_amount = 0.0;
            }

            if (existing != tags.end())
            {
                *existing = record;
            }
            else
            {
                tags.push_back(record);
            }
            if (!ExpenseTags::saveTags(kTagsFile, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to save 529 tags"));
            }
            return makeJsonResponse(200, std::string("{\"tag\":") + serializeTagRecord(record) + "}");
        }
```

- [ ] **Step 4: Build and deploy**

Run: `make && pm2 restart 3` (no header changed in this task; plain `make` is fine).

- [ ] **Step 5: Verify with curl**

```bash
# Pick a real key from spend data:
KEY=$(curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")

# Qualify it (default amount):
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\"}" http://localhost:8080/api/529/tag
# Expected: {"tag":{...,"status":"qualified","qualified_amount":<full amount>,...}}

# Edit the amount down:
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\",\"qualified_amount\":1.00}" http://localhost:8080/api/529/tag
# Expected: qualified_amount 1

# Over-amount rejected:
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\",\"qualified_amount\":999999}" http://localhost:8080/api/529/tag
# Expected: 400 error body

# List:
curl -s http://localhost:8080/api/529/tags
# Expected: the record appears

# Unknown key:
curl -s -X POST -H "Content-Type: application/json" -d '{"key":"nope-0","status":"qualified"}' http://localhost:8080/api/529/tag
# Expected: 404

# Remove:
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"none\"}" http://localhost:8080/api/529/tag
# Expected: {"removed":true}; /api/529/tags is empty again
```

Also confirm `data/529/tags.json` exists and is human-readable after the qualify step.

- [ ] **Step 6: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add 529 tag endpoints (list, upsert, dismiss, remove)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Receipt endpoints — upload, serve, delete

**Files:**
- Modify: `src/web_server.cpp` — three routes after the `/api/529/tag` block

**Interfaces:**
- Consumes: helpers from Task 4, `ExpenseTags::sanitizeFilename/isAllowedReceiptExtension/receiptMimeType` (Task 1).
- Produces (used by Task 6 and the frontend):
  - `POST /api/529/receipt?key=&filename=` — raw file bytes as body → `{"filename":"<stored name>"}` (stored name may differ: sanitized and/or `-2`, `-3` deduped)
  - `GET /api/529/receipt?key=&filename=` — file bytes with proper Content-Type
  - `DELETE /api/529/receipt?key=&filename=` → `{"removed":true}`
  - Receipt files live at `data/529/receipts/<key>/<stored name>`

- [ ] **Step 1: Add the three routes**

```cpp
        if (request.path == "/api/529/receipt")
        {
            const auto query_values = parseQuery(request.query);
            const auto key_it = query_values.find("key");
            const auto filename_it = query_values.find("filename");
            if (key_it == query_values.end() || key_it->second.empty() ||
                filename_it == query_values.end() || filename_it->second.empty())
            {
                return makeJsonResponse(400, makeErrorBody("key and filename query params are required"));
            }
            const std::string key = key_it->second;
            // The key itself lands in a filesystem path — restrict it hard.
            if (key.find_first_not_of("0123456789abcdef-") != std::string::npos)
            {
                return makeJsonResponse(400, makeErrorBody("Invalid key"));
            }
            const std::string safe_name = ExpenseTags::sanitizeFilename(filename_it->second);
            if (safe_name.empty())
            {
                return makeJsonResponse(400, makeErrorBody("Unusable filename"));
            }
            if (!ExpenseTags::isAllowedReceiptExtension(safe_name))
            {
                return makeJsonResponse(400, makeErrorBody("Only jpg, jpeg, png, heic, webp, pdf receipts are allowed"));
            }

            const std::filesystem::path receipt_dir = std::filesystem::path(kReceiptsDir) / key;

            if (request.method == "POST")
            {
                if (request.body.empty())
                {
                    return makeJsonResponse(400, makeErrorBody("Empty upload"));
                }
                if (request.body.size() > 25ULL * 1024 * 1024)
                {
                    return makeJsonResponse(413, makeErrorBody("Receipt too large (25 MB max)"));
                }

                std::lock_guard<std::mutex> lock(g_529_mutex);
                std::vector<ExpenseTags::TagRecord> tags;
                if (!ExpenseTags::loadTags(kTagsFile, tags))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
                }
                auto tag = std::find_if(tags.begin(), tags.end(),
                    [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
                if (tag == tags.end() || tag->status != "qualified")
                {
                    return makeJsonResponse(404, makeErrorBody("Qualify the charge before attaching receipts"));
                }

                std::error_code ec;
                std::filesystem::create_directories(receipt_dir, ec);
                if (ec)
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to create receipt directory"));
                }

                // Dedupe: receipt.jpg -> receipt-2.jpg -> receipt-3.jpg ...
                std::string stored_name = safe_name;
                const size_t dot = safe_name.find_last_of('.');
                const std::string stem = safe_name.substr(0, dot);
                const std::string ext = safe_name.substr(dot); // includes '.'
                int suffix = 2;
                while (std::filesystem::exists(receipt_dir / stored_name))
                {
                    stored_name = stem + "-" + std::to_string(suffix++) + ext;
                }

                const std::filesystem::path final_path = receipt_dir / stored_name;
                const std::filesystem::path temp_path = receipt_dir / (stored_name + ".tmp");
                {
                    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                    if (!file.is_open())
                    {
                        return makeJsonResponse(500, makeErrorBody("Failed to write receipt"));
                    }
                    file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
                    if (!file.good())
                    {
                        return makeJsonResponse(500, makeErrorBody("Failed to write receipt"));
                    }
                }
                std::filesystem::rename(temp_path, final_path, ec);
                if (ec)
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to finalize receipt"));
                }

                tag->receipts.push_back(stored_name);
                if (!ExpenseTags::saveTags(kTagsFile, tags))
                {
                    return makeJsonResponse(500, makeErrorBody("Receipt stored but tag update failed"));
                }
                return makeJsonResponse(201, std::string("{\"filename\":") + jsonString(stored_name) + "}");
            }

            if (request.method == "GET")
            {
                std::ifstream file(receipt_dir / safe_name, std::ios::binary);
                if (!file.is_open())
                {
                    return makeJsonResponse(404, makeErrorBody("Receipt not found"));
                }
                std::ostringstream buffer;
                buffer << file.rdbuf();
                HttpResponse response;
                response.status = 200;
                response.content_type = ExpenseTags::receiptMimeType(safe_name);
                response.body = buffer.str();
                return response;
            }

            if (request.method == "DELETE")
            {
                std::lock_guard<std::mutex> lock(g_529_mutex);
                std::vector<ExpenseTags::TagRecord> tags;
                if (!ExpenseTags::loadTags(kTagsFile, tags))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
                }
                std::error_code ec;
                std::filesystem::remove(receipt_dir / safe_name, ec);
                auto tag = std::find_if(tags.begin(), tags.end(),
                    [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
                if (tag != tags.end())
                {
                    tag->receipts.erase(
                        std::remove(tag->receipts.begin(), tag->receipts.end(), safe_name),
                        tag->receipts.end());
                    if (!ExpenseTags::saveTags(kTagsFile, tags))
                    {
                        return makeJsonResponse(500, makeErrorBody("Failed to save 529 tags"));
                    }
                }
                return makeJsonResponse(200, "{\"removed\":true}");
            }

            return makeJsonResponse(405, makeErrorBody("Method not allowed"));
        }
```

Check the file's existing includes for `<filesystem>`; add it if absent. Note the ETag/gzip post-processing (`weakEtagForBody`, ~line 2663) runs on GET 200 responses — receipts are binary and may be large; confirm gzip only fires for bodies it can compress and that ETag on a receipt GET is harmless (it is — it's content-derived). No change needed unless testing shows corruption.

- [ ] **Step 2: Build and deploy**

Run: `make && pm2 restart 3`

- [ ] **Step 3: Verify with curl**

```bash
KEY=$(curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\"}" http://localhost:8080/api/529/tag > /dev/null

# Make a tiny test image and upload it:
printf '\x89PNG\r\n\x1a\n' > /tmp/claude-test-receipt.png
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=$KEY&filename=test-receipt.png"
# Expected: {"filename":"test-receipt.png"}

# Duplicate name gets suffixed:
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=$KEY&filename=test-receipt.png"
# Expected: {"filename":"test-receipt-2.png"}

# Serve round-trips bytes:
curl -s "http://localhost:8080/api/529/receipt?key=$KEY&filename=test-receipt.png" | cmp - /tmp/claude-test-receipt.png && echo BYTES-OK
curl -s -I "http://localhost:8080/api/529/receipt?key=$KEY&filename=test-receipt.png" | grep -i content-type
# Expected: BYTES-OK and image/png

# Disallowed extension:
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=$KEY&filename=evil.sh"
# Expected: 400

# Traversal attempt:
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=$KEY&filename=..%2F..%2Fowned.png"
# Expected: stores as "owned.png" under the key dir (check: ls data/529/receipts/$KEY/) — nothing outside it

# Upload to non-qualified key:
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=ffff-0&filename=x.png"
# Expected: 404

# Delete:
curl -s -X DELETE "http://localhost:8080/api/529/receipt?key=$KEY&filename=test-receipt-2.png"
# Expected: {"removed":true}; file gone from data/529/receipts/$KEY/ and from the tag's receipts array

# Cleanup test tag:
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"none\"}" http://localhost:8080/api/529/tag > /dev/null
rm -rf "data/529/receipts/$KEY" /tmp/claude-test-receipt.png
```

- [ ] **Step 4: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add 529 receipt upload/serve/delete endpoints

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Export endpoints — CSV and ZIP

**Files:**
- Modify: `src/web_server.cpp` — two routes after the receipt block, one CSV helper in the anonymous namespace

**Interfaces:**
- Consumes: Task 4/5 helpers and stores.
- Produces (used by the frontend):
  - `GET /api/529/export.csv?from=YYYY-MM-DD&to=YYYY-MM-DD` — `text/csv` body
  - `GET /api/529/export.zip?from=&to=` — `application/zip` body containing `expenses.csv` + receipts named `YYYY-MM-DD_<merchant>_<filename>`

- [ ] **Step 1: Add the CSV builder helper**

```cpp
    std::string csvField(const std::string& value)
    {
        if (value.find_first_of(",\"\n\r") == std::string::npos)
        {
            return value;
        }
        std::string escaped = "\"";
        for (char c : value)
        {
            if (c == '"') escaped += "\"\"";
            else escaped += c;
        }
        escaped += "\"";
        return escaped;
    }

    std::vector<ExpenseTags::TagRecord> qualifiedTagsInRange(time_t from, time_t to, bool& ok)
    {
        std::vector<ExpenseTags::TagRecord> tags;
        ok = ExpenseTags::loadTags(kTagsFile, tags);
        std::vector<ExpenseTags::TagRecord> result;
        if (!ok) return result;
        for (const auto& tag : tags)
        {
            if (tag.status != "qualified") continue;
            if (tag.date < from || tag.date > to) continue;
            result.push_back(tag);
        }
        std::sort(result.begin(), result.end(),
                  [](const ExpenseTags::TagRecord& a, const ExpenseTags::TagRecord& b)
                  { return a.date < b.date; });
        return result;
    }

    std::string build529Csv(const std::vector<ExpenseTags::TagRecord>& tags)
    {
        std::ostringstream out;
        out << "date,account,merchant,category,charge_amount,qualified_amount,receipts\r\n";
        char date_buf[16];
        for (const auto& tag : tags)
        {
            std::tm tm_utc{};
            gmtime_r(&tag.date, &tm_utc);
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
            std::string receipts_joined;
            for (size_t i = 0; i < tag.receipts.size(); ++i)
            {
                if (i > 0) receipts_joined += "; ";
                receipts_joined += tag.receipts[i];
            }
            out << date_buf << ","
                << csvField(tag.account) << ","
                << csvField(tag.notes) << ","
                << csvField(tag.category) << ","
                << jsonNumber(tag.amount) << ","
                << jsonNumber(tag.qualified_amount) << ","
                << csvField(receipts_joined) << "\r\n";
        }
        return out.str();
    }
```

- [ ] **Step 2: Add the two routes**

Both parse `from`/`to` exactly like the `/api/spend` handler (copy that block: `parseIsoDateUTC`, end-of-day bump on `to`, defaults 1 year back / now).

```cpp
        if (request.method == "GET" && request.path == "/api/529/export.csv")
        {
            // ... from/to parsing identical to /api/spend ...
            std::lock_guard<std::mutex> lock(g_529_mutex);
            bool ok = false;
            const auto tags = qualifiedTagsInRange(from, to, ok);
            if (!ok)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            HttpResponse response;
            response.status = 200;
            response.content_type = "text/csv; charset=utf-8";
            response.body = build529Csv(tags);
            return response;
        }

        if (request.method == "GET" && request.path == "/api/529/export.zip")
        {
            // ... from/to parsing identical to /api/spend ...
            std::lock_guard<std::mutex> lock(g_529_mutex);
            bool ok = false;
            const auto tags = qualifiedTagsInRange(from, to, ok);
            if (!ok)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }

            const std::filesystem::path staging = "data/529/.export_tmp";
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            std::filesystem::create_directories(staging, ec);
            if (ec)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to create export staging dir"));
            }

            {
                std::ofstream csv(staging / "expenses.csv", std::ios::binary);
                csv << build529Csv(tags);
            }

            char date_buf[16];
            for (const auto& tag : tags)
            {
                std::tm tm_utc{};
                gmtime_r(&tag.date, &tm_utc);
                std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
                std::string merchant = ExpenseTags::sanitizeFilename(tag.notes);
                if (merchant.size() > 40) merchant.resize(40);
                if (merchant.empty()) merchant = "receipt";
                for (const auto& receipt : tag.receipts)
                {
                    const std::filesystem::path src =
                        std::filesystem::path(kReceiptsDir) / tag.key / receipt;
                    const std::filesystem::path dst =
                        staging / (std::string(date_buf) + "_" + merchant + "_" + receipt);
                    std::filesystem::copy_file(src, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    // Missing files are skipped: the CSV still lists the name.
                }
            }

            // macOS ships /usr/bin/zip. Quote the path defensively even though
            // staging is a constant.
            const std::string cmd =
                "cd '" + staging.string() + "' && /usr/bin/zip -q -X export.zip . -r";
            if (std::system(cmd.c_str()) != 0)
            {
                std::filesystem::remove_all(staging, ec);
                return makeJsonResponse(500, makeErrorBody("zip failed"));
            }

            std::ifstream zip_file(staging / "export.zip", std::ios::binary);
            if (!zip_file.is_open())
            {
                std::filesystem::remove_all(staging, ec);
                return makeJsonResponse(500, makeErrorBody("zip output missing"));
            }
            std::ostringstream buffer;
            buffer << zip_file.rdbuf();
            zip_file.close();
            std::filesystem::remove_all(staging, ec);

            HttpResponse response;
            response.status = 200;
            response.content_type = "application/zip";
            response.body = buffer.str();
            return response;
        }
```

- [ ] **Step 3: Build, deploy, verify**

Run: `make && pm2 restart 3`, then:

```bash
KEY=$(curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\",\"qualified_amount\":5.00}" http://localhost:8080/api/529/tag > /dev/null
printf '\x89PNG\r\n\x1a\n' > /tmp/claude-test-receipt.png
curl -s -X POST --data-binary @/tmp/claude-test-receipt.png "http://localhost:8080/api/529/receipt?key=$KEY&filename=r.png" > /dev/null

curl -s "http://localhost:8080/api/529/export.csv?from=2026-07-01&to=2026-08-05"
# Expected: header row + one data row with qualified_amount 5

curl -s -o /tmp/claude-529-export.zip "http://localhost:8080/api/529/export.zip?from=2026-07-01&to=2026-08-05"
unzip -l /tmp/claude-529-export.zip
# Expected: expenses.csv + one receipt named YYYY-MM-DD_<merchant>_r.png

# Cleanup:
curl -s -X DELETE "http://localhost:8080/api/529/receipt?key=$KEY&filename=r.png" > /dev/null
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"none\"}" http://localhost:8080/api/529/tag > /dev/null
rm -f /tmp/claude-test-receipt.png /tmp/claude-529-export.zip
ls data/529/.export_tmp 2>&1
# Expected: No such file or directory (staging cleaned up)
```

- [ ] **Step 4: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add 529 CSV and ZIP export endpoints

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Frontend rename + Spending sub-tabs scaffolding

**Files:**
- Modify: `web/index.html` — spendView section (~lines 53–104), asset version query strings (lines 10, 13)
- Modify: `web/app.js` — dashboard button label (~line 1909), breadcrumbs (~line 3289), `state.spend` (~line 285)
- Modify: `web/styles.css` — new tab styles
- Modify: `web/sw.js` — bump `CACHE_NAME` (line 4)

**Interfaces:**
- Produces (used by Tasks 8–11): DOM ids `spendTabAnalysis`, `spendTab529`, `spendAnalysisPane`, `spend529Pane`; `state.spend.tab` (`"analysis"` | `"529"`); function `setSpendTab(tab)` that toggles panes and calls `render529Tab()` (stub for now) when switching to 529.

- [ ] **Step 1: Rename user-visible copy**

- `web/index.html` line 57: `<h2>Spend Analysis</h2>` → `<h2>Spending</h2>`.
- `web/app.js` ~line 1909: button text `Spend Analysis` → `Spending`.
- `web/app.js` ~line 3289: breadcrumb `{ label: "Spend Analysis" }` → `{ label: "Spending" }`.
- Grep for any other user-visible "Spend Analysis" copy: `grep -n "Spend Analysis" web/*.js web/*.html` — update all hits (skip variable/id names).

- [ ] **Step 2: Add tab bar and panes to index.html**

Inside `#spendView`, immediately after the closing `</div>` of `.page-head`, insert:

```html
        <div class="spend-tabs" role="tablist">
          <button id="spendTabAnalysis" class="spend-tab is-active" type="button" role="tab">Analysis</button>
          <button id="spendTab529" class="spend-tab" type="button" role="tab">529 Expenses</button>
        </div>

        <div id="spendAnalysisPane">
```

Then close `</div>` after the last existing `</article>` (the By Category panel, line ~103), and add the 529 pane before `</section>`:

```html
        <div id="spend529Pane" hidden>
          <article class="panel">
            <div class="panel-head">
              <h3 id="total529Label">529 qualified total</h3>
              <div class="chart-tools">
                <button id="export529CsvBtn" class="ghost-btn" type="button">Export CSV</button>
                <button id="export529ZipBtn" class="ghost-btn" type="button">Export ZIP</button>
              </div>
            </div>
            <p id="summary529Meta" class="muted-note"></p>
          </article>

          <article class="panel">
            <div class="panel-head">
              <h3>Review Queue</h3>
              <p class="muted-note">Credit-card charges in grocery, food, and school-supply categories</p>
            </div>
            <div id="review529Table" class="stocks-table-wrap"></div>
          </article>

          <article class="panel">
            <div class="panel-head">
              <h3>Qualified Expenses</h3>
            </div>
            <div id="qualified529Table" class="stocks-table-wrap"></div>
          </article>
        </div>
```

- [ ] **Step 3: Add the qualify dialog to index.html**

Next to the other `<dialog>` elements (after `#transactionsDialog`, ~line 181):

```html
  <dialog id="qualify529Dialog" class="dialog">
    <h3>Qualify for 529</h3>
    <p id="qualify529Merchant" class="muted-note"></p>
    <label>
      Qualified amount
      <input id="qualify529Amount" type="number" step="0.01" min="0.01" inputmode="decimal" />
    </label>
    <label>
      Receipts (optional)
      <input id="qualify529Files" type="file" accept="image/*,.pdf,.heic" multiple />
    </label>
    <div class="dialog-actions">
      <button id="qualify529Cancel" class="ghost-btn" type="button">Cancel</button>
      <button id="qualify529Save" class="primary-btn" type="button">Save</button>
    </div>
  </dialog>
```

Match the markup conventions of an existing dialog (e.g. `#createPortfolioDialog`, ~line 247) — if those use `<form method="dialog">` or specific wrapper classes, mirror that structure instead.

- [ ] **Step 4: Wire tab switching in app.js**

In `state.spend` (~line 285) add `tab: "analysis",` and `tags529: {},`.

Near `showSpendAnalysis` (~line 3281) add:

```js
function setSpendTab(tab) {
  state.spend.tab = tab;
  const analysisPane = document.getElementById("spendAnalysisPane");
  const pane529 = document.getElementById("spend529Pane");
  const analysisBtn = document.getElementById("spendTabAnalysis");
  const btn529 = document.getElementById("spendTab529");
  if (analysisPane) analysisPane.hidden = tab !== "analysis";
  if (pane529) pane529.hidden = tab !== "529";
  if (analysisBtn) analysisBtn.classList.toggle("is-active", tab === "analysis");
  if (btn529) btn529.classList.toggle("is-active", tab === "529");
  if (tab === "529") render529Tab();
}

function render529Tab() {
  // populated in Task 8
}
```

In the init/wiring section where `backToDashFromSpendBtn` is hooked up (~line 3752), add:

```js
  const spendTabAnalysis = document.getElementById("spendTabAnalysis");
  const spendTab529 = document.getElementById("spendTab529");
  if (spendTabAnalysis) spendTabAnalysis.addEventListener("click", () => setSpendTab("analysis"));
  if (spendTab529) spendTab529.addEventListener("click", () => setSpendTab("529"));
```

- [ ] **Step 5: Add tab styles to styles.css**

Append, following the file's existing custom-property/color conventions (inspect nearby `.ghost-btn` / `.graph-period-select` rules and reuse their variables):

```css
.spend-tabs {
  display: flex;
  gap: 0.5rem;
  margin-bottom: 1rem;
}

.spend-tab {
  padding: 0.45rem 1rem;
  border: 1px solid rgba(34, 34, 34, 0.15);
  border-radius: 999px;
  background: transparent;
  font: inherit;
  cursor: pointer;
}

.spend-tab.is-active {
  background: #222;
  color: #fff;
  border-color: #222;
}
```

- [ ] **Step 6: Bump caches**

- `web/sw.js` line 4: `finance-tracker-shell-v2` → `finance-tracker-shell-v3`.
- `web/index.html` lines 10/13: bump `?v=` values on `styles.css` and `app.js` to today (e.g. `?v=20260805a`).

- [ ] **Step 7: Verify in browser**

Load the site, open the Spending page. Expected: header says "Spending", dashboard button says "Spending", tab bar shows Analysis (active, charts render as before) and 529 Expenses (empty panels). Tab clicks toggle panes. No console errors.

- [ ] **Step 8: Commit**

```bash
git add web/index.html web/app.js web/styles.css web/sw.js
git commit -m "Rename Spend Analysis to Spending, add 529 sub-tab scaffolding

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: 529 tab data plumbing + summary + qualified list

**Files:**
- Modify: `web/app.js` — `loadSpendData` (~line 3266), the `render529Tab` stub, new render helpers
- Modify: `web/styles.css` — chips/badges

**Interfaces:**
- Consumes: `/api/529/tags` (Task 4), `key` field on spend transactions (Task 3), DOM ids from Task 7.
- Produces (used by Tasks 9–11):
  - `state.spend.tags529` — object keyed by txn key → tag record
  - `CANDIDATE_529` predicate, `refresh529Tags()`, `render529Tab()` (renders summary + qualified list; queue table host filled in Task 9)
  - `saveTag529(key, status, qualifiedAmount)` helper that POSTs and refreshes

- [ ] **Step 1: Load tags alongside spend data**

Replace `loadSpendData` body so both requests run together:

```js
async function loadSpendData() {
  if (state.spend.loading) return;
  state.spend.loading = true;
  try {
    const { from, to } = rangeToDates(state.spend.range);
    const [payload, tagsPayload] = await Promise.all([
      apiGet(`/api/spend?from=${from}&to=${to}`),
      apiGet("/api/529/tags")
    ]);
    state.spend.transactions = Array.isArray(payload.transactions) ? payload.transactions : [];
    state.spend.tags529 = {};
    (Array.isArray(tagsPayload.tags) ? tagsPayload.tags : []).forEach((tag) => {
      state.spend.tags529[tag.key] = tag;
    });
    renderSpendAnalysis();
    if (state.spend.tab === "529") render529Tab();
  } catch (e) {
    showFlash(`Failed to load spend: ${e.message}`);
  } finally {
    state.spend.loading = false;
  }
}
```

- [ ] **Step 2: Add the candidate predicate and shared helpers**

```js
const CANDIDATE_529_GENERAL = new Set([
  "GENERAL_MERCHANDISE_OFFICE_SUPPLIES",
  "GENERAL_MERCHANDISE_BOOKSTORES_AND_NEWSSTANDS",
  "GENERAL_MERCHANDISE_SUPERSTORES",
  "GENERAL_MERCHANDISE_DEPARTMENT_STORES",
  "GENERAL_MERCHANDISE_DISCOUNT_STORES",
  "GENERAL_MERCHANDISE_OTHER_GENERAL_MERCHANDISE"
]);

function isCandidate529Category(category) {
  if (!category) return false;
  if (category === "FOOD_AND_DRINK_BEER_WINE_AND_LIQUOR") return false;
  if (category.startsWith("FOOD_AND_DRINK")) return true;
  return CANDIDATE_529_GENERAL.has(category);
}

function spendRangeBounds() {
  // Same range the /api/spend request used, as unix seconds for filtering
  // denormalized tag dates. "to" is bumped to end-of-day like the backend.
  const { from, to } = rangeToDates(state.spend.range);
  return {
    fromTs: Math.floor(new Date(`${from}T00:00:00Z`).getTime() / 1000),
    toTs: Math.floor(new Date(`${to}T00:00:00Z`).getTime() / 1000) + 86399
  };
}

async function refresh529Tags() {
  const tagsPayload = await apiGet("/api/529/tags");
  state.spend.tags529 = {};
  (Array.isArray(tagsPayload.tags) ? tagsPayload.tags : []).forEach((tag) => {
    state.spend.tags529[tag.key] = tag;
  });
  render529Tab();
}

async function saveTag529(key, status, qualifiedAmount) {
  const body = { key, status };
  if (status === "qualified" && qualifiedAmount != null) {
    body.qualified_amount = qualifiedAmount;
  }
  await apiPost("/api/529/tag", body);
  await refresh529Tags();
}
```

- [ ] **Step 3: Implement render529Tab (summary + qualified list)**

```js
function render529Tab() {
  const { fromTs, toTs } = spendRangeBounds();
  const txByKey = {};
  (state.spend.transactions || []).forEach((tx) => { txByKey[tx.key] = tx; });

  const qualified = Object.values(state.spend.tags529)
    .filter((tag) => tag.status === "qualified" && tag.date >= fromTs && tag.date <= toTs)
    .sort((a, b) => b.date - a.date);

  const total = qualified.reduce((sum, tag) => sum + (tag.qualified_amount || 0), 0);
  const missingReceipts = qualified.filter((tag) => (tag.receipts || []).length === 0).length;

  const totalLabel = document.getElementById("total529Label");
  if (totalLabel) totalLabel.textContent = `529 qualified total: ${currency(total)}`;
  const meta = document.getElementById("summary529Meta");
  if (meta) {
    const missingNote = missingReceipts > 0
      ? ` · ⚠ ${missingReceipts} missing receipt${missingReceipts === 1 ? "" : "s"}`
      : "";
    meta.textContent = `${qualified.length} qualified charge${qualified.length === 1 ? "" : "s"} in range${missingNote}`;
  }

  render529Queue(txByKey);           // Task 9
  render529QualifiedList(qualified, txByKey); // below
}

function render529QualifiedList(qualified, txByKey) {
  const host = document.getElementById("qualified529Table");
  if (!host) return;
  if (qualified.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem 0;">Nothing qualified in this range yet.</p>`;
    return;
  }
  const rows = qualified
    .map((tag) => {
      const orphaned = !txByKey[tag.key];
      const receipts = (tag.receipts || [])
        .map((name) =>
          `<a class="receipt-chip" target="_blank"
              href="${apiUrl(`/api/529/receipt?key=${encodeURIComponent(tag.key)}&filename=${encodeURIComponent(name)}`)}"
           >${escapeHtml(name)}</a>
           <button class="receipt-delete" type="button" data-key="${escapeHtml(tag.key)}"
                   data-filename="${escapeHtml(name)}" title="Delete receipt">×</button>`)
        .join(" ");
      const receiptCell = receipts ||
        `<span class="missing-receipt-badge">⚠ no receipt</span>`;
      return `<tr data-key="${escapeHtml(tag.key)}">
        <td>${dateLabel(tag.date)}</td>
        <td>${escapeHtml(tag.notes || "—")}${orphaned ? ` <span class="orphaned-badge" title="No longer in spend data">orphaned</span>` : ""}</td>
        <td class="num">${currency(tag.amount)}</td>
        <td class="num">
          <input class="qualified-amount-input num" type="number" step="0.01" min="0.01"
                 max="${tag.amount}" value="${(tag.qualified_amount || 0).toFixed(2)}"
                 data-key="${escapeHtml(tag.key)}" aria-label="Qualified amount" />
        </td>
        <td class="receipt-cell">${receiptCell}
          <button class="ghost-btn receipt-upload-btn" type="button" data-key="${escapeHtml(tag.key)}">＋ Receipt</button>
        </td>
        <td><button class="ghost-btn untag-529-btn" type="button" data-key="${escapeHtml(tag.key)}">Untag</button></td>
      </tr>`;
    })
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th class="num">Charge</th><th class="num">Qualified</th><th>Receipts</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  wire529QualifiedListEvents(host); // Task 9/10 attach handlers; define an empty stub now
}

function wire529QualifiedListEvents(host) {
  // handlers attached in Tasks 9 and 10
}

function render529Queue(txByKey) {
  // implemented in Task 9
}
```

- [ ] **Step 4: Add badge/chip styles**

Append to `web/styles.css`:

```css
.receipt-chip {
  display: inline-block;
  padding: 0.15rem 0.5rem;
  border-radius: 999px;
  background: rgba(34, 34, 34, 0.08);
  font-size: 0.8rem;
  text-decoration: none;
}

.receipt-delete {
  border: none;
  background: transparent;
  cursor: pointer;
  opacity: 0.6;
}

.missing-receipt-badge,
.orphaned-badge {
  font-size: 0.75rem;
  padding: 0.1rem 0.45rem;
  border-radius: 999px;
  background: rgba(214, 69, 65, 0.12);
  color: #b3372f;
}

.qualified-amount-input {
  width: 6.5rem;
  font: inherit;
  text-align: right;
}
```

- [ ] **Step 5: Verify in browser**

Qualify one charge via curl (as in Task 4 Step 5), reload the site, open Spending → 529 Expenses. Expected: total shows the qualified amount, count reads "1 qualified charge in range", the row renders with a "⚠ no receipt" badge, an amount input, and (non-functional, until Task 9/10) buttons. Switch ranges — a range that excludes the charge's date zeroes the total. Clean up the curl tag afterwards.

- [ ] **Step 6: Commit**

```bash
git add web/app.js web/styles.css
git commit -m "Render 529 summary and qualified list on Spending page

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Review queue + qualify dialog + tag actions

**Files:**
- Modify: `web/app.js` — implement `render529Queue`, the qualify dialog controller, `wire529QualifiedListEvents` (amount edit + untag), drilldown button

**Interfaces:**
- Consumes: everything from Task 8; dialog DOM from Task 7.
- Produces (used by Task 10): `openQualify529Dialog(tx)` where `tx` is a spend transaction object `{key, date, amount, notes, category}`; pending-files handling hook `qualify529PendingFiles` (array set by the dialog's file input).

- [ ] **Step 1: Implement the review queue**

```js
function render529Queue(txByKey) {
  const host = document.getElementById("review529Table");
  if (!host) return;
  const queue = (state.spend.transactions || [])
    .filter((tx) =>
      tx.account_type === "DEBT" &&
      isCandidate529Category(tx.category) &&
      !state.spend.tags529[tx.key])
    .sort((a, b) => (b.date || 0) - (a.date || 0));

  if (queue.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem 0;">Queue is clear — nothing to review in this range.</p>`;
    return;
  }
  const rows = queue
    .map((tx) => `<tr data-key="${escapeHtml(tx.key)}">
      <td>${dateLabel(tx.date)}</td>
      <td>${escapeHtml(tx.notes || "—")}</td>
      <td><span class="chip chip-category ${categoryChipClass(categoryPrimary(tx.category) || "OTHER")}">${escapeHtml(spendCategoryLabel(categoryPrimary(tx.category) || "OTHER"))}</span></td>
      <td class="num">${currency(tx.amount)}</td>
      <td>
        <button class="ghost-btn qualify-529-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Qualify</button>
        <button class="ghost-btn dismiss-529-btn" type="button" data-key="${escapeHtml(tx.key)}">✕ Dismiss</button>
      </td>
    </tr>`)
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th>Category</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;

  host.querySelectorAll(".qualify-529-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) openQualify529Dialog(tx);
    });
  });
  host.querySelectorAll(".dismiss-529-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTag529(btn.dataset.key, "dismissed", null);
      } catch (e) {
        showFlash(`Dismiss failed: ${e.message}`);
      }
    });
  });
}
```

(Uses existing helpers `categoryPrimary`, `categoryChipClass`, `spendCategoryLabel` — same trio the Analysis tables use.)

- [ ] **Step 2: Implement the qualify dialog controller**

```js
let qualify529Target = null;

function openQualify529Dialog(tx) {
  qualify529Target = tx;
  const dialog = document.getElementById("qualify529Dialog");
  const merchant = document.getElementById("qualify529Merchant");
  const amount = document.getElementById("qualify529Amount");
  const files = document.getElementById("qualify529Files");
  if (!dialog || !amount) return;
  if (merchant) merchant.textContent = `${tx.notes || "—"} · ${dateLabel(tx.date)} · ${currency(tx.amount)}`;
  amount.value = tx.amount.toFixed(2);
  amount.max = tx.amount;
  if (files) files.value = "";
  dialog.showModal();
}

function wireQualify529Dialog() {
  const dialog = document.getElementById("qualify529Dialog");
  const cancel = document.getElementById("qualify529Cancel");
  const save = document.getElementById("qualify529Save");
  if (cancel) cancel.addEventListener("click", () => dialog.close());
  if (save) {
    save.addEventListener("click", async () => {
      if (!qualify529Target) return;
      const amountInput = document.getElementById("qualify529Amount");
      const filesInput = document.getElementById("qualify529Files");
      const qualifiedAmount = Number.parseFloat(amountInput.value);
      if (!Number.isFinite(qualifiedAmount) || qualifiedAmount <= 0 ||
          qualifiedAmount > qualify529Target.amount + 0.005) {
        showFlash("Qualified amount must be between $0.01 and the charge amount.");
        return;
      }
      save.disabled = true;
      try {
        await saveTag529(qualify529Target.key, "qualified", qualifiedAmount);
        const files = filesInput ? Array.from(filesInput.files) : [];
        for (const file of files) {
          await apiUploadReceipt(qualify529Target.key, file); // Task 10
        }
        if (files.length > 0) await refresh529Tags();
        dialog.close();
      } catch (e) {
        showFlash(`Qualify failed: ${e.message}`);
      } finally {
        save.disabled = false;
      }
    });
  }
}
```

Call `wireQualify529Dialog()` from the same init block where the tab buttons were wired (Task 7 Step 4). Until Task 10 lands, add a temporary `async function apiUploadReceipt() {}` stub so qualifying without files works.

- [ ] **Step 3: Wire qualified-list events (amount edit + untag)**

Replace the `wire529QualifiedListEvents` stub:

```js
function wire529QualifiedListEvents(host) {
  host.querySelectorAll(".qualified-amount-input").forEach((input) => {
    input.addEventListener("change", async () => {
      const value = Number.parseFloat(input.value);
      const tag = state.spend.tags529[input.dataset.key];
      if (!tag) return;
      if (!Number.isFinite(value) || value <= 0 || value > tag.amount + 0.005) {
        showFlash("Qualified amount must be between $0.01 and the charge amount.");
        input.value = (tag.qualified_amount || 0).toFixed(2);
        return;
      }
      try {
        await saveTag529(input.dataset.key, "qualified", value);
      } catch (e) {
        showFlash(`Update failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll(".untag-529-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTag529(btn.dataset.key, "none", null);
      } catch (e) {
        showFlash(`Untag failed: ${e.message}`);
      }
    });
  });
  // receipt upload/delete handlers attach in Task 10
}
```

- [ ] **Step 4: Add the 529 button to the Analysis drilldown**

In `renderSpendDrilldownTable` (~line 3195), add a cell per row (and matching `<th></th>` in the header):

```js
        <td><button class="ghost-btn drilldown-529-btn" type="button" data-key="${escapeHtml(tx.key)}">529</button></td>
```

In `openSpendCategoryDrilldown` (~line 3216), after setting `innerHTML`, wire the buttons:

```js
  el.transactionsHistory.querySelectorAll(".drilldown-529-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) {
        el.transactionsDialog.close();
        setSpendTab("529");
        openQualify529Dialog(tx);
      }
    });
  });
```

- [ ] **Step 5: Verify in browser**

Bump `?v=` on app.js again if needed, hard-reload. Expected flow: review queue lists DEBT charges in candidate categories; Dismiss removes a row; Qualify opens the dialog with the full amount prefilled, Save moves the charge to the qualified list and updates the total; editing the amount inline persists (reload to confirm); Untag returns it to the queue; the Analysis drilldown rows show a working 529 button. Verify a dismissed charge stays out of the queue after reload.

- [ ] **Step 6: Commit**

```bash
git add web/app.js
git commit -m "Add 529 review queue, qualify dialog, and tag actions

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: Receipt upload UI

**Files:**
- Modify: `web/app.js` — real `apiUploadReceipt`, per-row upload, receipt delete, drag-and-drop
- Modify: `web/styles.css` — drop-target highlight

**Interfaces:**
- Consumes: `POST/DELETE /api/529/receipt` (Task 5), row markup from Task 8.
- Produces: fully working receipt attach/view/delete from both the qualify dialog and qualified-list rows.

- [ ] **Step 1: Implement the upload helper**

Replace the Task 9 stub, next to `apiPost` (~line 844):

```js
async function apiUploadReceipt(key, file) {
  beginRequest();
  try {
    const query = `key=${encodeURIComponent(key)}&filename=${encodeURIComponent(file.name)}`;
    const response = await fetch(apiUrl(`/api/529/receipt?${query}`), {
      method: "POST",
      headers: { "Content-Type": file.type || "application/octet-stream" },
      body: file
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Upload failed: ${response.status}`);
    }
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      throw new Error("You're offline — receipts can't upload until you reconnect.");
    }
    throw error;
  } finally {
    endRequest();
  }
}
```

- [ ] **Step 2: Per-row upload button + hidden file input**

Add one hidden input to the 529 pane markup in `web/index.html` (inside `#spend529Pane`, after the qualified panel):

```html
          <input id="receipt529FileInput" type="file" accept="image/*,.pdf,.heic" multiple hidden />
```

In `wire529QualifiedListEvents`, add:

```js
  host.querySelectorAll(".receipt-upload-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const fileInput = document.getElementById("receipt529FileInput");
      if (!fileInput) return;
      fileInput.dataset.key = btn.dataset.key;
      fileInput.value = "";
      fileInput.click();
    });
  });
  host.querySelectorAll(".receipt-delete").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await apiDelete(`/api/529/receipt?key=${encodeURIComponent(btn.dataset.key)}&filename=${encodeURIComponent(btn.dataset.filename)}`);
        await refresh529Tags();
      } catch (e) {
        showFlash(`Delete failed: ${e.message}`);
      }
    });
  });
```

Wire the shared input once in the init block:

```js
  const receiptInput = document.getElementById("receipt529FileInput");
  if (receiptInput) {
    receiptInput.addEventListener("change", async () => {
      const key = receiptInput.dataset.key;
      const files = Array.from(receiptInput.files);
      if (!key || files.length === 0) return;
      try {
        for (const file of files) {
          await apiUploadReceipt(key, file);
        }
        await refresh529Tags();
      } catch (e) {
        showFlash(`Upload failed: ${e.message}`);
      }
    });
  }
```

Note on mobile capture: `accept="image/*"` on iOS Safari offers camera + photo library from the same picker, which covers the phone flow without a `capture` attribute (which would force camera-only and block picking existing photos).

- [ ] **Step 3: Drag-and-drop onto qualified rows**

In `wire529QualifiedListEvents`, add:

```js
  host.querySelectorAll("tr[data-key]").forEach((row) => {
    row.addEventListener("dragover", (event) => {
      event.preventDefault();
      row.classList.add("receipt-drop-active");
    });
    row.addEventListener("dragleave", () => row.classList.remove("receipt-drop-active"));
    row.addEventListener("drop", async (event) => {
      event.preventDefault();
      row.classList.remove("receipt-drop-active");
      const files = Array.from(event.dataTransfer?.files || []);
      if (files.length === 0) return;
      try {
        for (const file of files) {
          await apiUploadReceipt(row.dataset.key, file);
        }
        await refresh529Tags();
      } catch (e) {
        showFlash(`Upload failed: ${e.message}`);
      }
    });
  });
```

Style:

```css
.receipt-drop-active {
  outline: 2px dashed #b3372f;
  outline-offset: -2px;
}
```

- [ ] **Step 4: Verify in browser**

Desktop: qualify a charge, upload a PNG via the ＋ Receipt button — chip appears, badge clears; click the chip — opens the image in a new tab; drag an image file onto the row — uploads; delete via × — badge returns. Reload — receipts persist. Try a `.txt` file — clean error flash, nothing attached. Phone (same LAN): open the site, ＋ Receipt offers camera/library, a photo uploads and renders.

- [ ] **Step 5: Commit**

```bash
git add web/app.js web/index.html web/styles.css
git commit -m "Add receipt upload, view, delete, and drag-drop UI

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: Export buttons

**Files:**
- Modify: `web/app.js` — export handlers in the init block

**Interfaces:**
- Consumes: export endpoints (Task 6), buttons from Task 7.

- [ ] **Step 1: Implement download helper + wiring**

```js
async function download529Export(kind) {
  const { from, to } = rangeToDates(state.spend.range);
  beginRequest();
  try {
    const response = await fetch(apiUrl(`/api/529/export.${kind}?from=${from}&to=${to}`));
    if (!response.ok) {
      let message = `Export failed: ${response.status}`;
      try {
        message = (await response.json()).error || message;
      } catch (_) { /* non-JSON error body */ }
      throw new Error(message);
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `529-expenses_${from}_to_${to}.${kind}`;
    link.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    showFlash(e.message);
  } finally {
    endRequest();
  }
}
```

In the init block:

```js
  const exportCsvBtn = document.getElementById("export529CsvBtn");
  const exportZipBtn = document.getElementById("export529ZipBtn");
  if (exportCsvBtn) exportCsvBtn.addEventListener("click", () => download529Export("csv"));
  if (exportZipBtn) exportZipBtn.addEventListener("click", () => download529Export("zip"));
```

- [ ] **Step 2: Verify in browser**

With at least one qualified charge + receipt in range: Export CSV downloads `529-expenses_<from>_to_<to>.csv` with correct rows; Export ZIP downloads a zip whose contents match Task 6's curl check. With a range containing nothing, CSV has only the header row and ZIP contains only `expenses.csv`.

- [ ] **Step 3: Commit**

```bash
git add web/app.js
git commit -m "Add 529 CSV and ZIP export buttons

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: Full verification pass + deploy

**Files:** none new — verification only.

- [ ] **Step 1: Full rebuild and all tests**

```bash
make clean && make && make test-529 && make test-persistence
```

Expected: clean build, both test binaries pass.

- [ ] **Step 2: Deploy and smoke-test the API**

```bash
pm2 restart 3
sleep 2
curl -s http://localhost:8080/api/health
curl -s "http://localhost:8080/api/spend?from=2026-08-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); assert all('key' in t for t in d['transactions']); print('keys ok', len(d['transactions']))"
curl -s http://localhost:8080/api/529/tags
```

- [ ] **Step 3: End-to-end browser walkthrough**

Desktop: Dashboard → Spending → both tabs render → qualify a real grocery charge with a partial amount and a receipt → total updates → drilldown 529 button works → exports download → untag/cleanup anything you tagged purely for testing (keep real ones).

Phone: open the site, Spending → 529 Expenses → qualify a charge and attach a camera photo.

Offline sanity: with the backend stopped briefly (`pm2 stop 3` then `pm2 start 3`), the Spending page should show the flash error, not a broken pane.

- [ ] **Step 4: Verify no regressions in Analysis tab**

Charts, category table, drilldown, custom ranges — all behave exactly as before the change.

- [ ] **Step 5: Final commit (if any fixups) and report**

Report to the user: what was built, where the data lives (`data/529/`), and that `data/529/` should be included in any backup routine.

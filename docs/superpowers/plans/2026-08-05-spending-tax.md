# Spending & Tax Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mark cash-account deposits as taxable income and expenses as tax-deductible (with receipts), totaled per tax year on a new Tax sub-tab of the renamed "Spending & Tax" page.

**Architecture:** Parallel sidecar stores (`data/tax/income.json`, `data/tax/deductions.json`) reusing `ExpenseTags::TagRecord`/`loadTags`/`saveTags` unchanged; a new `/api/income` collection mirroring `/api/spend`; the 529 receipt and tag-upsert route logic extracted into store-parameterized helpers shared by `/api/529/*` and `/api/tax/*`; frontend gains a third "Tax" tab with its own tax-year scope and a generalized mark dialog.

**Tech Stack:** C++17 (g++, no new libs), hand-rolled HTTP server in `src/web_server.cpp`, vanilla JS in `web/`, standalone test binaries.

**Spec:** `docs/superpowers/specs/2026-08-05-spending-tax-design.md` — read it before starting.

## Global Constraints

- C++17 only, no new libraries. 4-space indent, Allman braces. Vanilla JS, 2-space indent, double quotes.
- The Makefile has NO header dependency tracking: `make clean && make` after any `include/*.hpp` change (none are planned; plain `make` otherwise).
- Deploy = `make && pm2 restart 3` (backend API on :8080). Frontend is served separately (pm2 id 2, :5173); static files re-read per request, but cache-busting requires bumping BOTH `?v=` in index.html AND `CACHE_NAME` in sw.js.
- `expense_tags` module schema must NOT change. `data/529/*` must NOT be touched.
- Existing 529 behavior must not regress — every task that touches shared code re-verifies a 529 flow.
- Receipt rules (tax receipts identical to 529): 25 MB cap, extension allowlist jpg/jpeg/png/heic/webp/pdf, sanitized filenames, dedup suffixes, atomic writes, orphan cleanup on failed saveTags.
- All tax-store mutations under a new `g_tax_mutex` (mirror of `g_529_mutex`).
- CSV escaping via existing `csvField` (includes formula-injection guard).
- Element IDs never renamed; only user-visible copy changes ("Spending" → "Spending & Tax").
- Commit after every task; messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- Verification curls that create tags/receipts must clean up completely (remove tags with status "none", delete receipt files/dirs, delete temp files).

## Current-code anchors (post-529 merge, `src/web_server.cpp`)

- `struct SpendTxn` :3267, `collectSpendTransactions` :3278, `buildSpendJson` :3342
- `kTagsFile`/`kReceiptsDir` :3367-3368, `g_529_mutex` :3372, `g_529_export_mutex` :3376
- `serializeTagRecord` :3378, `ensure529Dirs` :3402, `csvField` :3410, `qualifiedTagsInRange` :3433, `build529Csv` :3451
- Routes: `/api/spend` :4274, `/api/529/tags` :4314, `/api/529/tag` :4333, `/api/529/receipt` :4444, `/api/529/export.csv` :4596, `/api/529/export.zip` :4647

Frontend (`web/app.js`): `apiUploadReceipt` :874, `loadSpendData` :3304, `setSpendTab` :3327, `spendRangeBounds` :3367, `refresh529Tags` :3377, `download529Export` :3390, `saveTag529(key,status,amount,refresh=true)` :3416, `render529Tab` :3425, `render529QualifiedList` :3451, `wire529QualifiedListEvents` :3494, `render529Queue` :3564, `qualify529Target`/`openQualify529Dialog`/`wireQualify529Dialog` :3613-3674, `showSpendAnalysis` :3676 (breadcrumb "Spending" at :3684).

Line numbers drift as tasks land — search for the names.

---

### Task 1: Income collection + GET /api/income

**Files:**
- Modify: `src/web_server.cpp`

**Interfaces:**
- Consumes: `SpendTxn`, `ExpenseTags::KeyAssigner`, `loadPortfolioCached`, `notesIsPending`, `notesStartsWithTxfr`, `parseIsoDateUTC` parsing pattern from the `/api/spend` route.
- Produces (Tasks 3, 5, and frontend rely on these):
  - `std::vector<SpendTxn> collectIncomeTransactions(PortfolioManager& manager, time_t from, time_t to)` (anonymous namespace, placed directly after `buildSpendJson`)
  - `std::string buildIncomeJson(PortfolioManager& manager, time_t from, time_t to)`
  - Route `GET /api/income?from=&to=` → same envelope as `/api/spend`: `{"from":..,"to":..,"transactions":[{key,date,amount,category,notes,account,account_type}...]}`

- [ ] **Step 1: Implement the collector and serializer**

Place after `buildSpendJson` (~line 3360):

```cpp
    // Income-side mirror of collectSpendTransactions: DEPOSIT and INTEREST
    // transactions on CASH portfolios, keyed over the FULL history before the
    // date filter so occurrence indices are range-stable. Income keys are a
    // separate namespace from spend keys (separate endpoint + tag files), so
    // a same-tuple deposit/withdrawal collision is harmless.
    std::vector<SpendTxn> collectIncomeTransactions(PortfolioManager& manager, time_t from, time_t to)
    {
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
            if (pt != PortfolioType::CASH) continue;

            for (const Transaction& tx : portfolio.getTransactions())
            {
                if (tx.type != TransactionType::DEPOSIT &&
                    tx.type != TransactionType::INTEREST) continue;
                if (notesIsPending(tx.notes)) continue;
                if (notesStartsWithTxfr(tx.notes)) continue;

                const std::string key = key_assigner.next(name, tx.date, tx.amount, tx.notes);

                if (tx.date < from || tx.date > to) continue;

                SpendTxn income_tx;
                income_tx.key = key;
                income_tx.date = tx.date;
                income_tx.amount = std::abs(tx.amount);
                income_tx.category = tx.category;
                income_tx.notes = tx.notes;
                income_tx.account = name;
                income_tx.account_type = portfolioTypeToString(pt);
                result.push_back(income_tx);
            }
        }
        return result;
    }

    std::string buildIncomeJson(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendTxn> txs = collectIncomeTransactions(manager, from, to);
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

Note: unlike spend, no `spend_overrides` category rewriting and no TRANSFER_IN/LOAN category exclusion — every non-pending, non-own-transfer deposit is a candidate (peer-transfer income must be markable).

- [ ] **Step 2: Extract shared range parsing and add the route**

The from/to parsing block currently appears verbatim in `/api/spend`, `/api/529/export.csv`, and `/api/529/export.zip` (a deferred-minor from the 529 review). Extract it once into the anonymous namespace (near `parseQuery` usage helpers):

```cpp
    // Parses optional from/to (YYYY-MM-DD) query params with the /api/spend
    // defaults: from = 365 days ago, to = now; "to" bumped to end-of-day.
    // Returns a 400 response on malformed values, else nullopt.
    std::optional<HttpResponse> parseRangeParams(const std::map<std::string, std::string>& query_values,
                                                 time_t& from, time_t& to)
    {
        const time_t now = std::time(nullptr);
        from = now - (365LL * 86400);
        to = now;
        auto from_it = query_values.find("from");
        if (from_it != query_values.end() && !from_it->second.empty())
        {
            from = parseIsoDateUTC(from_it->second);
            if (from == 0)
            {
                return makeJsonResponse(400, makeErrorBody("from must be YYYY-MM-DD"));
            }
        }
        auto to_it = query_values.find("to");
        if (to_it != query_values.end() && !to_it->second.empty())
        {
            to = parseIsoDateUTC(to_it->second);
            if (to == 0)
            {
                return makeJsonResponse(400, makeErrorBody("to must be YYYY-MM-DD"));
            }
            to += 86399; // end-of-day inclusive
        }
        return std::nullopt;
    }
```

Rewrite the `/api/spend`, `/api/529/export.csv`, and `/api/529/export.zip` routes' parsing to use it (behavior identical — same defaults, same error strings), then add the new route after `/api/spend`:

```cpp
        if (request.method == "GET" && request.path == "/api/income")
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            auto range_error = parseRangeParams(query_values, from, to);
            if (range_error.has_value())
            {
                return range_error.value();
            }
            return makeJsonResponse(200, buildIncomeJson(manager, from, to));
        }
```

After the rewrite, curl-verify the three refactored routes still behave: `/api/spend` with no params (365-day default), with a bad date (400 "from must be YYYY-MM-DD"), and `/api/529/export.csv?from=2026-01-01&to=2026-08-05` (200).

- [ ] **Step 3: Build, deploy, verify**

Run: `make && pm2 restart 3`, wait 2s. Then:

```bash
curl -s "http://localhost:8080/api/income?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d['transactions'])); print(json.dumps(d['transactions'][:2], indent=1))"
# Expected: deposit transactions with keys; CASH accounts only; amounts positive.

# Range stability: narrow keys ⊆ wide keys, no dupes
curl -s "http://localhost:8080/api/income?from=2026-07-01&to=2026-08-05" > /tmp/inc_narrow.json
curl -s "http://localhost:8080/api/income?from=2026-01-01&to=2026-08-05" > /tmp/inc_wide.json
python3 -c "
import json
n={t['key'] for t in json.load(open('/tmp/inc_narrow.json'))['transactions']}
wl=[t['key'] for t in json.load(open('/tmp/inc_wide.json'))['transactions']]
w=set(wl)
assert n <= w, 'narrow keys missing from wide'
assert len(wl)==len(w), 'duplicate keys'
print('income keys stable:', len(n), 'narrow /', len(w), 'wide')"
rm /tmp/inc_narrow.json /tmp/inc_wide.json

# 529 regression: /api/spend unchanged
curl -s "http://localhost:8080/api/spend?from=2026-08-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); assert all('key' in t for t in d['transactions']); print('spend ok', len(d['transactions']))"
```

- [ ] **Step 4: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add income transaction collection and GET /api/income

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Extract the receipt route into a store-parameterized helper

**Files:**
- Modify: `src/web_server.cpp` — the `/api/529/receipt` block (:4444-4594) and a new helper above the route dispatch

**Interfaces:**
- Consumes: the existing route body.
- Produces (Task 4 relies on this):
  - `HttpResponse handleReceiptRoute(const HttpRequest& request, const char* tags_file, const char* receipts_dir, std::mutex& store_mutex)` in the anonymous namespace, placed right after `build529Csv`.

- [ ] **Step 1: Extract**

Move the ENTIRE body of the current `if (request.path == "/api/529/receipt")` block into the new function, changing only:
- `kTagsFile` → `tags_file` (3 loadTags + 3 saveTags call sites)
- `kReceiptsDir` → `receipts_dir` (the `receipt_dir` construction)
- `g_529_mutex` → `store_mutex` (the two `std::lock_guard` declarations)

Everything else — key charset validation, sanitization, allowlist, 25 MB cap, qualified-status requirement, dedup loop, atomic write, orphan cleanup, DELETE 404 semantics, 405 fallthrough — is moved verbatim, not rewritten.

The route becomes:

```cpp
        if (request.path == "/api/529/receipt")
        {
            return handleReceiptRoute(request, kTagsFile, kReceiptsDir, g_529_mutex);
        }
```

- [ ] **Step 2: Build, deploy, regression-verify the 529 receipt cycle**

Run: `make && pm2 restart 3`, wait 2s. Then the standard cycle:

```bash
KEY=$(curl -s "http://localhost:8080/api/spend?from=2026-06-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"qualified\"}" http://localhost:8080/api/529/tag > /dev/null
printf '\x89PNG\r\n\x1a\n' > /tmp/t2-receipt.png
curl -s -X POST --data-binary @/tmp/t2-receipt.png -H "Content-Type: image/png" "http://localhost:8080/api/529/receipt?key=$KEY&filename=t2.png"
# Expected: {"filename":"t2.png"} (201)
curl -s "http://localhost:8080/api/529/receipt?key=$KEY&filename=t2.png" | cmp - /tmp/t2-receipt.png && echo BYTES-OK
curl -s -X DELETE "http://localhost:8080/api/529/receipt?key=$KEY&filename=t2.png"
# Expected: {"removed":true}
curl -s -X DELETE "http://localhost:8080/api/529/receipt?key=$KEY&filename=nope.png"
# Expected: 404 Receipt not found
# Cleanup:
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$KEY\",\"status\":\"none\"}" http://localhost:8080/api/529/tag > /dev/null
rm -rf "data/529/receipts/$KEY" /tmp/t2-receipt.png
```

- [ ] **Step 3: Commit**

```bash
git add src/web_server.cpp
git commit -m "Extract receipt route into store-parameterized helper

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Tax tag stores + GET /api/tax/tags + POST /api/tax/tag

**Files:**
- Modify: `src/web_server.cpp`

**Interfaces:**
- Consumes: `collectSpendTransactions`, `collectIncomeTransactions` (Task 1), `serializeTagRecord`, `ExpenseTags::loadTags/saveTags`.
- Produces (Tasks 4-5 and frontend rely on these):
  - Constants + mutex (anonymous namespace, next to the 529 ones):
    ```cpp
    const char* kTaxIncomeTagsFile = "data/tax/income.json";
    const char* kTaxDeductionTagsFile = "data/tax/deductions.json";
    const char* kTaxReceiptsDir = "data/tax/receipts";
    std::mutex g_tax_mutex;
    bool ensureTaxDirs();  // creates data/tax and data/tax/receipts, same shape as ensure529Dirs
    ```
  - `HttpResponse applyTagUpsert(const std::string& key, const std::string& status, std::optional<double> amount_opt, const char* tags_file, std::mutex& store_mutex, bool (*ensure_dirs)(), PortfolioManager& manager, std::vector<SpendTxn> (*collect)(PortfolioManager&, time_t, time_t))` — shared upsert used by BOTH `/api/529/tag` and `/api/tax/tag`.
  - Routes: `GET /api/tax/tags` → `{"income":[...],"deductions":[...]}`; `POST /api/tax/tag` body `{kind, key, status, amount?}` → `{"tag":{...}}` / `{"removed":true}`.

- [ ] **Step 1: Add constants, mutex, ensureTaxDirs**

`ensureTaxDirs` is `ensure529Dirs` with `"data/tax"` and `kTaxReceiptsDir`.

- [ ] **Step 2: Extract applyTagUpsert from the 529 tag route**

The current `/api/529/tag` POST body (after JSON parsing/validation of key+status) performs: lock → ensure dirs → load → find existing → status "none" erase/save → else build record (denormalized lookup via `collectSpendTransactions(manager, 0, now + 86400)` on miss → 404) → amount default/validate (`> 0`, `<= record.amount + 0.005`, else 400 "qualified_amount must be > 0 and <= the charge amount") → dismissed zeroes amount → save → `{"tag":...}`.

Extract exactly that into:

```cpp
    HttpResponse applyTagUpsert(const std::string& key, const std::string& status,
                                std::optional<double> amount_opt,
                                const char* tags_file, std::mutex& store_mutex,
                                bool (*ensure_dirs)(), PortfolioManager& manager,
                                std::vector<SpendTxn> (*collect)(PortfolioManager&, time_t, time_t))
```

with `kTagsFile→tags_file`, `g_529_mutex→store_mutex`, `ensure529Dirs→ensure_dirs`, `collectSpendTransactions→collect`. The `/api/529/tag` route keeps its own body-parsing (key/status/qualified_amount extraction and the status-value 400) and ends with:

```cpp
            return applyTagUpsert(key, status, raw_amount, kTagsFile, g_529_mutex,
                                  ensure529Dirs, manager, collectSpendTransactions);
```

(`raw_amount` is the `std::optional<double>` from `getObjectNumber(body, "qualified_amount")`.)

- [ ] **Step 3: Add the tax routes**

After the `/api/529/tag` route block:

```cpp
        if (request.method == "GET" && request.path == "/api/tax/tags")
        {
            std::lock_guard<std::mutex> lock(g_tax_mutex);
            std::vector<ExpenseTags::TagRecord> income_tags;
            std::vector<ExpenseTags::TagRecord> deduction_tags;
            if (!ExpenseTags::loadTags(kTaxIncomeTagsFile, income_tags) ||
                !ExpenseTags::loadTags(kTaxDeductionTagsFile, deduction_tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read tax tags"));
            }
            std::ostringstream out;
            out << "{\"income\":[";
            for (size_t i = 0; i < income_tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(income_tags[i]);
            }
            out << "],\"deductions\":[";
            for (size_t i = 0; i < deduction_tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(deduction_tags[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/tax/tag")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }
            const auto raw_kind = getObjectString(body, "kind");
            const auto raw_key = getObjectString(body, "key");
            const auto raw_status = getObjectString(body, "status");
            if (!raw_kind.has_value() || !raw_key.has_value() || !raw_status.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("kind, key, and status are required"));
            }
            const std::string kind = trim(raw_kind.value());
            const std::string key = trim(raw_key.value());
            const std::string status = trim(raw_status.value());
            if (kind != "income" && kind != "deduction")
            {
                return makeJsonResponse(400, makeErrorBody("kind must be income or deduction"));
            }
            if (status != "qualified" && status != "dismissed" && status != "none")
            {
                return makeJsonResponse(400, makeErrorBody("status must be qualified, dismissed, or none"));
            }
            const auto raw_amount = getObjectNumber(body, "amount");
            const char* tags_file = (kind == "income") ? kTaxIncomeTagsFile : kTaxDeductionTagsFile;
            auto collect = (kind == "income") ? collectIncomeTransactions : collectSpendTransactions;
            return applyTagUpsert(key, status, raw_amount, tags_file, g_tax_mutex,
                                  ensureTaxDirs, manager, collect);
        }
```

- [ ] **Step 4: Build, deploy, verify both kinds + 529 regression**

`make && pm2 restart 3`, wait 2s. Then:

```bash
# Income mark:
IKEY=$(curl -s "http://localhost:8080/api/income?from=2026-01-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"income\",\"key\":\"$IKEY\",\"status\":\"qualified\",\"amount\":1.00}" http://localhost:8080/api/tax/tag
# Expected: {"tag":{...,"qualified_amount":1,...}}

# Deduction mark on a spend key:
DKEY=$(curl -s "http://localhost:8080/api/spend?from=2026-06-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"deduction\",\"key\":\"$DKEY\",\"status\":\"qualified\"}" http://localhost:8080/api/tax/tag
# Expected: tag with full amount

curl -s http://localhost:8080/api/tax/tags
# Expected: one record in each array; verify data/tax/income.json + deductions.json exist

# Bad kind, bad key, over-amount:
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"x\",\"key\":\"$IKEY\",\"status\":\"qualified\"}" http://localhost:8080/api/tax/tag   # 400
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"income\",\"key\":\"ffff-0\",\"status\":\"qualified\"}" http://localhost:8080/api/tax/tag  # 404
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"income\",\"key\":\"$IKEY\",\"status\":\"qualified\",\"amount\":9999999}" http://localhost:8080/api/tax/tag  # 400

# Income key must NOT resolve as a deduction (separate lookup):
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"deduction\",\"key\":\"$IKEY\",\"status\":\"qualified\"}" http://localhost:8080/api/tax/tag
# Expected: 404 (unless a same-tuple withdrawal coincidentally exists — note result in report)

# 529 regression: qualify + unqualify a 529 tag still works
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$DKEY\",\"status\":\"qualified\"}" http://localhost:8080/api/529/tag > /dev/null
curl -s -X POST -H "Content-Type: application/json" -d "{\"key\":\"$DKEY\",\"status\":\"none\"}" http://localhost:8080/api/529/tag
# Expected: {"removed":true}

# Cleanup tax tags:
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"income\",\"key\":\"$IKEY\",\"status\":\"none\"}" http://localhost:8080/api/tax/tag > /dev/null
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"deduction\",\"key\":\"$DKEY\",\"status\":\"none\"}" http://localhost:8080/api/tax/tag > /dev/null
curl -s http://localhost:8080/api/tax/tags
# Expected: {"income":[],"deductions":[]}
```

- [ ] **Step 5: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add tax tag stores and endpoints via shared upsert helper

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Tax receipt routes

**Files:**
- Modify: `src/web_server.cpp` — one new route after `/api/529/receipt`

**Interfaces:**
- Consumes: `handleReceiptRoute` (Task 2), tax constants (Task 3).
- Produces: `POST/GET/DELETE /api/tax/receipt?key=&filename=` operating on `kTaxDeductionTagsFile` + `kTaxReceiptsDir` under `g_tax_mutex`. (Income records never get receipts because uploads require a qualified record in the DEDUCTIONS file — the helper enforces this by construction.)

- [ ] **Step 1: Add the route**

```cpp
        if (request.path == "/api/tax/receipt")
        {
            // Deduction receipts only: the store handed to the helper is the
            // deductions file, so income marks can never accept uploads.
            return handleReceiptRoute(request, kTaxDeductionTagsFile, kTaxReceiptsDir, g_tax_mutex);
        }
```

- [ ] **Step 2: Build, deploy, verify**

`make && pm2 restart 3`, wait 2s. Then:

```bash
DKEY=$(curl -s "http://localhost:8080/api/spend?from=2026-06-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"deduction\",\"key\":\"$DKEY\",\"status\":\"qualified\"}" http://localhost:8080/api/tax/tag > /dev/null
printf '\x89PNG\r\n\x1a\n' > /tmp/t4-receipt.png
curl -s -X POST --data-binary @/tmp/t4-receipt.png -H "Content-Type: image/png" "http://localhost:8080/api/tax/receipt?key=$DKEY&filename=donation.png"
# Expected: {"filename":"donation.png"}; file at data/tax/receipts/$DKEY/donation.png; receipts array updated in data/tax/deductions.json
curl -s "http://localhost:8080/api/tax/receipt?key=$DKEY&filename=donation.png" | cmp - /tmp/t4-receipt.png && echo BYTES-OK
# Upload to an income-marked key must 404 (no record in deductions file):
IKEY=$(curl -s "http://localhost:8080/api/income?from=2026-01-01&to=2026-08-05" | python3 -c "import json,sys; print(json.load(sys.stdin)['transactions'][0]['key'])")
curl -s -X POST --data-binary @/tmp/t4-receipt.png -H "Content-Type: image/png" "http://localhost:8080/api/tax/receipt?key=$IKEY&filename=x.png"
# Expected: 404 "Qualify the charge before attaching receipts"
# Cleanup:
curl -s -X DELETE "http://localhost:8080/api/tax/receipt?key=$DKEY&filename=donation.png" > /dev/null
curl -s -X POST -H "Content-Type: application/json" -d "{\"kind\":\"deduction\",\"key\":\"$DKEY\",\"status\":\"none\"}" http://localhost:8080/api/tax/tag > /dev/null
rm -rf "data/tax/receipts/$DKEY" /tmp/t4-receipt.png
```

- [ ] **Step 3: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add tax receipt routes reusing the parameterized handler

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Year-scoped CSV exports

**Files:**
- Modify: `src/web_server.cpp`

**Interfaces:**
- Consumes: `csvField`, `qualifiedTagsInRange`, `build529Csv`, tax constants.
- Produces:
  - `qualifiedTagsInRange` gains a leading `const char* tags_file` parameter; the two existing 529 call sites pass `kTagsFile`.
  - `std::string buildTagCsv(const std::vector<ExpenseTags::TagRecord>& tags, const char* amount_header, bool include_receipts)` — generalization of `build529Csv`; `build529Csv` becomes `return buildTagCsv(tags, "qualified_amount", true);` so the 529 CSV output is byte-identical.
  - `bool parseYearParam(const std::map<std::string,std::string>& query_values, time_t& from, time_t& to)` — requires `year` (4-digit, 2000-2100); from = Jan 1 00:00:00 UTC, to = Dec 31 23:59:59 UTC (use `std::tm` + `timegm`).
  - Routes: `GET /api/tax/export/income.csv?year=` (header `date,account,source,category,deposit_amount,taxable_amount`, no receipts column) and `GET /api/tax/export/deductions.csv?year=` (header `date,account,merchant,category,charge_amount,deductible_amount,receipts`).

- [ ] **Step 1: Generalize helpers**

`buildTagCsv` is `build529Csv`'s body with: the header row built as `"date,account,merchant..."` — parameterize precisely:
- income header: `date,account,source,category,deposit_amount,taxable_amount\r\n`
- deductions header: `date,account,merchant,category,charge_amount,deductible_amount,receipts\r\n`
- 529 header (unchanged): `date,account,merchant,category,charge_amount,qualified_amount,receipts\r\n`

Implement with two parameters: `const char* header_row` (full literal including `\r\n`) and `bool include_receipts` (skips the receipts column entirely when false — both the header's column and the per-row cell). So the signature is:

```cpp
    std::string buildTagCsv(const std::vector<ExpenseTags::TagRecord>& tags,
                            const char* header_row, bool include_receipts)
```

and `build529Csv(tags)` → `buildTagCsv(tags, "date,account,merchant,category,charge_amount,qualified_amount,receipts\r\n", true)`.

`parseYearParam`:

```cpp
    bool parseYearParam(const std::map<std::string, std::string>& query_values,
                        time_t& from, time_t& to)
    {
        const auto it = query_values.find("year");
        if (it == query_values.end() || it->second.empty()) return false;
        const auto maybe_year = parsePositiveInt(trim(it->second));
        if (!maybe_year.has_value()) return false;
        const int year = maybe_year.value();
        if (year < 2000 || year > 2100) return false;
        std::tm start{};
        start.tm_year = year - 1900;
        start.tm_mon = 0;
        start.tm_mday = 1;
        from = timegm(&start);
        std::tm end{};
        end.tm_year = year - 1900;
        end.tm_mon = 11;
        end.tm_mday = 31;
        end.tm_hour = 23;
        end.tm_min = 59;
        end.tm_sec = 59;
        to = timegm(&end);
        return true;
    }
```

- [ ] **Step 2: Add the two routes**

```cpp
        if (request.method == "GET" &&
            (request.path == "/api/tax/export/income.csv" ||
             request.path == "/api/tax/export/deductions.csv"))
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            if (!parseYearParam(query_values, from, to))
            {
                return makeJsonResponse(400, makeErrorBody("year must be YYYY (2000-2100)"));
            }
            const bool is_income = request.path == "/api/tax/export/income.csv";
            std::lock_guard<std::mutex> lock(g_tax_mutex);
            bool ok = false;
            const auto tags = qualifiedTagsInRange(
                is_income ? kTaxIncomeTagsFile : kTaxDeductionTagsFile, from, to, ok);
            if (!ok)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read tax tags"));
            }
            HttpResponse response;
            response.status = 200;
            response.content_type = "text/csv; charset=utf-8";
            response.body = is_income
                ? buildTagCsv(tags, "date,account,source,category,deposit_amount,taxable_amount\r\n", false)
                : buildTagCsv(tags, "date,account,merchant,category,charge_amount,deductible_amount,receipts\r\n", true);
            return response;
        }
```

- [ ] **Step 3: Build, deploy, verify**

`make && pm2 restart 3`, wait 2s. Then: mark one income (amount 2.50) + one deduction (amount 3.75) via curl as in Task 3; then:

```bash
curl -s "http://localhost:8080/api/tax/export/income.csv?year=2026"
# Expected: income header + one row, taxable_amount 2.5, NO receipts column
curl -s "http://localhost:8080/api/tax/export/deductions.csv?year=2026"
# Expected: deductions header + one row, deductible_amount 3.75, receipts column (empty)
curl -s "http://localhost:8080/api/tax/export/income.csv?year=2025"
# Expected: header only (no 2025 marks)
curl -s "http://localhost:8080/api/tax/export/income.csv"
# Expected: 400 year required
# 529 export regression (byte-identical shape):
curl -s "http://localhost:8080/api/529/export.csv?from=2026-01-01&to=2026-08-05" | head -1
# Expected: date,account,merchant,category,charge_amount,qualified_amount,receipts
# Cleanup both tax tags (status "none") and confirm /api/tax/tags empty.
```

- [ ] **Step 4: Run test suite regression**

Run: `make test-529 && make test-persistence` — both pass.

- [ ] **Step 5: Commit**

```bash
git add src/web_server.cpp
git commit -m "Add year-scoped tax CSV exports via generalized CSV builder

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Frontend rename + Tax tab scaffolding

**Files:**
- Modify: `web/index.html`, `web/app.js`, `web/styles.css`, `web/sw.js`

**Interfaces:**
- Produces (Tasks 7-9 rely on): DOM ids `spendTabTax`, `spendTaxPane`, `taxYearSelect`, `taxSummaryIncome`, `taxSummaryDeductions`, `exportTaxIncomeBtn`, `exportTaxDeductionsBtn`, `taxIncomeQueueTable`, `taxMarkedIncomeTable`, `taxDepositsSearch`, `taxDepositsTable`, `taxMarkedDeductionsTable`, `taxExpensesSearch`, `taxExpensesTable`, `receiptTaxFileInput`; dialog additions `qualify529Title` (id on the dialog `<h3>`) and `qualify529FilesWrap` (id on the file-input `<label>`); `state.tax` object; `setSpendTab` handling `"tax"`; stub `function renderTaxTab() {}`.

- [ ] **Step 1: Rename copy**

- `web/index.html`: `<h2>Spending</h2>` → `<h2>Spending & Tax</h2>` (in `#spendView`'s page-head; note `&` must be `&amp;` in HTML).
- `web/app.js`: dashboard button text `Spending` → `Spending & Tax` (~line 1909 area, `openSpendAnalysisBtn` button label); breadcrumb `{ label: "Spending" }` → `{ label: "Spending & Tax" }` in `showSpendAnalysis` (:3684).

- [ ] **Step 2: Third tab + pane in index.html**

In the `.spend-tabs` bar, after the 529 button:

```html
          <button id="spendTabTax" class="spend-tab" type="button" role="tab">Tax</button>
```

After `#spend529Pane`'s closing `</div>`, before `</section>`:

```html
        <div id="spendTaxPane" hidden>
          <article class="panel">
            <div class="panel-head">
              <h3>Tax Year Summary</h3>
              <div class="chart-tools">
                <select id="taxYearSelect" class="graph-period-select" aria-label="Tax year"></select>
                <button id="exportTaxIncomeBtn" class="ghost-btn" type="button">Export income CSV</button>
                <button id="exportTaxDeductionsBtn" class="ghost-btn" type="button">Export deductions CSV</button>
              </div>
            </div>
            <p id="taxSummaryIncome" class="muted-note"></p>
            <p id="taxSummaryDeductions" class="muted-note"></p>
          </article>

          <article class="panel">
            <div class="panel-head">
              <h3>Income — Review Queue</h3>
              <p class="muted-note">Deposits Plaid categorized as income</p>
            </div>
            <div id="taxIncomeQueueTable" class="stocks-table-wrap"></div>
          </article>

          <article class="panel">
            <div class="panel-head">
              <h3>Marked Taxable Income</h3>
            </div>
            <div id="taxMarkedIncomeTable" class="stocks-table-wrap"></div>
          </article>

          <article class="panel">
            <details class="tax-browser">
              <summary>All deposits</summary>
              <input id="taxDepositsSearch" class="tax-search" type="search" placeholder="Search deposits…" />
              <div id="taxDepositsTable" class="stocks-table-wrap"></div>
            </details>
          </article>

          <article class="panel">
            <div class="panel-head">
              <h3>Marked Deductions</h3>
            </div>
            <div id="taxMarkedDeductionsTable" class="stocks-table-wrap"></div>
          </article>

          <article class="panel">
            <details class="tax-browser">
              <summary>Expense browser</summary>
              <input id="taxExpensesSearch" class="tax-search" type="search" placeholder="Search expenses…" />
              <div id="taxExpensesTable" class="stocks-table-wrap"></div>
            </details>
          </article>

          <input id="receiptTaxFileInput" type="file" accept="image/*,.pdf,.heic" multiple hidden />
        </div>
```

- [ ] **Step 3: Dialog mode hooks in index.html**

In `#qualify529Dialog`: add `id="qualify529Title"` to the `<h3>` ("Qualify for 529"), and wrap id `qualify529FilesWrap` on the `<label>` containing `#qualify529Files` (the label element itself gets the id).

- [ ] **Step 4: app.js scaffolding**

Add to `state` (after the `spend` object):

```js
  tax: {
    year: new Date().getFullYear(),
    spendTxs: [],
    incomeTxs: [],
    tags: { income: {}, deductions: {} },
    loading: false,
    depositsSearch: "",
    expensesSearch: ""
  },
```

Extend `setSpendTab` — add the pane/button pairs:

```js
  const taxPane = document.getElementById("spendTaxPane");
  const taxBtn = document.getElementById("spendTabTax");
  if (taxPane) taxPane.hidden = tab !== "tax";
  if (taxBtn) taxBtn.classList.toggle("is-active", tab === "tax");
  if (tab === "tax") renderTaxTab();
```

Add `function renderTaxTab() { /* populated in Task 7 */ }` near `render529Tab`, and wire the tab button in the init block next to the other two:

```js
  const spendTabTax = document.getElementById("spendTabTax");
  if (spendTabTax) spendTabTax.addEventListener("click", () => setSpendTab("tax"));
```

- [ ] **Step 5: CSS**

Append to `web/styles.css` (flat corners per theme):

```css
.tax-browser summary {
  cursor: pointer;
  font-weight: 600;
  padding: 0.25rem 0;
}

.tax-search {
  width: 100%;
  margin: 0.5rem 0 0.75rem;
  padding: 0.4rem 0.6rem;
  font: inherit;
  border: 1px solid rgba(34, 34, 34, 0.25);
  border-radius: 0;
}
```

- [ ] **Step 6: Cache bumps + checks**

- index.html: `app.js?v=20260805d` → `?v=20260805e`; `styles.css?v=20260805a` → `?v=20260805b`.
- sw.js: `finance-tracker-shell-v7` → `v8`.
- Run `node --check web/app.js`; `grep -n "Spending &" web/index.html web/app.js` shows the three renamed spots; browser: three tabs render, Tax pane shows empty panels, no console errors.

- [ ] **Step 7: Commit**

```bash
git add web/index.html web/app.js web/styles.css web/sw.js
git commit -m "Rename page to Spending & Tax, scaffold Tax tab

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Tax data plumbing + summary + marked lists

**Files:**
- Modify: `web/app.js`

**Interfaces:**
- Consumes: `/api/income`, `/api/tax/tags`, `/api/tax/tag`, `/api/tax/receipt` (Tasks 1-4), DOM from Task 6, existing helpers (`apiGet`, `apiPost`, `apiDelete`, `currency`, `dateLabel`, `escapeHtml`, `showFlash`).
- Produces (Tasks 8-9 rely on):
  - `async function loadTaxData()` — fetches spend + income for the tax year + tags; sets `state.tax.*`; calls `renderTaxTab()`.
  - `async function refreshTaxTags()` — re-fetches `/api/tax/tags` only, re-renders; try/catch + flash, no rethrow.
  - `async function saveTaxTag(kind, key, status, amount, refresh = true)` — POSTs `/api/tax/tag`; refreshes when `refresh`.
  - `async function apiUploadReceiptTo(basePath, key, file)` — generalization of `apiUploadReceipt`; `apiUploadReceipt(key, file)` delegates to `apiUploadReceiptTo("/api/529/receipt", key, file)`.
  - `renderTaxTab()` real implementation calling `renderTaxSummary`, `renderTaxMarkedIncome`, `renderTaxMarkedDeductions`, plus stubs `renderTaxIncomeQueue()`, `renderTaxDeposits()`, `renderTaxExpenseBrowser()` (Task 8 fills them; define as empty functions now).
  - `wireTaxStaticControls()` — populates `#taxYearSelect` (current year down to current−3), wires its change → `loadTaxData()`, wires `#receiptTaxFileInput` change handler (same pattern as `receipt529FileInput` but with `apiUploadReceiptTo("/api/tax/receipt", ...)` and `refreshTaxTags()`); called once from the init block.

- [ ] **Step 1: Upload helper generalization**

Rename the body of `apiUploadReceipt` to:

```js
async function apiUploadReceiptTo(basePath, key, file) {
  beginRequest();
  try {
    const query = `key=${encodeURIComponent(key)}&filename=${encodeURIComponent(file.name)}`;
    const response = await fetch(apiUrl(`${basePath}?${query}`), {
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

async function apiUploadReceipt(key, file) {
  return apiUploadReceiptTo("/api/529/receipt", key, file);
}
```

- [ ] **Step 2: Data plumbing**

```js
function taxYearBounds() {
  const y = state.tax.year;
  return { from: `${y}-01-01`, to: `${y}-12-31` };
}

async function loadTaxData() {
  if (state.tax.loading) return;
  state.tax.loading = true;
  try {
    const { from, to } = taxYearBounds();
    const [spendPayload, incomePayload, tagsPayload] = await Promise.all([
      apiGet(`/api/spend?from=${from}&to=${to}`),
      apiGet(`/api/income?from=${from}&to=${to}`),
      apiGet("/api/tax/tags")
    ]);
    state.tax.spendTxs = Array.isArray(spendPayload.transactions) ? spendPayload.transactions : [];
    state.tax.incomeTxs = Array.isArray(incomePayload.transactions) ? incomePayload.transactions : [];
    state.tax.tags = { income: {}, deductions: {} };
    (Array.isArray(tagsPayload.income) ? tagsPayload.income : []).forEach((tag) => {
      state.tax.tags.income[tag.key] = tag;
    });
    (Array.isArray(tagsPayload.deductions) ? tagsPayload.deductions : []).forEach((tag) => {
      state.tax.tags.deductions[tag.key] = tag;
    });
    renderTaxTab();
  } catch (e) {
    showFlash(`Failed to load tax data: ${e.message}`);
  } finally {
    state.tax.loading = false;
  }
}

async function refreshTaxTags() {
  try {
    const tagsPayload = await apiGet("/api/tax/tags");
    state.tax.tags = { income: {}, deductions: {} };
    (Array.isArray(tagsPayload.income) ? tagsPayload.income : []).forEach((tag) => {
      state.tax.tags.income[tag.key] = tag;
    });
    (Array.isArray(tagsPayload.deductions) ? tagsPayload.deductions : []).forEach((tag) => {
      state.tax.tags.deductions[tag.key] = tag;
    });
    renderTaxTab();
  } catch (e) {
    showFlash(`Failed to refresh tax tags: ${e.message}`);
  }
}

async function saveTaxTag(kind, key, status, amount, refresh = true) {
  const body = { kind, key, status };
  if (status === "qualified" && amount != null) {
    body.amount = amount;
  }
  await apiPost("/api/tax/tag", body);
  if (refresh) await refreshTaxTags();
}
```

Change `setSpendTab`'s tax branch to `if (tab === "tax") loadTaxData();` (first entry loads; `renderTaxTab` alone can't — there's no data yet).

- [ ] **Step 3: Rendering — summary + marked lists**

```js
function taxYearRecordFilter(tagMap) {
  const y = state.tax.year;
  const fromTs = Math.floor(Date.UTC(y, 0, 1) / 1000);
  const toTs = Math.floor(Date.UTC(y, 11, 31, 23, 59, 59) / 1000);
  return Object.values(tagMap)
    .filter((tag) => tag.status === "qualified" && tag.date >= fromTs && tag.date <= toTs)
    .sort((a, b) => b.date - a.date);
}

function renderTaxTab() {
  const markedIncome = taxYearRecordFilter(state.tax.tags.income);
  const markedDeductions = taxYearRecordFilter(state.tax.tags.deductions);

  const incomeTotal = markedIncome.reduce((sum, t) => sum + (t.qualified_amount || 0), 0);
  const deductionTotal = markedDeductions.reduce((sum, t) => sum + (t.qualified_amount || 0), 0);
  const missingReceipts = markedDeductions.filter((t) => (t.receipts || []).length === 0).length;

  const incomeSummary = document.getElementById("taxSummaryIncome");
  if (incomeSummary) {
    incomeSummary.textContent = `Taxable income: ${currency(incomeTotal)} · ${markedIncome.length} marked`;
  }
  const deductionSummary = document.getElementById("taxSummaryDeductions");
  if (deductionSummary) {
    const missingNote = missingReceipts > 0
      ? ` · ⚠ ${missingReceipts} missing receipt${missingReceipts === 1 ? "" : "s"}`
      : "";
    deductionSummary.textContent = `Deductible: ${currency(deductionTotal)} · ${markedDeductions.length} marked${missingNote}`;
  }

  const incomeTxByKey = {};
  state.tax.incomeTxs.forEach((tx) => { incomeTxByKey[tx.key] = tx; });
  const spendTxByKey = {};
  state.tax.spendTxs.forEach((tx) => { spendTxByKey[tx.key] = tx; });

  renderTaxIncomeQueue(incomeTxByKey);
  renderTaxMarkedIncome(markedIncome, incomeTxByKey);
  renderTaxDeposits(incomeTxByKey);
  renderTaxMarkedDeductions(markedDeductions, spendTxByKey);
  renderTaxExpenseBrowser(spendTxByKey);
}
```

`renderTaxMarkedIncome(marked, txByKey)` renders into `#taxMarkedIncomeTable`: empty-state `<p class="muted-note" style="padding:0.75rem;">No income marked for ${state.tax.year} yet.</p>`; otherwise a `tx-table` with Date / Source (+`orphaned-badge` when `!txByKey[tag.key]`) / Deposit / Taxable (editable `<input class="tax-amount-input num" ...>` mirroring the 529 qualified-amount input, `data-kind="income"`) / Unmark button (`tax-unmark-btn`, `data-kind="income"`).

`renderTaxMarkedDeductions(marked, txByKey)` renders into `#taxMarkedDeductionsTable`: same shape as `render529QualifiedList` — Date / Merchant (+orphan) / Charge / Deductible (editable input, `data-kind="deduction"`) / Receipts cell (chips linking `apiUrl(\`/api/tax/receipt?...\`)` wrapped in `escapeHtml`, delete `×` buttons `tax-receipt-delete`, upload button `tax-receipt-upload-btn`, `missing-receipt-badge` when none) / Unmark.

Both marked lists share one event-wiring function:

```js
function wireTaxMarkedListEvents(host) {
  host.querySelectorAll(".tax-amount-input").forEach((input) => {
    input.addEventListener("change", async () => {
      const kind = input.dataset.kind;
      const tagMap = kind === "income" ? state.tax.tags.income : state.tax.tags.deductions;
      const tag = tagMap[input.dataset.key];
      if (!tag) return;
      const value = Number.parseFloat(input.value);
      if (!Number.isFinite(value) || value <= 0 || value > tag.amount + 0.005) {
        showFlash("Amount must be between $0.01 and the transaction amount.");
        input.value = (tag.qualified_amount || 0).toFixed(2);
        return;
      }
      try {
        await saveTaxTag(kind, input.dataset.key, "qualified", value);
      } catch (e) {
        showFlash(`Update failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll(".tax-unmark-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTaxTag(btn.dataset.kind, btn.dataset.key, "none", null);
      } catch (e) {
        showFlash(`Unmark failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll(".tax-receipt-upload-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const fileInput = document.getElementById("receiptTaxFileInput");
      if (!fileInput) return;
      fileInput.dataset.key = btn.dataset.key;
      fileInput.value = "";
      fileInput.click();
    });
  });
  host.querySelectorAll(".tax-receipt-delete").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await apiDelete(`/api/tax/receipt?key=${encodeURIComponent(btn.dataset.key)}&filename=${encodeURIComponent(btn.dataset.filename)}`);
        await refreshTaxTags();
      } catch (e) {
        showFlash(`Delete failed: ${e.message}`);
      }
    });
  });
}
```

Drag-drop on marked-deduction rows: same pattern as `wire529QualifiedListEvents`'s drop handlers but calling `apiUploadReceiptTo("/api/tax/receipt", row.dataset.key, file)` and `refreshTaxTags()` in a finally.

- [ ] **Step 4: wireTaxStaticControls**

```js
function wireTaxStaticControls() {
  const yearSelect = document.getElementById("taxYearSelect");
  if (yearSelect) {
    const current = new Date().getFullYear();
    for (let y = current; y >= current - 3; y--) {
      const opt = document.createElement("option");
      opt.value = String(y);
      opt.textContent = String(y);
      yearSelect.appendChild(opt);
    }
    yearSelect.value = String(state.tax.year);
    yearSelect.addEventListener("change", () => {
      state.tax.year = Number.parseInt(yearSelect.value, 10);
      loadTaxData();
    });
  }
  const receiptInput = document.getElementById("receiptTaxFileInput");
  if (receiptInput) {
    receiptInput.addEventListener("change", async () => {
      const key = receiptInput.dataset.key;
      const files = Array.from(receiptInput.files);
      if (!key || files.length === 0) return;
      try {
        for (const file of files) {
          await apiUploadReceiptTo("/api/tax/receipt", key, file);
        }
      } catch (e) {
        showFlash(`Upload failed: ${e.message}`);
      } finally {
        await refreshTaxTags();
      }
    });
  }
}
```

Call `wireTaxStaticControls();` in the init block next to `wireQualify529Dialog()`. Define empty stubs `renderTaxIncomeQueue`, `renderTaxDeposits`, `renderTaxExpenseBrowser` (Task 8 fills them).

- [ ] **Step 5: Verify**

`node --check web/app.js`. Curl-mark one income + one deduction (Task 3 pattern, amounts 2.50/3.75), reload browser → Tax tab: totals show $2.50 / $3.75, marked rows render, amount edit persists, unmark works, receipt upload button on the deduction row uploads a file and the chip appears (verify then delete). Clean up all test marks. Note remaining browser verification falls to controller if you cannot drive a browser — state exactly what you verified.

- [ ] **Step 6: Commit**

```bash
git add web/app.js
git commit -m "Add tax data plumbing, summary, and marked lists

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Mark dialog generalization + income queue + browsers + drilldown Deduct

**Files:**
- Modify: `web/app.js`, `web/index.html` (dialog title text only if needed — ids exist from Task 6)

**Interfaces:**
- Consumes: everything from Task 7; dialog ids `qualify529Title`, `qualify529FilesWrap`.
- Produces:
  - `let markDialogMode = "529";` and `function openMarkDialog(mode, tx)` — `mode` ∈ `"529" | "income" | "deduction"`. `openQualify529Dialog(tx)` becomes `openMarkDialog("529", tx)` (keep the old function as a one-line delegate; all existing 529 call sites unchanged).
  - Real `renderTaxIncomeQueue(incomeTxByKey)`, `renderTaxDeposits(incomeTxByKey)`, `renderTaxExpenseBrowser(spendTxByKey)`.
  - Drilldown rows gain a `Deduct` button (`drilldown-deduct-btn`).

- [ ] **Step 1: Generalize the dialog**

Replace `qualify529Target` handling:

```js
let qualify529Target = null;
let markDialogMode = "529";

const MARK_DIALOG_COPY = {
  "529": { title: "Qualify for 529", verb: "Qualify", showFiles: true },
  "income": { title: "Mark taxable income", verb: "Mark", showFiles: false },
  "deduction": { title: "Mark tax deductible", verb: "Mark", showFiles: true }
};

function openMarkDialog(mode, tx) {
  markDialogMode = mode;
  qualify529Target = tx;
  const dialog = document.getElementById("qualify529Dialog");
  const title = document.getElementById("qualify529Title");
  const merchant = document.getElementById("qualify529Merchant");
  const amount = document.getElementById("qualify529Amount");
  const files = document.getElementById("qualify529Files");
  const filesWrap = document.getElementById("qualify529FilesWrap");
  if (!dialog || !amount) return;
  const copy = MARK_DIALOG_COPY[mode];
  if (title) title.textContent = copy.title;
  if (merchant) merchant.textContent = `${tx.notes || "—"} · ${dateLabel(tx.date)} · ${currency(tx.amount)}`;
  amount.value = tx.amount.toFixed(2);
  amount.max = tx.amount;
  if (files) files.value = "";
  if (filesWrap) filesWrap.hidden = !copy.showFiles;
  dialog.showModal();
}

function openQualify529Dialog(tx) {
  openMarkDialog("529", tx);
}
```

The Save handler branches on `markDialogMode`:
- `"529"`: existing behavior verbatim (amount validation → academic-year gate → saveTag529 ± uploads to `/api/529/receipt`).
- `"income"`: amount validation (same bounds message: "Amount must be between $0.01 and the transaction amount.") → `await saveTaxTag("income", key, "qualified", amount)` → close. No academic-year gate, no files.
- `"deduction"`: amount validation → if files selected: `await saveTaxTag("deduction", key, "qualified", amount, false)`, upload each via `apiUploadReceiptTo("/api/tax/receipt", ...)` in try/finally with `await refreshTaxTags()` in the finally; else `await saveTaxTag("deduction", key, "qualified", amount)` → close. No academic-year gate.

Structure it as a small `if/else if/else` inside the existing try block; keep `save.disabled` bracketing and the dialog-stays-open-on-error semantics for all modes.

- [ ] **Step 2: Income queue**

```js
function renderTaxIncomeQueue(incomeTxByKey) {
  const host = document.getElementById("taxIncomeQueueTable");
  if (!host) return;
  const queue = state.tax.incomeTxs
    .filter((tx) =>
      (tx.category || "").startsWith("INCOME_") &&
      !state.tax.tags.income[tx.key])
    .sort((a, b) => (b.date || 0) - (a.date || 0));
  if (queue.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">Queue is clear — no unreviewed income deposits in ${escapeHtml(String(state.tax.year))}.</p>`;
    return;
  }
  const rows = queue
    .map((tx) => `<tr data-key="${escapeHtml(tx.key)}">
      <td>${dateLabel(tx.date)}</td>
      <td>${escapeHtml(tx.notes || "—")}</td>
      <td>${escapeHtml(tx.account || "")}</td>
      <td class="num">${currency(tx.amount)}</td>
      <td>
        <button class="ghost-btn tax-mark-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Mark taxable</button>
        <button class="ghost-btn tax-dismiss-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✕ Dismiss</button>
      </td>
    </tr>`)
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Source</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  host.querySelectorAll(".tax-mark-income-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = incomeTxByKey[btn.dataset.key];
      if (tx) openMarkDialog("income", tx);
    });
  });
  host.querySelectorAll(".tax-dismiss-income-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTaxTag("income", btn.dataset.key, "dismissed", null);
      } catch (e) {
        showFlash(`Dismiss failed: ${e.message}`);
      }
    });
  });
}
```

- [ ] **Step 3: Browsers with search**

Shared row-shape; deposits browser shows Mark button when the record is absent OR dismissed (dismissed deposits remain markable per spec):

```js
function renderTaxDeposits(incomeTxByKey) {
  const host = document.getElementById("taxDepositsTable");
  if (!host) return;
  const needle = state.tax.depositsSearch.trim().toLowerCase();
  const rows = state.tax.incomeTxs
    .filter((tx) => !needle || (tx.notes || "").toLowerCase().includes(needle))
    .sort((a, b) => (b.date || 0) - (a.date || 0))
    .map((tx) => {
      const tag = state.tax.tags.income[tx.key];
      const marked = tag && tag.status === "qualified";
      const action = marked
        ? `<span class="muted-note">marked</span>`
        : `<button class="ghost-btn tax-mark-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Mark taxable</button>`;
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${escapeHtml(tx.notes || "—")}</td>
        <td>${escapeHtml(tx.account || "")}</td>
        <td class="num">${currency(tx.amount)}</td>
        <td>${action}</td>
      </tr>`;
    })
    .join("");
  host.innerHTML = rows
    ? `<table class="tx-table"><thead><tr><th>Date</th><th>Source</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
    : `<p class="muted-note" style="padding:0.75rem;">No deposits match.</p>`;
  host.querySelectorAll(".tax-mark-income-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = incomeTxByKey[btn.dataset.key];
      if (tx) openMarkDialog("income", tx);
    });
  });
}
```

`renderTaxExpenseBrowser(spendTxByKey)` is identical in shape over `state.tax.spendTxs`, searching `notes`, columns Date / Merchant / Account / Amount / action, action = "marked" when a qualified deduction record exists else `✓ Deduct` button opening `openMarkDialog("deduction", tx)`.

Wire the search inputs once in `wireTaxStaticControls` (Task 7's function — extend it):

```js
  const depositsSearch = document.getElementById("taxDepositsSearch");
  if (depositsSearch) {
    depositsSearch.addEventListener("input", () => {
      state.tax.depositsSearch = depositsSearch.value;
      const incomeTxByKey = {};
      state.tax.incomeTxs.forEach((tx) => { incomeTxByKey[tx.key] = tx; });
      renderTaxDeposits(incomeTxByKey);
    });
  }
  const expensesSearch = document.getElementById("taxExpensesSearch");
  if (expensesSearch) {
    expensesSearch.addEventListener("input", () => {
      state.tax.expensesSearch = expensesSearch.value;
      const spendTxByKey = {};
      state.tax.spendTxs.forEach((tx) => { spendTxByKey[tx.key] = tx; });
      renderTaxExpenseBrowser(spendTxByKey);
    });
  }
```

- [ ] **Step 4: Drilldown Deduct button**

In `renderSpendDrilldownTable`, add next to the existing 529 button:

```js
        <td>
          <button class="ghost-btn drilldown-529-btn" type="button" data-key="${escapeHtml(tx.key)}">529</button>
          <button class="ghost-btn drilldown-deduct-btn" type="button" data-key="${escapeHtml(tx.key)}">Deduct</button>
        </td>
```

(one `<td>` holding both buttons — remove the separate 529 `<td>` and its extra header cell so the header keeps a single trailing `<th></th>`).

In `openSpendCategoryDrilldown`'s wiring, add:

```js
  el.transactionsHistory.querySelectorAll(".drilldown-deduct-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) {
        el.transactionsDialog.close();
        setSpendTab("tax");
        openMarkDialog("deduction", tx);
      }
    });
  });
```

Note: the drilldown tx comes from `state.spend.transactions` (page range) — its key is range-stable so the mark lands correctly even if the Tax tab's year differs; after marking, `refreshTaxTags` re-renders the Tax tab.

- [ ] **Step 5: Verify**

`node --check web/app.js`. Browser: income queue lists INCOME_* deposits with working Mark/Dismiss; dialog title switches per mode and hides the file input for income; deposits browser search filters and marks; expense browser search + Deduct works; drilldown Deduct jumps to Tax tab with dialog open; 529 qualify flow still works including academic-year gate and file upload. Clean up any test marks.

- [ ] **Step 6: Commit**

```bash
git add web/app.js web/index.html
git commit -m "Add mark dialog modes, income queue, browsers, drilldown Deduct

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Export buttons + full verification pass

**Files:**
- Modify: `web/app.js`

**Interfaces:**
- Consumes: export endpoints (Task 5), buttons from Task 6.

- [ ] **Step 1: Download helper + wiring**

```js
async function downloadTaxExport(kind) {
  const year = state.tax.year;
  beginRequest();
  try {
    const response = await fetch(apiUrl(`/api/tax/export/${kind}.csv?year=${year}`));
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
    link.download = `tax-${kind}_${year}.csv`;
    link.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    showFlash(e.message);
  } finally {
    endRequest();
  }
}
```

Wire in the init block:

```js
  const exportTaxIncomeBtn = document.getElementById("exportTaxIncomeBtn");
  const exportTaxDeductionsBtn = document.getElementById("exportTaxDeductionsBtn");
  if (exportTaxIncomeBtn) exportTaxIncomeBtn.addEventListener("click", () => downloadTaxExport("income"));
  if (exportTaxDeductionsBtn) exportTaxDeductionsBtn.addEventListener("click", () => downloadTaxExport("deductions"));
```

- [ ] **Step 2: Full rebuild + tests + deploy**

```bash
make clean && make && make test-529 && make test-persistence
pm2 restart 3
sleep 2
curl -s http://localhost:8080/api/health
```

- [ ] **Step 3: API smoke**

```bash
curl -s "http://localhost:8080/api/income?from=2026-01-01&to=2026-08-05" | python3 -c "import json,sys; d=json.load(sys.stdin); assert all('key' in t for t in d['transactions']); print('income ok', len(d['transactions']))"
curl -s http://localhost:8080/api/tax/tags
curl -s -o /dev/null -w "%{http_code} %{content_type}\n" "http://localhost:8080/api/tax/export/income.csv?year=2026"
curl -s -o /dev/null -w "%{http_code} %{content_type}\n" "http://localhost:8080/api/tax/export/deductions.csv?year=2026"
# 529 regression endpoints:
curl -s -o /dev/null -w "%{http_code}\n" "http://localhost:8080/api/529/export.csv?from=2026-01-01&to=2026-08-05"
curl -s http://localhost:8080/api/529/tags
```

- [ ] **Step 4: Report**

Note in the report that the end-to-end browser walkthrough (all three tabs, mark flows, exports downloading, 529 regression) falls to the controller.

- [ ] **Step 5: Commit**

```bash
git add web/app.js
git commit -m "Add tax CSV export buttons

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

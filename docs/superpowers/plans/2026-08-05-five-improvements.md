# Five Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 529 withdrawal reconciliation, an income-vs-spending cash-flow panel, merchant display names, nightly in-server backups, and optional API bearer-token auth.

**Architecture:** Five independent features on the established patterns: sidecar TagRecord stores + manual-entry validation for withdrawals; existing chart/bucket helpers for cash flow; the spend_overrides pipeline for display names; the detached wall-clock-polling thread pattern for backups; a single gate in `routeRequest` for auth.

**Tech Stack:** C++17 (no new libs), vanilla JS, Chart.js (already loaded), `/usr/bin/tar`.

**Spec:** `docs/superpowers/specs/2026-08-05-five-improvements-design.md` — read it before starting.

## Global Constraints

- C++17, 4-space indent, Allman braces. Vanilla JS, 2-space indent, double quotes. Flat theme (border-radius: 0).
- Makefile has NO header dep tracking: `make clean && make` after any `include/*.hpp` change (none planned); else plain `make`. Deploy `pm2 restart 3`, wait 2s. Backend :8080; frontend served separately (:5173).
- Cache busting on any frontend change: bump `?v=` in index.html AND `CACHE_NAME` in sw.js (current: `app.js?v=20260805i`, `styles.css?v=20260805c`, `finance-tracker-shell-v13` — each task that touches frontend bumps by one letter/number and notes the new value for the next task).
- Fingerprint keys MUST keep hashing raw (pre-display) notes — display renames apply only after key assignment.
- All data/529 mutations under `g_529_mutex`; data/tax under `g_tax_mutex`.
- Every task: run its verification steps INCLUDING cleanup of any test data; 529/tax regression check when touching shared code.
- Commits end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## Current-code anchors (src/web_server.cpp unless noted)

- `struct SpendOverride` :3225, `loadSpendOverrides` :3235, first override application inside `collectSpendTransactions` :3280 area; `collectIncomeTransactions` follows `buildSpendJson`.
- `routeRequest(request, manager)` :4648 — OPTIONS 204 early-return, then `/api/health`, then the route chain. Manual-income creation block inside `POST /api/tax/tag` (~:4790).
- CORS headers :2598-2600 (`Access-Control-Allow-Headers: Content-Type`).
- Server startup threads: :6050-6200 (startup sync thread, daily wall-clock-polling sync thread — the polling comment there explains why NOT to sleep long intervals; copy that pattern).
- web/app.js: `API_STORAGE_KEY` :1, `state.apiBase` :257, `apiUrl` :655, fetch helpers `apiGet` :800 area / `apiGetWithTimeout` / `apiPost` / `apiDelete` / `apiPatch` / `apiUploadReceiptTo` (:880 area), `download529Export` + `downloadTaxExport` (raw fetches), `destroyChart` :1577, `bucketSpendTransactions` :3133 (returns `[{key,label,total,byPrimary}]` sorted), `loadSpendData` (~:3300), 529 render functions (~:3800+), API settings form wiring ~:4590-4630.
- web/index.html: API settings panel ~line 27-39; 529 pane `#spend529Pane` ~line 113; dialogs at bottom (`#manualIncomeDialog` is the model for the withdrawal dialog).

Line numbers drift — search by name.

---

### Task 1: Merchant display names

**Files:**
- Modify: `src/web_server.cpp`

**Interfaces:**
- Produces: `SpendOverride` gains `std::string display;` (empty = no rename). `loadSpendOverrides` parses optional `"display"` per rule. Both collectors rewrite the emitted `notes` with the FIRST matching rule's non-empty `display`, AFTER `key_assigner.next(...)`.

- [ ] **Step 1: Extend the struct + parser**

In `struct SpendOverride` add `std::string display;`. In `loadSpendOverrides`, wherever a rule's `category` is extracted, also extract optional `display` the same way (inspect the existing parsing; it is a small hand-rolled scan — mirror exactly how `category` is pulled for the `display` key; a rule with neither category nor display is skipped as today, a rule with only one of them keeps the other empty).

- [ ] **Step 2: Apply in both collectors**

In `collectSpendTransactions`, the override loop currently sets `effective_category`. Extend it: track the first matching rule and, after the loop, if that rule's `display` is non-empty, use it as the emitted notes:

```cpp
                std::string effective_category = tx.category;
                std::string effective_notes = tx.notes;
                if (!overrides.empty())
                {
                    const std::string notes_lower = lowerCopy(tx.notes);
                    for (const auto& rule : overrides)
                    {
                        if (notes_lower.find(rule.match_lower) != std::string::npos)
                        {
                            if (!rule.category.empty())
                            {
                                effective_category = rule.category;
                            }
                            if (!rule.display.empty())
                            {
                                effective_notes = rule.display;
                            }
                            break;
                        }
                    }
                }
```

and emit `spend_tx.notes = effective_notes;`. CRITICAL: the `key_assigner.next(name, tx.date, tx.amount, tx.notes)` call keeps the RAW `tx.notes` — do not move it below the rewrite. Apply the identical block to `collectIncomeTransactions` (which currently has no override handling at all — add `const std::vector<SpendOverride> overrides = loadSpendOverrides();` at its top and the same loop; keys there also keep hashing raw notes).

Note: if the existing category loop breaks on first match already, preserve that; the only change is capturing `display` from that same first match and guarding `category` assignment on non-empty (a display-only rule must not blank the category).

- [ ] **Step 3: Build, deploy, verify with a temporary rule**

`make && pm2 restart 3`, wait 2s. `data/spend_overrides.json` is the user's live file: SAVE A COPY FIRST (`cp data/spend_overrides.json /tmp/spend_overrides.bak`), add a test rule `{"match": "Streqps", "display": "TEST RENAME"}` to the rules array, then:

```bash
# Key stability: capture keys before/after the rule for the affected txn
curl -s "http://localhost:8080/api/spend?from=2026-07-01&to=2026-08-05" | python3 -c "import json,sys; [print(t['key'], t['notes']) for t in json.load(sys.stdin)['transactions'] if 'TEST RENAME' in t['notes']]"
# Expected: the STREQPS transaction shows notes "TEST RENAME" with the SAME key it had before
# (compare against: restore backup, re-curl, confirm same key with original notes)
curl -s "http://localhost:8080/api/income?from=2026-01-01&to=2026-08-05" | python3 -c "import json,sys; print(sum(1 for t in json.load(sys.stdin)['transactions']))"
# Expected: same count as before the change (no filtering side effects)
```

Restore the file byte-for-byte: `cp /tmp/spend_overrides.bak data/spend_overrides.json && rm /tmp/spend_overrides.bak`, re-curl to confirm original notes are back.

- [ ] **Step 4: Regression + commit**

`make test-529 && make test-persistence` pass.

```bash
git add src/web_server.cpp
git commit -m "Support merchant display names in spend overrides

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: 529 withdrawals — backend

**Files:**
- Modify: `src/web_server.cpp`

**Interfaces:**
- Produces:
  - `const char* k529WithdrawalsFile = "data/529/withdrawals.json";` (next to `kTagsFile`).
  - `HttpResponse createManualRecord(const char* tags_file, std::mutex& store_mutex, bool (*ensure_dirs)(), const std::string& date_str, double amount, const std::string& notes, const char* account, const char* category)` — extracted from the manual-income creation block in `POST /api/tax/tag`; that block becomes a call to it with `("Manual", "MANUAL")`, and behavior there stays byte-identical (same validation order, same error strings, same `manual-<epoch>-<n>` key dedup, same 201 `{"tag":...}` response).
  - Routes:
    - `GET /api/529/withdrawals` → `{"withdrawals":[TagRecord...]}` under `g_529_mutex`.
    - `POST /api/529/withdrawal` — body either `{date, amount, notes}` (create → delegates to `createManualRecord(k529WithdrawalsFile, g_529_mutex, ensure529Dirs, ..., "529 Plan", "WITHDRAWAL")`) or `{key, status:"none"}` (remove; 404 if absent — reuse the load/erase/save shape from `applyTagUpsert`'s none-branch, inline, under `g_529_mutex`).

- [ ] **Step 1: Extract createManualRecord**

The manual-income block inside `POST /api/tax/tag` (search `is_manual`) currently: validates kind, extracts date/amount/notes, `parseIsoDateUTC` (400 "date must be YYYY-MM-DD"), amount > 0 (400 "amount must be > 0"), notes non-empty (400 "notes must not be empty"), locks, ensures dirs, loads, generates unique `manual-<epoch>-<n>` key, builds the record (status "qualified", qualified_amount = amount = amount, account "Manual", category "MANUAL"), saves, returns 201 `{"tag":...}`. Extract everything from the date validation onward into `createManualRecord` (signature above; the kind check stays in the route). The route's manual branch becomes:

```cpp
                return createManualRecord(kTaxIncomeTagsFile, g_tax_mutex, ensureTaxDirs,
                                          raw_date.value(), manual_amount.value(),
                                          raw_notes.value(), "Manual", "MANUAL");
```

(with the missing-field 400 for date/amount/notes staying in the route, before the call).

- [ ] **Step 2: Add the withdrawal routes**

After the `/api/529/tag` route block:

```cpp
        if (request.method == "GET" && request.path == "/api/529/withdrawals")
        {
            std::lock_guard<std::mutex> lock(g_529_mutex);
            std::vector<ExpenseTags::TagRecord> withdrawals;
            if (!ExpenseTags::loadTags(k529WithdrawalsFile, withdrawals))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 withdrawals"));
            }
            std::ostringstream out;
            out << "{\"withdrawals\":[";
            for (size_t i = 0; i < withdrawals.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(withdrawals[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/529/withdrawal")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }

            const auto raw_status = getObjectString(body, "status");
            if (raw_status.has_value() && trim(raw_status.value()) == "none")
            {
                const auto raw_key = getObjectString(body, "key");
                if (!raw_key.has_value() || trim(raw_key.value()).empty())
                {
                    return makeJsonResponse(400, makeErrorBody("key is required to remove a withdrawal"));
                }
                const std::string key = trim(raw_key.value());
                std::lock_guard<std::mutex> lock(g_529_mutex);
                std::vector<ExpenseTags::TagRecord> withdrawals;
                if (!ExpenseTags::loadTags(k529WithdrawalsFile, withdrawals))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to read 529 withdrawals"));
                }
                auto existing = std::find_if(withdrawals.begin(), withdrawals.end(),
                    [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
                if (existing == withdrawals.end())
                {
                    return makeJsonResponse(404, makeErrorBody("No withdrawal for that key"));
                }
                withdrawals.erase(existing);
                if (!ExpenseTags::saveTags(k529WithdrawalsFile, withdrawals))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save 529 withdrawals"));
                }
                return makeJsonResponse(200, "{\"removed\":true}");
            }

            const auto raw_date = getObjectString(body, "date");
            const auto raw_amount = getObjectNumber(body, "amount");
            const auto raw_notes = getObjectString(body, "notes");
            if (!raw_date.has_value() || !raw_amount.has_value() || !raw_notes.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("date, amount, and notes are required"));
            }
            return createManualRecord(k529WithdrawalsFile, g_529_mutex, ensure529Dirs,
                                      raw_date.value(), raw_amount.value(),
                                      raw_notes.value(), "529 Plan", "WITHDRAWAL");
        }
```

- [ ] **Step 3: Build, deploy, verify**

`make && pm2 restart 3`, wait 2s:

```bash
curl -s -X POST -H "Content-Type: application/json" -d '{"date":"2026-05-10","amount":500,"notes":"Spring semester withdrawal"}' http://localhost:8080/api/529/withdrawal
# Expected: 201, record with account "529 Plan", category "WITHDRAWAL", key manual-...
curl -s http://localhost:8080/api/529/withdrawals
# Expected: one record; data/529/withdrawals.json exists
curl -s -X POST -H "Content-Type: application/json" -d '{"date":"bad","amount":500,"notes":"x"}' http://localhost:8080/api/529/withdrawal   # 400
curl -s -X POST -H "Content-Type: application/json" -d '{"key":"nope-0","status":"none"}' http://localhost:8080/api/529/withdrawal          # 404
# Manual-income regression (extraction must not have changed it):
curl -s -X POST -H "Content-Type: application/json" -d '{"kind":"income","manual":true,"date":"2026-02-01","amount":1,"notes":"regression probe"}' http://localhost:8080/api/tax/tag
# Expected: 201 with account "Manual", category "MANUAL"
# Cleanup: remove the probe income (kind income, status none, its key) and the test withdrawal (status none); confirm both stores back to prior state.
```

- [ ] **Step 4: Regression + commit**

`make test-529 && make test-persistence` pass.

```bash
git add src/web_server.cpp
git commit -m "Add 529 withdrawal store and endpoints via shared manual-record helper

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: 529 withdrawals — frontend (year reconciliation + panel)

**Files:**
- Modify: `web/index.html`, `web/app.js`, `web/sw.js`

**Interfaces:**
- Consumes: Task 2 endpoints; existing 529 tab functions (`render529Tab`, `state.spend.tags529`), `#manualIncomeDialog` as the dialog model, `taxYearRecordFilter`-style year filtering.
- Produces: `state.spend529 = { year: current, withdrawals: [] }`; DOM ids `tax529YearSelect`, `summary529Reconciliation`, `withdrawals529Table`, `addWithdrawalBtn`, `withdrawalDialog` (+`withdrawalDate`, `withdrawalAmount`, `withdrawalNotes`, `withdrawalCancel`, `withdrawalSave`); `loadWithdrawals529()`, `render529Reconciliation()`.

- [ ] **Step 1: index.html**

In the 529 summary panel (`#total529Label` panel): add to its `.chart-tools`, BEFORE the export buttons:

```html
                <select id="tax529YearSelect" class="graph-period-select" aria-label="529 tax year"></select>
```

and after `#summary529Meta`:

```html
            <p id="summary529Reconciliation" class="muted-note"></p>
```

After the Qualified Expenses panel, add:

```html
          <article class="panel">
            <div class="panel-head">
              <h3>Withdrawals</h3>
              <button id="addWithdrawalBtn" class="ghost-btn" type="button">＋ Add withdrawal</button>
            </div>
            <div id="withdrawals529Table" class="stocks-table-wrap"></div>
          </article>
```

Dialog (next to `#manualIncomeDialog`, same structure):

```html
  <dialog id="withdrawalDialog" class="dialog">
    <h3>Add 529 withdrawal</h3>
    <p class="muted-note">Withdrawals you took from the 529 plan, for same-year reconciliation.</p>
    <label>
      Date
      <input id="withdrawalDate" type="date" />
    </label>
    <label>
      Amount
      <input id="withdrawalAmount" type="number" step="0.01" min="0.01" inputmode="decimal" />
    </label>
    <label>
      Description
      <input id="withdrawalNotes" type="text" placeholder="e.g. Fall housing payment" />
    </label>
    <div class="dialog-actions">
      <button id="withdrawalCancel" class="ghost-btn" type="button">Cancel</button>
      <button id="withdrawalSave" class="primary-btn" type="button">Save</button>
    </div>
  </dialog>
```

- [ ] **Step 2: app.js**

State (after `state.tax`): `spend529: { year: new Date().getFullYear(), withdrawals: [] },`

```js
async function loadWithdrawals529() {
  try {
    const payload = await apiGet("/api/529/withdrawals");
    state.spend529.withdrawals = Array.isArray(payload.withdrawals) ? payload.withdrawals : [];
    render529Reconciliation();
  } catch (e) {
    showFlash(`Failed to load 529 withdrawals: ${e.message}`);
  }
}

function render529Reconciliation() {
  const y = state.spend529.year;
  const fromTs = Math.floor(Date.UTC(y, 0, 1) / 1000);
  const toTs = Math.floor(Date.UTC(y, 11, 31, 23, 59, 59) / 1000);
  const inYear = (r) => r.date >= fromTs && r.date <= toTs;

  const qualified = Object.values(state.spend.tags529)
    .filter((t) => t.status === "qualified" && inYear(t))
    .reduce((sum, t) => sum + (t.qualified_amount || 0), 0);
  const yearWithdrawals = state.spend529.withdrawals.filter(inYear);
  const withdrawn = yearWithdrawals.reduce((sum, w) => sum + (w.amount || 0), 0);
  const headroom = qualified - withdrawn;

  const line = document.getElementById("summary529Reconciliation");
  if (line) {
    line.innerHTML = `${escapeHtml(String(y))}: qualified ${escapeHtml(currency(qualified))} · withdrawn ${escapeHtml(currency(withdrawn))} · headroom <span class="${headroom < 0 ? "negative" : ""}">${escapeHtml(currency(headroom))}</span>`;
  }

  const host = document.getElementById("withdrawals529Table");
  if (!host) return;
  if (yearWithdrawals.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">No withdrawals recorded for ${escapeHtml(String(y))}.</p>`;
    return;
  }
  const rows = yearWithdrawals
    .sort((a, b) => b.date - a.date)
    .map((w) => `<tr>
      <td>${dateLabel(w.date)}</td>
      <td>${escapeHtml(w.notes || "—")}</td>
      <td class="num">${currency(w.amount)}</td>
      <td><button class="ghost-btn withdrawal-remove-btn" type="button" data-key="${escapeHtml(w.key)}">Remove</button></td>
    </tr>`)
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Description</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  host.querySelectorAll(".withdrawal-remove-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      try {
        await apiPost("/api/529/withdrawal", { key: btn.dataset.key, status: "none" });
        await loadWithdrawals529();
      } catch (e) {
        showFlash(`Remove failed: ${e.message}`);
      } finally {
        btn.disabled = false;
      }
    });
  });
}
```

Wiring (init block, next to the other 529 wiring): populate `#tax529YearSelect` (current..current−3) with change → `state.spend529.year = ...; render529Reconciliation();`. Wire `#addWithdrawalBtn`/dialog exactly like the manual-income dialog (default date today, validations: date regex, amount > 0, notes non-empty; save → `apiPost("/api/529/withdrawal", {date, amount, notes})` → `loadWithdrawals529()` → close; `Add withdrawal failed:` flash prefix; disable save during submit).

Hook into data flow: at the end of `refresh529Tags()`'s try block and inside `loadSpendData` after `renderSpendAnalysis()` (and when the 529 tab renders), call `render529Reconciliation()`; call `loadWithdrawals529()` once when the 529 tab is opened (`setSpendTab`'s "529" branch) — it re-renders reconciliation itself.

- [ ] **Step 3: Cache bumps + verify**

Bump `app.js?v=20260805i` → `?v=20260805j`; CACHE_NAME v13 → v14. `node --check web/app.js`. Curl-create a withdrawal, reload browser if possible (else note controller verification): reconciliation line shows the year math, panel lists it, Remove works, year selector switches. Clean up test withdrawals.

- [ ] **Step 4: Commit**

```bash
git add web/index.html web/app.js web/sw.js
git commit -m "Add 529 withdrawal reconciliation panel and year selector

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Cash-flow panel

**Files:**
- Modify: `web/index.html`, `web/app.js`, `web/sw.js`

**Interfaces:**
- Consumes: `/api/income`, `bucketSpendTransactions(transactions, bucket)` (returns `[{key,label,total,byPrimary}]` sorted by key), `destroyChart`, `state.charts`, `loadSpendData`, existing range/bucket selectors.
- Produces: `state.spend.incomeTxs`; `#cashFlowChart` canvas; `renderCashFlow()` called from `renderSpendAnalysis()`.

- [ ] **Step 1: index.html**

In `#spendAnalysisPane`, after the "By Category" panel:

```html
          <article class="panel chart-panel">
            <div class="panel-head">
              <h3>Cash Flow</h3>
            </div>
            <canvas id="cashFlowChart" aria-label="Income vs spending"></canvas>
          </article>
```

- [ ] **Step 2: app.js**

`state.spend` gains `incomeTxs: [],`. In `loadSpendData`'s `Promise.all`, add `apiGet(\`/api/income?from=${from}&to=${to}\`)` and store `state.spend.incomeTxs = Array.isArray(incomePayload.transactions) ? incomePayload.transactions : [];`.

Add after `renderSpendCategoryTable`-area helpers:

```js
function renderCashFlow() {
  const canvas = document.getElementById("cashFlowChart");
  if (!canvas) return;
  destroyChart("cashFlow");

  const spendBuckets = bucketSpendTransactions(state.spend.transactions || [], state.spend.bucket);
  const incomeBuckets = bucketSpendTransactions(state.spend.incomeTxs || [], state.spend.bucket);

  const keys = Array.from(new Set([
    ...spendBuckets.map((b) => b.key),
    ...incomeBuckets.map((b) => b.key)
  ])).sort((a, b) => a - b);
  const spendByKey = new Map(spendBuckets.map((b) => [b.key, b]));
  const incomeByKey = new Map(incomeBuckets.map((b) => [b.key, b]));
  const labels = keys.map((k) => (spendByKey.get(k) || incomeByKey.get(k)).label);
  const spending = keys.map((k) => Number(((spendByKey.get(k) || {}).total || 0).toFixed(2)));
  const income = keys.map((k) => Number(((incomeByKey.get(k) || {}).total || 0).toFixed(2)));
  const net = keys.map((k, i) => Number((income[i] - spending[i]).toFixed(2)));

  state.charts.cashFlow = new Chart(canvas, {
    data: {
      labels,
      datasets: [
        { type: "bar", label: "Income", data: income, backgroundColor: "#60d394" },
        { type: "bar", label: "Spending", data: spending, backgroundColor: "#ee6055" },
        { type: "line", label: "Net", data: net, borderColor: "#222", backgroundColor: "#222", tension: 0.2, pointRadius: 2 }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: { legend: { labels: { color: "#555" } } },
      scales: {
        x: { ticks: { color: "#555" }, grid: { display: false } },
        y: {
          ticks: { callback: (v) => compactCurrency(v), color: "#555" },
          grid: { color: "rgba(34, 34, 34, 0.08)" }
        }
      }
    }
  });
}
```

Call `renderCashFlow();` at the end of `renderSpendAnalysis()`. (Inspect the existing chart constructions around `createSpendBarChart` first and match their option conventions if they differ from the above — the existing code is authoritative for styling.)

- [ ] **Step 3: Cache bumps + verify**

Bump `?v=` → `20260805k`, CACHE_NAME → v15. `node --check web/app.js`. Verify data plumbing via curl (`/api/income` for the page's default 3M range returns transactions). Visual check falls to the controller; note it.

- [ ] **Step 4: Commit**

```bash
git add web/index.html web/app.js web/sw.js
git commit -m "Add income vs spending cash-flow panel to Analysis

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Nightly in-server backup

**Files:**
- Modify: `src/web_server.cpp` (server startup section, ~:6050), `.gitignore`

**Interfaces:**
- Produces: a detached thread started with the other startup threads: snapshots `data/` → `backups/data-YYYY-MM-DD.tar.gz`, prunes to newest 14, runs at startup and daily thereafter.

- [ ] **Step 1: Implement**

Add `backups/` to `.gitignore`. In the anonymous namespace (near the other helpers):

```cpp
    // Daily tar snapshot of data/ (tags, receipts, tax records, portfolios).
    // Runs in a detached thread: once at startup, then once per wall-clock day.
    // Never throws into the server; failures only log to stderr (pm2 logs).
    void runDataBackup()
    {
        std::error_code ec;
        std::filesystem::create_directories("backups", ec);
        if (ec)
        {
            std::cerr << "Backup: failed to create backups/ directory" << std::endl;
            return;
        }

        const time_t now = std::time(nullptr);
        std::tm tm_utc{};
        gmtime_r(&now, &tm_utc);
        char date_buf[16];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
        const std::string snapshot = std::string("backups/data-") + date_buf + ".tar.gz";
        if (std::filesystem::exists(snapshot))
        {
            return; // today's snapshot already taken (restart-safe)
        }

        const std::string temp_snapshot = snapshot + ".tmp";
        const std::string cmd = "/usr/bin/tar -czf '" + temp_snapshot + "' data 2>/dev/null";
        if (std::system(cmd.c_str()) != 0)
        {
            std::cerr << "Backup: tar failed for " << snapshot << std::endl;
            std::filesystem::remove(temp_snapshot, ec);
            return;
        }
        std::filesystem::rename(temp_snapshot, snapshot, ec);
        if (ec)
        {
            std::cerr << "Backup: failed to finalize " << snapshot << std::endl;
            return;
        }
        std::cout << "Backup: wrote " << snapshot << std::endl;

        // Prune to the newest 14 snapshots (names sort chronologically).
        std::vector<std::string> snapshots;
        for (const auto& entry : std::filesystem::directory_iterator("backups", ec))
        {
            const std::string filename = entry.path().filename().string();
            if (filename.rfind("data-", 0) == 0 && filename.size() > 7 &&
                filename.substr(filename.size() - 7) == ".tar.gz")
            {
                snapshots.push_back(filename);
            }
        }
        std::sort(snapshots.begin(), snapshots.end());
        while (snapshots.size() > 14)
        {
            std::filesystem::remove(std::filesystem::path("backups") / snapshots.front(), ec);
            std::cout << "Backup: pruned " << snapshots.front() << std::endl;
            snapshots.erase(snapshots.begin());
        }
    }
```

In the server startup section, after the existing daily-sync thread, add (copying the wall-clock-polling pattern documented there — poll every few minutes, act when the UTC day changes):

```cpp
    // Nightly data/ snapshot with 14-day retention. Wall-clock polled for the
    // same suspend-safety reasons as the daily sync thread above.
    std::thread(
        []()
        {
            long long last_backup_day = -1;
            while (true)
            {
                const time_t now = std::time(nullptr);
                const long long current_day = static_cast<long long>(now / 86400);
                if (current_day != last_backup_day)
                {
                    runDataBackup();
                    last_backup_day = current_day;
                }
                std::this_thread::sleep_for(std::chrono::minutes(5));
            }
        }
    ).detach();
```

(Startup run is implicit: the first loop iteration fires immediately. Inspect the existing daily-sync thread for the exact sleep helper/duration convention used and match it.)

- [ ] **Step 2: Build, deploy, verify**

`make && pm2 restart 3`, wait 5s:

```bash
ls -la backups/
# Expected: data-<today>.tar.gz exists and is non-trivially sized
tar -tzf backups/data-*.tar.gz | head -5
# Expected: data/... entries including data/529/tags.json
pm2 restart 3 && sleep 5 && ls backups/ | wc -l
# Expected: still ONE snapshot for today (restart-safe skip)
# Prune check without waiting 14 days: create dummy old snapshots and re-trigger
for i in $(seq -w 1 20); do touch "backups/data-2025-01-$i.tar.gz"; done
rm backups/data-$(date -u +%F).tar.gz && pm2 restart 3 && sleep 5
ls backups/ | wc -l
# Expected: 14 (newest kept, oldest pruned) — then clean the dummies:
ls backups/
rm backups/data-2025-*.tar.gz 2>/dev/null; ls backups/
# Expected: only today's real snapshot remains
```

- [ ] **Step 3: Regression + commit**

`make test-529 && make test-persistence` pass.

```bash
git add src/web_server.cpp .gitignore
git commit -m "Add nightly data backups with 14-day retention

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Optional API bearer token + final verification

**Files:**
- Modify: `src/web_server.cpp` (CORS :2600, `routeRequest` :4648), `web/index.html` (API settings panel), `web/app.js` (fetch helpers), `web/sw.js`

**Interfaces:**
- Produces: `API_TOKEN` env gate on all `/api/*` except `/api/health` and OPTIONS; frontend token storage `localStorage["portfolio-api-token"]` + `state.apiToken` + `authHeaders()` helper merged into every request.

- [ ] **Step 1: Backend gate**

CORS line 2600: `Content-Type` → `Content-Type, Authorization`.

In the anonymous namespace:

```cpp
    // Constant-time comparison so token checking doesn't leak length/prefix
    // timing. Both sides are small; XOR-accumulate over the full provided value.
    bool tokenMatches(const std::string& provided, const std::string& expected)
    {
        if (provided.size() != expected.size())
        {
            return false;
        }
        volatile unsigned char acc = 0;
        for (size_t i = 0; i < provided.size(); ++i)
        {
            acc = static_cast<unsigned char>(acc | (provided[i] ^ expected[i]));
        }
        return acc == 0;
    }
```

At the top of `routeRequest`, AFTER the OPTIONS 204 return and AFTER the `/api/health` return, insert:

```cpp
        // Optional bearer-token auth: opt-in via API_TOKEN in the environment.
        // Health stays open for pm2 checks; OPTIONS preflights returned above.
        const char* api_token = std::getenv("API_TOKEN");
        if (api_token != nullptr && api_token[0] != '\0')
        {
            const auto auth_it = request.headers.find("authorization");
            const std::string expected = std::string("Bearer ") + api_token;
            if (auth_it == request.headers.end() || !tokenMatches(auth_it->second, expected))
            {
                return makeJsonResponse(401, makeErrorBody("Missing or invalid API token"));
            }
        }
```

(Confirm request.headers keys are lowercased — the JSON body parsing already looks up "content-type" lowercase, so they are; verify once in parseHttpRequest.) `#include <cstdlib>` if not present.

- [ ] **Step 2: Frontend**

index.html API settings form — after the Base URL input:

```html
        <label for="apiTokenInput">API token (optional)</label>
        <input id="apiTokenInput" name="apiToken" type="password" placeholder="Leave empty if the server has no API_TOKEN" autocomplete="off" />
```

app.js:
- `const API_TOKEN_STORAGE_KEY = "portfolio-api-token";` next to `API_STORAGE_KEY`; `state.apiToken = localStorage.getItem(API_TOKEN_STORAGE_KEY) || "";` next to `state.apiBase`.
- Helper next to `apiUrl`:

```js
function authHeaders(extra) {
  const headers = Object.assign({}, extra || {});
  if (state.apiToken) {
    headers["Authorization"] = `Bearer ${state.apiToken}`;
  }
  return headers;
}
```

- Thread it through EVERY fetch: `apiGet` and `apiGetWithTimeout` gain `headers: authHeaders()` in their fetch options; `apiPost`/`apiPatch` change `headers: { "Content-Type": "application/json" }` → `headers: authHeaders({ "Content-Type": "application/json" })`; `apiDelete` gains `headers: authHeaders()`; `apiUploadReceiptTo` wraps its Content-Type object in `authHeaders({...})`; the raw fetches in `download529Export` and `downloadTaxExport` gain `{ headers: authHeaders() }`.
- API settings form save handler: also read `#apiTokenInput`, store trimmed value to `state.apiToken` + localStorage (remove the key when empty); reset handler clears both; on init, populate the input from state like the base URL is.
- 401 hint requirement: no extra code needed — the server's 401 body `"Missing or invalid API token"` flows through the existing `data.error` → thrown Error → caller `showFlash` path, which satisfies the spec's "401 flashes a hint" requirement. Confirm this by observing the flash during verification.

- [ ] **Step 3: Cache bumps, build, deploy, verify**

Bump `?v=` → `20260805l`, CACHE_NAME → v16. `node --check web/app.js`. `make && pm2 restart 3` (token unset):

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/api/portfolios   # 200 (no token configured — open)
# Enable auth for the test WITHOUT touching .env permanently:
API_TOKEN=testsecret pm2 restart 3 --update-env && sleep 2
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/api/portfolios                                   # 401
curl -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer testsecret" http://localhost:8080/api/portfolios   # 200
curl -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer wrong" http://localhost:8080/api/portfolios        # 401
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/api/health                                        # 200 (exempt)
# Restore open mode:
API_TOKEN= pm2 restart 3 --update-env && sleep 2
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/api/portfolios   # 200
```

(If `--update-env` with an empty value doesn't clear it, use `pm2 restart 3 --update-env` after `unset API_TOKEN` in the invoking shell, and verify the 200. Report exactly what worked.)

- [ ] **Step 4: Full final verification**

```bash
make clean && make && make test-529 && make test-persistence
pm2 restart 3 && sleep 2 && curl -s http://localhost:8080/api/health
# Feature smoke: withdrawals GET, /api/income, 529 tags, tax tags, exports — all 200
```

Note in the report that the browser walkthrough (reconciliation panel, cash-flow chart, token field, display renames) falls to the controller.

- [ ] **Step 5: Commit**

```bash
git add src/web_server.cpp web/index.html web/app.js web/sw.js
git commit -m "Add optional API bearer-token auth

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

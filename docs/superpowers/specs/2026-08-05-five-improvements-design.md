# Five Improvements — Design

**Date:** 2026-08-05
**Status:** Approved

Five independent improvements to the finance tracker, approved as one batch. Builds
on the 529 tracker and Spending & Tax architectures shipped earlier today (see
`2026-08-05-529-expense-tracker-design.md` and `2026-08-05-spending-tax-design.md`).

## 1. 529 withdrawal reconciliation

No 529 plan account is Plaid-connected, so withdrawals are **manual entries**,
reconciled **per calendar year** (withdrawals must match same-year qualified
expenses for tax purposes).

- **Storage:** `data/529/withdrawals.json` — array of `ExpenseTags::TagRecord`
  (same load/save, atomic). Field mapping: key `manual-<epoch>-<n>` (unique within
  file), `status "qualified"`, `amount` = `qualified_amount` = withdrawal amount,
  `account` = `"529 Plan"`, `category` = `"WITHDRAWAL"`, `notes` = user description,
  `date` = withdrawal date. Mutations under `g_529_mutex`.
- **Backend:** `GET /api/529/withdrawals` → `{"withdrawals":[...]}`;
  `POST /api/529/withdrawal` with `{date:"YYYY-MM-DD", amount>0, notes non-empty}`
  → 201 + record (validation identical to manual income entries);
  `POST /api/529/withdrawal` with `{key, status:"none"}` → remove (404 if absent).
- **Frontend (529 tab):**
  - Summary panel gains a **year selector** (`#tax529YearSelect`, current + 3 prior)
    and a reconciliation line: `Qualified $X · Withdrawn $Y · Headroom $Z` where X =
    qualified 529 amounts in that calendar year (tag dates), Y = withdrawals in that
    year, Z = X − Y (negative rendered in the theme's bad color). The existing
    range-scoped total and panels below are unchanged.
  - New **Withdrawals** panel: "＋ Add withdrawal" button → dialog (date/amount/
    description, mirror of the manual-income dialog); list of withdrawals for the
    selected year with a Remove button per row.

## 2. Cash-flow panel (Analysis tab)

- New panel under the existing Analysis charts: per-bucket **income bars** and
  **spending bars** side by side plus a **net line** (income − spending), driven by
  the page's existing range selector and weekly/monthly bucket toggle.
- Data: `loadSpendData` additionally fetches `/api/income` for the same range and
  stores `state.spend.incomeTxs`. Bucketing reuses the existing
  `bucketSpendTransactions` helper (or an equivalent generalization) applied to both
  sets.
- Chart.js mixed chart (two bar datasets + one line dataset), colors from the
  existing theme palette (`--good`-family for income, existing spend color for
  spending, `--ink` for the net line). Chart lifecycle follows the existing
  `destroyChart`/`state.charts` pattern (`state.charts.cashFlow`).

## 3. Merchant display names

- `spend_overrides.json` rules gain an optional `"display"` field:
  `{"match":"STREQPS", "category":"...", "display":"Lehigh Bookstore"}` —
  `category` and `display` are each optional, `match` required.
- `loadSpendOverrides` parses `display`; `collectSpendTransactions` and
  `collectIncomeTransactions` apply the FIRST matching rule's `display` (if any) to
  the emitted `notes` — **after** key assignment, so fingerprint keys keep hashing
  the raw notes and existing tags/keys are unaffected. Case-insensitive substring
  match, same semantics as today's category rewriting.
- Renames therefore flow everywhere downstream: Analysis drilldowns, 529/tax
  queues and browsers, and the denormalized notes captured on newly-created tags.
  Existing tag records keep the notes captured at mark time (accepted).

## 4. Nightly in-server backup

- A detached background thread in the C++ server (same pattern as the existing
  sync threads in `main.cpp`/`web_server.cpp` startup): on startup and then once
  per day, snapshot `data/` to `backups/data-YYYY-MM-DD.tar.gz` (relative to the
  working directory) via `/usr/bin/tar -czf`, then prune `backups/` to the newest
  **14** snapshots.
- Skips the snapshot if today's file already exists (restart-safe). Failures log
  to stderr (`pm2 logs`) and never affect request serving. `backups/` is
  gitignored.

## 5. Optional API bearer token

- If `API_TOKEN` is set (non-empty) in the environment (`.env`, loaded by the
  existing env loader in `main.cpp`), every `/api/*` request except `/api/health`
  requires `Authorization: Bearer <token>`; mismatch/missing → 401 JSON error.
  Unset → open access (current behavior; opt-in only).
- Token comparison is constant-time (length check + volatile accumulator XOR).
- CORS: `Access-Control-Allow-Headers` gains `Authorization`; OPTIONS preflight
  (if the server handles it) stays exempt from auth so preflights succeed.
- **Frontend:** the API Settings panel gains an "API token (optional)" password
  input stored in localStorage (`portfolio-api-token`, matching the existing
  `portfolio-api-base` convention) beside the base URL; every fetch
  helper (`apiGet`, `apiGetWithTimeout`, `apiPost`, `apiDelete`, `apiPatch`,
  `apiUploadReceiptTo`, `downloadTaxExport`, `download529Export`) attaches the
  header when a token is stored. A 401 response flashes a hint to check the token.

## Cross-cutting

- Style, cache-busting (`?v=` + `CACHE_NAME`), commit conventions, and deployment
  (`make` / `make clean && make` when headers change; `pm2 restart 3`) as
  established today.
- Verification: curl per endpoint (auth on/off round-trip, withdrawal CRUD,
  display rewrite with key stability re-check), forced backup run + prune check,
  `make test-529` + `make test-persistence`, browser walkthrough.

## Out of scope

- Connecting a 529 plan account via Plaid; automatic withdrawal import.
- In-app editor for spend_overrides rules.
- Off-site/remote backups.
- Multi-user auth, sessions, or HTTPS termination (single bearer token only).

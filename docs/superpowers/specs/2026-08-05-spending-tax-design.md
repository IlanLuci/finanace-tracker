# Spending & Tax — Design

**Date:** 2026-08-05
**Status:** Approved

## Goal

Extend the Spending page into **Spending & Tax**: mark cash-account deposits as
taxable income, mark expenses as tax-deductible (with receipts), and total both per
tax year. Builds directly on the 529 tracker architecture (sidecar tag store,
transaction fingerprint keys, review workflow) shipped earlier today — see
`2026-08-05-529-expense-tracker-design.md`.

## Decisions (from brainstorming)

- **Income flow:** review queue of deposits with Plaid `INCOME_*` categories, PLUS a
  browsable all-deposits list (with search) so peer-transfer income (Venmo etc.) can
  be marked too.
- **Deduction flow:** no auto-queue (no reliable category signal). Mark from the
  Analysis drilldown ("Deduct" button) and from a searchable expense browser on the
  Tax tab.
- **Receipts:** deductions only (charity letters, invoices). Income marks carry no
  files.
  - **REMOVED (2026-08-05, post-v1, user decision):** deduction receipts dropped
    entirely — no receipts UI on the Tax tab, `/api/tax/receipt` route deleted,
    deductions CSV no longer has a receipts column. 529 receipts are unaffected.
- **Manual income entries (added 2026-08-05, post-v1):** Plaid history doesn't
  reach before each account's sync start (earliest: 2026-03-02), so pre-sync or
  off-platform income can be added manually: "＋ Add income manually" on the
  Marked Taxable Income panel → date/amount/description → POST `/api/tax/tag`
  with `{manual:true, kind:"income", date, amount, notes}`. Creates a
  self-contained record (key `manual-<epoch>-<n>`, account "Manual", category
  "MANUAL") that flows through totals/exports via its own denormalized fields.
  Renders with a "manual" chip instead of the orphaned badge; unmark/amount-edit
  work like any record. Income only.
- **Total scope:** the Tax tab has its own tax-year dropdown (calendar year; current +
  3 prior years), independent of the page's date-range selector.
- **Amounts:** editable — marking defaults to the full transaction amount, editable
  down (partial-business phone bill, part-reimbursement deposit).
- **Layout:** third sub-tab: **Analysis | 529 Expenses | Tax**. Page renamed
  "Spending & Tax" (header, breadcrumb, dashboard button). Element IDs unchanged.
- **Export:** CSV only — one for income marks, one for deduction marks, per tax year.
  No ZIP.

## Architecture

**Parallel sidecar stores reusing the ExpenseTags module unchanged.**

- `data/tax/income.json` and `data/tax/deductions.json` — each a JSON array of the
  existing `ExpenseTags::TagRecord` struct, read/written with the existing
  `loadTags`/`saveTags` (atomic temp+rename). Field mapping: `status "qualified"` =
  marked, `"dismissed"` = dismissed from queue; `qualified_amount` = taxable /
  deductible amount; `receipts` used only in deductions.json.
- Deduction receipt files: `data/tax/receipts/<key>/<sanitized-filename>` (same
  allowlist and sanitization as 529).
- No changes to `data/529/*` or to the `expense_tags` module's schema.
- The 529 endpoints' tag-upsert and receipt-handling logic is extracted into
  store-parameterized helpers (tags-file path, receipts dir, transaction lookup)
  shared by the `/api/529/*` and `/api/tax/*` routes — refactor, not copy.

### Transaction keys

- **New income collection:** `collectIncomeTransactions(manager, from, to)` — the
  mirror of `collectSpendTransactions`: `DEPOSIT` and `INTEREST` transactions on CASH
  portfolios, excluding pending (`notesIsPending`) and `[TXFR]` own-account
  transfers. Keys assigned by `ExpenseTags::KeyAssigner` over the FULL history before
  date filtering (range-stable), in a fresh assigner instance.
- Income keys and spend keys are separate namespaces (separate endpoints, separate
  tag files). A same-day deposit and withdrawal with identical account/amount/notes
  hash to the same key harmlessly — marks are validated against and joined to their
  own transaction set only.
- Deduction marks key against the existing spend transactions (`/api/spend` keys).

## Backend endpoints (`src/web_server.cpp`)

| Endpoint | Behavior |
|---|---|
| `GET /api/income?from=&to=` | Income transactions with keys: `{key, date, amount (positive), category, notes, account}`. Same from/to parsing as `/api/spend`. |
| `GET /api/tax/tags` | `{"income":[TagRecord...], "deductions":[TagRecord...]}`. |
| `POST /api/tax/tag` | `{kind:"income"\|"deduction", key, status, amount?}`. Same semantics as `/api/529/tag`: status `qualified`/`dismissed`/`none`; amount defaults to full, validated `> 0` and `<= amount + 0.005`; `none` removes the record; denormalized fields captured at mark time from the matching transaction set (income → collectIncomeTransactions, deduction → collectSpendTransactions); 404 on unknown key. |
| `POST /api/tax/receipt?key=&filename=` | Deduction receipts only: raw body upload into `data/tax/receipts/<key>/`, appended to the deduction record. Same 25 MB cap, extension allowlist, sanitization, dedup suffixes, atomic write, orphan cleanup as 529. Requires an existing marked (`qualified`) deduction record. |
| `GET /api/tax/receipt?key=&filename=` | Serve with correct MIME. |
| `DELETE /api/tax/receipt?key=&filename=` | Remove file + record entry; 404 when neither existed. |
| `GET /api/tax/export/income.csv?year=YYYY` | CSV: date, account, source (notes), category, deposit amount, taxable amount. Marked income records dated within the calendar year, sorted by date. |
| `GET /api/tax/export/deductions.csv?year=YYYY` | CSV: date, account, merchant (notes), category, charge amount, deductible amount, receipts. Same year scoping. |

All tag/receipt mutations for the tax stores serialize under a dedicated
`g_tax_mutex` (mirroring `g_529_mutex`); CSV field escaping reuses `csvField`
(including the formula-injection guard).

## Frontend (`web/index.html`, `web/app.js`, `web/styles.css`, `web/sw.js`)

### Rename

"Spending" → "Spending & Tax": page `<h2>`, breadcrumb label, dashboard button.
Element IDs unchanged.

### Tax tab

Third sub-tab button ("Tax") and pane. Contents top to bottom:

1. **Summary panel:** tax-year `<select>` (current year + 3 prior); "Taxable income:
   $X · N marked"; "Deductible: $Y · M marked · ⚠ K missing receipts"; "Export
   income CSV" and "Export deductions CSV" buttons. All figures scoped to the
   selected calendar year using each record's denormalized date.
2. **Income — review queue:** unmarked, undismissed deposits in the tax year whose
   category starts with `INCOME_`. Rows: date, source, category chip, amount,
   **✓ Mark taxable** / **✕ Dismiss**.
3. **Income — marked list:** editable taxable amount (validated like 529), orphan
   badge, **Unmark**.
4. **Income — all deposits (collapsible):** every income transaction in the tax
   year with a client-side text filter over source/notes; unmarked rows get **✓ Mark
   taxable**. Dismissed deposits appear here and remain markable.
5. **Deductions — marked list:** editable deductible amount, receipt chips + upload
   (shared hidden file input + drag-drop, same UI as 529), missing-receipt badge,
   orphan badge, **Unmark**.
6. **Deductions — expense browser (collapsible):** all spend transactions in the tax
   year (any account type) with client-side merchant search; rows get **✓ Deduct**.

The Tax tab fetches its own data on year change: `/api/spend` and `/api/income` for
`YYYY-01-01..YYYY-12-31`, cached in `state.tax` (separate from `state.spend`), plus
`/api/tax/tags`.

### Mark dialog

The 529 qualify dialog generalizes into one mark dialog driven by a mode: title/label
text, amount default/max from the transaction, file input visible only in deduction
mode. The 529 flow keeps its existing behavior (including the academic-year gate,
which does NOT apply to tax marks).

### Analysis drilldown

Rows gain a **Deduct** button next to the existing **529** button — same
close-dialog → switch-tab → open-mark-dialog flow, targeting deduction mode.

### Exports

The two CSV buttons hit the export endpoints with the selected year and download via
the existing blob/anchor helper (`tax-income_YYYY.csv`, `tax-deductions_YYYY.csv`).

## Edge cases & error handling

- **Orphaned marks:** counted via denormalized fields, orphan badge, unmark to clean.
- **529 independence:** a charge may be both 529-qualified and deductible; the
  systems do not interact.
- **Dismissed income:** hidden from the queue, visible and markable in the
  all-deposits browser.
- **Cross-store key collision** (deposit vs withdrawal with identical tuple): benign —
  separate files, separate lookups; documented here, no code needed.
- **Upload/network failures:** same flash + refresh-in-finally patterns as 529.

## Testing

- `test_529.cpp` already covers the store round-trip; add unit checks only if helper
  extraction changes signatures (it should not — parameterization happens in
  `web_server.cpp`, not the module).
- Curl verification per endpoint (income keys' range stability, tag upsert for both
  kinds, receipt round-trip, year-scoped CSVs, cleanup).
- Browser walkthrough of the full Tax tab flow; `make clean && make`;
  `pm2 restart 3`; cache bumps (`?v=`, `CACHE_NAME`).

## Out of scope

- Tax rate/liability estimation; quarterly estimates.
- Auto-categorization of deductions.
- ZIP export of deduction receipts (CSV only, per decision).
- W-2 wage import (deposits are markable manually if desired).

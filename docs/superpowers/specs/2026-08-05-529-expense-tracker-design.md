# 529 Qualified Expense Tracker — Design

**Date:** 2026-08-05
**Status:** Approved

## Goal

Track 529-qualified expenses (groceries, other food, school supplies) by tagging
credit-card charges already synced via Plaid, attaching receipt files to them, and
summing tagged amounts into a 529 qualified-expense total. Lives on the renamed
**Spending** page (formerly "Spend Analysis").

## Decisions (from brainstorming)

- **Workflow:** Both a review queue of candidate charges AND ad-hoc tagging from the
  existing Analysis drilldown tables.
- **Amounts:** Partial qualification allowed — qualified amount defaults to the full
  charge, editable down.
- **Receipts:** Optional but flagged — charges without receipts count toward the total
  but show a "missing receipt" warning.
- **Total scope:** Follows the Spending page's date-range selector.
- **Attachments:** One or more files per charge; each file belongs to exactly one charge.
- **Upload UX:** Camera capture on mobile + drag-drop/file-pick on desktop; images and PDFs.
- **Layout:** Sub-tabs on the Spending page: "Analysis" (existing) and "529 Expenses" (new).
- **Export:** CSV of qualified expenses + ZIP (CSV + receipt files) for the selected range.

## Architecture

**Sidecar store keyed by transaction fingerprint.** Tags and receipts live in
`data/529/`, entirely separate from portfolio persistence. No changes to
`portfolio.dat` format or the `Transaction` struct's persisted fields. Plaid re-syncs
cannot wipe tags.

### Transaction keys

Transactions have no stable IDs, so the server computes a deterministic key per spend
transaction: a hash (e.g., FNV-1a) of `account|date|amount_cents|notes`, suffixed with
an occurrence index (`-0`, `-1`, …) distinguishing identical same-day duplicates,
ordered chronologically. Keys are stable because:

- Pending Plaid transactions are already excluded from spend data (`notesIsPending`).
- Posted Plaid transactions do not change.

Key computation lives server-side only. `GET /api/spend` gains a `key` field per
transaction; the frontend joins on it and never re-derives keys.

## Data model

`data/529/tags.json` — JSON array, one record per touched transaction:

```json
{
  "key": "a3f9c2-0",
  "status": "qualified",          // "qualified" | "dismissed"
  "qualified_amount": 40.00,       // only meaningful when qualified
  "receipts": ["target-receipt.jpg"],
  "account": "Capital One Balance",
  "date": 1722400000,
  "amount": 120.00,
  "notes": "Target",
  "category": "GENERAL_MERCHANDISE_SUPERSTORES",
  "created": 1722500000
}
```

Source fields (`account`/`date`/`amount`/`notes`/`category`) are denormalized for
export and orphan handling; live joins use `key` only.

Receipt files: `data/529/receipts/<key>/<sanitized-filename>`. Allowed extensions:
jpg/jpeg, png, heic, webp, pdf.

All writes to `tags.json` are atomic (write temp file, then rename), matching the
repo's persistence-hardened patterns.

## Backend endpoints (`src/web_server.cpp`)

| Endpoint | Behavior |
|---|---|
| `GET /api/529/tags` | Return all tag records. |
| `POST /api/529/tag` | Upsert `{key, status, qualified_amount}`. `status:"none"` removes the record (receipt files stay on disk until deleted explicitly). Denormalized source fields are captured server-side from spend data at tag time. |
| `POST /api/529/receipt?key=&filename=` | Raw file bytes as request body (no multipart). Saves under the key's receipt dir and appends to the tag's `receipts` list. |
| `GET /api/529/receipt?key=&filename=` | Serve the file with correct MIME type. |
| `DELETE /api/529/receipt?key=&filename=` | Delete the file and remove it from the tag record. |
| `GET /api/529/export.csv?from=&to=` | CSV: date, account, merchant (notes), category, charge amount, qualified amount, receipt filenames. Qualified records in range only. |
| `GET /api/529/export.zip?from=&to=` | ZIP containing the CSV plus all receipt files for the range, built by shelling out to macOS's bundled `zip`. Receipt files prefixed `YYYY-MM-DD_<merchant>_` so the folder is self-explanatory. |

Upload constraints: 25 MB max per file; extension allowlist; filenames sanitized
(strip path separators, dotfiles, traversal). Duplicate filename on the same charge
gets a numeric suffix rather than overwriting.

## Frontend (`web/index.html`, `web/app.js`, `web/styles.css`)

### Rename

"Spend Analysis" → "Spending" everywhere: nav/dashboard buttons, page header, any
labels referencing the old name. Internal IDs (`spendView`, etc.) stay as-is.

### Page structure

Sub-tabs under the Spending header: **Analysis** (existing charts + tables, unchanged)
and **529 Expenses** (new). The existing date-range/bucket selectors remain page-level
and drive both tabs.

### 529 Expenses tab

- **Summary card:** qualified total for the selected range, tagged-charge count, and a
  "missing receipts: N" warning chip when any qualified charge in range has no files.
- **Review queue:** spend transactions in range that are (a) on DEBT (credit-card)
  accounts, (b) untagged and undismissed, and (c) in a candidate category:
  - Groceries: `FOOD_AND_DRINK_GROCERIES`
  - Other food: all other `FOOD_AND_DRINK_*` **except** `FOOD_AND_DRINK_BEER_WINE_AND_LIQUOR`
  - School supplies: `GENERAL_MERCHANDISE_OFFICE_SUPPLIES`,
    `GENERAL_MERCHANDISE_BOOKSTORES_AND_NEWSSTANDS`, `GENERAL_MERCHANDISE_SUPERSTORES`,
    `GENERAL_MERCHANDISE_DEPARTMENT_STORES`, `GENERAL_MERCHANDISE_DISCOUNT_STORES`,
    `GENERAL_MERCHANDISE_OTHER_GENERAL_MERCHANDISE`

  Each row: date, merchant, category, amount, **✓ Qualify** (opens inline editor:
  qualified amount defaulting to full charge + receipt upload) and **✕ Dismiss**.
- **Qualified list:** tagged charges in range with editable qualified amount, receipt
  chips (tap to open full-size in a new tab), per-row upload control
  (`<input type="file" accept="image/*,.pdf" multiple capture="environment">` for
  mobile camera; drag-and-drop target on desktop), missing-receipt flag, untag control.
- **Export:** "Export CSV" and "Export ZIP" buttons hitting the export endpoints with
  the current range.

### Tag from Analysis drilldown

Each row in the existing category drilldown table gets a small "529" button that opens
the same qualify editor — works for any account/category, covering Plaid
miscategorizations.

## Edge cases & error handling

- **Orphaned tags:** if a tagged transaction no longer appears in spend data (Plaid
  reversal), it still counts toward the total (denormalized fields) but renders with an
  "orphaned" marker and can be untagged.
- **Duplicate charges:** occurrence-index suffix keeps keys distinct; chronological
  ordering makes indices deterministic.
- **Upload failures:** surface the server error in the UI; never mark a receipt
  attached unless the server confirmed the write.
- **HEIC:** accepted and stored; mobile Safari typically converts camera captures to
  JPEG. Files are served as-is.

## Testing

- Unit-style backend checks (following `test_persistence.cpp` conventions): key
  stability and duplicate-collision handling, tag upsert/remove round-trip, filename
  sanitization.
- Manual: curl the upload/serve/export endpoints; browser verification of the full
  flow on desktop and phone.
- Build/deploy: `make clean && make` (headers may change), then `pm2 restart 3`.

## Out of scope (v1)

- OCR / auto-matching receipts to charges.
- 529 withdrawal/reimbursement reconciliation.
- Per-tax-year reporting beyond what the date-range selector provides.

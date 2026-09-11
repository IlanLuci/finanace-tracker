# Balance-vs-Ledger Reconciliation Engine

**Date:** 2026-09-10
**Status:** Approved (bounded→architectural; brainstormed & approved in chat)

## Problem

Account cash balances are anchored to Plaid's reported `current_balance` at
sync time (`syncInvestmentAccount` / `syncCashAccount` →
`setAvailableCapital(anchor)`). During a same-owner transfer between two
connected accounts, Plaid updates the two legs at different times: the
destination's balance rises immediately while the source's balance does not
fall until the debit posts (hours–days later). During that window the same
money is counted in both accounts, inflating Total Assets.

Concrete case that triggered this: a pending $5,700 transfer from
`Vanguard_Brokeridge` → `Vanguard_Roth_IRA`. The Roth's cash already includes
the +$5,700; the brokerage's cash has not dropped. No transaction row exists in
either account for the move (investment sync emits no pending/`[TXFR]` rows), so
the existing `computeInTransit` reconciliation (cash-account `[TXFR]` pairs
only) cannot see it. The double-count lives purely in the two balance anchors.

## Principle

Cash (`available_capital` = settlement + money-market) only moves via events
that are transactions (deposits, withdrawals, trades, dividends, interest).
Market swings hit holdings value, not cash. Therefore, between syncs, the cash
delta should equal the sum of new cash-affecting transactions; any residual is
an **unexplained balance movement**.

Only count balance the ledger can explain. An unexplained cash **increase** is
provisional; if it is a same-owner transfer it is double-counted and must be
held out of Total Assets until the transfer finalizes. Real external deposits
must NOT be hidden — so an unexplained increase is held out only after the user
confirms it is a transfer (one tap). Unexplained debits are out of scope here.

## Data model

New sidecar `data/reconciliation.json` (follows `spend_overrides.json`; MUST be
added to the nightly backup set):

```json
{
  "snapshots": {
    "<portfolio_name>": { "anchor": <cash>, "synced_at": <unix> }
  },
  "events": [
    {
      "id": "<string>",
      "dest_account": "<portfolio_name>",
      "amount": <double>,
      "detected_at": <unix>,
      "dest_anchor_at_detection": <double>,
      "status": "pending | transfer | deposit | cleared",
      "source_account": "<portfolio_name|null>",
      "source_anchor_at_confirm": <double>,
      "cleared_at": <unix>,
      "clear_reason": "source_dropped | txn_posted | reverted | expired | dismissed | manual"
    }
  ]
}
```

## Components

### 1. Detection — hook at end of each account sync
After the sync computes `new_anchor`:
```
snap            = snapshots[account]         // may be absent on first ever sync
balance_delta   = new_anchor - snap.anchor
explained_delta = Σ cash-amount of txns with date >= snap.synced_at
unexplained     = balance_delta - explained_delta
if snap exists and unexplained >= DETECT_FLOOR ($50):
    create pending event { dest=account, amount=unexplained, ... }
snapshots[account] = { anchor: new_anchor, synced_at: now }
```
First observation of an account (no prior snapshot) only records a baseline —
no event. `$50` floor absorbs dividend / MM-sweep timing noise. Credits only.

### 2. Classification — one tap
`GET /api/reconciliation` returns pending + active-transfer events. Dashboard
renders pending events: "$X appeared in <dest> with no matching transaction."
Buttons: pick a source account (→ transfer) or "Real deposit" (→ dismiss).
`POST /api/reconciliation/{id}/classify` body
`{ "status": "transfer", "source_account": "<name>" }` or `{ "status": "deposit" }`.
Also `POST /api/reconciliation` to **manually create** a transfer event
`{ source_account, dest_account, amount }` — used to seed transfers that
predate tracking (the current $5,700).

### 3. Read-time application
In `buildPortfolioSummaryJson`, for each `status=transfer` event subtract
`amount` from the **source** account's `estimated_total_value` and displayed
`available_capital`. The money then shows only in the destination. Applied at
read time → survives syncs.

### 4. Auto-clear — evaluated on sync and on read
A transfer event clears (status→cleared) when any of:
- source cash dropped ≥ amount − TOL since confirm → `source_dropped` (primary)
- dest gained ≥ amount − TOL of new credit txns since detection → `txn_posted`
- dest anchor reverted down ≥ amount − TOL → `reverted`
- now − detected_at > 14 days → `expired` (backstop)

Pending (unconfirmed) events auto-dismiss to `deposit` after 7 days, or drop if
the dest anchor reverts before confirmation. `TOL = max($1, 0.5% of amount)`.

## Testing (TDD)
`test_reconciliation.cpp` + Makefile `test-reconciliation` target, mirroring
`test_persistence`. Failing-first. Cases: baseline-only on first sync; detect
credit above floor; ignore sub-floor noise; ignore transaction-explained delta;
confirm transfer applies hold-out; each clear path (source_dropped, txn_posted,
reverted, expired); pending auto-dismiss; manual create.

## Scope boundaries (YAGNI)
- Credits only; unexplained debits out of scope.
- No automatic cross-account matching — user confirms (one tap).
- Source picker reuses the existing connected-account list.

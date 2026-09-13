#include "reconciliation.hpp"

#include <iostream>
#include <cmath>
#include <string>

using namespace Reconciliation;

static int g_failures = 0;

static void check(bool cond, const std::string& label)
{
    if (cond)
    {
        std::cout << "  ✓ " << label << std::endl;
    }
    else
    {
        std::cout << "  ✗ " << label << std::endl;
        ++g_failures;
    }
}

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Simple in-memory AnchorLookup for sweep tests.
struct MockLookup : public AnchorLookup
{
    std::map<std::string, double> anchors;
    std::map<std::string, double> credits;  // creditSince result keyed by account

    bool anchor(const std::string& account, double& out) const override
    {
        auto it = anchors.find(account);
        if (it == anchors.end()) return false;
        out = it->second;
        return true;
    }
    double creditSince(const std::string& account, time_t) const override
    {
        auto it = credits.find(account);
        return it == credits.end() ? 0.0 : it->second;
    }
};

static const time_t DAY = 86400;
static const time_t T0 = 1789000000;  // fixed base time (no wall clock in tests)

int main()
{
    std::cout << "=== Reconciliation engine tests ===" << std::endl;

    // 1. First observation only records a baseline snapshot, never an event.
    {
        State s;
        std::string id = observe(s, "Roth", 9542.45, 0.0, T0, "evt1");
        check(id.empty(), "first sync creates no event");
        check(s.events.empty(), "no events after baseline");
        check(s.snapshots.count("Roth") == 1, "baseline snapshot stored");
        check(approx(s.snapshots["Roth"].anchor, 9542.45), "baseline anchor recorded");
    }

    // 2. An unexplained credit above the floor is detected.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "evtA");            // baseline
        std::string id = observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evtB"); // +5700, no ledger
        check(id == "evtB", "unexplained +5700 detected");
        check(s.events.size() == 1, "one event created");
        check(approx(s.events[0].amount, 5700.0), "event amount = unexplained delta");
        check(s.events[0].status == EventStatus::Pending, "new event is pending");
        check(s.events[0].dest_account == "Roth", "dest account recorded");
    }

    // 3. Sub-floor residual (e.g. dividend timing) is ignored.
    {
        State s;
        observe(s, "Roth", 100.0, 0.0, T0, "b");
        std::string id = observe(s, "Roth", 140.0, 0.0, T0 + DAY, "c"); // +40 < $50 floor
        check(id.empty() && s.events.empty(), "sub-floor delta ignored");
    }

    // 4. A balance change fully explained by the ledger produces no event.
    {
        State s;
        observe(s, "Broker", 1000.0, 0.0, T0, "b");
        std::string id = observe(s, "Broker", 3000.0, 2000.0, T0 + DAY, "c"); // +2000 all explained
        check(id.empty() && s.events.empty(), "ledger-explained credit ignored");
    }

    // 5. Confirm transfer -> hold-out applied against the SOURCE account.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt");
        bool ok = classifyTransfer(s, "evt", "Broker", 8887.68, T0 + DAY);
        check(ok, "classifyTransfer succeeds");
        check(s.events[0].status == EventStatus::Transfer, "status now Transfer");
        check(approx(heldOutTotal(s), 5700.0), "held-out total = 5700");
        auto by = heldOutBySource(s);
        check(approx(by["Broker"], 5700.0), "held out against source Broker");
    }

    // 6. Classify as real deposit -> no hold-out.
    {
        State s;
        observe(s, "Roth", 0.0, 0.0, T0, "b");
        observe(s, "Roth", 5700.0, 0.0, T0 + DAY, "evt");
        bool ok = classifyDeposit(s, "evt", T0 + DAY);
        check(ok, "classifyDeposit succeeds");
        check(s.events[0].status == EventStatus::Deposit, "status now Deposit");
        check(approx(heldOutTotal(s), 0.0), "deposit is not held out");
    }

    // 7. Clear path: source cash drops by ~amount (Plaid debited the source).
    {
        State s;
        std::string id = createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 9542.45, T0, "m");
        check(approx(heldOutTotal(s), 5700.0), "manual transfer held out immediately");
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68 - 5700.0;   // source finally dropped
        lk.anchors["Roth"]   = 9542.45;            // dest unchanged (no dest_rose)
        bool changed = sweep(s, lk, T0 + DAY);
        check(changed, "sweep reports change");
        check(s.events[0].status == EventStatus::Cleared, "transfer cleared");
        check(s.events[0].clear_reason == "source_dropped", "reason = source_dropped");
        check(approx(heldOutTotal(s), 0.0), "no hold-out after clear");
    }

    // 8. Clear path: destination deposit finally posts as a transaction.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt");
        classifyTransfer(s, "evt", "Broker", 8887.68, T0 + DAY);
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;            // source still high
        lk.anchors["Roth"]   = 9542.45;
        lk.credits["Roth"]   = 5700.0;             // dest deposit posted
        bool changed = sweep(s, lk, T0 + 2 * DAY);
        check(changed && s.events[0].status == EventStatus::Cleared, "cleared on txn_posted");
        check(s.events[0].clear_reason == "txn_posted", "reason = txn_posted");
    }

    // 9. Clear path: destination anchor reverts (transfer cancelled).
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt");
        classifyTransfer(s, "evt", "Broker", 8887.68, T0 + DAY);
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;
        lk.anchors["Roth"]   = 3842.45;            // reverted to pre-credit level
        bool changed = sweep(s, lk, T0 + 2 * DAY);
        check(changed && s.events[0].clear_reason == "reverted", "cleared on revert");
    }

    // 10. Backstop: a stale transfer clears after TRANSFER_EXPIRY_BUSINESS_DAYS.
    {
        State s;
        createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 9542.45, T0, "m");
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;            // never dropped
        lk.anchors["Roth"]   = 9542.45;            // dest unchanged (no dest_rose)
        bool changed = sweep(s, lk, T0 + 7 * DAY); // well past 3 business days
        check(changed && s.events[0].clear_reason == "expired", "stale transfer expires");
    }

    // 10a. Business-day backstop does NOT count a weekend as settlement time.
    {
        const time_t FRI = T0 + DAY;               // T0 is a Thursday, so T0+1d is Friday
        State s;
        createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 9542.45, FRI, "m");
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;            // never dropped
        lk.anchors["Roth"]   = 9542.45;            // no dest_rose
        // +3 calendar days = Monday -> only 1 business day elapsed, must NOT expire.
        bool over_weekend = sweep(s, lk, FRI + 3 * DAY);
        check(!over_weekend && s.events[0].status == EventStatus::Transfer,
              "3 calendar days across a weekend does not expire");
        // +5 calendar days = Wednesday -> 3 business days elapsed, expires.
        bool by_wed = sweep(s, lk, FRI + 5 * DAY);
        check(by_wed && s.events[0].clear_reason == "expired",
              "3 business days later the transfer expires");
    }

    // 10b. businessDaysBetween counts weekdays in (from, to]. T0 is a Thursday.
    {
        check(businessDaysBetween(T0, T0) == 0, "no elapsed time -> 0 business days");
        check(businessDaysBetween(T0, T0 + 1 * DAY) == 1, "Thu->Fri = 1 business day");
        check(businessDaysBetween(T0, T0 + 2 * DAY) == 1, "Thu->Sat still 1 (Sat excluded)");
        check(businessDaysBetween(T0, T0 + 3 * DAY) == 1, "Thu->Sun still 1 (Sun excluded)");
        check(businessDaysBetween(T0, T0 + 4 * DAY) == 2, "Thu->Mon = 2 business days");
        check(businessDaysBetween(T0, T0 + 5 * DAY) == 3, "Thu->Tue = 3 business days");
    }

    // 10c. dest_rose: a manual transfer clears when the destination balance rises
    //      by ~amount, even though the source never dropped and no txn posted
    //      (the Vanguard settlement-fund case that used to get stuck).
    {
        State s;
        createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 3842.45, T0, "m");
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;            // source cash never visibly dropped
        lk.anchors["Roth"]   = 3842.45 + 5700.0;   // money landed in the destination
        bool changed = sweep(s, lk, T0 + DAY);
        check(changed && s.events[0].status == EventStatus::Cleared,
              "manual transfer clears when destination balance rises by ~amount");
        check(s.events[0].clear_reason == "dest_rose", "reason = dest_rose");
        check(approx(heldOutTotal(s), 0.0), "no hold-out after dest_rose");
    }

    // 10d. dest_rose must NOT fire for an auto-detected transfer, whose baseline
    //      is already the post-arrival (elevated) anchor.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt"); // dest_anchor_at_detection = 9542.45
        classifyTransfer(s, "evt", "Broker", 8887.68, T0 + DAY);
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;            // source not dropped
        lk.anchors["Roth"]   = 9542.45;            // dest steady at its elevated level
        bool changed = sweep(s, lk, T0 + 2 * DAY);
        check(!changed && s.events[0].status == EventStatus::Transfer,
              "auto-detected transfer not spuriously cleared by dest_rose");
    }

    // 10e. dest_rose is disabled when the baseline is unknown (0), so a genuine
    //      balance can't be mistaken for a rise-from-zero.
    {
        State s;
        createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 0.0, T0, "m");
        MockLookup lk;
        lk.anchors["Broker"] = 8887.68;
        lk.anchors["Roth"]   = 5700.0;             // looks like a rise from 0, but baseline unknown
        bool changed = sweep(s, lk, T0 + DAY);
        check(!changed && s.events[0].status == EventStatus::Transfer,
              "dest_rose disabled when baseline unknown (0)");
    }

    // 11. Pending auto-dismiss to deposit after PENDING_EXPIRY_DAYS.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt");
        MockLookup lk;
        lk.anchors["Roth"] = 9542.45;              // still high, never classified
        bool changed = sweep(s, lk, T0 + DAY + (PENDING_EXPIRY_DAYS + 1) * DAY);
        check(changed && s.events[0].status == EventStatus::Deposit, "stale pending -> deposit");
    }

    // 12. Serialize/parse round-trips state.
    {
        State s;
        observe(s, "Roth", 3842.45, 0.0, T0, "b");
        observe(s, "Roth", 9542.45, 0.0, T0 + DAY, "evt");
        classifyTransfer(s, "evt", "Broker", 8887.68, T0 + DAY);
        std::string json = serialize(s);
        State r = parse(json);
        check(r.snapshots.count("Roth") == 1 && r.snapshots.count("Broker") == 0,
              "snapshots round-trip");
        check(r.events.size() == 1, "events round-trip");
        check(r.events[0].id == "evt" && r.events[0].source_account == "Broker",
              "event fields round-trip");
        check(r.events[0].status == EventStatus::Transfer, "status round-trips");
        check(approx(r.events[0].amount, 5700.0), "amount round-trips");
        check(approx(heldOutTotal(r), 5700.0), "hold-out survives round-trip");
    }

    // 13a. createTransfer is idempotent for an already-active transfer.
    {
        State s;
        std::string a = createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 9542.45, T0, "m1");
        std::string b = createTransfer(s, "Broker", "Roth", 5700.0, 8887.68, 9542.45, T0, "m2");
        check(a == "m1" && b == "m1", "duplicate manual transfer returns existing id");
        check(s.events.size() == 1, "no duplicate event appended");
        check(approx(heldOutTotal(s), 5700.0), "held-out not double-counted");
    }

    // 13. Malformed JSON parses to an empty, usable state.
    {
        State r = parse("not json at all {");
        check(r.snapshots.empty() && r.events.empty(), "malformed -> empty state");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string(g_failures))
              << std::endl;
    return g_failures == 0 ? 0 : 1;
}

#pragma once

// Balance-vs-ledger reconciliation engine.
//
// Account cash (available_capital) is anchored to Plaid's reported balance each
// sync. During a same-owner transfer the two legs update at different times, so
// the destination's cash rises before the source's cash falls and the same
// money is briefly counted in both accounts, inflating Total Assets.
//
// Cash only moves via events that are transactions, so between syncs the cash
// delta should equal the sum of new cash-affecting transactions; any residual
// is an "unexplained" balance movement. An unexplained credit is provisional:
// if the user confirms it is a transfer, its amount is held out of Total Assets
// (charged against the SOURCE account) until the transfer finalizes.
//
// This module is a pure state machine plus self-contained JSON persistence. All
// ledger/anchor scanning lives in the caller and is injected via AnchorLookup,
// so the engine is unit-testable without Plaid or the portfolio store.

#include <string>
#include <vector>
#include <map>
#include <ctime>

namespace Reconciliation
{
    enum class EventStatus
    {
        Pending,   // detected unexplained credit, awaiting user classification
        Transfer,  // confirmed in-flight transfer, held out of Total Assets
        Deposit,   // classified real deposit, counts normally (record only)
        Cleared    // transfer finalized / reverted / expired
    };

    std::string statusToString(EventStatus s);
    EventStatus statusFromString(const std::string& s);

    // Last-synced cash anchor for an account; the baseline for delta detection.
    struct Snapshot
    {
        double anchor = 0.0;
        time_t synced_at = 0;
    };

    struct Event
    {
        std::string id;
        std::string dest_account;              // where the unexplained credit appeared
        double      amount = 0.0;
        time_t      detected_at = 0;
        double      dest_anchor_at_detection = 0.0;
        EventStatus status = EventStatus::Pending;
        std::string source_account;            // set on transfer confirm; "" otherwise
        double      source_anchor_at_confirm = 0.0;
        time_t      cleared_at = 0;
        std::string clear_reason;              // source_dropped|txn_posted|reverted|expired|dismissed
    };

    struct State
    {
        std::map<std::string, Snapshot> snapshots;
        std::vector<Event> events;
    };

    // Tunables.
    constexpr double DETECT_FLOOR = 50.0;      // ignore sub-$50 residual (dividend/MM noise)
    constexpr int    PENDING_EXPIRY_DAYS = 7;  // unconfirmed events auto-dismiss to deposit
    constexpr int    TRANSFER_EXPIRY_DAYS = 14; // held transfers auto-clear as a backstop

    double tolerance(double amount);           // max($1, 0.5% of |amount|)

    // Detection. Call once per account sync with the freshly-computed cash
    // anchor and the ledger-explained cash delta since the prior snapshot.
    // Creates a pending event (id = new_event_id) when the unexplained credit
    // clears DETECT_FLOOR and a prior snapshot exists. Always updates the
    // snapshot. Returns the new event id, or "" if none created.
    std::string observe(State& state, const std::string& account,
                        double new_anchor, double explained_delta,
                        time_t now, const std::string& new_event_id);

    // Classification of a pending event.
    bool classifyTransfer(State& state, const std::string& event_id,
                          const std::string& source_account,
                          double source_anchor_now, time_t now);
    bool classifyDeposit(State& state, const std::string& event_id, time_t now);

    // Manually record an already-known in-flight transfer (predates tracking).
    std::string createTransfer(State& state, const std::string& source_account,
                               const std::string& dest_account, double amount,
                               double source_anchor_now, time_t now,
                               const std::string& event_id);

    // Injected read-time view of current account state, used by sweep().
    struct AnchorLookup
    {
        virtual ~AnchorLookup() = default;
        // Sets `out` to the account's current cash anchor; returns false if unknown.
        virtual bool anchor(const std::string& account, double& out) const = 0;
        // Sum of positive cash transactions on `account` dated >= `since`.
        virtual double creditSince(const std::string& account, time_t since) const = 0;
    };

    // Auto-clear/auto-dismiss sweep. Returns true if any event changed.
    bool sweep(State& state, const AnchorLookup& lookup, time_t now);

    // Active hold-out (status == Transfer) grouped by source account, and total.
    std::map<std::string, double> heldOutBySource(const State& state);
    double heldOutTotal(const State& state);

    // Self-contained persistence. parse() returns an empty State on malformed input.
    std::string serialize(const State& state);
    State parse(const std::string& json);
}

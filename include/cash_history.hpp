#pragma once

// Persisted per-account cash-balance history.
//
// A brokerage's cash-over-time cannot be reconstructed from its transaction
// ledger when the broker moves cash through a settlement fund (e.g. Vanguard's
// VMFXX): those movements never post as transactions, so replaying the ledger
// backward from the current balance flat-fills today's cash across the past.
//
// Instead we snapshot each account's ACTUAL cash anchor once per calendar day at
// sync time and persist it as an append-only series. This is authoritative going
// forward and independent of what Plaid emits as transactions. It is a pure data
// store plus self-contained JSON persistence (unit-testable without the server).

#include <string>
#include <vector>
#include <map>
#include <ctime>

namespace CashHistory
{
    struct Point
    {
        time_t day = 0;   // UTC day-start (midnight) the snapshot belongs to
        double cash = 0.0;
    };

    // account name -> chronological points, at most one per calendar day.
    using State = std::map<std::string, std::vector<Point>>;

    // Floor a timestamp to its UTC day-start.
    time_t dayStart(time_t t);

    // Record `cash` for `account` on the day containing `now`. The latest write
    // for a given day wins (a re-sync overwrites that day's value). Points stay
    // sorted by day. Returns true if the state changed.
    bool record(State& state, const std::string& account, double cash, time_t now);

    // Self-contained persistence. parse() returns an empty State on malformed input.
    std::string serialize(const State& state);
    State parse(const std::string& json);
}

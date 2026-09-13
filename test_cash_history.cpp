#include "cash_history.hpp"

#include <iostream>
#include <cmath>
#include <string>

using namespace CashHistory;

static int g_failures = 0;

static void check(bool cond, const std::string& label)
{
    std::cout << (cond ? "  ✓ " : "  ✗ ") << label << std::endl;
    if (!cond) ++g_failures;
}

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-6; }

static const time_t DAY = 86400;
static const time_t T0 = 1789000000;  // fixed base time (no wall clock in tests)

int main()
{
    std::cout << "=== Cash history store tests ===" << std::endl;

    // 1. dayStart floors to UTC midnight.
    {
        check(dayStart(T0) % DAY == 0, "dayStart is a midnight boundary");
        check(dayStart(T0 + 3600) == dayStart(T0), "same day floors equal");
        check(dayStart(T0 + DAY) == dayStart(T0) + DAY, "next day is one DAY later");
    }

    // 2. First record for an account appends a point at the day boundary.
    {
        State s;
        bool changed = record(s, "Vanguard", 6189.72, T0 + 3600);
        check(changed, "first record reports change");
        check(s["Vanguard"].size() == 1, "one point stored");
        check(s["Vanguard"][0].day == dayStart(T0), "point stored at day-start");
        check(approx(s["Vanguard"][0].cash, 6189.72), "cash value stored");
    }

    // 3. A second sync the SAME day overwrites (latest wins), no duplicate.
    {
        State s;
        record(s, "Vanguard", 6189.72, T0 + 3600);
        bool changed = record(s, "Vanguard", 6500.00, T0 + 7200); // later, same day
        check(changed, "same-day update reports change");
        check(s["Vanguard"].size() == 1, "still one point for the day");
        check(approx(s["Vanguard"][0].cash, 6500.00), "latest same-day value wins");
    }

    // 4. Re-recording the identical same-day value is a no-op.
    {
        State s;
        record(s, "Vanguard", 6189.72, T0 + 3600);
        bool changed = record(s, "Vanguard", 6189.72, T0 + 7200);
        check(!changed, "identical same-day value reports no change");
        check(s["Vanguard"].size() == 1, "no duplicate appended");
    }

    // 5. A later day appends a new point, kept in chronological order.
    {
        State s;
        record(s, "Vanguard", 100.0, T0);
        record(s, "Vanguard", 200.0, T0 + DAY);
        record(s, "Vanguard", 300.0, T0 + 2 * DAY);
        check(s["Vanguard"].size() == 3, "three daily points");
        check(s["Vanguard"][0].day < s["Vanguard"][1].day &&
              s["Vanguard"][1].day < s["Vanguard"][2].day, "points sorted by day");
        check(approx(s["Vanguard"].back().cash, 300.0), "latest day is last");
    }

    // 6. Accounts are tracked independently.
    {
        State s;
        record(s, "Vanguard", 6189.72, T0);
        record(s, "Roth", 9570.09, T0);
        check(s.size() == 2, "two accounts tracked");
        check(approx(s["Roth"][0].cash, 9570.09), "per-account value isolated");
    }

    // 7. serialize/parse round-trips the series.
    {
        State s;
        record(s, "Vanguard", 6189.72, T0);
        record(s, "Vanguard", 6500.00, T0 + DAY);
        record(s, "Roth", 9570.09, T0);
        State r = parse(serialize(s));
        check(r.size() == 2, "accounts round-trip");
        check(r["Vanguard"].size() == 2, "points round-trip");
        check(approx(r["Vanguard"][1].cash, 6500.00), "cash values round-trip");
        check(r["Vanguard"][1].day == dayStart(T0 + DAY), "day values round-trip");
    }

    // 8. Malformed JSON parses to an empty, usable state.
    {
        State r = parse("not json at all {");
        check(r.empty(), "malformed -> empty state");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string(g_failures))
              << std::endl;
    return g_failures == 0 ? 0 : 1;
}

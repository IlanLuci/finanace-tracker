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

    // --- despike: remove transient single-day pulses ------------------------

    auto series = [](std::vector<double> cashes)
    {
        std::vector<Point> pts;
        for (size_t i = 0; i < cashes.size(); ++i)
            pts.push_back(Point{T0 + (time_t)i * DAY, cashes[i]});
        return pts;
    };

    // 9. The real Vanguard case: a +$556 anchor that reverts the next day is
    //    replaced with the neighbor baseline, not left as a spike.
    {
        auto out = despike(series({6190.24, 6746.15, 6252.44}));
        check(out.size() == 3, "despike keeps every day");
        check(out[0].cash == 6190.24 && out[2].cash == 6252.44,
              "neighbors of a pulse are untouched");
        check(out[1].cash < 6300.0, "up-pulse collapsed toward the baseline");
        check(approx(out[1].cash, (6190.24 + 6252.44) / 2.0),
              "pulse replaced with neighbor midpoint");
    }

    // 10. A transient DROP (e.g. Plaid briefly reads a low balance) is also
    //     flattened back to the baseline.
    {
        auto out = despike(series({600.0, 100.0, 590.0}));
        check(approx(out[1].cash, (600.0 + 590.0) / 2.0), "down-pulse collapsed");
    }

    // 11. A genuine STEP (deposit that stays) is preserved: the next day does
    //     not revert, so it is not a pulse.
    {
        auto out = despike(series({100.0, 600.0, 610.0}));
        check(approx(out[1].cash, 600.0), "real step preserved");
    }

    // 12. Gradual monotonic drift is preserved (no interior point protrudes).
    {
        auto out = despike(series({100.0, 150.0, 200.0, 250.0}));
        check(approx(out[1].cash, 150.0) && approx(out[2].cash, 200.0),
              "gradual drift preserved");
    }

    // 13. The LAST point (today's live value) is never modified, even if it
    //     looks like a spike relative to the prior day.
    {
        auto out = despike(series({100.0, 100.0, 700.0}));
        check(approx(out[2].cash, 700.0), "last point never despiked");
    }

    // 14. The FIRST point is never modified.
    {
        auto out = despike(series({700.0, 100.0, 100.0}));
        check(approx(out[0].cash, 700.0), "first point never despiked");
    }

    // 15. Series shorter than three points are returned unchanged.
    {
        auto out = despike(series({100.0, 700.0}));
        check(out.size() == 2 && approx(out[1].cash, 700.0),
              "short series untouched");
    }

    // 16. Small jitter below the dollar floor is left alone.
    {
        auto out = despike(series({1000.0, 1040.0, 1005.0}));
        check(approx(out[1].cash, 1040.0), "sub-threshold jitter preserved");
    }

    // 17. A REAL one-day excursion backed by money movement — a big deposit that
    //     is then transferred out — leaves the neighbors at DIFFERENT levels (a
    //     stepped baseline, drift far above the floor). That is real history, not
    //     a settlement-fund phantom, so it must be preserved even though it
    //     reverts. (Mirrors the real USAA Checking 09-11 anchor.)
    {
        auto out = despike(series({1669.56, 5243.74, 2243.74}));
        check(approx(out[1].cash, 5243.74),
              "real round-trip with stepped baseline is preserved");
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string(g_failures))
              << std::endl;
    return g_failures == 0 ? 0 : 1;
}

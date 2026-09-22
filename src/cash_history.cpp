#include "cash_history.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace CashHistory
{
    time_t dayStart(time_t t)
    {
        return (t / 86400) * 86400;
    }

    bool record(State& state, const std::string& account, double cash, time_t now)
    {
        const time_t day = dayStart(now);
        auto& points = state[account];

        // Common case: same day as the last point -> overwrite (latest wins).
        if (!points.empty() && points.back().day == day)
        {
            if (std::fabs(points.back().cash - cash) < 1e-9) return false;
            points.back().cash = cash;
            return true;
        }
        // Fast path: a strictly later day -> append (syncs are chronological).
        if (points.empty() || day > points.back().day)
        {
            points.push_back(Point{day, cash});
            return true;
        }
        // Out-of-order (rare): find the day, update in place, else insert sorted.
        auto it = std::lower_bound(points.begin(), points.end(), day,
                                   [](const Point& p, time_t d) { return p.day < d; });
        if (it != points.end() && it->day == day)
        {
            if (std::fabs(it->cash - cash) < 1e-9) return false;
            it->cash = cash;
            return true;
        }
        points.insert(it, Point{day, cash});
        return true;
    }

    std::vector<Point> despike(const std::vector<Point>& points)
    {
        // A point is a transient phantom pulse only when it protrudes from BOTH
        // neighbours in the same direction AND those neighbours form a flat
        // baseline (they agree within a tolerance) — i.e. no real money moved,
        // the balance left and returned to the same place. That is the
        // settlement-fund/pending signature (e.g. Vanguard VMFXX).
        //
        // A real deposit-then-transfer leaves the neighbours at DIFFERENT levels
        // (a stepped baseline): real history that reverts, which we must keep.
        // Requiring neighbour agreement is what separates the two without needing
        // the transaction ledger.
        // Two independent thresholds, scaled to the local balance:
        //  - agreement: how close the neighbours must be to count as a flat
        //    baseline. Generous, so legitimate settlement-fund drift (VMFXX
        //    accrual, small dividends ~tens of dollars) still reads as flat,
        //    while a real transfer (hundreds+) reads as a step and is preserved.
        //  - floor: the smallest pulse worth removing (ignore sub-$50 jitter).
        const auto agreement = [](double a, double c)
        { return std::max(150.0, 0.01 * std::max(std::fabs(a), std::fabs(c))); };
        const auto floor = [](double a, double c)
        { return std::max(50.0, 0.005 * std::max(std::fabs(a), std::fabs(c))); };

        std::vector<Point> out = points;
        if (points.size() < 3) return out;

        // Evaluate neighbours from the ORIGINAL series so a corrected point does
        // not cascade into its neighbour's decision.
        for (size_t i = 1; i + 1 < points.size(); ++i)
        {
            const double a = points[i - 1].cash;
            const double b = points[i].cash;
            const double c = points[i + 1].cash;

            const double dev_prev = b - a;
            const double dev_next = b - c;
            if (dev_prev * dev_next <= 0.0) continue;  // step/monotonic, not a pulse

            const double protrusion = std::min(std::fabs(dev_prev), std::fabs(dev_next));
            const double drift = std::fabs(a - c);  // baseline change across the pulse

            // Flat baseline (neighbours agree) + a pulse above the floor. A
            // stepped baseline (drift beyond agreement) means real money moved
            // and stayed moved, so the reverting point is real history.
            if (drift <= agreement(a, c) && protrusion >= floor(a, c))
            {
                out[i].cash = (a + c) / 2.0;  // collapse to the neighbour baseline
            }
        }
        return out;
    }

    // ---- Self-contained JSON persistence -----------------------------------

    namespace
    {
        void appendEscaped(std::ostringstream& out, const std::string& s)
        {
            out << '"';
            for (char c : s)
            {
                switch (c)
                {
                    case '"':  out << "\\\""; break;
                    case '\\': out << "\\\\"; break;
                    case '\n': out << "\\n";  break;
                    case '\r': out << "\\r";  break;
                    case '\t': out << "\\t";  break;
                    default:   out << c;      break;
                }
            }
            out << '"';
        }

        std::string num(double v)
        {
            std::ostringstream o;
            o.precision(10);
            o << v;
            return o.str();
        }

        // Minimal recursive-descent JSON parser sufficient for our own output.
        struct Parser
        {
            const std::string& in;
            size_t i = 0;
            bool ok = true;
            explicit Parser(const std::string& s) : in(s) {}

            void ws()
            {
                while (i < in.size() &&
                       (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r'))
                    ++i;
            }
            bool lit(const char* s)
            {
                ws();
                size_t n = 0;
                while (s[n]) ++n;
                if (in.compare(i, n, s) == 0) { i += n; return true; }
                return false;
            }
            std::string str()
            {
                ws();
                std::string out;
                if (i >= in.size() || in[i] != '"') { ok = false; return out; }
                ++i;
                while (i < in.size())
                {
                    char c = in[i++];
                    if (c == '"') return out;
                    if (c == '\\' && i < in.size())
                    {
                        char e = in[i++];
                        switch (e)
                        {
                            case 'n': out += '\n'; break;
                            case 'r': out += '\r'; break;
                            case 't': out += '\t'; break;
                            default:  out += e;    break;
                        }
                    }
                    else out += c;
                }
                ok = false;
                return out;
            }
            double number()
            {
                ws();
                size_t start = i;
                while (i < in.size() &&
                       (std::isdigit((unsigned char)in[i]) || in[i] == '-' || in[i] == '+' ||
                        in[i] == '.' || in[i] == 'e' || in[i] == 'E'))
                    ++i;
                if (i == start) { ok = false; return 0.0; }
                return std::strtod(in.substr(start, i - start).c_str(), nullptr);
            }
            void skipValue()
            {
                ws();
                if (i >= in.size()) { ok = false; return; }
                char c = in[i];
                if (c == '"') { str(); }
                else if (c == '{')
                {
                    ++i; ws();
                    if (in[i] == '}') { ++i; return; }
                    while (ok)
                    {
                        str(); lit(":"); skipValue(); ws();
                        if (i < in.size() && in[i] == ',') { ++i; continue; }
                        lit("}"); break;
                    }
                }
                else if (c == '[')
                {
                    ++i; ws();
                    if (in[i] == ']') { ++i; return; }
                    while (ok)
                    {
                        skipValue(); ws();
                        if (i < in.size() && in[i] == ',') { ++i; continue; }
                        lit("]"); break;
                    }
                }
                else if (lit("true") || lit("false") || lit("null")) { }
                else { number(); }
            }
        };
    }

    std::string serialize(const State& state)
    {
        std::ostringstream out;
        out << "{";
        bool first_acct = true;
        for (const auto& kv : state)
        {
            out << (first_acct ? "\n  " : ",\n  ");
            first_acct = false;
            appendEscaped(out, kv.first);
            out << ": [";
            bool first_pt = true;
            for (const auto& p : kv.second)
            {
                out << (first_pt ? "" : ", ");
                first_pt = false;
                out << "{\"day\": " << (long long)p.day
                    << ", \"cash\": " << num(p.cash) << "}";
            }
            out << "]";
        }
        out << (first_acct ? "" : "\n") << "}\n";
        return out.str();
    }

    State parse(const std::string& json)
    {
        State state;
        Parser p(json);
        if (!p.lit("{")) return state;

        p.ws();
        if (p.i < json.size() && json[p.i] == '}') { ++p.i; return state; }

        while (p.ok)
        {
            std::string account = p.str();
            if (!p.ok) return State{};
            p.lit(":");
            if (!p.lit("[")) return State{};

            std::vector<Point> points;
            p.ws();
            if (p.i < json.size() && json[p.i] == ']') { ++p.i; }
            else
            {
                while (p.ok)
                {
                    p.lit("{");
                    Point pt;
                    while (p.ok)
                    {
                        std::string f = p.str();
                        p.lit(":");
                        if (f == "day") pt.day = (time_t)p.number();
                        else if (f == "cash") pt.cash = p.number();
                        else p.skipValue();
                        p.ws();
                        if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                        p.lit("}");
                        break;
                    }
                    points.push_back(pt);
                    p.ws();
                    if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                    p.lit("]");
                    break;
                }
            }
            if (!account.empty()) state[account] = points;

            p.ws();
            if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
            p.lit("}");
            break;
        }
        return p.ok ? state : State{};
    }
}

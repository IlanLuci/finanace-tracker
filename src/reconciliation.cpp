#include "reconciliation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace Reconciliation
{
    std::string statusToString(EventStatus s)
    {
        switch (s)
        {
            case EventStatus::Pending:  return "pending";
            case EventStatus::Transfer: return "transfer";
            case EventStatus::Deposit:  return "deposit";
            case EventStatus::Cleared:  return "cleared";
        }
        return "pending";
    }

    EventStatus statusFromString(const std::string& s)
    {
        if (s == "transfer") return EventStatus::Transfer;
        if (s == "deposit")  return EventStatus::Deposit;
        if (s == "cleared")  return EventStatus::Cleared;
        return EventStatus::Pending;
    }

    double tolerance(double amount)
    {
        const double pct = std::fabs(amount) * 0.005;
        return pct > 1.0 ? pct : 1.0;
    }

    int businessDaysBetween(time_t from, time_t to)
    {
        if (to <= from) return 0;
        // 1970-01-01 (day index 0) was a Thursday, i.e. tm_wday == 4.
        const long d0 = static_cast<long>(from / 86400);
        const long d1 = static_cast<long>(to / 86400);
        int count = 0;
        for (long d = d0 + 1; d <= d1; ++d)
        {
            const int wday = static_cast<int>(((d % 7) + 4) % 7); // 0=Sun .. 6=Sat
            if (wday != 0 && wday != 6) ++count;
        }
        return count;
    }

    namespace
    {
        Event* findEvent(State& state, const std::string& id)
        {
            for (auto& e : state.events)
            {
                if (e.id == id) return &e;
            }
            return nullptr;
        }
    }

    std::string observe(State& state, const std::string& account,
                        double new_anchor, double explained_delta,
                        time_t now, const std::string& new_event_id)
    {
        std::string created;
        auto it = state.snapshots.find(account);
        if (it != state.snapshots.end())
        {
            const double balance_delta = new_anchor - it->second.anchor;
            const double unexplained = balance_delta - explained_delta;
            if (unexplained >= DETECT_FLOOR)
            {
                Event e;
                e.id = new_event_id;
                e.dest_account = account;
                e.amount = unexplained;
                e.detected_at = now;
                e.dest_anchor_at_detection = new_anchor;
                e.status = EventStatus::Pending;
                state.events.push_back(e);
                created = new_event_id;
            }
        }
        state.snapshots[account] = Snapshot{new_anchor, now};
        return created;
    }

    bool classifyTransfer(State& state, const std::string& event_id,
                          const std::string& source_account,
                          double source_anchor_now, time_t /*now*/)
    {
        Event* e = findEvent(state, event_id);
        if (!e || e->status != EventStatus::Pending || source_account.empty()) return false;
        e->status = EventStatus::Transfer;
        e->source_account = source_account;
        e->source_anchor_at_confirm = source_anchor_now;
        return true;
    }

    bool classifyDeposit(State& state, const std::string& event_id, time_t now)
    {
        Event* e = findEvent(state, event_id);
        if (!e || e->status != EventStatus::Pending) return false;
        e->status = EventStatus::Deposit;
        e->cleared_at = now;
        e->clear_reason = "dismissed";
        return true;
    }

    std::string createTransfer(State& state, const std::string& source_account,
                               const std::string& dest_account, double amount,
                               double source_anchor_now, double dest_anchor_now,
                               time_t now, const std::string& event_id)
    {
        // Idempotent: never hold out the same in-flight transfer twice. If an
        // active transfer for this source/dest/amount already exists, return it.
        const double tol = tolerance(amount);
        for (const auto& existing : state.events)
        {
            if (existing.status == EventStatus::Transfer &&
                existing.source_account == source_account &&
                existing.dest_account == dest_account &&
                std::fabs(existing.amount - amount) <= tol)
            {
                return existing.id;
            }
        }

        Event e;
        e.id = event_id;
        e.dest_account = dest_account;
        e.amount = amount;
        e.detected_at = now;
        // Destination balance BEFORE the money lands; sweep() clears once the
        // anchor rises by ~amount above it. 0 means unknown (signal disabled).
        e.dest_anchor_at_detection = dest_anchor_now;
        e.status = EventStatus::Transfer;
        e.source_account = source_account;
        e.source_anchor_at_confirm = source_anchor_now;
        state.events.push_back(e);
        return event_id;
    }

    bool sweep(State& state, const AnchorLookup& lookup, time_t now)
    {
        bool changed = false;
        for (auto& e : state.events)
        {
            const double tol = tolerance(e.amount);

            if (e.status == EventStatus::Transfer)
            {
                double cur = 0.0;

                // Primary: the source account's cash finally dropped by ~amount.
                if (!e.source_account.empty() && lookup.anchor(e.source_account, cur) &&
                    cur <= e.source_anchor_at_confirm - e.amount + tol)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "source_dropped";
                    changed = true;
                    continue;
                }

                // The destination deposit finally posted as a transaction.
                if (lookup.creditSince(e.dest_account, e.detected_at) >= e.amount - tol)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "txn_posted";
                    changed = true;
                    continue;
                }

                // The destination's balance rose by ~amount since the transfer was
                // recorded — the money landed even though no cash transaction posted
                // (e.g. an inter-brokerage move that only shifts settlement-fund cash,
                // which the investment sync folds into balances rather than the ledger).
                // Requires a known baseline; dest_anchor_at_detection == 0 disables it.
                if (e.dest_anchor_at_detection > 0.0 && lookup.anchor(e.dest_account, cur) &&
                    cur >= e.dest_anchor_at_detection + e.amount - tol)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "dest_rose";
                    changed = true;
                    continue;
                }

                // The credit reverted (transfer cancelled at the destination).
                if (e.dest_anchor_at_detection > 0.0 && lookup.anchor(e.dest_account, cur) &&
                    cur <= e.dest_anchor_at_detection - e.amount + tol)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "reverted";
                    changed = true;
                    continue;
                }

                // Backstop: give the transfer a few business days to settle, then
                // assume it landed (weekends don't count as settlement time).
                if (businessDaysBetween(e.detected_at, now) >= TRANSFER_EXPIRY_BUSINESS_DAYS)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "expired";
                    changed = true;
                    continue;
                }
            }
            else if (e.status == EventStatus::Pending)
            {
                double cur = 0.0;

                // Credit reverted before the user ever classified it.
                if (e.dest_anchor_at_detection > 0.0 && lookup.anchor(e.dest_account, cur) &&
                    cur <= e.dest_anchor_at_detection - e.amount + tol)
                {
                    e.status = EventStatus::Cleared;
                    e.cleared_at = now;
                    e.clear_reason = "reverted";
                    changed = true;
                    continue;
                }

                // Never classified -> assume real deposit so it stops nagging.
                if (now - e.detected_at > (time_t)PENDING_EXPIRY_DAYS * 86400)
                {
                    e.status = EventStatus::Deposit;
                    e.cleared_at = now;
                    e.clear_reason = "expired";
                    changed = true;
                    continue;
                }
            }
        }
        return changed;
    }

    std::map<std::string, double> heldOutBySource(const State& state)
    {
        std::map<std::string, double> out;
        for (const auto& e : state.events)
        {
            if (e.status == EventStatus::Transfer && !e.source_account.empty())
            {
                out[e.source_account] += e.amount;
            }
        }
        return out;
    }

    double heldOutTotal(const State& state)
    {
        double total = 0.0;
        for (const auto& e : state.events)
        {
            if (e.status == EventStatus::Transfer) total += e.amount;
        }
        return total;
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
            // Skip an arbitrary value (used for unknown/ignored fields).
            void skipValue()
            {
                ws();
                if (i >= in.size()) { ok = false; return; }
                char c = in[i];
                if (c == '"') { str(); }
                else if (c == '{')
                {
                    ++i;
                    ws();
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
                    ++i;
                    ws();
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
        out << "{\n  \"snapshots\": {";
        bool first = true;
        for (const auto& kv : state.snapshots)
        {
            out << (first ? "\n    " : ",\n    ");
            first = false;
            appendEscaped(out, kv.first);
            out << ": {\"anchor\": " << num(kv.second.anchor)
                << ", \"synced_at\": " << (long long)kv.second.synced_at << "}";
        }
        out << (first ? "" : "\n  ") << "},\n  \"events\": [";
        first = true;
        for (const auto& e : state.events)
        {
            out << (first ? "\n    " : ",\n    ");
            first = false;
            out << "{\"id\": ";       appendEscaped(out, e.id);
            out << ", \"dest_account\": "; appendEscaped(out, e.dest_account);
            out << ", \"amount\": " << num(e.amount)
                << ", \"detected_at\": " << (long long)e.detected_at
                << ", \"dest_anchor_at_detection\": " << num(e.dest_anchor_at_detection)
                << ", \"status\": ";  appendEscaped(out, statusToString(e.status));
            out << ", \"source_account\": "; appendEscaped(out, e.source_account);
            out << ", \"source_anchor_at_confirm\": " << num(e.source_anchor_at_confirm)
                << ", \"cleared_at\": " << (long long)e.cleared_at
                << ", \"clear_reason\": "; appendEscaped(out, e.clear_reason);
            out << "}";
        }
        out << (first ? "" : "\n  ") << "]\n}\n";
        return out.str();
    }

    State parse(const std::string& json)
    {
        State state;
        Parser p(json);
        if (!p.lit("{")) return state;

        while (p.ok)
        {
            p.ws();
            if (p.i < json.size() && json[p.i] == '}') { ++p.i; break; }
            std::string key = p.str();
            if (!p.ok) return State{};
            p.lit(":");

            if (key == "snapshots")
            {
                if (!p.lit("{")) return State{};
                p.ws();
                if (p.i < json.size() && json[p.i] == '}') { ++p.i; }
                else
                {
                    while (p.ok)
                    {
                        std::string name = p.str();
                        p.lit(":");
                        p.lit("{");
                        Snapshot snap;
                        while (p.ok)
                        {
                            std::string f = p.str();
                            p.lit(":");
                            if (f == "anchor") snap.anchor = p.number();
                            else if (f == "synced_at") snap.synced_at = (time_t)p.number();
                            else p.skipValue();
                            p.ws();
                            if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                            p.lit("}");
                            break;
                        }
                        if (!name.empty()) state.snapshots[name] = snap;
                        p.ws();
                        if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                        p.lit("}");
                        break;
                    }
                }
            }
            else if (key == "events")
            {
                if (!p.lit("[")) return State{};
                p.ws();
                if (p.i < json.size() && json[p.i] == ']') { ++p.i; }
                else
                {
                    while (p.ok)
                    {
                        p.lit("{");
                        Event e;
                        while (p.ok)
                        {
                            std::string f = p.str();
                            p.lit(":");
                            if (f == "id") e.id = p.str();
                            else if (f == "dest_account") e.dest_account = p.str();
                            else if (f == "amount") e.amount = p.number();
                            else if (f == "detected_at") e.detected_at = (time_t)p.number();
                            else if (f == "dest_anchor_at_detection") e.dest_anchor_at_detection = p.number();
                            else if (f == "status") e.status = statusFromString(p.str());
                            else if (f == "source_account") e.source_account = p.str();
                            else if (f == "source_anchor_at_confirm") e.source_anchor_at_confirm = p.number();
                            else if (f == "cleared_at") e.cleared_at = (time_t)p.number();
                            else if (f == "clear_reason") e.clear_reason = p.str();
                            else p.skipValue();
                            p.ws();
                            if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                            p.lit("}");
                            break;
                        }
                        if (!e.id.empty()) state.events.push_back(e);
                        p.ws();
                        if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
                        p.lit("]");
                        break;
                    }
                }
            }
            else
            {
                p.skipValue();
            }

            p.ws();
            if (p.i < json.size() && json[p.i] == ',') { ++p.i; continue; }
            p.lit("}");
            break;
        }

        if (!p.ok) return State{};
        return state;
    }
}

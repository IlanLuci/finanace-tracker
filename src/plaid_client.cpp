#include "plaid_client.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace Plaid
{
namespace
{
    std::string envOr(const char* key, const std::string& fallback)
    {
        const char* raw = std::getenv(key);
        if (raw == nullptr || raw[0] == '\0') return fallback;
        return std::string(raw);
    }

    std::string shellEscape(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (char ch : value)
        {
            if (ch == '\'') out += "'\\''";
            else out.push_back(ch);
        }
        return out;
    }

    std::string baseUrlFor(const std::string& env)
    {
        if (env == "production") return "https://production.plaid.com";
        if (env == "development")
        {
            // Plaid retired the Development environment in 2024. Dashboard tiers
            // formerly called "Development" now issue Production credentials.
            static bool warned = false;
            if (!warned)
            {
                std::cerr << "[plaid] PLAID_ENV=development is retired; routing to production.plaid.com. "
                          << "Update .env to PLAID_ENV=production." << std::endl;
                warned = true;
            }
            return "https://production.plaid.com";
        }
        return "https://sandbox.plaid.com";
    }

    // Minimal JSON string escape for body assembly.
    std::string jsonStr(const std::string& v)
    {
        std::string out;
        out.reserve(v.size() + 2);
        out.push_back('"');
        for (unsigned char c : v)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
        return out;
    }

    // POST a JSON body via curl. Returns true on HTTP 2xx, with response body in out.
    bool httpPostJson(const std::string& url,
                      const std::string& body,
                      std::string& out_body,
                      int& out_status,
                      std::string& error)
    {
        // Write body to a temp file to avoid shell-quoting fragility.
        char tmpl[] = "/tmp/plaid_req_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0)
        {
            error = "mkstemp failed";
            return false;
        }
        const std::string tmp_path = tmpl;
        FILE* tf = fdopen(fd, "wb");
        if (tf == nullptr)
        {
            ::close(fd);
            std::remove(tmp_path.c_str());
            error = "fdopen tmp failed";
            return false;
        }
        std::fwrite(body.data(), 1, body.size(), tf);
        std::fclose(tf);

        const std::string command =
            "curl -sS --compressed -w '\\n%{http_code}' --connect-timeout 15 --max-time 60 "
            "-H 'Content-Type: application/json' "
            "-H 'Accept: application/json' "
            "-X POST --data-binary @'" + shellEscape(tmp_path) + "' "
            "'" + shellEscape(url) + "'";

        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
        {
            std::remove(tmp_path.c_str());
            error = "popen failed";
            return false;
        }

        out_body.clear();
        std::array<char, 4096> buf = {};
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        {
            out_body += buf.data();
        }
        const int rc = pclose(pipe);
        std::remove(tmp_path.c_str());

        if (rc != 0)
        {
            error = "curl exited non-zero";
            return false;
        }

        const size_t last_nl = out_body.rfind('\n');
        std::string status_str = "0";
        if (last_nl != std::string::npos && last_nl + 1 < out_body.size())
        {
            status_str = out_body.substr(last_nl + 1);
            while (!status_str.empty() &&
                   std::isspace(static_cast<unsigned char>(status_str.back())))
            {
                status_str.pop_back();
            }
            out_body = out_body.substr(0, last_nl);
        }

        try { out_status = std::stoi(status_str); }
        catch (...) { out_status = 0; }

        if (out_status < 200 || out_status >= 300)
        {
            error = "HTTP " + std::to_string(out_status) + ": " + out_body.substr(0, 400);
            return false;
        }
        return true;
    }

    // Find the index of `"key"` used as a JSON KEY (immediately followed by optional
    // whitespace and a colon). This avoids false matches where the string appears as
    // a value inside another array, e.g. `"products":["transactions"]`.
    size_t findJsonKey(const std::string& content, const std::string& key, size_t from = 0)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = from;
        while (true)
        {
            pos = content.find(needle, pos);
            if (pos == std::string::npos) return std::string::npos;
            size_t after = pos + needle.size();
            while (after < content.size() &&
                   std::isspace(static_cast<unsigned char>(content[after])))
            {
                ++after;
            }
            if (after < content.size() && content[after] == ':')
            {
                return pos;
            }
            pos += needle.size();
        }
    }

    // Locate the value span (after the colon) for a JSON key inside `content`.
    // Returns the index where the value starts, or npos.
    size_t findValueStart(const std::string& content, const std::string& key, size_t from = 0)
    {
        size_t key_pos = findJsonKey(content, key, from);
        if (key_pos == std::string::npos) return std::string::npos;
        size_t pos = content.find(':', key_pos + key.size() + 2);
        if (pos == std::string::npos) return std::string::npos;
        ++pos;
        while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) ++pos;
        return pos;
    }

    bool extractString(const std::string& content, const std::string& key, std::string& out, size_t from = 0)
    {
        size_t pos = findValueStart(content, key, from);
        if (pos == std::string::npos || pos >= content.size() || content[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < content.size() && content[pos] != '"')
        {
            char c = content[pos];
            if (c == '\\' && pos + 1 < content.size())
            {
                char n = content[pos + 1];
                switch (n)
                {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:   out.push_back(n);    break;
                }
                pos += 2;
            }
            else
            {
                out.push_back(c);
                ++pos;
            }
        }
        return true;
    }

    bool extractNumber(const std::string& content, const std::string& key, double& out, size_t from = 0)
    {
        size_t pos = findValueStart(content, key, from);
        if (pos == std::string::npos || pos >= content.size()) return false;
        size_t end = pos;
        if (content[end] == '-' || content[end] == '+') ++end;
        while (end < content.size() &&
               (std::isdigit(static_cast<unsigned char>(content[end])) ||
                content[end] == '.' || content[end] == 'e' || content[end] == 'E' ||
                content[end] == '-' || content[end] == '+'))
        {
            ++end;
        }
        if (end == pos) return false;
        try { out = std::stod(content.substr(pos, end - pos)); return true; }
        catch (...) { return false; }
    }

    // Given the index of an opening `{` or `[`, return the index of the matching close,
    // or npos if unbalanced.
    size_t findMatchingBracket(const std::string& s, size_t open_idx)
    {
        if (open_idx >= s.size()) return std::string::npos;
        char open_c = s[open_idx];
        char close_c = (open_c == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        bool esc = false;
        for (size_t i = open_idx; i < s.size(); ++i)
        {
            char c = s[i];
            if (in_str)
            {
                if (esc) { esc = false; continue; }
                if (c == '\\') { esc = true; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == open_c) ++depth;
            else if (c == close_c)
            {
                --depth;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    // Convert "YYYY-MM-DD" to unix timestamp at UTC midnight.
    time_t parsePlaidDate(const std::string& s)
    {
        if (s.size() < 10) return 0;
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = 12; // noon UTC to avoid TZ edge cases
        return timegm(&tm);
    }

    // Extract each top-level element of a JSON array starting at `array_start_idx`
    // (which must point at `[`). Returns the slices.
    std::vector<std::string> splitTopLevelArray(const std::string& s, size_t array_start_idx)
    {
        std::vector<std::string> out;
        if (array_start_idx >= s.size() || s[array_start_idx] != '[') return out;
        size_t i = array_start_idx + 1;
        while (i < s.size())
        {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size() || s[i] == ']') break;
            size_t element_start = i;
            if (s[i] == '{' || s[i] == '[')
            {
                size_t close = findMatchingBracket(s, i);
                if (close == std::string::npos) break;
                out.push_back(s.substr(element_start, close - element_start + 1));
                i = close + 1;
            }
            else
            {
                // scalar element — read until , or ]
                while (i < s.size() && s[i] != ',' && s[i] != ']') ++i;
                out.push_back(s.substr(element_start, i - element_start));
            }
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] == ',') ++i;
        }
        return out;
    }
}

Config configFromEnvironment()
{
    Config c;
    c.client_id = envOr("PLAID_CLIENT_ID", "");
    c.secret = envOr("PLAID_SECRET", "");
    c.environment = envOr("PLAID_ENV", "sandbox");
    c.client_name = envOr("PLAID_CLIENT_NAME", "Finance Tracker");
    c.country_codes = envOr("PLAID_COUNTRY_CODES", "US");
    c.language = envOr("PLAID_LANGUAGE", "en");
    return c;
}

bool isConfigured(const Config& config)
{
    return !config.client_id.empty() && !config.secret.empty();
}

bool createLinkToken(const Config& config,
                     const std::string& user_id,
                     std::string& out_link_token,
                     std::string& error)
{
    if (!isConfigured(config))
    {
        error = "Plaid not configured: set PLAID_CLIENT_ID and PLAID_SECRET";
        return false;
    }

    std::ostringstream body;
    body << "{"
         << "\"client_id\":" << jsonStr(config.client_id) << ","
         << "\"secret\":" << jsonStr(config.secret) << ","
         << "\"client_name\":" << jsonStr(config.client_name) << ","
         << "\"language\":" << jsonStr(config.language) << ","
         << "\"country_codes\":[" << jsonStr(config.country_codes) << "],"
         << "\"products\":[\"transactions\"],"
         << "\"optional_products\":[\"investments\"],"
         << "\"user\":{\"client_user_id\":" << jsonStr(user_id) << "}"
         << "}";

    std::string response;
    int status = 0;
    if (!httpPostJson(baseUrlFor(config.environment) + "/link/token/create",
                      body.str(), response, status, error))
    {
        return false;
    }

    if (!extractString(response, "link_token", out_link_token))
    {
        error = "link_token missing from response: " + response.substr(0, 400);
        return false;
    }
    return true;
}

bool exchangePublicToken(const Config& config,
                         const std::string& public_token,
                         std::string& out_access_token,
                         std::string& out_item_id,
                         std::string& error)
{
    if (!isConfigured(config)) { error = "Plaid not configured"; return false; }

    std::ostringstream body;
    body << "{"
         << "\"client_id\":" << jsonStr(config.client_id) << ","
         << "\"secret\":" << jsonStr(config.secret) << ","
         << "\"public_token\":" << jsonStr(public_token)
         << "}";

    std::string response;
    int status = 0;
    if (!httpPostJson(baseUrlFor(config.environment) + "/item/public_token/exchange",
                      body.str(), response, status, error))
    {
        return false;
    }

    if (!extractString(response, "access_token", out_access_token) ||
        !extractString(response, "item_id", out_item_id))
    {
        error = "exchange response missing fields: " + response.substr(0, 400);
        return false;
    }
    return true;
}

bool getAccounts(const Config& config,
                 const std::string& access_token,
                 std::vector<AccountSummary>& out_accounts,
                 std::string& out_institution_name,
                 std::string& out_institution_id,
                 std::string& error)
{
    if (!isConfigured(config)) { error = "Plaid not configured"; return false; }

    out_accounts.clear();
    out_institution_name.clear();
    out_institution_id.clear();

    // /auth/get returns accounts + item.institution_id; for the institution name we
    // need /institutions/get_by_id. Use /accounts/get first.
    std::ostringstream body;
    body << "{"
         << "\"client_id\":" << jsonStr(config.client_id) << ","
         << "\"secret\":" << jsonStr(config.secret) << ","
         << "\"access_token\":" << jsonStr(access_token)
         << "}";

    std::string response;
    int status = 0;
    if (!httpPostJson(baseUrlFor(config.environment) + "/accounts/get",
                      body.str(), response, status, error))
    {
        return false;
    }

    // institution_id is nested inside "item": { ... "institution_id": "..." ... }
    size_t item_val = findValueStart(response, "item");
    if (item_val != std::string::npos && response[item_val] == '{')
    {
        size_t item_obj_end = findMatchingBracket(response, item_val);
        if (item_obj_end != std::string::npos)
        {
            const std::string item_slice =
                response.substr(item_val, item_obj_end - item_val + 1);
            extractString(item_slice, "institution_id", out_institution_id);
        }
    }

    size_t arr_start = findValueStart(response, "accounts");
    if (arr_start == std::string::npos || response[arr_start] != '[')
    {
        error = "no accounts array in response";
        return false;
    }
    const std::vector<std::string> elements = splitTopLevelArray(response, arr_start);
    for (const auto& el : elements)
    {
        AccountSummary acc;
        extractString(el, "account_id", acc.account_id);
        extractString(el, "name", acc.name);
        extractString(el, "official_name", acc.official_name);
        extractString(el, "type", acc.type);
        extractString(el, "subtype", acc.subtype);
        extractString(el, "mask", acc.mask);

        // Pull current balance out of the nested "balances" object.
        size_t bal_val = findValueStart(el, "balances");
        if (bal_val != std::string::npos && el[bal_val] == '{')
        {
            size_t bal_end = findMatchingBracket(el, bal_val);
            if (bal_end != std::string::npos)
            {
                const std::string bal_slice = el.substr(bal_val, bal_end - bal_val + 1);
                double cur = 0.0;
                if (extractNumber(bal_slice, "current", cur))
                {
                    acc.current_balance = cur;
                    acc.has_balance = true;
                }
            }
        }

        if (!acc.account_id.empty()) out_accounts.push_back(acc);
    }

    // Look up institution name (best-effort).
    if (!out_institution_id.empty())
    {
        std::ostringstream inst_body;
        inst_body << "{"
                  << "\"client_id\":" << jsonStr(config.client_id) << ","
                  << "\"secret\":" << jsonStr(config.secret) << ","
                  << "\"institution_id\":" << jsonStr(out_institution_id) << ","
                  << "\"country_codes\":[" << jsonStr(config.country_codes) << "]"
                  << "}";
        std::string inst_resp;
        int inst_status = 0;
        std::string inst_err;
        if (httpPostJson(baseUrlFor(config.environment) + "/institutions/get_by_id",
                         inst_body.str(), inst_resp, inst_status, inst_err))
        {
            size_t inst_val = findValueStart(inst_resp, "institution");
            if (inst_val != std::string::npos && inst_resp[inst_val] == '{')
            {
                size_t obj_end = findMatchingBracket(inst_resp, inst_val);
                if (obj_end != std::string::npos)
                {
                    const std::string slice =
                        inst_resp.substr(inst_val, obj_end - inst_val + 1);
                    extractString(slice, "name", out_institution_name);
                }
            }
        }
    }

    return true;
}

bool getTransactions(const Config& config,
                     const std::string& access_token,
                     const std::string& start_date,
                     const std::string& end_date,
                     std::vector<Transaction>& out_transactions,
                     std::string& error)
{
    if (!isConfigured(config)) { error = "Plaid not configured"; return false; }

    out_transactions.clear();
    int offset = 0;
    const int page_size = 500;

    while (true)
    {
        std::ostringstream body;
        body << "{"
             << "\"client_id\":" << jsonStr(config.client_id) << ","
             << "\"secret\":" << jsonStr(config.secret) << ","
             << "\"access_token\":" << jsonStr(access_token) << ","
             << "\"start_date\":" << jsonStr(start_date) << ","
             << "\"end_date\":" << jsonStr(end_date) << ","
             << "\"options\":{\"count\":" << page_size << ",\"offset\":" << offset << "}"
             << "}";

        std::string response;
        int status = 0;
        if (!httpPostJson(baseUrlFor(config.environment) + "/transactions/get",
                          body.str(), response, status, error))
        {
            return false;
        }

        size_t arr_start = findValueStart(response, "transactions");
        if (arr_start == std::string::npos || response[arr_start] != '[') break;
        const std::vector<std::string> elements = splitTopLevelArray(response, arr_start);
        if (elements.empty()) break;

        for (const auto& el : elements)
        {
            Transaction tx;
            extractString(el, "transaction_id", tx.transaction_id);
            extractString(el, "account_id", tx.account_id);
            extractString(el, "name", tx.name);
            extractString(el, "merchant_name", tx.merchant_name);
            extractString(el, "iso_currency_code", tx.iso_currency_code);
            std::string date_str;
            extractString(el, "date", date_str);
            tx.date = parsePlaidDate(date_str);
            extractNumber(el, "amount", tx.amount);
            size_t p_pos = findValueStart(el, "pending");
            if (p_pos != std::string::npos)
            {
                tx.pending = (el.compare(p_pos, 4, "true") == 0);
            }
            if (!tx.transaction_id.empty()) out_transactions.push_back(tx);
        }

        double total = 0.0;
        extractNumber(response, "total_transactions", total);
        offset += static_cast<int>(elements.size());
        if (offset >= static_cast<int>(total) || elements.empty()) break;
        if (elements.size() < static_cast<size_t>(page_size)) break;
    }

    return true;
}

namespace
{
    void parseSecuritiesArray(const std::string& response, std::vector<Security>& out_securities)
    {
        size_t arr_start = findValueStart(response, "securities");
        if (arr_start == std::string::npos || response[arr_start] != '[') return;
        const std::vector<std::string> elements = splitTopLevelArray(response, arr_start);
        for (const auto& el : elements)
        {
            Security s;
            extractString(el, "security_id", s.security_id);
            extractString(el, "ticker_symbol", s.ticker_symbol);
            extractString(el, "name", s.name);
            extractString(el, "type", s.type);
            // is_cash_equivalent is a boolean — look for literal "true" after the colon.
            size_t ce_pos = findValueStart(el, "is_cash_equivalent");
            if (ce_pos != std::string::npos)
            {
                s.is_cash_equivalent = (el.compare(ce_pos, 4, "true") == 0);
            }
            if (!s.security_id.empty()) out_securities.push_back(s);
        }
    }
}

bool getHoldings(const Config& config,
                 const std::string& access_token,
                 std::vector<Holding>& out_holdings,
                 std::vector<Security>& out_securities,
                 std::string& error)
{
    if (!isConfigured(config)) { error = "Plaid not configured"; return false; }
    out_holdings.clear();
    out_securities.clear();

    std::ostringstream body;
    body << "{"
         << "\"client_id\":" << jsonStr(config.client_id) << ","
         << "\"secret\":" << jsonStr(config.secret) << ","
         << "\"access_token\":" << jsonStr(access_token)
         << "}";

    std::string response;
    int status = 0;
    if (!httpPostJson(baseUrlFor(config.environment) + "/investments/holdings/get",
                      body.str(), response, status, error))
    {
        return false;
    }

    {
        size_t arr_start = findValueStart(response, "holdings");
        if (arr_start != std::string::npos && response[arr_start] == '[')
        {
            const std::vector<std::string> elements = splitTopLevelArray(response, arr_start);
            for (const auto& el : elements)
            {
                Holding h;
                extractString(el, "account_id", h.account_id);
                extractString(el, "security_id", h.security_id);
                extractString(el, "iso_currency_code", h.iso_currency_code);
                extractNumber(el, "quantity", h.quantity);
                extractNumber(el, "cost_basis", h.cost_basis);
                extractNumber(el, "institution_price", h.institution_price);
                extractNumber(el, "institution_value", h.institution_value);
                if (!h.security_id.empty()) out_holdings.push_back(h);
            }
        }
    }

    parseSecuritiesArray(response, out_securities);
    return true;
}

bool getInvestmentTransactions(const Config& config,
                                const std::string& access_token,
                                const std::string& start_date,
                                const std::string& end_date,
                                std::vector<InvestmentTransaction>& out_transactions,
                                std::vector<Security>& out_securities,
                                std::string& error)
{
    if (!isConfigured(config)) { error = "Plaid not configured"; return false; }
    out_transactions.clear();
    out_securities.clear();

    int offset = 0;
    const int page_size = 500;

    while (true)
    {
        std::ostringstream body;
        body << "{"
             << "\"client_id\":" << jsonStr(config.client_id) << ","
             << "\"secret\":" << jsonStr(config.secret) << ","
             << "\"access_token\":" << jsonStr(access_token) << ","
             << "\"start_date\":" << jsonStr(start_date) << ","
             << "\"end_date\":" << jsonStr(end_date) << ","
             << "\"options\":{\"count\":" << page_size << ",\"offset\":" << offset << "}"
             << "}";

        std::string response;
        int status = 0;
        if (!httpPostJson(baseUrlFor(config.environment) + "/investments/transactions/get",
                          body.str(), response, status, error))
        {
            return false;
        }

        // Merge securities from this page
        parseSecuritiesArray(response, out_securities);

        size_t arr_start = findValueStart(response, "investment_transactions");
        if (arr_start == std::string::npos || response[arr_start] != '[') break;
        const std::vector<std::string> elements = splitTopLevelArray(response, arr_start);
        if (elements.empty()) break;

        for (const auto& el : elements)
        {
            InvestmentTransaction tx;
            extractString(el, "investment_transaction_id", tx.investment_transaction_id);
            extractString(el, "account_id", tx.account_id);
            extractString(el, "security_id", tx.security_id);
            extractString(el, "type", tx.type);
            extractString(el, "subtype", tx.subtype);
            extractString(el, "name", tx.name);
            extractString(el, "iso_currency_code", tx.iso_currency_code);
            extractNumber(el, "quantity", tx.quantity);
            extractNumber(el, "price", tx.price);
            extractNumber(el, "amount", tx.amount);
            extractNumber(el, "fees", tx.fees);
            std::string date_str;
            extractString(el, "date", date_str);
            tx.date = parsePlaidDate(date_str);
            if (!tx.investment_transaction_id.empty()) out_transactions.push_back(tx);
        }

        double total = 0.0;
        extractNumber(response, "total_investment_transactions", total);
        offset += static_cast<int>(elements.size());
        if (offset >= static_cast<int>(total) || elements.empty()) break;
        if (elements.size() < static_cast<size_t>(page_size)) break;
    }

    return true;
}
} // namespace Plaid

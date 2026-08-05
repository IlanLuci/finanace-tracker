#include "web_server.hpp"

#include "expense_tags.hpp"
#include "market_data_sync.hpp"
#include "plaid_client.hpp"
#include "portfolio_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdio>
#include <map>
#include <set>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zlib.h>

namespace
{
    // Keep in sync with portfolio_data.cpp; bump when the Portfolio file
    // format changes so hasSupportedPortfolioHeader() accepts new versions.
    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 4;
    constexpr uint32_t OLDEST_SUPPORTED_FILE_VERSION = 1;
    constexpr int DAILY_SYNC_HOUR_LOCAL = 18;
    constexpr int DAILY_SYNC_MINUTE_LOCAL = 5;
    constexpr int DAILY_SYNC_RETRY_MINUTES = 15;
    constexpr int DAILY_SYNC_POLL_SECONDS = 60;
    constexpr int PLAID_SYNC_INTERVAL_SECONDS = 3600;   // pull connected accounts once per hour
    constexpr int PLAID_SYNC_POLL_SECONDS = 60;         // wall-clock poll interval
    constexpr long long SECONDS_PER_DAY = 86400;
    std::mutex g_data_access_mutex;

    long long dayBucketForTimestamp(time_t ts)
    {
        return static_cast<long long>(ts) / SECONDS_PER_DAY;
    }

    bool isWeekdayBucket(long long day_bucket)
    {
        const time_t ts = static_cast<time_t>(day_bucket * SECONDS_PER_DAY);
        std::tm* local = std::localtime(&ts);
        if (local == nullptr)
        {
            return false;
        }

        return local->tm_wday >= 1 && local->tm_wday <= 5;
    }

    // Day change should only reflect movement during today's trading session.
    // On weekends, holidays, or pre-market, the latest available close is from
    // a prior trading day — comparing it to the day before that would surface
    // Friday's gain on Saturday as if it were today's. Use local-day comparison
    // so the cutoff aligns with the user's calendar, not the UTC day boundary.
    bool isLatestPriceFromToday(time_t latest_date)
    {
        if (latest_date <= 0)
        {
            return false;
        }
        const time_t now = std::time(nullptr);
        std::tm latest_tm{};
        std::tm now_tm{};
        {
            std::tm* p = std::localtime(&latest_date);
            if (p == nullptr) return false;
            latest_tm = *p;
        }
        {
            std::tm* p = std::localtime(&now);
            if (p == nullptr) return false;
            now_tm = *p;
        }
        return latest_tm.tm_year == now_tm.tm_year && latest_tm.tm_yday == now_tm.tm_yday;
    }

    long long latestExpectedDailySyncDayLocal(time_t now)
    {
        long long day = dayBucketForTimestamp(now);
        std::tm* local = std::localtime(&now);
        if (local == nullptr)
        {
            return day;
        }

        const bool before_daily_sync_cutoff =
            (local->tm_hour < DAILY_SYNC_HOUR_LOCAL) ||
            (local->tm_hour == DAILY_SYNC_HOUR_LOCAL && local->tm_min < DAILY_SYNC_MINUTE_LOCAL);
        if (before_daily_sync_cutoff)
        {
            --day;
        }

        while (day > 0 && !isWeekdayBucket(day))
        {
            --day;
        }

        return day;
    }

    struct HttpRequest
    {
        std::string method;
        std::string target;
        std::string path;
        std::string query;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string content_type = "application/json";
        std::string body;
        std::map<std::string, std::string> headers;
    };

    enum class JsonType
    {
        NIL,
        BOOL,
        NUMBER,
        STRING,
        OBJECT,
        ARRAY
    };

    struct JsonValue
    {
        JsonType type = JsonType::NIL;
        bool bool_value = false;
        double number_value = 0.0;
        std::string string_value;
        std::map<std::string, JsonValue> object_value;
        std::vector<JsonValue> array_value;

        static JsonValue makeNull()
        {
            return JsonValue{};
        }

        static JsonValue makeBool(bool value)
        {
            JsonValue json;
            json.type = JsonType::BOOL;
            json.bool_value = value;
            return json;
        }

        static JsonValue makeNumber(double value)
        {
            JsonValue json;
            json.type = JsonType::NUMBER;
            json.number_value = value;
            return json;
        }

        static JsonValue makeString(const std::string& value)
        {
            JsonValue json;
            json.type = JsonType::STRING;
            json.string_value = value;
            return json;
        }

        static JsonValue makeObject(const std::map<std::string, JsonValue>& value)
        {
            JsonValue json;
            json.type = JsonType::OBJECT;
            json.object_value = value;
            return json;
        }

        static JsonValue makeArray(const std::vector<JsonValue>& value)
        {
            JsonValue json;
            json.type = JsonType::ARRAY;
            json.array_value = value;
            return json;
        }
    };

    class JsonParser
    {
    private:
        const std::string& input;
        size_t index = 0;

        void skipWhitespace()
        {
            while (index < input.size() &&
                   (input[index] == ' ' || input[index] == '\t' || input[index] == '\n' || input[index] == '\r'))
            {
                ++index;
            }
        }

        bool consumeLiteral(const std::string& literal)
        {
            if (input.compare(index, literal.size(), literal) != 0)
            {
                return false;
            }
            index += literal.size();
            return true;
        }

        std::optional<std::string> parseStringRaw()
        {
            if (index >= input.size() || input[index] != '"')
            {
                return std::nullopt;
            }
            ++index;

            std::string out;
            while (index < input.size())
            {
                char ch = input[index++];
                if (ch == '"')
                {
                    return out;
                }

                if (ch == '\\')
                {
                    if (index >= input.size())
                    {
                        return std::nullopt;
                    }

                    char esc = input[index++];
                    switch (esc)
                    {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        default:
                            return std::nullopt;
                    }
                }
                else
                {
                    out.push_back(ch);
                }
            }

            return std::nullopt;
        }

        std::optional<JsonValue> parseNumber()
        {
            const size_t start = index;
            if (index < input.size() && (input[index] == '-' || input[index] == '+'))
            {
                ++index;
            }

            bool has_digit = false;
            while (index < input.size() && std::isdigit(static_cast<unsigned char>(input[index])))
            {
                has_digit = true;
                ++index;
            }

            if (index < input.size() && input[index] == '.')
            {
                ++index;
                while (index < input.size() && std::isdigit(static_cast<unsigned char>(input[index])))
                {
                    has_digit = true;
                    ++index;
                }
            }

            if (!has_digit)
            {
                return std::nullopt;
            }

            if (index < input.size() && (input[index] == 'e' || input[index] == 'E'))
            {
                ++index;
                if (index < input.size() && (input[index] == '-' || input[index] == '+'))
                {
                    ++index;
                }

                bool exp_digit = false;
                while (index < input.size() && std::isdigit(static_cast<unsigned char>(input[index])))
                {
                    exp_digit = true;
                    ++index;
                }

                if (!exp_digit)
                {
                    return std::nullopt;
                }
            }

            char* end_ptr = nullptr;
            const std::string number_text = input.substr(start, index - start);
            const double value = std::strtod(number_text.c_str(), &end_ptr);
            if (end_ptr == nullptr || *end_ptr != '\0')
            {
                return std::nullopt;
            }

            return JsonValue::makeNumber(value);
        }

        std::optional<JsonValue> parseArray()
        {
            if (index >= input.size() || input[index] != '[')
            {
                return std::nullopt;
            }
            ++index;

            std::vector<JsonValue> values;
            skipWhitespace();
            if (index < input.size() && input[index] == ']')
            {
                ++index;
                return JsonValue::makeArray(values);
            }

            while (true)
            {
                skipWhitespace();
                auto maybe_value = parseValue();
                if (!maybe_value.has_value())
                {
                    return std::nullopt;
                }
                values.push_back(std::move(maybe_value.value()));

                skipWhitespace();
                if (index >= input.size())
                {
                    return std::nullopt;
                }

                if (input[index] == ']')
                {
                    ++index;
                    return JsonValue::makeArray(values);
                }

                if (input[index] != ',')
                {
                    return std::nullopt;
                }
                ++index;
            }
        }

        std::optional<JsonValue> parseObject()
        {
            if (index >= input.size() || input[index] != '{')
            {
                return std::nullopt;
            }
            ++index;

            std::map<std::string, JsonValue> values;
            skipWhitespace();
            if (index < input.size() && input[index] == '}')
            {
                ++index;
                return JsonValue::makeObject(values);
            }

            while (true)
            {
                skipWhitespace();
                auto maybe_key = parseStringRaw();
                if (!maybe_key.has_value())
                {
                    return std::nullopt;
                }

                skipWhitespace();
                if (index >= input.size() || input[index] != ':')
                {
                    return std::nullopt;
                }
                ++index;

                skipWhitespace();
                auto maybe_value = parseValue();
                if (!maybe_value.has_value())
                {
                    return std::nullopt;
                }

                values[maybe_key.value()] = std::move(maybe_value.value());

                skipWhitespace();
                if (index >= input.size())
                {
                    return std::nullopt;
                }

                if (input[index] == '}')
                {
                    ++index;
                    return JsonValue::makeObject(values);
                }

                if (input[index] != ',')
                {
                    return std::nullopt;
                }
                ++index;
            }
        }

    public:
        explicit JsonParser(const std::string& json_text)
            : input(json_text)
        {
        }

        std::optional<JsonValue> parseValue()
        {
            skipWhitespace();
            if (index >= input.size())
            {
                return std::nullopt;
            }

            if (input[index] == '{')
            {
                return parseObject();
            }
            if (input[index] == '[')
            {
                return parseArray();
            }
            if (input[index] == '"')
            {
                auto text = parseStringRaw();
                if (!text.has_value())
                {
                    return std::nullopt;
                }
                return JsonValue::makeString(text.value());
            }
            if (input[index] == 't')
            {
                if (!consumeLiteral("true"))
                {
                    return std::nullopt;
                }
                return JsonValue::makeBool(true);
            }
            if (input[index] == 'f')
            {
                if (!consumeLiteral("false"))
                {
                    return std::nullopt;
                }
                return JsonValue::makeBool(false);
            }
            if (input[index] == 'n')
            {
                if (!consumeLiteral("null"))
                {
                    return std::nullopt;
                }
                return JsonValue::makeNull();
            }

            return parseNumber();
        }

        std::optional<JsonValue> parseRoot()
        {
            auto maybe_value = parseValue();
            if (!maybe_value.has_value())
            {
                return std::nullopt;
            }

            skipWhitespace();
            if (index != input.size())
            {
                return std::nullopt;
            }

            return maybe_value;
        }
    };

    std::string trim(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    std::string lowerCopy(const std::string& input)
    {
        std::string lower = input;
        std::transform(
            lower.begin(),
            lower.end(),
            lower.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
        );
        return lower;
    }

    std::string upperCopy(const std::string& input)
    {
        std::string upper = input;
        std::transform(
            upper.begin(),
            upper.end(),
            upper.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); }
        );
        return upper;
    }

    std::string normalizeMarketState(const std::string& raw)
    {
        std::string normalized = upperCopy(trim(raw));
        if (normalized.empty())
        {
            return "UNKNOWN";
        }

        if (normalized == "OPEN" || normalized == "TRADING" || normalized == "OPEN_MARKET")
        {
            return "REGULAR";
        }

        for (char& ch : normalized)
        {
            if (ch == ' ' || ch == '-')
            {
                ch = '_';
            }
        }

        if (normalized.find("REGULAR") != std::string::npos)
        {
            return "REGULAR";
        }
        if (normalized == "PRE" || normalized == "PREPRE" ||
            normalized.find("PRE") == 0)
        {
            return "PRE";
        }
        if (normalized == "POST" || normalized == "POSTPOST" ||
            normalized.find("POST") == 0)
        {
            return "POST";
        }
        if (normalized.find("CLOSED") != std::string::npos)
        {
            return "CLOSED";
        }

        return normalized;
    }

    std::string shellEscapeSingleQuoted(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8);

        for (const char ch : value)
        {
            if (ch == '\'')
            {
                escaped += "'\\''";
            }
            else
            {
                escaped.push_back(ch);
            }
        }

        return escaped;
    }

    bool isFinitePositive(double value)
    {
        return std::isfinite(value) && value > 0.0;
    }

    bool isFiniteNonNegative(double value)
    {
        return std::isfinite(value) && value >= 0.0;
    }

    bool isValidTickerSymbol(const std::string& ticker)
    {
        if (ticker.empty() || ticker.size() > 12)
        {
            return false;
        }

        for (const char ch : ticker)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (!std::isalnum(uch) && ch != '.' && ch != '-' && ch != '_')
            {
                return false;
            }
        }

        return true;
    }

    bool isValidPortfolioName(const std::string& name)
    {
        if (name.empty() || name.size() > 80)
        {
            return false;
        }

        if (name == "." || name == "..")
        {
            return false;
        }

        for (const char ch : name)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (!std::isalnum(uch) && ch != '_' && ch != '-' && ch != '.' && ch != ' ')
            {
                return false;
            }
        }

        return true;
    }

    // ISO 4217 codes are 3 uppercase letters (we already upperCopy on input).
    bool isValidCurrencyCode(const std::string& ccy)
    {
        if (ccy.size() != 3)
        {
            return false;
        }
        for (const char ch : ccy)
        {
            if (ch < 'A' || ch > 'Z')
            {
                return false;
            }
        }
        return true;
    }

    std::optional<PortfolioType> parsePortfolioType(const std::string& raw)
    {
        std::string normalized = upperCopy(trim(raw));
        for (char& ch : normalized)
        {
            if (ch == ' ' || ch == '-')
            {
                ch = '_';
            }
        }

        if (normalized == "BROKERAGE")
        {
            return PortfolioType::BROKERAGE;
        }
        if (normalized == "ROTH_IRA")
        {
            return PortfolioType::ROTH_IRA;
        }
        if (normalized == "TRADITIONAL_IRA")
        {
            return PortfolioType::TRADITIONAL_IRA;
        }
        if (normalized == "WATCHLIST")
        {
            return PortfolioType::WATCHLIST;
        }
        if (normalized == "CASH")
        {
            return PortfolioType::CASH;
        }
        if (normalized == "CRYPTO")
        {
            return PortfolioType::CRYPTO;
        }
        if (normalized == "DEBT")
        {
            return PortfolioType::DEBT;
        }

        return std::nullopt;
    }

    bool maybeTickerExistsOnYahoo(const std::string& ticker, bool& is_invalid_ticker)
    {
        is_invalid_ticker = false;

        const std::string url =
            "https://query1.finance.yahoo.com/v8/finance/chart/" + ticker +
            "?interval=1d&range=5d";

        const std::string command =
            "curl -sS --compressed -w '\\n%{http_code}' --connect-timeout 8 --max-time 15 "
            "-H 'User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36' "
            "-H 'Accept: application/json' "
            "'" + shellEscapeSingleQuoted(url) + "'";

        std::array<char, 4096> buffer = {};
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
        {
            return false;
        }

        std::string response;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            response += buffer.data();
        }

        const int status = pclose(pipe);
        if (status != 0)
        {
            return false;
        }

        const size_t last_newline = response.rfind('\n');
        if (last_newline == std::string::npos || last_newline + 1 >= response.size())
        {
            return false;
        }

        std::string http_status_str = response.substr(last_newline + 1);
        const size_t end = http_status_str.find_last_not_of(" \n\r\t");
        if (end == std::string::npos)
        {
            return false;
        }
        http_status_str.erase(end + 1);
        response = response.substr(0, last_newline);

        int http_code = 0;
        try
        {
            http_code = std::stoi(http_status_str);
        }
        catch (const std::exception&)
        {
            return false;
        }

        if (http_code == 404)
        {
            is_invalid_ticker = true;
            return true;
        }

        if (http_code < 200 || http_code >= 300)
        {
            return false;
        }

        if (response.find("\"result\":null") != std::string::npos ||
            response.find("No data found, symbol may be delisted") != std::string::npos ||
            response.find("\"No matching Symbol.\"") != std::string::npos)
        {
            is_invalid_ticker = true;
            return true;
        }

        return true;
    }

    // Tuple layout: <regular_market_price, regular_market_time, market_state, regular_market_open>
    bool fetchYahooLiveQuotes(const std::vector<std::string>& symbols,
                              std::map<std::string, std::tuple<double, time_t, std::string, double>>& out_quotes,
                              std::string& error)
    {
        out_quotes.clear();
        if (symbols.empty())
        {
            return true;
        }

        std::ostringstream symbols_param;
        for (size_t i = 0; i < symbols.size(); ++i)
        {
            if (i > 0)
            {
                symbols_param << ',';
            }
            symbols_param << upperCopy(trim(symbols[i]));
        }

        const std::string url =
            "https://query1.finance.yahoo.com/v7/finance/quote?symbols=" + symbols_param.str();

        const std::string command =
            "curl -sS --compressed -w '\\n%{http_code}' --connect-timeout 8 --max-time 20 "
            "-H 'User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36' "
            "-H 'Accept: application/json' "
            "'" + shellEscapeSingleQuoted(url) + "'";

        std::array<char, 4096> buffer = {};
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
        {
            error = "Failed to execute quote lookup";
            return false;
        }

        std::string response;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            response += buffer.data();
        }

        const int status = pclose(pipe);
        if (status != 0)
        {
            error = "Quote request failed";
            return false;
        }

        const size_t last_newline = response.rfind('\n');
        if (last_newline == std::string::npos || last_newline + 1 >= response.size())
        {
            error = "Malformed quote response";
            return false;
        }

        std::string http_status_str = response.substr(last_newline + 1);
        const size_t end = http_status_str.find_last_not_of(" \n\r\t");
        if (end == std::string::npos)
        {
            error = "Quote response did not include status";
            return false;
        }
        http_status_str.erase(end + 1);
        response = response.substr(0, last_newline);

        int http_code = 0;
        try
        {
            http_code = std::stoi(http_status_str);
        }
        catch (const std::exception&)
        {
            error = "Failed to parse quote HTTP status";
            return false;
        }

        if (http_code != 200)
        {
            // Some Yahoo environments intermittently return 401 for the batch quote API.
            // Fall back to per-symbol chart endpoint so live features still work.
            if (http_code == 401)
            {
                auto parseJsonNumberField = [](const std::string& json,
                                               const std::string& key,
                                               double& out_value) -> bool
                {
                    const std::string marker = "\"" + key + "\":";
                    const size_t pos = json.find(marker);
                    if (pos == std::string::npos)
                    {
                        return false;
                    }

                    size_t start = pos + marker.size();
                    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])))
                    {
                        ++start;
                    }

                    size_t end = start;
                    while (end < json.size())
                    {
                        const char ch = json[end];
                        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E')
                        {
                            ++end;
                            continue;
                        }
                        break;
                    }

                    if (end <= start)
                    {
                        return false;
                    }

                    try
                    {
                        out_value = std::stod(json.substr(start, end - start));
                        return std::isfinite(out_value);
                    }
                    catch (const std::exception&)
                    {
                        return false;
                    }
                };

                auto parseJsonStringField = [](const std::string& json,
                                               const std::string& key,
                                               std::string& out_value) -> bool
                {
                    const std::string marker = "\"" + key + "\":";
                    const size_t pos = json.find(marker);
                    if (pos == std::string::npos)
                    {
                        return false;
                    }

                    size_t quote_start = json.find('"', pos + marker.size());
                    if (quote_start == std::string::npos)
                    {
                        return false;
                    }

                    size_t quote_end = json.find('"', quote_start + 1);
                    if (quote_end == std::string::npos)
                    {
                        return false;
                    }

                    out_value = json.substr(quote_start + 1, quote_end - quote_start - 1);
                    return true;
                };

                size_t fallback_success_count = 0;
                for (const std::string& symbol_raw : symbols)
                {
                    const std::string symbol = upperCopy(trim(symbol_raw));
                    if (symbol.empty())
                    {
                        continue;
                    }

                    const std::string chart_url =
                        "https://query1.finance.yahoo.com/v8/finance/chart/" + symbol +
                        "?interval=1d&range=1d";

                    const std::string chart_command =
                        "curl -sS --compressed -w '\\n%{http_code}' --connect-timeout 8 --max-time 20 "
                        "-H 'User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36' "
                        "-H 'Accept: application/json' "
                        "'" + shellEscapeSingleQuoted(chart_url) + "'";

                    std::array<char, 4096> chart_buffer = {};
                    FILE* chart_pipe = popen(chart_command.c_str(), "r");
                    if (chart_pipe == nullptr)
                    {
                        continue;
                    }

                    std::string chart_response;
                    while (fgets(chart_buffer.data(), static_cast<int>(chart_buffer.size()), chart_pipe) != nullptr)
                    {
                        chart_response += chart_buffer.data();
                    }

                    const int chart_status = pclose(chart_pipe);
                    if (chart_status != 0)
                    {
                        continue;
                    }

                    const size_t chart_last_newline = chart_response.rfind('\n');
                    if (chart_last_newline == std::string::npos || chart_last_newline + 1 >= chart_response.size())
                    {
                        continue;
                    }

                    std::string chart_http_status_str = chart_response.substr(chart_last_newline + 1);
                    const size_t chart_trim_end = chart_http_status_str.find_last_not_of(" \n\r\t");
                    if (chart_trim_end == std::string::npos)
                    {
                        continue;
                    }
                    chart_http_status_str.erase(chart_trim_end + 1);
                    chart_response = chart_response.substr(0, chart_last_newline);

                    int chart_http_code = 0;
                    try
                    {
                        chart_http_code = std::stoi(chart_http_status_str);
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }

                    if (chart_http_code != 200)
                    {
                        continue;
                    }

                    double market_price = 0.0;
                    if (!parseJsonNumberField(chart_response, "regularMarketPrice", market_price) || market_price <= 0.0)
                    {
                        continue;
                    }

                    double market_time_raw = static_cast<double>(std::time(nullptr));
                    parseJsonNumberField(chart_response, "regularMarketTime", market_time_raw);
                    const time_t market_time =
                        (std::isfinite(market_time_raw) && market_time_raw > 0.0)
                            ? static_cast<time_t>(std::llround(market_time_raw))
                            : std::time(nullptr);

                    std::string market_state = "UNKNOWN";
                    if (parseJsonStringField(chart_response, "marketState", market_state))
                    {
                        market_state = normalizeMarketState(market_state);
                    }

                    double market_open = 0.0;
                    if (!parseJsonNumberField(chart_response, "regularMarketOpen", market_open) || !std::isfinite(market_open) || market_open <= 0.0)
                    {
                        market_open = 0.0;
                    }

                    out_quotes[symbol] = std::make_tuple(market_price, market_time, market_state, market_open);
                    ++fallback_success_count;
                }

                if (fallback_success_count > 0)
                {
                    return true;
                }
            }

            error = "Yahoo quote request returned HTTP " + std::to_string(http_code);
            return false;
        }

        JsonParser parser(response);
        auto root = parser.parseRoot();
        if (!root.has_value() || root->type != JsonType::OBJECT)
        {
            error = "Malformed Yahoo quote JSON";
            return false;
        }

        auto quote_response_it = root->object_value.find("quoteResponse");
        if (quote_response_it == root->object_value.end() || quote_response_it->second.type != JsonType::OBJECT)
        {
            error = "Yahoo quote payload missing quoteResponse";
            return false;
        }

        auto result_it = quote_response_it->second.object_value.find("result");
        if (result_it == quote_response_it->second.object_value.end() || result_it->second.type != JsonType::ARRAY)
        {
            error = "Yahoo quote payload missing result array";
            return false;
        }

        for (const JsonValue& entry : result_it->second.array_value)
        {
            if (entry.type != JsonType::OBJECT)
            {
                continue;
            }

            auto symbol_it = entry.object_value.find("symbol");
            auto price_it = entry.object_value.find("regularMarketPrice");
            if (symbol_it == entry.object_value.end() || price_it == entry.object_value.end())
            {
                continue;
            }
            if (symbol_it->second.type != JsonType::STRING || price_it->second.type != JsonType::NUMBER)
            {
                continue;
            }

            const std::string symbol = upperCopy(trim(symbol_it->second.string_value));
            const double price = price_it->second.number_value;
            if (symbol.empty() || !std::isfinite(price) || price <= 0.0)
            {
                continue;
            }

            std::string market_state = "UNKNOWN";
            auto market_state_it = entry.object_value.find("marketState");
            if (market_state_it != entry.object_value.end() && market_state_it->second.type == JsonType::STRING)
            {
                market_state = normalizeMarketState(market_state_it->second.string_value);
            }

            time_t market_time = std::time(nullptr);
            auto market_time_it = entry.object_value.find("regularMarketTime");
            if (market_time_it != entry.object_value.end() && market_time_it->second.type == JsonType::NUMBER)
            {
                const double parsed_time = market_time_it->second.number_value;
                if (std::isfinite(parsed_time) && parsed_time > 0.0)
                {
                    market_time = static_cast<time_t>(std::llround(parsed_time));
                }
            }

            double market_open = 0.0;
            auto market_open_it = entry.object_value.find("regularMarketOpen");
            if (market_open_it != entry.object_value.end() && market_open_it->second.type == JsonType::NUMBER)
            {
                const double parsed_open = market_open_it->second.number_value;
                if (std::isfinite(parsed_open) && parsed_open > 0.0)
                {
                    market_open = parsed_open;
                }
            }

            out_quotes[symbol] = std::make_tuple(price, market_time, market_state, market_open);
        }

        return true;
    }

    struct LiveQuoteCacheEntry
    {
        std::tuple<double, time_t, std::string, double> quote;
        time_t cached_at;
    };

    std::mutex g_live_quote_cache_mutex;
    std::map<std::string, LiveQuoteCacheEntry> g_live_quote_cache;
    std::map<std::string, time_t> g_live_quote_failure_cache;
    constexpr time_t LIVE_QUOTE_CACHE_TTL_SECONDS = 30;

    bool fetchLiveQuotesCached(const std::vector<std::string>& symbols,
                               std::map<std::string, std::tuple<double, time_t, std::string, double>>& out_quotes,
                               std::string& error)
    {
        out_quotes.clear();

        std::lock_guard<std::mutex> lock(g_live_quote_cache_mutex);
        const time_t check_time = std::time(nullptr);

        std::vector<std::string> tickers_to_fetch;
        tickers_to_fetch.reserve(symbols.size());

        for (const std::string& raw : symbols)
        {
            const std::string normalized = upperCopy(trim(raw));
            if (normalized.empty())
            {
                continue;
            }
            auto cached_it = g_live_quote_cache.find(normalized);
            if (cached_it != g_live_quote_cache.end() &&
                (check_time - cached_it->second.cached_at) <= LIVE_QUOTE_CACHE_TTL_SECONDS)
            {
                out_quotes[normalized] = cached_it->second.quote;
                continue;
            }
            // Skip symbols that recently failed: prevents every request from
            // re-running an 8s+ curl timeout when Yahoo is unreachable.
            auto failure_it = g_live_quote_failure_cache.find(normalized);
            if (failure_it != g_live_quote_failure_cache.end() &&
                (check_time - failure_it->second) <= LIVE_QUOTE_CACHE_TTL_SECONDS)
            {
                continue;
            }
            tickers_to_fetch.push_back(normalized);
        }

        if (tickers_to_fetch.empty())
        {
            return true;
        }

        std::map<std::string, std::tuple<double, time_t, std::string, double>> fresh;
        const bool fetch_ok = fetchYahooLiveQuotes(tickers_to_fetch, fresh, error);
        const time_t fetched_at = std::time(nullptr);

        for (const auto& entry : fresh)
        {
            g_live_quote_cache[entry.first] = LiveQuoteCacheEntry{entry.second, fetched_at};
            g_live_quote_failure_cache.erase(entry.first);
            out_quotes[entry.first] = entry.second;
        }
        for (const std::string& ticker : tickers_to_fetch)
        {
            if (fresh.find(ticker) == fresh.end())
            {
                g_live_quote_failure_cache[ticker] = fetched_at;
            }
        }

        return fetch_ok;
    }

    // Disk-read caches keyed by file path + (mtime, size). A fresh
    // PortfolioManager is constructed per request, so these are process-global
    // (like g_live_quote_cache) to survive across requests. Entries are
    // validated by file metadata on every access: when a sync rewrites a
    // portfolio or stock file its mtime/size change and the stale entry is
    // re-read. Worst case on a concurrent write is one extra read, never a
    // stale serve. The dashboard polls every 15s and recomputes the same
    // portfolio/stock metrics several times per request, so caching parsed
    // data turns that into a single parse per file per change.
    std::mutex g_file_cache_mutex;

    struct StockCacheEntry
    {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
        StockData data;
    };
    std::unordered_map<std::string, StockCacheEntry> g_stock_cache;

    struct PortfolioCacheEntry
    {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
        Portfolio data;
    };
    std::unordered_map<std::string, PortfolioCacheEntry> g_portfolio_cache;

    struct StockListCacheEntry
    {
        std::filesystem::file_time_type dir_mtime;
        std::vector<std::string> tickers;
    };
    std::unordered_map<std::string, StockListCacheEntry> g_stock_list_cache;

    bool loadStockDataCached(PortfolioManager& manager,
                             const std::string& portfolio_name,
                             const std::string& ticker,
                             StockData& out)
    {
        const std::string path = manager.getStockFilePath(portfolio_name, ticker);
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return false;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            auto it = g_stock_cache.find(path);
            if (it != g_stock_cache.end() && it->second.mtime == mtime && it->second.size == size)
            {
                out = it->second.data;
                return true;
            }
        }

        StockData data;
        if (!manager.loadStockData(portfolio_name, ticker, data))
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            g_stock_cache[path] = StockCacheEntry{mtime, size, data};
        }
        out = std::move(data);
        return true;
    }

    bool loadPortfolioCached(PortfolioManager& manager,
                             const std::string& name,
                             Portfolio& out)
    {
        const std::string path = manager.getPortfolioFilePath(name);
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return false;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            auto it = g_portfolio_cache.find(path);
            if (it != g_portfolio_cache.end() && it->second.mtime == mtime && it->second.size == size)
            {
                out = it->second.data;
                return true;
            }
        }

        Portfolio data;
        if (!manager.loadPortfolio(name, data))
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            g_portfolio_cache[path] = PortfolioCacheEntry{mtime, size, data};
        }
        out = std::move(data);
        return true;
    }

    std::vector<std::string> listStocksCached(PortfolioManager& manager,
                                              const std::string& portfolio_name)
    {
        const std::string dir = manager.getStocksDirectoryPath(portfolio_name);
        std::error_code ec;
        const auto dir_mtime = std::filesystem::last_write_time(dir, ec);
        if (ec)
        {
            // No stocks directory yet (e.g. cash/debt/empty account).
            return {};
        }

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            auto it = g_stock_list_cache.find(dir);
            if (it != g_stock_list_cache.end() && it->second.dir_mtime == dir_mtime)
            {
                return it->second.tickers;
            }
        }

        std::vector<std::string> tickers = manager.listStocks(portfolio_name);

        {
            std::lock_guard<std::mutex> lock(g_file_cache_mutex);
            g_stock_list_cache[dir] = StockListCacheEntry{dir_mtime, tickers};
        }
        return tickers;
    }

    // Returns USD-per-unit of the given foreign currency. USD -> 1.0.
    // Tries Yahoo Finance FX pair like "EURUSD=X" via the live-quote cache.
    // Returns 1.0 on lookup failure so totals still render (best-effort).
    double fetchUsdRateForCurrency(const std::string& currency_code)
    {
        const std::string ccy = upperCopy(trim(currency_code));
        if (ccy.empty() || ccy == "USD")
        {
            return 1.0;
        }
        if (!isValidCurrencyCode(ccy))
        {
            return 1.0;
        }

        const std::string pair = ccy + "USD=X";
        std::map<std::string, std::tuple<double, time_t, std::string, double>> quotes;
        std::string error;
        if (!fetchLiveQuotesCached({pair}, quotes, error))
        {
            return 1.0;
        }
        auto it = quotes.find(pair);
        if (it == quotes.end())
        {
            return 1.0;
        }
        const double rate = std::get<0>(it->second);
        return (std::isfinite(rate) && rate > 0.0) ? rate : 1.0;
    }

    bool persistLiveQuoteForTicker(PortfolioManager& manager,
                                   const std::string& portfolio_name,
                                   const std::string& ticker,
                                   std::string& error)
    {
        const std::string normalized_ticker = upperCopy(trim(ticker));
        if (!isValidTickerSymbol(normalized_ticker))
        {
            error = "Invalid ticker";
            return false;
        }

        std::map<std::string, std::tuple<double, time_t, std::string, double>> quotes;
        std::string quote_error;
        if (!fetchYahooLiveQuotes({normalized_ticker}, quotes, quote_error))
        {
            error = quote_error;
            return false;
        }

        auto quote_it = quotes.find(normalized_ticker);
        if (quote_it == quotes.end())
        {
            error = "No live quote returned";
            return false;
        }

        StockData stock;
        if (!manager.loadStockData(portfolio_name, normalized_ticker, stock))
        {
            error = "Stock file not found";
            return false;
        }

        const double quote_price = std::get<0>(quote_it->second);
        const time_t quote_as_of = std::get<1>(quote_it->second);

        stock.addDailyClosePrice(quote_as_of, quote_price);
        if (!manager.saveStockData(portfolio_name, stock))
        {
            error = "Failed to persist live quote";
            return false;
        }

        return true;
    }

    std::string jsonEscape(const std::string& value)
    {
        std::ostringstream out;
        for (char ch : value)
        {
            switch (ch)
            {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                    {
                        out << "\\u"
                            << std::hex
                            << std::uppercase
                            << std::setw(4)
                            << std::setfill('0')
                            << static_cast<int>(static_cast<unsigned char>(ch))
                            << std::dec;
                    }
                    else
                    {
                        out << ch;
                    }
                    break;
            }
        }

        return out.str();
    }

    std::string jsonString(const std::string& value)
    {
        return "\"" + jsonEscape(value) + "\"";
    }

    std::string jsonNumber(double value)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(6) << value;
        std::string rendered = out.str();

        while (rendered.size() > 1 && rendered.back() == '0')
        {
            rendered.pop_back();
        }
        if (!rendered.empty() && rendered.back() == '.')
        {
            rendered.push_back('0');
        }

        return rendered;
    }

    std::string transactionTypeToString(TransactionType type)
    {
        switch (type)
        {
            case TransactionType::DEPOSIT: return "DEPOSIT";
            case TransactionType::WITHDRAWAL: return "WITHDRAWAL";
            case TransactionType::BUY_STOCK: return "BUY_STOCK";
            case TransactionType::SELL_STOCK: return "SELL_STOCK";
            case TransactionType::DIVIDEND: return "DIVIDEND";
            case TransactionType::INTEREST: return "INTEREST";
            case TransactionType::TRANSFER_IN_ASSET: return "TRANSFER_IN_ASSET";
            case TransactionType::TRANSFER_OUT_ASSET: return "TRANSFER_OUT_ASSET";
        }

        return "UNKNOWN";
    }

    std::string portfolioTypeToString(PortfolioType type)
    {
        switch (type)
        {
            case PortfolioType::BROKERAGE: return "BROKERAGE";
            case PortfolioType::ROTH_IRA: return "ROTH_IRA";
            case PortfolioType::TRADITIONAL_IRA: return "TRADITIONAL_IRA";
            case PortfolioType::WATCHLIST: return "WATCHLIST";
            case PortfolioType::CASH: return "CASH";
            case PortfolioType::CRYPTO: return "CRYPTO";
            case PortfolioType::DEBT: return "DEBT";
        }

        return "UNKNOWN";
    }

    std::string stockEventTypeToString(StockEventType type)
    {
        switch (type)
        {
            case StockEventType::BUY: return "BUY";
            case StockEventType::SELL: return "SELL";
            case StockEventType::DIVIDEND: return "DIVIDEND";
        }

        return "UNKNOWN";
    }

    std::vector<std::string> splitPath(const std::string& path)
    {
        std::vector<std::string> segments;
        std::string current;
        for (char ch : path)
        {
            if (ch == '/')
            {
                if (!current.empty())
                {
                    segments.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }

        if (!current.empty())
        {
            segments.push_back(current);
        }

        return segments;
    }

    std::string percentDecode(const std::string& raw)
    {
        std::string decoded;
        decoded.reserve(raw.size());

        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '%' && i + 2 < raw.size())
            {
                const std::string hex = raw.substr(i + 1, 2);
                char* end_ptr = nullptr;
                const long value = std::strtol(hex.c_str(), &end_ptr, 16);
                if (end_ptr != nullptr && *end_ptr == '\0')
                {
                    decoded.push_back(static_cast<char>(value));
                    i += 2;
                    continue;
                }
            }

            if (raw[i] == '+')
            {
                decoded.push_back(' ');
            }
            else
            {
                decoded.push_back(raw[i]);
            }
        }

        return decoded;
    }

    std::map<std::string, std::string> parseQuery(const std::string& query)
    {
        std::map<std::string, std::string> values;
        size_t start = 0;

        while (start < query.size())
        {
            size_t amp = query.find('&', start);
            if (amp == std::string::npos)
            {
                amp = query.size();
            }

            const std::string pair = query.substr(start, amp - start);
            const size_t eq = pair.find('=');

            if (eq == std::string::npos)
            {
                values[percentDecode(pair)] = "";
            }
            else
            {
                const std::string key = percentDecode(pair.substr(0, eq));
                const std::string value = percentDecode(pair.substr(eq + 1));
                values[key] = value;
            }

            start = amp + 1;
        }

        return values;
    }

    std::optional<int> parsePositiveInt(const std::string& value)
    {
        if (value.empty())
        {
            return std::nullopt;
        }

        char* end_ptr = nullptr;
        long parsed = std::strtol(value.c_str(), &end_ptr, 10);
        if (end_ptr == nullptr || *end_ptr != '\0' || parsed <= 0)
        {
            return std::nullopt;
        }

        return static_cast<int>(parsed);
    }

    std::optional<double> getObjectNumber(const JsonValue& object, const std::string& key)
    {
        if (object.type != JsonType::OBJECT)
        {
            return std::nullopt;
        }

        auto it = object.object_value.find(key);
        if (it == object.object_value.end() || it->second.type != JsonType::NUMBER)
        {
            return std::nullopt;
        }

        return it->second.number_value;
    }

    std::optional<std::string> getObjectString(const JsonValue& object, const std::string& key)
    {
        if (object.type != JsonType::OBJECT)
        {
            return std::nullopt;
        }

        auto it = object.object_value.find(key);
        if (it == object.object_value.end() || it->second.type != JsonType::STRING)
        {
            return std::nullopt;
        }

        return it->second.string_value;
    }

    std::string makeErrorBody(const std::string& message)
    {
        return std::string("{\"error\":") + jsonString(message) + "}";
    }

    std::string serializeDailyValues(const std::vector<DailyPortfolioValue>& values)
    {
        std::vector<DailyPortfolioValue> sorted = values;
        std::sort(
            sorted.begin(),
            sorted.end(),
            [](const DailyPortfolioValue& lhs, const DailyPortfolioValue& rhs)
            {
                return lhs.date < rhs.date;
            }
        );

        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }

            out << "{"
                << "\"date\":" << static_cast<long long>(sorted[i].date) << ","
                << "\"value\":" << jsonNumber(sorted[i].value) << ","
                << "\"last_updated\":" << static_cast<long long>(sorted[i].last_updated)
                << "}";
        }
        out << "]";
        return out.str();
    }


    std::string serializeTransactions(std::vector<Transaction> txs, int limit)
    {
        // Compute realized profit per SELL_STOCK by walking transactions in chronological order
        // and tracking running per-ticker share count and cost basis.
        std::vector<size_t> ascending_order;
        ascending_order.reserve(txs.size());
        for (size_t i = 0; i < txs.size(); ++i)
        {
            ascending_order.push_back(i);
        }
        std::sort(
            ascending_order.begin(),
            ascending_order.end(),
            [&txs](size_t a, size_t b)
            {
                return txs[a].date < txs[b].date;
            }
        );

        struct Lot { double shares = 0.0; double total_cost = 0.0; };
        std::map<std::string, Lot> lots;
        std::map<size_t, double> profit_by_index;
        for (size_t idx : ascending_order)
        {
            const Transaction& tx = txs[idx];
            if (tx.stock_symbol.empty())
            {
                continue;
            }
            const std::string ticker = upperCopy(tx.stock_symbol);
            Lot& lot = lots[ticker];
            if (tx.type == TransactionType::BUY_STOCK)
            {
                lot.shares += tx.shares;
                lot.total_cost += -tx.amount; // amount is negative for buys
            }
            else if (tx.type == TransactionType::TRANSFER_IN_ASSET)
            {
                lot.shares += tx.shares;
                lot.total_cost += tx.amount; // amount holds the cost basis for transfers in
            }
            else if (tx.type == TransactionType::TRANSFER_OUT_ASSET)
            {
                const double avg_cost = lot.shares > 0.0 ? lot.total_cost / lot.shares : 0.0;
                const double cost_basis = avg_cost * tx.shares;
                lot.shares -= tx.shares;
                if (lot.shares <= 1e-9)
                {
                    lot.shares = 0.0;
                    lot.total_cost = 0.0;
                }
                else
                {
                    lot.total_cost -= cost_basis;
                    if (lot.total_cost < 0.0)
                    {
                        lot.total_cost = 0.0;
                    }
                }
            }
            else if (tx.type == TransactionType::SELL_STOCK)
            {
                const double avg_cost = lot.shares > 0.0 ? lot.total_cost / lot.shares : 0.0;
                const double cost_basis = avg_cost * tx.shares;
                profit_by_index[idx] = tx.amount - cost_basis;
                lot.shares -= tx.shares;
                if (lot.shares <= 1e-9)
                {
                    lot.shares = 0.0;
                    lot.total_cost = 0.0;
                }
                else
                {
                    lot.total_cost -= cost_basis;
                    if (lot.total_cost < 0.0)
                    {
                        lot.total_cost = 0.0;
                    }
                }
            }
        }

        std::vector<size_t> descending_order;
        descending_order.reserve(txs.size());
        for (size_t i = 0; i < txs.size(); ++i)
        {
            descending_order.push_back(i);
        }
        std::sort(
            descending_order.begin(),
            descending_order.end(),
            [&txs](size_t a, size_t b)
            {
                return txs[a].date > txs[b].date;
            }
        );

        if (limit > 0 && static_cast<size_t>(limit) < descending_order.size())
        {
            descending_order.resize(static_cast<size_t>(limit));
        }

        std::ostringstream out;
        out << "[";
        for (size_t k = 0; k < descending_order.size(); ++k)
        {
            if (k > 0)
            {
                out << ",";
            }
            const size_t idx = descending_order[k];
            const Transaction& tx = txs[idx];
            out << "{"
                << "\"date\":" << static_cast<long long>(tx.date) << ","
                << "\"amount\":" << jsonNumber(tx.amount) << ","
                << "\"type\":" << jsonString(transactionTypeToString(tx.type)) << ","
                << "\"stock_symbol\":" << jsonString(tx.stock_symbol) << ","
                << "\"shares\":" << jsonNumber(tx.shares) << ","
                << "\"notes\":" << jsonString(tx.notes) << ","
                << "\"category\":" << jsonString(tx.category);
            auto profit_it = profit_by_index.find(idx);
            if (profit_it != profit_by_index.end())
            {
                out << ",\"realized_profit\":" << jsonNumber(profit_it->second);
            }
            out << "}";
        }
        out << "]";
        return out.str();
    }

    bool hasSupportedPortfolioHeader(const std::string& file_path)
    {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        uint32_t version = 0;
        uint8_t type = 255;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&type), sizeof(type));

        if (!file.good())
        {
            return false;
        }

        if (version < OLDEST_SUPPORTED_FILE_VERSION || version > CURRENT_PORTFOLIO_FILE_VERSION)
        {
            return false;
        }

        return type <= static_cast<uint8_t>(PortfolioType::DEBT);
    }

    double estimatePortfolioTotalValue(const Portfolio& portfolio, PortfolioManager& manager, const std::string& portfolio_name)
    {
        if (portfolio.getType() == PortfolioType::WATCHLIST)
        {
            return 0.0;
        }

        // DEBT accounts store outstanding balance as a positive available_capital
        // but contribute negatively to overall totals.
        if (portfolio.getType() == PortfolioType::DEBT)
        {
            return -portfolio.getAvailableCapital();
        }

        // For foreign-currency cash accounts, convert the balance into USD so the
        // dashboard total is unified. Stocks/crypto stay in USD already.
        double cash_in_usd = portfolio.getAvailableCapital();
        if (portfolio.getType() == PortfolioType::CASH)
        {
            const std::string ccy = upperCopy(trim(portfolio.getCurrency()));
            if (!ccy.empty() && ccy != "USD")
            {
                cash_in_usd *= fetchUsdRateForCurrency(ccy);
            }
        }

        double total = cash_in_usd;
        const std::vector<std::string> tickers = listStocksCached(manager, portfolio_name);

        for (const std::string& ticker : tickers)
        {
            StockData stock;
            if (!loadStockDataCached(manager, portfolio_name, ticker, stock))
            {
                continue;
            }

            const auto& prices = stock.getPriceHistory();
            if (prices.empty())
            {
                // No market data (e.g. Yahoo fetch has never succeeded for this
                // ticker) — value the position at cost basis rather than $0.
                total += stock.getSharesOwned() * stock.getAveragePurchasePrice();
                continue;
            }

            auto latest_it = std::max_element(
                prices.begin(),
                prices.end(),
                [](const DailyStockPrice& lhs, const DailyStockPrice& rhs)
                {
                    return lhs.date < rhs.date;
                }
            );

            total += stock.getSharesOwned() * latest_it->close_price;
        }

        return total;
    }

    bool getLatestAndPreviousStockPrices(const StockData& stock,
                                         double& latest_price,
                                         time_t& latest_date,
                                         double& previous_price,
                                         time_t& previous_date);

    bool calculateWatchlistDayChangeTotals(PortfolioManager& manager,
                                           const std::string& portfolio_name,
                                           double& day_change_amount,
                                           double& previous_close_total)
    {
        day_change_amount = 0.0;
        previous_close_total = 0.0;

        bool has_previous_close = false;
        const std::vector<std::string> tickers = listStocksCached(manager, portfolio_name);
        for (const std::string& ticker : tickers)
        {
            StockData stock;
            if (!loadStockDataCached(manager, portfolio_name, ticker, stock))
            {
                continue;
            }

            double latest_price = 0.0;
            time_t latest_date = 0;
            double previous_price = 0.0;
            time_t previous_date = 0;
            if (!getLatestAndPreviousStockPrices(stock, latest_price, latest_date, previous_price, previous_date))
            {
                continue;
            }

            if (!isLatestPriceFromToday(latest_date))
            {
                continue;
            }

            day_change_amount += (latest_price - previous_price);
            previous_close_total += previous_price;
            has_previous_close = true;
        }

        return has_previous_close;
    }

    double calculatePortfolioDayChangeAmount(const Portfolio& portfolio,
                                             PortfolioManager& manager,
                                             const std::string& portfolio_name)
    {
        if (portfolio.getType() == PortfolioType::WATCHLIST)
        {
            double day_change_amount = 0.0;
            double previous_close_total = 0.0;
            if (!calculateWatchlistDayChangeTotals(manager, portfolio_name, day_change_amount, previous_close_total))
            {
                return 0.0;
            }

            return day_change_amount;
        }

        if (portfolio.getType() == PortfolioType::CASH ||
            portfolio.getType() == PortfolioType::DEBT)
        {
            return 0.0;
        }

        // Market-driven day change only: sum over current holdings of
        // (latest_price - previous_close) * shares_owned. Excludes cash flows
        // (deposits, withdrawals, asset transfers) so they don't show up as
        // a fake "day change" on the dashboard.
        double day_change_amount = 0.0;
        for (const std::string& ticker : listStocksCached(manager, portfolio_name))
        {
            StockData stock;
            if (!loadStockDataCached(manager, portfolio_name, ticker, stock))
            {
                continue;
            }

            double latest_price = 0.0;
            time_t latest_date = 0;
            double previous_price = 0.0;
            time_t previous_date = 0;
            if (!getLatestAndPreviousStockPrices(stock, latest_price, latest_date, previous_price, previous_date))
            {
                continue;
            }

            if (!isLatestPriceFromToday(latest_date))
            {
                continue;
            }

            day_change_amount += stock.getSharesOwned() * (latest_price - previous_price);
        }

        return day_change_amount;
    }

    double calculatePortfolioDayChangePercent(const Portfolio& portfolio,
                                              PortfolioManager& manager,
                                              const std::string& portfolio_name)
    {
        if (portfolio.getType() == PortfolioType::WATCHLIST)
        {
            double day_change_amount = 0.0;
            double previous_close_total = 0.0;
            if (!calculateWatchlistDayChangeTotals(manager, portfolio_name, day_change_amount, previous_close_total))
            {
                return 0.0;
            }

            if (std::abs(previous_close_total) < 1e-9)
            {
                return 0.0;
            }

            return (day_change_amount / std::abs(previous_close_total)) * 100.0;
        }

        if (portfolio.getType() == PortfolioType::CASH ||
            portfolio.getType() == PortfolioType::DEBT)
        {
            return 0.0;
        }

        const double day_change_amount = calculatePortfolioDayChangeAmount(portfolio, manager, portfolio_name);
        const double current_total = estimatePortfolioTotalValue(portfolio, manager, portfolio_name);
        const double previous_total = current_total - day_change_amount;
        if (std::abs(previous_total) < 1e-9)
        {
            return 0.0;
        }
        return (day_change_amount / std::abs(previous_total)) * 100.0;
    }

    bool getLatestAndPreviousStockPrices(const StockData& stock,
                                         double& latest_price,
                                         time_t& latest_date,
                                         double& previous_price,
                                         time_t& previous_date)
    {
        const auto& prices = stock.getPriceHistory();
        if (prices.empty())
        {
            return false;
        }

        std::vector<DailyStockPrice> sorted_prices = prices;
        std::sort(
            sorted_prices.begin(),
            sorted_prices.end(),
            [](const DailyStockPrice& lhs, const DailyStockPrice& rhs)
            {
                return lhs.date < rhs.date;
            }
        );

        const DailyStockPrice& latest = sorted_prices.back();
        latest_price = latest.close_price;
        latest_date = latest.date;

        const long long latest_bucket = dayBucketForTimestamp(latest.date);
        for (auto it = sorted_prices.rbegin() + 1; it != sorted_prices.rend(); ++it)
        {
            if (dayBucketForTimestamp(it->date) < latest_bucket)
            {
                previous_price = it->close_price;
                previous_date = it->date;
                return true;
            }
        }

        return false;
    }

    double calculateStockDayChangeAmount(const StockData& stock)
    {
        double latest_price = 0.0;
        time_t latest_date = 0;
        double previous_price = 0.0;
        time_t previous_date = 0;
        if (!getLatestAndPreviousStockPrices(stock, latest_price, latest_date, previous_price, previous_date))
        {
            return 0.0;
        }

        if (!isLatestPriceFromToday(latest_date))
        {
            return 0.0;
        }

        return latest_price - previous_price;
    }

    double calculateStockDayChangePercent(const StockData& stock)
    {
        double latest_price = 0.0;
        time_t latest_date = 0;
        double previous_price = 0.0;
        time_t previous_date = 0;
        if (!getLatestAndPreviousStockPrices(stock, latest_price, latest_date, previous_price, previous_date))
        {
            return 0.0;
        }

        if (!isLatestPriceFromToday(latest_date))
        {
            return 0.0;
        }

        if (std::abs(previous_price) < 1e-9)
        {
            return 0.0;
        }

        return ((latest_price - previous_price) / std::abs(previous_price)) * 100.0;
    }

    double calculateSharesOwnedFromTransactions(const Portfolio& portfolio, const std::string& ticker)
    {
        const std::string normalized_ticker = upperCopy(ticker);
        double shares_owned = 0.0;

        for (const auto& tx : portfolio.getTransactions())
        {
            if (upperCopy(tx.stock_symbol) != normalized_ticker)
            {
                continue;
            }

            if (tx.type == TransactionType::BUY_STOCK ||
                tx.type == TransactionType::TRANSFER_IN_ASSET)
            {
                shares_owned += tx.shares;
            }
            else if (tx.type == TransactionType::SELL_STOCK ||
                     tx.type == TransactionType::TRANSFER_OUT_ASSET)
            {
                shares_owned -= tx.shares;
            }
        }

        return shares_owned;
    }

    std::string buildPortfolioSummaryJson(PortfolioManager& manager)
    {
        std::ostringstream out;
        std::vector<std::string> names;
        if (manager.scanPortfolios())
        {
            names = manager.getPortfolioNames();
        }

        out << "{\"portfolios\":[";

        bool first = true;
        for (const auto& name : names)
        {
            const std::string file_path = manager.getPortfolioFilePath(name);
            if (!std::filesystem::exists(file_path) || !hasSupportedPortfolioHeader(file_path))
            {
                continue;
            }

            Portfolio portfolio;
            if (!loadPortfolioCached(manager, name, portfolio))
            {
                continue;
            }

            if (!first)
            {
                out << ",";
            }
            first = false;

            const std::string portfolio_currency = upperCopy(trim(portfolio.getCurrency()));
            const std::string ccy_out = portfolio_currency.empty() ? std::string("USD") : portfolio_currency;
            const double fx_rate = (portfolio.getType() == PortfolioType::CASH && ccy_out != "USD")
                                       ? fetchUsdRateForCurrency(ccy_out)
                                       : 1.0;

            // Cheap connection peek for the dashboard (no need to load full token).
            bool is_synced = manager.hasConnection(name);
            std::string institution_name;
            bool needs_reauth = false;
            time_t reauth_detected_at = 0;
            if (is_synced)
            {
                PortfolioConnection peek;
                if (manager.loadConnection(name, peek))
                {
                    institution_name = peek.institution_name;
                    needs_reauth = peek.needs_reauth;
                    reauth_detected_at = peek.reauth_detected_at;
                }
            }

            out << "{"
                << "\"name\":" << jsonString(name) << ","
                << "\"type\":" << jsonString(portfolioTypeToString(portfolio.getType())) << ","
                << "\"currency\":" << jsonString(ccy_out) << ","
                << "\"fx_to_usd\":" << jsonNumber(fx_rate) << ","
                << "\"available_capital\":" << jsonNumber(portfolio.getAvailableCapital()) << ","
                << "\"reported_total_value\":" << jsonNumber(portfolio.getCurrentPortfolioValue()) << ","
                << "\"estimated_total_value\":" << jsonNumber(estimatePortfolioTotalValue(portfolio, manager, name)) << ","
                << "\"day_change_amount\":" << jsonNumber(calculatePortfolioDayChangeAmount(portfolio, manager, name)) << ","
                << "\"day_change_percent\":" << jsonNumber(calculatePortfolioDayChangePercent(portfolio, manager, name)) << ","
                << "\"stock_count\":" << listStocksCached(manager, name).size() << ","
                << "\"transaction_count\":" << portfolio.getTransactions().size() << ","
                << "\"is_synced\":" << (is_synced ? "true" : "false") << ","
                << "\"institution_name\":" << jsonString(institution_name) << ","
                << "\"needs_reauth\":" << (needs_reauth ? "true" : "false") << ","
                << "\"reauth_detected_at\":" << static_cast<long long>(reauth_detected_at) << ","
                << "\"daily_values\":" << serializeDailyValues(portfolio.getDailyValues())
                << "}";
        }

        out << "]}";
        return out.str();
    }

    std::string buildConnectionJson(const PortfolioManager& manager, const std::string& portfolio_name)
    {
        PortfolioConnection conn;
        if (!manager.loadConnection(portfolio_name, conn))
        {
            return "null";
        }
        std::ostringstream out;
        out << "{"
            << "\"provider\":" << jsonString(conn.provider) << ","
            << "\"institution_name\":" << jsonString(conn.institution_name) << ","
            << "\"institution_id\":" << jsonString(conn.institution_id) << ","
            << "\"account_id\":" << jsonString(conn.account_id) << ","
            << "\"connected_at\":" << static_cast<long long>(conn.connected_at) << ","
            << "\"last_synced\":" << static_cast<long long>(conn.last_synced) << ","
            << "\"needs_reauth\":" << (conn.needs_reauth ? "true" : "false") << ","
            << "\"reauth_detected_at\":" << static_cast<long long>(conn.reauth_detected_at)
            << "}";
        return out.str();
    }

    std::string buildPortfolioDetailJson(PortfolioManager& manager, const std::string& name)
    {
        Portfolio portfolio;
        if (!loadPortfolioCached(manager, name, portfolio))
        {
            return "";
        }

        const std::string portfolio_currency = upperCopy(trim(portfolio.getCurrency()));
        const std::string ccy_out = portfolio_currency.empty() ? std::string("USD") : portfolio_currency;
        const double fx_rate = (portfolio.getType() == PortfolioType::CASH && ccy_out != "USD")
                                   ? fetchUsdRateForCurrency(ccy_out)
                                   : 1.0;

        std::ostringstream out;
        out << "{"
            << "\"name\":" << jsonString(name) << ","
            << "\"type\":" << jsonString(portfolioTypeToString(portfolio.getType())) << ","
            << "\"currency\":" << jsonString(ccy_out) << ","
            << "\"fx_to_usd\":" << jsonNumber(fx_rate) << ","
            << "\"available_capital\":" << jsonNumber(portfolio.getAvailableCapital()) << ","
            << "\"reported_total_value\":" << jsonNumber(portfolio.getCurrentPortfolioValue()) << ","
            << "\"estimated_total_value\":" << jsonNumber(estimatePortfolioTotalValue(portfolio, manager, name)) << ","
            << "\"day_change_amount\":" << jsonNumber(calculatePortfolioDayChangeAmount(portfolio, manager, name)) << ","
            << "\"day_change_percent\":" << jsonNumber(calculatePortfolioDayChangePercent(portfolio, manager, name)) << ","
            << "\"daily_values\":" << serializeDailyValues(portfolio.getDailyValues()) << ","
            << "\"transaction_count\":" << portfolio.getTransactions().size() << ","
            << "\"connection\":" << buildConnectionJson(manager, name)
            << "}";

        return out.str();
    }

    std::string buildStocksJson(PortfolioManager& manager, const std::string& portfolio_name)
    {
        Portfolio portfolio;
        const bool has_portfolio = loadPortfolioCached(manager, portfolio_name, portfolio);
        std::ostringstream out;
        const std::vector<std::string> tickers = listStocksCached(manager, portfolio_name);

        out << "{\"portfolio\":" << jsonString(portfolio_name) << ",\"stocks\":[";

        bool first_stock = true;
        for (const auto& ticker : tickers)
        {
            StockData stock;
            if (!loadStockDataCached(manager, portfolio_name, ticker, stock))
            {
                continue;
            }

            if (!first_stock)
            {
                out << ",";
            }
            first_stock = false;

            double latest_price = 0.0;
            time_t latest_price_date = 0;
            double previous_price = 0.0;
            time_t previous_price_date = 0;
            if (!stock.getPriceHistory().empty())
            {
                getLatestAndPreviousStockPrices(stock, latest_price, latest_price_date, previous_price, previous_price_date);
            }
            if (latest_price <= 0.0)
            {
                // No market close available (fetch never succeeded) — fall back
                // to cost basis so the position isn't shown as a $0 / -100% loss.
                // latest_close_date stays 0 to signal the price is not a real close.
                latest_price = stock.getAveragePurchasePrice();
            }

            const double day_change_amount = calculateStockDayChangeAmount(stock);
            const double day_change_percent = calculateStockDayChangePercent(stock);
            const double position_day_change_amount = stock.getSharesOwned() * day_change_amount;

            const bool is_watchlist_portfolio = has_portfolio && portfolio.getType() == PortfolioType::WATCHLIST;
            const double display_market_value = is_watchlist_portfolio
                ? latest_price
                : stock.getSharesOwned() * latest_price;

            out << "{"
                << "\"ticker\":" << jsonString(stock.getTicker()) << ","
                << "\"company_name\":" << jsonString(stock.getCompanyName()) << ","
                << "\"shares_owned\":" << jsonNumber(stock.getSharesOwned()) << ","
                << "\"average_purchase_price\":" << jsonNumber(stock.getAveragePurchasePrice()) << ","
                << "\"last_updated\":" << static_cast<long long>(stock.getLastUpdated()) << ","
                << "\"latest_close_price\":" << jsonNumber(latest_price) << ","
                << "\"latest_close_date\":" << static_cast<long long>(latest_price_date) << ","
                << "\"previous_close_price\":" << jsonNumber(previous_price) << ","
                << "\"day_change_amount\":" << jsonNumber(day_change_amount) << ","
                << "\"day_change_percent\":" << jsonNumber(day_change_percent) << ","
                << "\"position_day_change_amount\":" << jsonNumber(position_day_change_amount) << ","
                << "\"position_market_value\":" << jsonNumber(display_market_value) << ","
                << "\"target_price\":" << jsonNumber(stock.getTargetPrice()) << ","
                << "\"watchlist_notes\":" << jsonString(stock.getWatchlistNotes()) << ","
                << "\"event_count\":" << stock.getEvents().size() << ","
                << "\"recent_events\":";

            std::vector<StockEvent> all_events = stock.getEvents();
            std::sort(
                all_events.begin(),
                all_events.end(),
                [](const StockEvent& lhs, const StockEvent& rhs)
                {
                    return lhs.date < rhs.date;
                }
            );

            std::map<size_t, double> event_profit_by_index;
            double running_shares = 0.0;
            double running_cost = 0.0;
            for (size_t i = 0; i < all_events.size(); ++i)
            {
                const StockEvent& event = all_events[i];
                if (event.type == StockEventType::BUY)
                {
                    running_shares += event.shares;
                    running_cost += event.shares * event.price_per_share;
                }
                else if (event.type == StockEventType::SELL)
                {
                    const double avg_cost = running_shares > 0.0 ? running_cost / running_shares : 0.0;
                    const double cost_basis = avg_cost * event.shares;
                    const double proceeds = event.shares * event.price_per_share;
                    event_profit_by_index[i] = proceeds - cost_basis;
                    running_shares -= event.shares;
                    if (running_shares <= 1e-9)
                    {
                        running_shares = 0.0;
                        running_cost = 0.0;
                    }
                    else
                    {
                        running_cost -= cost_basis;
                        if (running_cost < 0.0)
                        {
                            running_cost = 0.0;
                        }
                    }
                }
            }

            std::vector<size_t> descending_event_order;
            descending_event_order.reserve(all_events.size());
            for (size_t i = 0; i < all_events.size(); ++i)
            {
                descending_event_order.push_back(i);
            }
            std::sort(
                descending_event_order.begin(),
                descending_event_order.end(),
                [&all_events](size_t a, size_t b)
                {
                    return all_events[a].date > all_events[b].date;
                }
            );
            if (descending_event_order.size() > 5)
            {
                descending_event_order.resize(5);
            }

            out << "[";
            for (size_t k = 0; k < descending_event_order.size(); ++k)
            {
                if (k > 0)
                {
                    out << ",";
                }

                const size_t idx = descending_event_order[k];
                const StockEvent& event = all_events[idx];
                out << "{"
                    << "\"date\":" << static_cast<long long>(event.date) << ","
                    << "\"type\":" << jsonString(stockEventTypeToString(event.type)) << ","
                    << "\"shares\":" << jsonNumber(event.shares) << ","
                    << "\"price_per_share\":" << jsonNumber(event.price_per_share) << ","
                    << "\"cash_amount\":" << jsonNumber(event.cash_amount) << ","
                    << "\"notes\":" << jsonString(event.notes);
                auto profit_it = event_profit_by_index.find(idx);
                if (profit_it != event_profit_by_index.end())
                {
                    out << ",\"realized_profit\":" << jsonNumber(profit_it->second);
                }
                out << "}";
            }
            out << "]";
            out << "}";
        }

        out << "]}";
        return out.str();
    }

    std::optional<HttpRequest> parseHttpRequest(const std::string& raw_request)
    {
        const std::string delimiter = "\r\n\r\n";
        const size_t header_end = raw_request.find(delimiter);
        if (header_end == std::string::npos)
        {
            return std::nullopt;
        }

        const std::string head = raw_request.substr(0, header_end);
        const std::string body = raw_request.substr(header_end + delimiter.size());

        std::istringstream head_stream(head);
        std::string request_line;
        if (!std::getline(head_stream, request_line))
        {
            return std::nullopt;
        }

        if (!request_line.empty() && request_line.back() == '\r')
        {
            request_line.pop_back();
        }

        std::istringstream request_line_stream(request_line);
        HttpRequest request;
        std::string http_version;
        if (!(request_line_stream >> request.method >> request.target >> http_version))
        {
            return std::nullopt;
        }

        size_t query_pos = request.target.find('?');
        if (query_pos == std::string::npos)
        {
            request.path = request.target;
            request.query.clear();
        }
        else
        {
            request.path = request.target.substr(0, query_pos);
            request.query = request.target.substr(query_pos + 1);
        }

        std::string header_line;
        while (std::getline(head_stream, header_line))
        {
            if (!header_line.empty() && header_line.back() == '\r')
            {
                header_line.pop_back();
            }

            const size_t colon = header_line.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }

            const std::string key = lowerCopy(trim(header_line.substr(0, colon)));
            const std::string value = trim(header_line.substr(colon + 1));
            request.headers[key] = value;
        }

        request.body = body;
        return request;
    }

    std::string reasonPhraseForStatus(int status)
    {
        switch (status)
        {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 415: return "Unsupported Media Type";
            case 500: return "Internal Server Error";
            default: return "Unknown";
        }
    }

    std::string makeHttpResponseText(const HttpResponse& response)
    {
        std::ostringstream out;
        out << "HTTP/1.1 " << response.status << " " << reasonPhraseForStatus(response.status) << "\r\n";
        out << "Content-Type: " << response.content_type << "\r\n";
        out << "Access-Control-Allow-Origin: *\r\n";
        out << "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
        out << "Access-Control-Allow-Headers: Content-Type\r\n";
        for (const auto& header : response.headers)
        {
            out << header.first << ": " << header.second << "\r\n";
        }
        out << "Content-Length: " << response.body.size() << "\r\n";
        out << "Connection: close\r\n\r\n";
        out << response.body;
        return out.str();
    }

    // gzip-compress for HTTP Content-Encoding. windowBits 15+16 emits a gzip
    // wrapper (rather than raw zlib) so browsers decode it transparently.
    bool gzipCompress(const std::string& input, std::string& output)
    {
        z_stream zs{};
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return false;
        }

        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());

        output.clear();
        char buffer[32768];
        int ret = Z_OK;
        do
        {
            zs.next_out = reinterpret_cast<Bytef*>(buffer);
            zs.avail_out = sizeof(buffer);
            ret = deflate(&zs, Z_FINISH);
            if (ret == Z_STREAM_ERROR)
            {
                deflateEnd(&zs);
                return false;
            }
            output.append(buffer, sizeof(buffer) - zs.avail_out);
        } while (ret != Z_STREAM_END);

        deflateEnd(&zs);
        return true;
    }

    // FNV-1a 64-bit. Deterministic across process restarts so a client's stored
    // ETag still matches after the server is rebuilt/restarted (unlike std::hash).
    std::string weakEtagForBody(const std::string& body)
    {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : body)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        std::ostringstream out;
        out << "\"" << std::hex << hash << "-" << body.size() << "\"";
        return out.str();
    }

    // Adds ETag/Cache-Control and gzip to cacheable GET responses. ETag is
    // computed over the uncompressed body so it is stable regardless of
    // encoding; a matching If-None-Match short-circuits to 304 (no body).
    void applyContentNegotiation(HttpResponse& response, const HttpRequest& request)
    {
        if (request.method != "GET" || response.status != 200 || response.body.empty())
        {
            return;
        }

        const std::string etag = weakEtagForBody(response.body);
        response.headers["ETag"] = etag;
        response.headers["Cache-Control"] = "no-cache";

        auto if_none_match = request.headers.find("if-none-match");
        if (if_none_match != request.headers.end() && if_none_match->second == etag)
        {
            response.status = 304;
            response.body.clear();
            return;
        }

        auto accept_encoding = request.headers.find("accept-encoding");
        if (response.body.size() >= 1024 &&
            accept_encoding != request.headers.end() &&
            lowerCopy(accept_encoding->second).find("gzip") != std::string::npos)
        {
            std::string compressed;
            if (gzipCompress(response.body, compressed) && compressed.size() < response.body.size())
            {
                response.body = std::move(compressed);
                response.headers["Content-Encoding"] = "gzip";
                response.headers["Vary"] = "Accept-Encoding";
            }
        }
    }

    bool sendAll(int client_fd, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            ssize_t bytes = send(client_fd, data.data() + sent, data.size() - sent, 0);
            if (bytes <= 0)
            {
                return false;
            }
            sent += static_cast<size_t>(bytes);
        }

        return true;
    }

    std::optional<std::string> readHttpMessage(int client_fd)
    {
        std::string data;
        char buffer[4096];
        size_t content_length = 0;
        bool parsed_headers = false;

        while (true)
        {
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes < 0)
            {
                return std::nullopt;
            }
            if (bytes == 0)
            {
                break;
            }

            data.append(buffer, static_cast<size_t>(bytes));

            if (!parsed_headers)
            {
                const size_t header_end = data.find("\r\n\r\n");
                if (header_end != std::string::npos)
                {
                    parsed_headers = true;
                    const std::string headers = data.substr(0, header_end);
                    const std::string needle = "content-length:";
                    const std::string lower_headers = lowerCopy(headers);
                    const size_t pos = lower_headers.find(needle);
                    if (pos != std::string::npos)
                    {
                        const size_t line_end = lower_headers.find("\r\n", pos);
                        const std::string len_line = headers.substr(pos + needle.size(), line_end - (pos + needle.size()));
                        auto maybe_length = parsePositiveInt(trim(len_line));
                        if (maybe_length.has_value())
                        {
                            content_length = static_cast<size_t>(maybe_length.value());
                        }
                    }
                }
            }

            if (parsed_headers)
            {
                // Reject oversized requests early once we know Content-Length.
                // 26 MB gives headroom above the 25 MB receipt-upload body cap.
                if (content_length > 26ULL * 1024 * 1024)
                {
                    return std::nullopt;
                }
                const size_t header_end = data.find("\r\n\r\n");
                if (header_end == std::string::npos)
                {
                    continue;
                }
                const size_t body_start = header_end + 4;
                if (data.size() >= body_start + content_length)
                {
                    break;
                }
            }

            // Safety cap: receipts up to 25 MB plus headers — raise to 26 MB.
            if (data.size() > 26ULL * 1024 * 1024)
            {
                return std::nullopt;
            }
        }

        return data;
    }

    HttpResponse makeJsonResponse(int status, const std::string& json)
    {
        HttpResponse response;
        response.status = status;
        response.content_type = "application/json";
        response.body = json;
        return response;
    }

    HttpResponse parseJsonBodyObject(const HttpRequest& request, JsonValue& parsed_body)
    {
        auto content_type_it = request.headers.find("content-type");
        if (content_type_it == request.headers.end() ||
            lowerCopy(content_type_it->second).find("application/json") == std::string::npos)
        {
            return makeJsonResponse(415, makeErrorBody("Content-Type must be application/json"));
        }

        JsonParser parser(request.body);
        auto maybe_json = parser.parseRoot();
        if (!maybe_json.has_value() || maybe_json->type != JsonType::OBJECT)
        {
            return makeJsonResponse(400, makeErrorBody("Invalid JSON body"));
        }

        parsed_body = std::move(maybe_json.value());
        return HttpResponse{};
    }

    HttpResponse appendCashTransaction(PortfolioManager& manager,
                                       const std::string& portfolio_name,
                                       TransactionType type,
                                       double cash_delta,
                                       time_t date,
                                       const std::string& notes)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
        }

        const double next_capital = portfolio.getAvailableCapital() + cash_delta;
        if (next_capital < -1e-9)
        {
            return makeJsonResponse(409, makeErrorBody("Insufficient available capital"));
        }

        portfolio.addTransaction(date, cash_delta, type, notes);
        portfolio.setAvailableCapital(next_capital);

        if (!manager.savePortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to save portfolio"));
        }

        if (!MarketDataSync::recomputePortfolioDailyValues(manager, portfolio_name))
        {
            return makeJsonResponse(500, makeErrorBody("Transaction saved but daily portfolio values failed to recompute"));
        }

        std::ostringstream out;
        out << "{"
            << "\"status\":\"ok\"," 
            << "\"portfolio\":" << jsonString(portfolio_name) << ","
            << "\"available_capital\":" << jsonNumber(next_capital)
            << "}";

        return makeJsonResponse(201, out.str());
    }

    HttpResponse appendStockTransaction(PortfolioManager& manager,
                                        const std::string& portfolio_name,
                                        TransactionType type,
                                        const std::string& ticker,
                                        double shares,
                                        double price_per_share,
                                        double amount,
                                        time_t date,
                                        const std::string& notes)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
        }

        const std::string normalized_ticker = upperCopy(ticker);
        if (normalized_ticker.empty())
        {
            return makeJsonResponse(400, makeErrorBody("Ticker is required"));
        }

        if (!isValidTickerSymbol(normalized_ticker))
        {
            return makeJsonResponse(400, makeErrorBody("Ticker contains invalid characters or length"));
        }

        if (!isFinitePositive(shares))
        {
            if (type != TransactionType::DIVIDEND)
            {
                return makeJsonResponse(400, makeErrorBody("Shares must be > 0"));
            }
        }

        if (type == TransactionType::SELL_STOCK)
        {
            const double shares_owned = calculateSharesOwnedFromTransactions(portfolio, normalized_ticker);
            if (shares > shares_owned + 1e-9)
            {
                return makeJsonResponse(409, makeErrorBody("Not enough shares to sell"));
            }
        }

        if (type == TransactionType::BUY_STOCK)
        {
            bool is_invalid_ticker = false;
            const bool checked = maybeTickerExistsOnYahoo(normalized_ticker, is_invalid_ticker);
            if (checked && is_invalid_ticker)
            {
                return makeJsonResponse(400, makeErrorBody("Ticker was not found by market data provider"));
            }
        }

        const double next_capital = portfolio.getAvailableCapital() + amount;
        if (!std::isfinite(next_capital))
        {
            return makeJsonResponse(400, makeErrorBody("Invalid transaction amount"));
        }

        if (next_capital < -1e-9)
        {
            return makeJsonResponse(409, makeErrorBody("Insufficient available capital"));
        }

        portfolio.addTransaction(date, amount, type, normalized_ticker, shares, notes);
        portfolio.setAvailableCapital(next_capital);

        if (!manager.savePortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to save portfolio"));
        }

        const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
        if (!MarketDataSync::syncPortfolio(manager, portfolio_name, sync_config))
        {
            return makeJsonResponse(500, makeErrorBody("Transaction saved but market-data sync/daily value recompute failed"));
        }

        // Best-effort immediate quote persistence so newly added symbols show a price without
        // waiting for end-of-day sync.
        {
            std::string live_quote_error;
            if (!persistLiveQuoteForTicker(manager, portfolio_name, normalized_ticker, live_quote_error))
            {
                std::cerr << "Immediate live quote persist skipped for " << normalized_ticker
                          << ": " << live_quote_error << std::endl;
            }
        }

        std::ostringstream out;
        out << "{"
            << "\"status\":\"ok\","
            << "\"portfolio\":" << jsonString(portfolio_name) << ","
            << "\"ticker\":" << jsonString(normalized_ticker) << ","
            << "\"shares\":" << jsonNumber(shares) << ","
            << "\"price_per_share\":" << jsonNumber(price_per_share) << ","
            << "\"amount\":" << jsonNumber(amount) << ","
            << "\"available_capital\":" << jsonNumber(next_capital)
            << "}";

        return makeJsonResponse(201, out.str());
    }

    HttpResponse appendAssetTransferTransaction(PortfolioManager& manager,
                                                const std::string& portfolio_name,
                                                TransactionType type,
                                                const std::string& ticker,
                                                double shares,
                                                double cost_basis_per_share,
                                                time_t date,
                                                const std::string& notes)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
        }

        const std::string normalized_ticker = upperCopy(ticker);
        if (normalized_ticker.empty() || !isValidTickerSymbol(normalized_ticker))
        {
            return makeJsonResponse(400, makeErrorBody("Ticker is required and must be valid"));
        }

        if (!isFinitePositive(shares))
        {
            return makeJsonResponse(400, makeErrorBody("Shares must be > 0"));
        }

        if (!isFiniteNonNegative(cost_basis_per_share))
        {
            return makeJsonResponse(400, makeErrorBody("Cost basis per share must be >= 0"));
        }

        if (type == TransactionType::TRANSFER_OUT_ASSET)
        {
            const double shares_owned = calculateSharesOwnedFromTransactions(portfolio, normalized_ticker);
            if (shares > shares_owned + 1e-9)
            {
                return makeJsonResponse(409, makeErrorBody("Not enough shares to transfer out"));
            }
        }

        if (type == TransactionType::TRANSFER_IN_ASSET)
        {
            bool is_invalid_ticker = false;
            const bool checked = maybeTickerExistsOnYahoo(normalized_ticker, is_invalid_ticker);
            if (checked && is_invalid_ticker)
            {
                return makeJsonResponse(400, makeErrorBody("Ticker was not found by market data provider"));
            }
        }

        // amount carries cost basis for transfer_in (so the rebuild can derive
        // price_per_share). transfer_out keeps amount at 0. Available capital
        // is intentionally untouched: transfers do not move cash.
        const double amount = (type == TransactionType::TRANSFER_IN_ASSET)
            ? shares * cost_basis_per_share
            : 0.0;

        portfolio.addTransaction(date, amount, type, normalized_ticker, shares, notes);

        if (!manager.savePortfolio(portfolio_name, portfolio))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to save portfolio"));
        }

        const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
        if (!MarketDataSync::syncPortfolio(manager, portfolio_name, sync_config))
        {
            return makeJsonResponse(500, makeErrorBody("Transaction saved but market-data sync/daily value recompute failed"));
        }

        {
            std::string live_quote_error;
            if (!persistLiveQuoteForTicker(manager, portfolio_name, normalized_ticker, live_quote_error))
            {
                std::cerr << "Immediate live quote persist skipped for " << normalized_ticker
                          << ": " << live_quote_error << std::endl;
            }
        }

        std::ostringstream out;
        out << "{"
            << "\"status\":\"ok\","
            << "\"portfolio\":" << jsonString(portfolio_name) << ","
            << "\"ticker\":" << jsonString(normalized_ticker) << ","
            << "\"shares\":" << jsonNumber(shares) << ","
            << "\"cost_basis_per_share\":" << jsonNumber(cost_basis_per_share) << ","
            << "\"available_capital\":" << jsonNumber(portfolio.getAvailableCapital())
            << "}";

        return makeJsonResponse(201, out.str());
    }

    std::string formatPlaidDateUTC(time_t t)
    {
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::string(buf);
    }

    // ---- In-transit transfer matching ------------------------------------
    //
    // Transfers between the user's own connected accounts produce a transient
    // gap: Plaid reflects the outflow at the source instantly but the matching
    // inflow at the destination can lag by hours or days. We tag transfer-typed
    // Plaid transactions with "[TXFR] " in notes (see syncCashAccount), then at
    // read time pair them up across accounts. Unmatched recent outflows are the
    // money still in flight — added back to the dashboard's Total Assets so the
    // user doesn't see a temporary dip.
    constexpr int TXFR_LOOKBACK_DAYS = 14;       // window we scan for pairs
    constexpr int TXFR_TRANSIT_WINDOW_DAYS = 5;  // outflows older than this and
                                                  // still unmatched are treated
                                                  // as money truly gone (paid out),
                                                  // not in transit.
    constexpr double TXFR_AMOUNT_TOLERANCE = 1.0; // $1 tolerance on match

    struct InTransitEntry
    {
        std::string portfolio_name;
        time_t      date;
        double      amount;   // absolute value of the unmatched outflow
        std::string notes;    // display name, [TXFR] prefix stripped
    };

    struct InTransitSummary
    {
        double total;
        std::vector<InTransitEntry> entries;

        InTransitSummary() : total(0.0) {}
    };

    // Pending Plaid txs are prefixed "[PENDING] " (see syncCashAccount). Strip
    // it before further note-prefix checks so a pending transfer still matches
    // "[TXFR] ".
    bool notesIsPending(const std::string& notes)
    {
        return notes.compare(0, 10, "[PENDING] ") == 0;
    }

    std::string notesAfterPending(const std::string& notes)
    {
        return notesIsPending(notes) ? notes.substr(10) : notes;
    }

    bool notesStartsWithTxfr(const std::string& notes)
    {
        const std::string body = notesAfterPending(notes);
        return body.compare(0, 7, "[TXFR] ") == 0;
    }

    InTransitSummary computeInTransit(PortfolioManager& manager)
    {
        const time_t now = std::time(nullptr);
        const time_t lookback_floor = now - (TXFR_LOOKBACK_DAYS * 86400);
        const time_t transit_floor = now - (TXFR_TRANSIT_WINDOW_DAYS * 86400);

        manager.scanPortfolios();

        struct TxfrEvent
        {
            std::string portfolio_name;
            time_t date;
            double amount;       // our sign: positive = inflow, negative = outflow
            std::string notes;
            bool matched;
        };

        std::vector<TxfrEvent> outflows;
        std::vector<TxfrEvent> inflows;

        for (const std::string& name : manager.getPortfolioNames())
        {
            if (!manager.hasConnection(name)) continue;
            Portfolio p;
            if (!loadPortfolioCached(manager, name, p)) continue;
            for (const auto& t : p.getTransactions())
            {
                if (t.date < lookback_floor) continue;
                if (!notesStartsWithTxfr(t.notes)) continue;
                if (t.amount < 0.0)
                {
                    outflows.push_back({name, t.date, t.amount, t.notes, false});
                }
                else if (t.amount > 0.0)
                {
                    inflows.push_back({name, t.date, t.amount, t.notes, false});
                }
            }
        }

        InTransitSummary result;
        for (auto& out : outflows)
        {
            const double out_abs = -out.amount;
            bool matched = false;
            for (auto& in : inflows)
            {
                if (in.matched) continue;
                if (in.portfolio_name == out.portfolio_name) continue;
                if (std::fabs(in.amount - out_abs) > TXFR_AMOUNT_TOLERANCE) continue;
                if (in.date < out.date) continue;
                if (in.date > out.date + (TXFR_TRANSIT_WINDOW_DAYS * 86400)) continue;
                in.matched = true;
                matched = true;
                break;
            }
            if (matched) continue;
            if (out.date < transit_floor) continue;  // too old, treat as out, not transit

            InTransitEntry entry;
            entry.portfolio_name = out.portfolio_name;
            entry.date = out.date;
            entry.amount = out_abs;
            // Strip optional "[PENDING] " then the required "[TXFR] " before display.
            {
                const std::string after_pending = notesAfterPending(out.notes);
                entry.notes = after_pending.size() > 7 ? after_pending.substr(7) : std::string();
            }
            result.entries.push_back(entry);
            result.total += out_abs;
        }
        return result;
    }

    std::string buildInTransitJson(PortfolioManager& manager)
    {
        const InTransitSummary summary = computeInTransit(manager);
        std::ostringstream out;
        out << "{"
            << "\"total\":" << jsonNumber(summary.total) << ","
            << "\"entries\":[";
        bool first = true;
        for (const auto& e : summary.entries)
        {
            if (!first) out << ",";
            first = false;
            out << "{"
                << "\"portfolio\":" << jsonString(e.portfolio_name) << ","
                << "\"date\":" << static_cast<long long>(e.date) << ","
                << "\"amount\":" << jsonNumber(e.amount) << ","
                << "\"notes\":" << jsonString(e.notes)
                << "}";
        }
        out << "]}";
        return out.str();
    }

    // Parse "YYYY-MM-DD" into a UTC unix timestamp at noon. Returns 0 on failure.
    time_t parseIsoDateUTC(const std::string& s)
    {
        if (s.size() < 10) return 0;
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = 12;
        return timegm(&tm);
    }

    struct SpendOverride
    {
        std::string match_lower; // case-insensitive substring on tx notes
        std::string category;    // PFC string to substitute (detailed or primary_*)
        std::string display;     // optional display name override (empty = no rename)
    };

    // Load merchant-name → category overrides from data/spend_overrides.json.
    // Plaid's auto-classification is wrong for some recurring merchants — this
    // lets the user pin them to the right primary without editing tx history.
    // Returns an empty vector silently if the file is missing or malformed.
    std::vector<SpendOverride> loadSpendOverrides()
    {
        std::vector<SpendOverride> rules;
        std::ifstream f("data/spend_overrides.json");
        if (!f.is_open()) return rules;
        std::stringstream buffer;
        buffer << f.rdbuf();
        const std::string content = buffer.str();
        JsonParser parser(content);
        auto root = parser.parseRoot();
        if (!root.has_value() || root->type != JsonType::OBJECT) return rules;
        auto it = root->object_value.find("rules");
        if (it == root->object_value.end() || it->second.type != JsonType::ARRAY) return rules;
        for (const auto& el : it->second.array_value)
        {
            if (el.type != JsonType::OBJECT) continue;
            auto m_it = el.object_value.find("match");
            if (m_it == el.object_value.end() || m_it->second.type != JsonType::STRING) continue;
            SpendOverride rule;
            rule.match_lower = lowerCopy(m_it->second.string_value);
            if (rule.match_lower.empty()) continue;
            auto c_it = el.object_value.find("category");
            if (c_it != el.object_value.end() && c_it->second.type == JsonType::STRING)
            {
                rule.category = c_it->second.string_value;
            }
            auto d_it = el.object_value.find("display");
            if (d_it != el.object_value.end() && d_it->second.type == JsonType::STRING)
            {
                rule.display = d_it->second.string_value;
            }
            if (rule.category.empty() && rule.display.empty()) continue;
            rules.push_back(rule);
        }
        return rules;
    }

    // Withdrawals on CASH/DEBT accounts, normalized to positive "spend" amounts.
    // Excludes inter-account transfers (notes prefixed "[TXFR]" by syncCashAccount)
    // and Plaid TRANSFER_*/LOAN_PAYMENTS categories — paying down a credit card is
    // not spend, it just shifts the liability.
    struct SpendTxn
    {
        std::string key;
        time_t date;
        double amount;            // positive spend amount
        std::string category;     // effective category (after spend_overrides)
        std::string notes;
        std::string account;
        std::string account_type; // "CASH" | "DEBT"
    };

    std::vector<SpendTxn> collectSpendTransactions(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendOverride> overrides = loadSpendOverrides();
        std::vector<SpendTxn> result;
        ExpenseTags::KeyAssigner key_assigner;

        if (!manager.scanPortfolios())
        {
            return result;
        }

        const std::vector<std::string> names = manager.getPortfolioNames();
        for (const std::string& name : names)
        {
            Portfolio portfolio;
            if (!loadPortfolioCached(manager, name, portfolio)) continue;
            const PortfolioType pt = portfolio.getType();
            if (pt != PortfolioType::CASH && pt != PortfolioType::DEBT) continue;

            for (const Transaction& tx : portfolio.getTransactions())
            {
                if (tx.type != TransactionType::WITHDRAWAL) continue;
                if (notesIsPending(tx.notes)) continue;
                if (notesStartsWithTxfr(tx.notes)) continue;

                const std::string upper_cat = upperCopy(tx.category);
                if (upper_cat.rfind("TRANSFER_IN", 0) == 0) continue;
                if (upper_cat.rfind("TRANSFER_OUT", 0) == 0) continue;
                if (upper_cat.rfind("LOAN_PAYMENTS", 0) == 0) continue;

                // Key BEFORE the range filter: occurrence indices must be
                // stable across different requested ranges.
                const std::string key = key_assigner.next(name, tx.date, tx.amount, tx.notes);

                if (tx.date < from || tx.date > to) continue;

                std::string effective_category = tx.category;
                std::string effective_notes = tx.notes;
                if (!overrides.empty())
                {
                    const std::string notes_lower = lowerCopy(tx.notes);
                    for (const auto& rule : overrides)
                    {
                        if (notes_lower.find(rule.match_lower) != std::string::npos)
                        {
                            if (!rule.category.empty())
                            {
                                effective_category = rule.category;
                            }
                            if (!rule.display.empty())
                            {
                                effective_notes = rule.display;
                            }
                            break;
                        }
                    }
                }

                SpendTxn spend_tx;
                spend_tx.key = key;
                spend_tx.date = tx.date;
                spend_tx.amount = std::abs(tx.amount);
                spend_tx.category = effective_category;
                spend_tx.notes = effective_notes;
                spend_tx.account = name;
                spend_tx.account_type = portfolioTypeToString(pt);
                result.push_back(spend_tx);
            }
        }
        return result;
    }

    // Parses optional from/to (YYYY-MM-DD) query params with the /api/spend
    // defaults: from = 365 days ago, to = now; "to" bumped to end-of-day.
    // Returns a 400 response on malformed values, else nullopt.
    std::optional<HttpResponse> parseRangeParams(const std::map<std::string, std::string>& query_values,
                                                 time_t& from, time_t& to)
    {
        const time_t now = std::time(nullptr);
        from = now - (365LL * 86400);
        to = now;
        auto from_it = query_values.find("from");
        if (from_it != query_values.end() && !from_it->second.empty())
        {
            from = parseIsoDateUTC(from_it->second);
            if (from == 0)
            {
                return makeJsonResponse(400, makeErrorBody("from must be YYYY-MM-DD"));
            }
        }
        auto to_it = query_values.find("to");
        if (to_it != query_values.end() && !to_it->second.empty())
        {
            to = parseIsoDateUTC(to_it->second);
            if (to == 0)
            {
                return makeJsonResponse(400, makeErrorBody("to must be YYYY-MM-DD"));
            }
            to += 86399; // end-of-day inclusive
        }
        return std::nullopt;
    }

    bool parseYearParam(const std::map<std::string, std::string>& query_values,
                        time_t& from, time_t& to)
    {
        const auto it = query_values.find("year");
        if (it == query_values.end() || it->second.empty()) return false;
        const auto maybe_year = parsePositiveInt(trim(it->second));
        if (!maybe_year.has_value()) return false;
        const int year = maybe_year.value();
        if (year < 2000 || year > 2100) return false;
        std::tm start{};
        start.tm_year = year - 1900;
        start.tm_mon = 0;
        start.tm_mday = 1;
        from = timegm(&start);
        std::tm end{};
        end.tm_year = year - 1900;
        end.tm_mon = 11;
        end.tm_mday = 31;
        end.tm_hour = 23;
        end.tm_min = 59;
        end.tm_sec = 59;
        to = timegm(&end);
        return true;
    }

    std::string buildSpendJson(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendTxn> txs = collectSpendTransactions(manager, from, to);
        std::ostringstream out;
        out << "{"
            << "\"from\":" << static_cast<long long>(from) << ","
            << "\"to\":" << static_cast<long long>(to) << ","
            << "\"transactions\":[";
        for (size_t i = 0; i < txs.size(); ++i)
        {
            if (i > 0) out << ",";
            out << "{"
                << "\"key\":" << jsonString(txs[i].key) << ","
                << "\"date\":" << static_cast<long long>(txs[i].date) << ","
                << "\"amount\":" << jsonNumber(txs[i].amount) << ","
                << "\"category\":" << jsonString(txs[i].category) << ","
                << "\"notes\":" << jsonString(txs[i].notes) << ","
                << "\"account\":" << jsonString(txs[i].account) << ","
                << "\"account_type\":" << jsonString(txs[i].account_type)
                << "}";
        }
        out << "]}";
        return out.str();
    }

    // Income-side mirror of collectSpendTransactions: DEPOSIT and INTEREST
    // transactions on CASH portfolios, keyed over the FULL history before the
    // date filter so occurrence indices are range-stable. Income keys are a
    // separate namespace from spend keys (separate endpoint + tag files), so
    // a same-tuple deposit/withdrawal collision is harmless.
    std::vector<SpendTxn> collectIncomeTransactions(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendOverride> overrides = loadSpendOverrides();
        std::vector<SpendTxn> result;
        ExpenseTags::KeyAssigner key_assigner;

        if (!manager.scanPortfolios())
        {
            return result;
        }

        const std::vector<std::string> names = manager.getPortfolioNames();
        for (const std::string& name : names)
        {
            Portfolio portfolio;
            if (!loadPortfolioCached(manager, name, portfolio)) continue;
            const PortfolioType pt = portfolio.getType();
            if (pt != PortfolioType::CASH) continue;

            for (const Transaction& tx : portfolio.getTransactions())
            {
                if (tx.type != TransactionType::DEPOSIT &&
                    tx.type != TransactionType::INTEREST) continue;
                if (notesIsPending(tx.notes)) continue;
                if (notesStartsWithTxfr(tx.notes)) continue;

                const std::string key = key_assigner.next(name, tx.date, tx.amount, tx.notes);

                if (tx.date < from || tx.date > to) continue;

                std::string effective_category = tx.category;
                std::string effective_notes = tx.notes;
                if (!overrides.empty())
                {
                    const std::string notes_lower = lowerCopy(tx.notes);
                    for (const auto& rule : overrides)
                    {
                        if (notes_lower.find(rule.match_lower) != std::string::npos)
                        {
                            if (!rule.category.empty())
                            {
                                effective_category = rule.category;
                            }
                            if (!rule.display.empty())
                            {
                                effective_notes = rule.display;
                            }
                            break;
                        }
                    }
                }

                SpendTxn income_tx;
                income_tx.key = key;
                income_tx.date = tx.date;
                income_tx.amount = std::abs(tx.amount);
                income_tx.category = effective_category;
                income_tx.notes = effective_notes;
                income_tx.account = name;
                income_tx.account_type = portfolioTypeToString(pt);
                result.push_back(income_tx);
            }
        }
        return result;
    }

    std::string buildIncomeJson(PortfolioManager& manager, time_t from, time_t to)
    {
        const std::vector<SpendTxn> txs = collectIncomeTransactions(manager, from, to);
        std::ostringstream out;
        out << "{"
            << "\"from\":" << static_cast<long long>(from) << ","
            << "\"to\":" << static_cast<long long>(to) << ","
            << "\"transactions\":[";
        for (size_t i = 0; i < txs.size(); ++i)
        {
            if (i > 0) out << ",";
            out << "{"
                << "\"key\":" << jsonString(txs[i].key) << ","
                << "\"date\":" << static_cast<long long>(txs[i].date) << ","
                << "\"amount\":" << jsonNumber(txs[i].amount) << ","
                << "\"category\":" << jsonString(txs[i].category) << ","
                << "\"notes\":" << jsonString(txs[i].notes) << ","
                << "\"account\":" << jsonString(txs[i].account) << ","
                << "\"account_type\":" << jsonString(txs[i].account_type)
                << "}";
        }
        out << "]}";
        return out.str();
    }

    const char* kTagsFile = "data/529/tags.json";
    const char* kReceiptsDir = "data/529/receipts";
    const char* k529WithdrawalsFile = "data/529/withdrawals.json";

    // Request handling is one-thread-per-connection; serialize every
    // load-modify-save of tags.json and every receipt-dir mutation.
    std::mutex g_529_mutex;

    // Serializes concurrent ZIP exports: they share the staging dir
    // data/529/.export_tmp.
    std::mutex g_529_export_mutex;

    std::string serializeTagRecord(const ExpenseTags::TagRecord& tag)
    {
        std::ostringstream out;
        out << "{"
            << "\"key\":" << jsonString(tag.key) << ","
            << "\"status\":" << jsonString(tag.status) << ","
            << "\"qualified_amount\":" << jsonNumber(tag.qualified_amount) << ","
            << "\"receipts\":[";
        for (size_t i = 0; i < tag.receipts.size(); ++i)
        {
            if (i > 0) out << ",";
            out << jsonString(tag.receipts[i]);
        }
        out << "],"
            << "\"account\":" << jsonString(tag.account) << ","
            << "\"date\":" << static_cast<long long>(tag.date) << ","
            << "\"amount\":" << jsonNumber(tag.amount) << ","
            << "\"notes\":" << jsonString(tag.notes) << ","
            << "\"category\":" << jsonString(tag.category) << ","
            << "\"created\":" << static_cast<long long>(tag.created)
            << "}";
        return out.str();
    }

    bool ensure529Dirs()
    {
        std::error_code ec;
        std::filesystem::create_directories("data/529", ec);
        std::filesystem::create_directories(kReceiptsDir, ec);
        return !ec;
    }

    const char* kTaxIncomeTagsFile = "data/tax/income.json";
    const char* kTaxDeductionTagsFile = "data/tax/deductions.json";
    const char* kTaxReceiptsDir = "data/tax/receipts";
    std::mutex g_tax_mutex;

    bool ensureTaxDirs()
    {
        std::error_code ec;
        std::filesystem::create_directories("data/tax", ec);
        std::filesystem::create_directories(kTaxReceiptsDir, ec);
        return !ec;
    }

    HttpResponse createManualRecord(const char* tags_file, std::mutex& store_mutex,
                                    bool (*ensure_dirs)(), const std::string& date_str,
                                    double amount, const std::string& notes,
                                    const char* account, const char* category)
    {
        const time_t entry_date = parseIsoDateUTC(date_str);
        if (entry_date == 0)
        {
            return makeJsonResponse(400, makeErrorBody("date must be YYYY-MM-DD"));
        }
        if (!(amount > 0.0))
        {
            return makeJsonResponse(400, makeErrorBody("amount must be > 0"));
        }
        const std::string trimmed_notes = trim(notes);
        if (trimmed_notes.empty())
        {
            return makeJsonResponse(400, makeErrorBody("notes must not be empty"));
        }

        std::lock_guard<std::mutex> lock(store_mutex);
        if (!ensure_dirs())
        {
            return makeJsonResponse(500, makeErrorBody("Failed to create data directory"));
        }
        std::vector<ExpenseTags::TagRecord> tags;
        if (!ExpenseTags::loadTags(tags_file, tags))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to read records"));
        }

        const time_t now = std::time(nullptr);
        std::string manual_key;
        int suffix = 0;
        do
        {
            manual_key = "manual-" + std::to_string(static_cast<long long>(now)) +
                         "-" + std::to_string(suffix++);
        } while (std::any_of(tags.begin(), tags.end(),
                 [&manual_key](const ExpenseTags::TagRecord& t)
                 {
                     return t.key == manual_key;
                 }));

        ExpenseTags::TagRecord record;
        record.key = manual_key;
        record.status = "qualified";
        record.qualified_amount = amount;
        record.account = account;
        record.date = entry_date;
        record.amount = amount;
        record.notes = trimmed_notes;
        record.category = category;
        record.created = now;
        tags.push_back(record);
        if (!ExpenseTags::saveTags(tags_file, tags))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to save records"));
        }
        return makeJsonResponse(201, std::string("{\"tag\":") + serializeTagRecord(record) + "}");
    }

    // Daily tar snapshot of data/ (tags, receipts, tax records, portfolios).
    // Runs in a detached thread: once at startup, then once per wall-clock day.
    // Never throws into the server; failures only log to stderr (pm2 logs).
    void runDataBackup()
    {
        std::error_code ec;
        std::filesystem::create_directories("backups", ec);
        if (ec)
        {
            std::cerr << "Backup: failed to create backups/ directory" << std::endl;
            return;
        }

        const time_t now = std::time(nullptr);
        std::tm tm_utc{};
        gmtime_r(&now, &tm_utc);
        char date_buf[16];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
        const std::string snapshot = std::string("backups/data-") + date_buf + ".tar.gz";
        if (std::filesystem::exists(snapshot))
        {
            return; // today's snapshot already taken (restart-safe)
        }

        const std::string temp_snapshot = snapshot + ".tmp";
        const std::string cmd = "/usr/bin/tar -czf '" + temp_snapshot + "' data 2>/dev/null";
        if (std::system(cmd.c_str()) != 0)
        {
            std::cerr << "Backup: tar failed for " << snapshot << std::endl;
            std::filesystem::remove(temp_snapshot, ec);
            return;
        }
        std::filesystem::rename(temp_snapshot, snapshot, ec);
        if (ec)
        {
            std::cerr << "Backup: failed to finalize " << snapshot << std::endl;
            return;
        }
        std::cout << "Backup: wrote " << snapshot << std::endl;

        // Prune to the newest 14 snapshots (names sort chronologically).
        std::vector<std::string> snapshots;
        for (const auto& entry : std::filesystem::directory_iterator("backups", ec))
        {
            const std::string filename = entry.path().filename().string();
            if (filename.rfind("data-", 0) == 0 && filename.size() > 7 &&
                filename.substr(filename.size() - 7) == ".tar.gz")
            {
                snapshots.push_back(filename);
            }
        }
        std::sort(snapshots.begin(), snapshots.end());
        while (snapshots.size() > 14)
        {
            std::filesystem::remove(std::filesystem::path("backups") / snapshots.front(), ec);
            std::cout << "Backup: pruned " << snapshots.front() << std::endl;
            snapshots.erase(snapshots.begin());
        }
    }

    HttpResponse applyTagUpsert(const std::string& key, const std::string& status,
                                std::optional<double> amount_opt,
                                const char* tags_file, std::mutex& store_mutex,
                                bool (*ensure_dirs)(), PortfolioManager& manager,
                                std::vector<SpendTxn> (*collect)(PortfolioManager&, time_t, time_t))
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        if (!ensure_dirs())
        {
            return makeJsonResponse(500, makeErrorBody("Failed to create data directory"));
        }
        std::vector<ExpenseTags::TagRecord> tags;
        if (!ExpenseTags::loadTags(tags_file, tags))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to read tags"));
        }

        auto existing = std::find_if(tags.begin(), tags.end(),
            [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });

        if (status == "none")
        {
            if (existing == tags.end())
            {
                return makeJsonResponse(404, makeErrorBody("No tag for that key"));
            }
            // Receipt files stay on disk (spec: only explicit DELETE removes them).
            tags.erase(existing);
            if (!ExpenseTags::saveTags(tags_file, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to save tags"));
            }
            return makeJsonResponse(200, "{\"removed\":true}");
        }

        ExpenseTags::TagRecord record;
        if (existing != tags.end())
        {
            record = *existing;
        }
        else
        {
            // New tag: capture denormalized source fields from transaction data.
            const time_t now = std::time(nullptr);
            const std::vector<SpendTxn> all = collect(manager, 0, now + 86400);
            auto txn = std::find_if(all.begin(), all.end(),
                [&key](const SpendTxn& t) { return t.key == key; });
            if (txn == all.end())
            {
                return makeJsonResponse(404, makeErrorBody("Transaction not found for that key"));
            }
            record.key = key;
            record.account = txn->account;
            record.date = txn->date;
            record.amount = txn->amount;
            record.notes = txn->notes;
            record.category = txn->category;
            record.created = now;
        }

        record.status = status;
        if (status == "qualified")
        {
            double qualified_amount = record.amount; // default: full charge
            if (amount_opt.has_value())
            {
                qualified_amount = amount_opt.value();
            }
            if (!(qualified_amount > 0.0) || qualified_amount > record.amount + 0.005)
            {
                return makeJsonResponse(400, makeErrorBody("qualified_amount must be > 0 and <= the charge amount"));
            }
            record.qualified_amount = qualified_amount;
        }
        else
        {
            record.qualified_amount = 0.0;
        }

        if (existing != tags.end())
        {
            *existing = record;
        }
        else
        {
            tags.push_back(record);
        }
        if (!ExpenseTags::saveTags(tags_file, tags))
        {
            return makeJsonResponse(500, makeErrorBody("Failed to save tags"));
        }
        return makeJsonResponse(200, std::string("{\"tag\":") + serializeTagRecord(record) + "}");
    }

    std::string csvField(const std::string& value)
    {
        // Prefix formula-injection characters so Excel/Sheets don't execute
        // merchant-controlled cell content as formulas.
        const bool inject_guard =
            !value.empty() &&
            (value[0] == '=' || value[0] == '+' || value[0] == '-' || value[0] == '@');

        if (!inject_guard && value.find_first_of(",\"\n\r") == std::string::npos)
        {
            return value;
        }
        std::string escaped = "\"";
        if (inject_guard) escaped += '\'';
        for (char c : value)
        {
            if (c == '"') escaped += "\"\"";
            else escaped += c;
        }
        escaped += "\"";
        return escaped;
    }

    std::vector<ExpenseTags::TagRecord> qualifiedTagsInRange(const char* tags_file, time_t from, time_t to, bool& ok)
    {
        std::vector<ExpenseTags::TagRecord> tags;
        ok = ExpenseTags::loadTags(tags_file, tags);
        std::vector<ExpenseTags::TagRecord> result;
        if (!ok) return result;
        for (const auto& tag : tags)
        {
            if (tag.status != "qualified") continue;
            if (tag.date < from || tag.date > to) continue;
            result.push_back(tag);
        }
        std::sort(result.begin(), result.end(),
                  [](const ExpenseTags::TagRecord& a, const ExpenseTags::TagRecord& b)
                  { return a.date < b.date; });
        return result;
    }

    std::string buildTagCsv(const std::vector<ExpenseTags::TagRecord>& tags,
                            const char* header_row, bool include_receipts)
    {
        std::ostringstream out;
        out << header_row;
        char date_buf[16];
        for (const auto& tag : tags)
        {
            std::tm tm_utc{};
            gmtime_r(&tag.date, &tm_utc);
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
            out << date_buf << ","
                << csvField(tag.account) << ","
                << csvField(tag.notes) << ","
                << csvField(tag.category) << ","
                << jsonNumber(tag.amount) << ","
                << jsonNumber(tag.qualified_amount);
            if (include_receipts)
            {
                std::string receipts_joined;
                for (size_t i = 0; i < tag.receipts.size(); ++i)
                {
                    if (i > 0) receipts_joined += "; ";
                    receipts_joined += tag.receipts[i];
                }
                out << "," << csvField(receipts_joined);
            }
            out << "\r\n";
        }
        return out.str();
    }

    std::string build529Csv(const std::vector<ExpenseTags::TagRecord>& tags)
    {
        return buildTagCsv(tags, "date,account,merchant,category,charge_amount,qualified_amount,receipts\r\n", true);
    }

    HttpResponse handleReceiptRoute(const HttpRequest& request, const char* tags_file, const char* receipts_dir, std::mutex& store_mutex)
    {
        const auto query_values = parseQuery(request.query);
        const auto key_it = query_values.find("key");
        const auto filename_it = query_values.find("filename");
        if (key_it == query_values.end() || key_it->second.empty() ||
            filename_it == query_values.end() || filename_it->second.empty())
        {
            return makeJsonResponse(400, makeErrorBody("key and filename query params are required"));
        }
        const std::string key = key_it->second;
        // The key itself lands in a filesystem path — restrict it hard.
        if (key.find_first_not_of("0123456789abcdef-") != std::string::npos)
        {
            return makeJsonResponse(400, makeErrorBody("Invalid key"));
        }
        const std::string safe_name = ExpenseTags::sanitizeFilename(filename_it->second);
        if (safe_name.empty())
        {
            return makeJsonResponse(400, makeErrorBody("Unusable filename"));
        }
        if (!ExpenseTags::isAllowedReceiptExtension(safe_name))
        {
            return makeJsonResponse(400, makeErrorBody("Only jpg, jpeg, png, heic, webp, pdf receipts are allowed"));
        }

        const std::filesystem::path receipt_dir = std::filesystem::path(receipts_dir) / key;

        if (request.method == "POST")
        {
            if (request.body.empty())
            {
                return makeJsonResponse(400, makeErrorBody("Empty upload"));
            }
            if (request.body.size() > 25ULL * 1024 * 1024)
            {
                return makeJsonResponse(413, makeErrorBody("Receipt too large (25 MB max)"));
            }

            std::lock_guard<std::mutex> lock(store_mutex);
            std::vector<ExpenseTags::TagRecord> tags;
            if (!ExpenseTags::loadTags(tags_file, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            auto tag = std::find_if(tags.begin(), tags.end(),
                [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
            if (tag == tags.end() || tag->status != "qualified")
            {
                return makeJsonResponse(404, makeErrorBody("Qualify the charge before attaching receipts"));
            }

            std::error_code ec;
            std::filesystem::create_directories(receipt_dir, ec);
            if (ec)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to create receipt directory"));
            }

            // Dedupe: receipt.jpg -> receipt-2.jpg -> receipt-3.jpg ...
            std::string stored_name = safe_name;
            const size_t dot = safe_name.find_last_of('.');
            const std::string stem = safe_name.substr(0, dot);
            const std::string ext = safe_name.substr(dot); // includes '.'
            int suffix = 2;
            while (std::filesystem::exists(receipt_dir / stored_name))
            {
                stored_name = stem + "-" + std::to_string(suffix++) + ext;
            }

            const std::filesystem::path final_path = receipt_dir / stored_name;
            const std::filesystem::path temp_path = receipt_dir / (stored_name + ".tmp");
            {
                std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to write receipt"));
                }
                file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
                if (!file.good())
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to write receipt"));
                }
            }
            std::filesystem::rename(temp_path, final_path, ec);
            if (ec)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to finalize receipt"));
            }

            tag->receipts.push_back(stored_name);
            if (!ExpenseTags::saveTags(tags_file, tags))
            {
                // Don't orphan the file: no tag record references it and it
                // would shift future dedup suffixes. Best-effort removal.
                std::filesystem::remove(final_path, ec);
                return makeJsonResponse(500, makeErrorBody("Receipt stored but tag update failed"));
            }
            return makeJsonResponse(201, std::string("{\"filename\":") + jsonString(stored_name) + "}");
        }

        if (request.method == "GET")
        {
            std::ifstream file(receipt_dir / safe_name, std::ios::binary);
            if (!file.is_open())
            {
                return makeJsonResponse(404, makeErrorBody("Receipt not found"));
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            HttpResponse response;
            response.status = 200;
            response.content_type = ExpenseTags::receiptMimeType(safe_name);
            response.body = buffer.str();
            return response;
        }

        if (request.method == "DELETE")
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            std::vector<ExpenseTags::TagRecord> tags;
            if (!ExpenseTags::loadTags(tags_file, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            std::error_code ec;
            const bool file_removed = std::filesystem::remove(receipt_dir / safe_name, ec);
            bool record_removed = false;
            auto tag = std::find_if(tags.begin(), tags.end(),
                [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
            if (tag != tags.end())
            {
                const size_t before = tag->receipts.size();
                tag->receipts.erase(
                    std::remove(tag->receipts.begin(), tag->receipts.end(), safe_name),
                    tag->receipts.end());
                record_removed = tag->receipts.size() != before;
                if (record_removed && !ExpenseTags::saveTags(tags_file, tags))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save 529 tags"));
                }
            }
            if (!file_removed && !record_removed)
            {
                return makeJsonResponse(404, makeErrorBody("Receipt not found"));
            }
            return makeJsonResponse(200, "{\"removed\":true}");
        }

        return makeJsonResponse(405, makeErrorBody("Method not allowed"));
    }

    // Collect lowercased + normalized institution names from every connected
    // portfolio so syncCashAccount can decide whether a Plaid TRANSFER is
    // between the user's own connected accounts vs a peer-to-peer payment.
    // Normalization strips a trailing " - <suffix>" (Plaid adds qualifiers
    // like "Venmo - Personal") so a merchant_name of just "Venmo" still
    // matches against the connection's institution_name.
    std::string normalizeInstitutionName(const std::string& raw)
    {
        std::string lower = lowerCopy(raw);
        const size_t dash = lower.find(" - ");
        if (dash != std::string::npos) lower.resize(dash);
        return trim(lower);
    }

    std::set<std::string> connectedInstitutionNamesLower(PortfolioManager& manager)
    {
        std::set<std::string> out;
        manager.scanPortfolios();
        for (const std::string& name : manager.getPortfolioNames())
        {
            if (!manager.hasConnection(name)) continue;
            PortfolioConnection peek;
            if (!manager.loadConnection(name, peek)) continue;
            const std::string normalized = normalizeInstitutionName(peek.institution_name);
            if (!normalized.empty()) out.insert(normalized);
        }
        return out;
    }

    // Decide whether a TRANSFER_IN/OUT Plaid transaction is between the user's
    // own connected accounts (tag [TXFR], eligible for in-transit) vs a peer-
    // to-peer payment like a Zelle/Venmo to a friend (count as a real outflow).
    // Signals, strongest first:
    //   1. Plaid enrichment: counterparty type "financial_institution" → own;
    //      types "user" / "payment_app" → peer (overrides the name match so
    //      "Venmo Payment to John" doesn't false-positive on the Venmo institution).
    //   2. Name match: counterparty/merchant name contains, or is contained in,
    //      a connected institution name (substring works either way to handle
    //      "Venmo" ⇄ "Venmo - Personal" and "Wells" ⇄ "Wells Fargo").
    bool transferIsOwnAccount(const Plaid::Transaction& tx,
                              const std::set<std::string>& connected_institutions_lower)
    {
        // ATM/cash withdrawals are TRANSFER_OUT in Plaid's taxonomy but the
        // money lands in the user's wallet, not another connected account, so
        // there's no inflow to reconcile.
        if (tx.pfc_detailed == "TRANSFER_OUT_WITHDRAWAL") return false;

        // Name match is the strongest positive signal. It wins even when Plaid
        // returns counterparty_type="payment_app" (Venmo is both a payment app
        // AND a connected account here — the money to/from Venmo lands in the
        // user's Venmo balance, which is own-account).
        const std::string haystack = lowerCopy(
            !tx.counterparty_name.empty() ? tx.counterparty_name :
            (!tx.merchant_name.empty() ? tx.merchant_name : tx.name)
        );
        if (!haystack.empty())
        {
            for (const std::string& inst : connected_institutions_lower)
            {
                if (inst.size() < 3) continue;  // skip degenerate names that match too freely
                if (haystack.find(inst) != std::string::npos) return true;
                if (haystack.size() >= 3 && inst.find(haystack) != std::string::npos) return true;
            }
        }

        // No name match. Trust Plaid's enrichment as a secondary signal —
        // counterparty_type=financial_institution implies own-account even if
        // the institution isn't in our connected set yet.
        if (tx.counterparty_type == "financial_institution") return true;
        return false;
    }

    // If a Plaid call surfaced ITEM_LOGIN_REQUIRED / INVALID_ACCESS_TOKEN / etc.,
    // flag the connection so the UI can prompt the user to re-link and the
    // hourly auto-sync stops hammering Plaid with a token that will never work.
    void markConnectionReauthIfNeeded(PortfolioManager& manager,
                                      const std::string& portfolio_name,
                                      PortfolioConnection& conn,
                                      const std::string& error)
    {
        if (!Plaid::errorRequiresReauth(error)) return;
        if (conn.needs_reauth) return;  // already marked — no need to rewrite the file
        conn.needs_reauth = true;
        conn.reauth_detected_at = std::time(nullptr);
        manager.saveConnection(portfolio_name, conn);
        std::cerr << "[plaid] connection " << portfolio_name
                  << " flagged for re-auth: " << error << std::endl;
    }

    // Plaid /transactions/get sign: positive = money OUT. Our sign: positive = cash IN.
    // So for the cash path, our_amount = -plaid.amount.
    HttpResponse syncCashAccount(PortfolioManager& manager,
                                  const std::string& portfolio_name,
                                  const Plaid::Config& plaid_config,
                                  PortfolioConnection& conn,
                                  const std::string& start_date,
                                  const std::string& end_date,
                                  const Portfolio& original_portfolio,
                                  const std::vector<Plaid::AccountSummary>& accounts_summary)
    {
        std::vector<Plaid::Transaction> plaid_txs;
        std::string plaid_error;
        if (!Plaid::getTransactions(plaid_config, conn.access_token,
                                    start_date, end_date, plaid_txs, plaid_error))
        {
            markConnectionReauthIfNeeded(manager, portfolio_name, conn, plaid_error);
            return makeJsonResponse(502, makeErrorBody("Plaid sync failed: " + plaid_error));
        }

        std::vector<Plaid::Transaction> filtered;
        filtered.reserve(plaid_txs.size());
        for (const auto& tx : plaid_txs)
        {
            if (!conn.account_id.empty() && tx.account_id != conn.account_id) continue;
            // Keep pending transactions so the user sees recent activity even
            // before it posts at the bank. They're rebuilt from scratch on every
            // sync (we don't merge), so when a pending row posts at Plaid it
            // simply replaces itself with the posted row — no manual dedup needed.
            filtered.push_back(tx);
        }

        // Anchor available_capital to Plaid's currently reported balance so the
        // recompute can walk backwards to find historical balances. Fall back to
        // the transaction sum (legacy behavior) if no balance is available.
        double anchor_balance = 0.0;
        bool have_anchor = false;
        for (const auto& a : accounts_summary)
        {
            if (a.account_id == conn.account_id && a.has_balance)
            {
                anchor_balance = a.current_balance;
                have_anchor = true;
                break;
            }
        }

        const std::set<std::string> connected_institutions = connectedInstitutionNamesLower(manager);

        Portfolio rebuilt(original_portfolio.getType(), 0.0, original_portfolio.getCurrency());
        double tx_sum = 0.0;
        for (const auto& tx : filtered)
        {
            const double our_amount = -tx.amount;
            const TransactionType type = (our_amount >= 0.0)
                ? TransactionType::DEPOSIT
                : TransactionType::WITHDRAWAL;
            const std::string display_name = !tx.merchant_name.empty() ? tx.merchant_name : tx.name;
            // Only tag transfers we believe land in another connected account
            // of the user's. Plaid tags Zelle-to-friend as TRANSFER_OUT too, but
            // its counterparty type is "user" / "payment_app" — those should
            // count as real outflows, not in-transit reconciliations.
            const bool plaid_says_transfer =
                tx.pfc_primary == "TRANSFER_IN" || tx.pfc_primary == "TRANSFER_OUT";
            const bool is_own_account_transfer =
                plaid_says_transfer && transferIsOwnAccount(tx, connected_institutions);
            std::string notes = is_own_account_transfer
                ? std::string("[TXFR] ") + display_name
                : display_name;
            // Mark pending so the UI can render them differently and the user
            // knows the balance anchor doesn't yet reflect this row. Prefix is
            // detectable from JS without a schema change.
            if (tx.pending)
            {
                notes = std::string("[PENDING] ") + notes;
            }
            // Persist the spend category for later analysis (credit cards in particular).
            // Prefer Plaid's detailed PFC ("FOOD_AND_DRINK_RESTAURANTS"); fall back to primary.
            const std::string category = !tx.pfc_detailed.empty()
                ? tx.pfc_detailed
                : tx.pfc_primary;
            rebuilt.addTransaction(tx.date, our_amount, type, notes, category);
            tx_sum += our_amount;
        }
        rebuilt.setAvailableCapital(have_anchor ? anchor_balance : tx_sum);

        if (!manager.savePortfolio(portfolio_name, rebuilt))
        {
            return makeJsonResponse(500, makeErrorBody("Sync succeeded but failed to save portfolio"));
        }
        if (!MarketDataSync::recomputePortfolioDailyValues(manager, portfolio_name))
        {
            return makeJsonResponse(500, makeErrorBody("Sync saved but daily totals failed to recompute"));
        }

        const time_t now = std::time(nullptr);
        conn.last_synced = now;
        manager.saveConnection(portfolio_name, conn);

        std::ostringstream out;
        out << "{"
            << "\"status\":\"ok\","
            << "\"portfolio\":" << jsonString(portfolio_name) << ","
            << "\"account_type\":\"cash\","
            << "\"transactions_imported\":" << filtered.size() << ","
            << "\"holdings_imported\":0,"
            << "\"available_capital\":" << jsonNumber(rebuilt.getAvailableCapital()) << ","
            << "\"last_synced\":" << static_cast<long long>(now)
            << "}";
        return makeJsonResponse(200, out.str());
    }

    // Plaid /investments/transactions/get sign: positive amount = cash IN (matches our sign).
    // Plaid quantity: positive = shares received, negative = shares delivered.
    HttpResponse syncInvestmentAccount(PortfolioManager& manager,
                                        const std::string& portfolio_name,
                                        const Plaid::Config& plaid_config,
                                        PortfolioConnection& conn,
                                        const std::string& start_date,
                                        const std::string& end_date,
                                        const Portfolio& original_portfolio,
                                        const std::vector<Plaid::AccountSummary>& accounts_summary)
    {
        // Pull investment transactions + holdings.
        std::vector<Plaid::InvestmentTransaction> inv_txs;
        std::vector<Plaid::Security> securities_a;
        std::string err;
        if (!Plaid::getInvestmentTransactions(plaid_config, conn.access_token,
                                              start_date, end_date,
                                              inv_txs, securities_a, err))
        {
            markConnectionReauthIfNeeded(manager, portfolio_name, conn, err);
            return makeJsonResponse(502, makeErrorBody("Plaid investments tx sync failed: " + err));
        }

        std::vector<Plaid::Holding> holdings;
        std::vector<Plaid::Security> securities_b;
        if (!Plaid::getHoldings(plaid_config, conn.access_token,
                                holdings, securities_b, err))
        {
            markConnectionReauthIfNeeded(manager, portfolio_name, conn, err);
            return makeJsonResponse(502, makeErrorBody("Plaid holdings fetch failed: " + err));
        }

        // Merge securities into one lookup table (security_id -> Security).
        std::map<std::string, Plaid::Security> sec_by_id;
        for (const auto& s : securities_a) sec_by_id[s.security_id] = s;
        for (const auto& s : securities_b) sec_by_id[s.security_id] = s;

        // Treat money-market / settlement funds as cash. Plaid sets is_cash_equivalent
        // for these; we also keep a known-ticker fallback for brokers that don't set it.
        static const std::set<std::string> CASH_EQUIV_TICKERS = {
            "VMFXX", "VUSXX", "VMSXX",                        // Vanguard
            "SPAXX", "FDRXX", "FCASH", "SPRXX", "FZFXX",      // Fidelity
            "SWVXX", "SNVXX",                                 // Schwab
            "TMCXX"                                           // T. Rowe Price
        };
        const auto isCashEquivSec = [&](const std::string& security_id) -> bool
        {
            auto it = sec_by_id.find(security_id);
            if (it == sec_by_id.end()) return false;
            if (it->second.is_cash_equivalent) return true;
            return CASH_EQUIV_TICKERS.count(it->second.ticker_symbol) > 0;
        };

        // Filter by connected account
        const auto matchesAccount = [&conn](const std::string& acct)
        {
            return conn.account_id.empty() || acct == conn.account_id;
        };

        // NOTE: stock files are intentionally NOT wiped here. Deleting them
        // destroys each ticker's fetched price history, and if the Yahoo
        // re-fetch after the rebuild fails (flaky network) the position is
        // left priceless and valued at $0 in totals/profits. Stale tickers
        // are pruned after the rebuilt transaction set is known, just before
        // savePortfolio below.

        // Rebuild portfolio from scratch
        Portfolio rebuilt(original_portfolio.getType(), 0.0, original_portfolio.getCurrency());

        // Per-ticker StockEvent accumulator (we'll save StockData files after).
        std::map<std::string, std::vector<StockEvent>> events_by_ticker;
        std::map<std::string, std::string> company_name_by_ticker;
        double running_cash = 0.0;

        const auto ensure_ticker = [&](const std::string& security_id) -> std::string
        {
            auto it = sec_by_id.find(security_id);
            if (it == sec_by_id.end()) return "";
            const std::string& ticker = it->second.ticker_symbol;
            if (ticker.empty()) return "";
            if (!company_name_by_ticker.count(ticker))
            {
                company_name_by_ticker[ticker] = it->second.name.empty() ? ticker : it->second.name;
            }
            return ticker;
        };

        // Sort investment txs by date so the StockData replay is well-ordered.
        std::sort(inv_txs.begin(), inv_txs.end(),
                  [](const Plaid::InvestmentTransaction& a, const Plaid::InvestmentTransaction& b)
                  { return a.date < b.date; });

        int investment_tx_imported = 0;
        for (const auto& tx : inv_txs)
        {
            if (!matchesAccount(tx.account_id)) continue;
            if (tx.type == "cancel") continue;

            // Money-market sweeps (VMFXX etc.) are internal cash movements within
            // the brokerage; ignore them entirely — the holdings step folds their
            // value into available_capital instead.
            if (isCashEquivSec(tx.security_id)) continue;

            const std::string ticker = ensure_ticker(tx.security_id);
            const double abs_shares = std::abs(tx.quantity);
            // Plaid /investments/transactions/get convention:
            //   amount > 0 = cash OUT (buys, withdrawals, fees)
            //   amount < 0 = cash IN  (sells, dividends, deposits, interest)
            // Our convention: amount > 0 = cash IN. So flip the sign.
            const double cash = -tx.amount;

            if (tx.type == "buy")
            {
                if (ticker.empty() || abs_shares <= 0.0)
                {
                    // Money-market sweep / reinvested dividend with no share movement.
                    // Record as a plain cash flow so available_capital stays accurate.
                    if (cash != 0.0)
                    {
                        const TransactionType ct = cash >= 0.0
                            ? TransactionType::DEPOSIT
                            : TransactionType::WITHDRAWAL;
                        rebuilt.addTransaction(tx.date, cash, ct, tx.name);
                        running_cash += cash;
                        ++investment_tx_imported;
                    }
                    continue;
                }
                rebuilt.addTransaction(tx.date, cash, TransactionType::BUY_STOCK,
                                       ticker, abs_shares, tx.name);
                events_by_ticker[ticker].emplace_back(
                    tx.date, StockEventType::BUY, abs_shares, tx.price, cash, tx.name);
                running_cash += cash;
                ++investment_tx_imported;
            }
            else if (tx.type == "sell")
            {
                if (ticker.empty() || abs_shares <= 0.0)
                {
                    if (cash != 0.0)
                    {
                        const TransactionType ct = cash >= 0.0
                            ? TransactionType::DEPOSIT
                            : TransactionType::WITHDRAWAL;
                        rebuilt.addTransaction(tx.date, cash, ct, tx.name);
                        running_cash += cash;
                        ++investment_tx_imported;
                    }
                    continue;
                }
                rebuilt.addTransaction(tx.date, cash, TransactionType::SELL_STOCK,
                                       ticker, abs_shares, tx.name);
                events_by_ticker[ticker].emplace_back(
                    tx.date, StockEventType::SELL, abs_shares, tx.price, cash, tx.name);
                running_cash += cash;
                ++investment_tx_imported;
            }
            else if (tx.type == "cash")
            {
                // Subtype-driven mapping
                if (tx.subtype == "dividend" && !ticker.empty())
                {
                    rebuilt.addTransaction(tx.date, cash, TransactionType::DIVIDEND,
                                           ticker, 0.0, tx.name);
                    events_by_ticker[ticker].emplace_back(
                        tx.date, StockEventType::DIVIDEND, 0.0, 0.0, cash, tx.name);
                }
                else if (tx.subtype == "interest" || tx.subtype == "interest income")
                {
                    rebuilt.addTransaction(tx.date, cash, TransactionType::INTEREST, tx.name);
                }
                else if (cash >= 0.0)
                {
                    rebuilt.addTransaction(tx.date, cash, TransactionType::DEPOSIT, tx.name);
                }
                else
                {
                    rebuilt.addTransaction(tx.date, cash, TransactionType::WITHDRAWAL, tx.name);
                }
                running_cash += cash;
                ++investment_tx_imported;
            }
            else if (tx.type == "transfer")
            {
                if (ticker.empty() || abs_shares <= 0.0) continue;
                // Share transfer with no cash movement on this side.
                const TransactionType ttype = (tx.quantity >= 0.0)
                    ? TransactionType::TRANSFER_IN_ASSET
                    : TransactionType::TRANSFER_OUT_ASSET;
                rebuilt.addTransaction(tx.date, 0.0, ttype, ticker, abs_shares, tx.name);
                // Approximate per-share cost basis as institution_price if available.
                if (ttype == TransactionType::TRANSFER_IN_ASSET)
                {
                    events_by_ticker[ticker].emplace_back(
                        tx.date, StockEventType::BUY, abs_shares,
                        tx.price > 0.0 ? tx.price : 0.0, 0.0, tx.name + " (transfer in)");
                }
                else
                {
                    events_by_ticker[ticker].emplace_back(
                        tx.date, StockEventType::SELL, abs_shares,
                        tx.price > 0.0 ? tx.price : 0.0, 0.0, tx.name + " (transfer out)");
                }
                ++investment_tx_imported;
            }
            else if (tx.type == "fee")
            {
                rebuilt.addTransaction(tx.date, cash, TransactionType::WITHDRAWAL, tx.name);
                running_cash += cash;
                ++investment_tx_imported;
            }
        }

        // Plaid's balances.current on an investment account = total account value
        // (cash + cash-equiv + stocks), not just settlement cash. To get the actual
        // available cash (including any cash-equiv sweep funds), subtract the value
        // of non-cash holdings from the total.
        double plaid_total_account_value = 0.0;
        bool have_balance_anchor = false;
        for (const auto& a : accounts_summary)
        {
            if (a.account_id == conn.account_id && a.has_balance)
            {
                plaid_total_account_value = a.current_balance;
                have_balance_anchor = true;
                break;
            }
        }

        double non_cash_holdings_value = 0.0;
        double cash_equiv_value = 0.0;
        for (const auto& holding : holdings)
        {
            if (!matchesAccount(holding.account_id)) continue;
            const double val = holding.institution_value > 0.0
                ? holding.institution_value
                : holding.quantity * holding.institution_price;
            if (isCashEquivSec(holding.security_id))
            {
                cash_equiv_value += val;
            }
            else
            {
                non_cash_holdings_value += val;
            }
        }

        // Trust the cash-equiv holdings sum directly — Plaid's account-level
        // current_balance can lag a dividend/interest sweep into the MM fund
        // by a day, while the per-holding institution_value reflects it
        // immediately. Leftover (settlement cash beyond the MM fund) is what
        // total minus stocks minus MM gives us, clamped at zero so a stale
        // total can't drag cash below the holdings-reported MM balance.
        double settlement_cash = plaid_total_account_value
                                 - non_cash_holdings_value
                                 - cash_equiv_value;
        if (settlement_cash < 0.0) settlement_cash = 0.0;
        rebuilt.setAvailableCapital(have_balance_anchor
            ? cash_equiv_value + settlement_cash
            : running_cash + cash_equiv_value);

        // Plaid only returns ~2 years of transactions, so a ticker may have SELLs
        // in our window without the matching BUYs (or only holdings with no buys at
        // all). For each gap, synthesize a paired DEPOSIT + BUY_STOCK at the earliest
        // possible date. The pair is cash-neutral but produces a labeled "Buy" event
        // with the correct cost basis, so the holdings table and chart look right.
        const time_t now = std::time(nullptr);
        const time_t fallback_synth_date = now - (2LL * 365 * 86400); // 2 years ago

        // Derives a per-share cost basis for a synthesized "prior history" buy.
        // Prefers the live holding's reported basis; when the position is no
        // longer held (e.g. fully sold, so Plaid reports no holding), falls back
        // to the ticker's historical market close on `when` so the synthesized
        // buy has a realistic cost basis instead of $0 — otherwise a later SELL
        // shows the entire proceeds as realized profit.
        const auto perShareBasisFromHolding = [&](const std::string& ticker, time_t when) -> double
        {
            for (const auto& h : holdings)
            {
                if (!matchesAccount(h.account_id)) continue;
                auto sit = sec_by_id.find(h.security_id);
                if (sit == sec_by_id.end()) continue;
                if (sit->second.ticker_symbol != ticker) continue;
                if (h.quantity > 0.0 && h.cost_basis > 0.0)
                {
                    return h.cost_basis / h.quantity;
                }
                if (h.institution_price > 0.0) return h.institution_price;
                break;
            }

            double historical_price = 0.0;
            std::string hist_err;
            if (MarketDataSync::fetchHistoricalClose(ticker, when, historical_price, hist_err) &&
                historical_price > 0.0)
            {
                return historical_price;
            }
            if (!hist_err.empty())
            {
                std::cerr << "Historical cost-basis lookup failed for " << ticker
                          << ": " << hist_err << std::endl;
            }
            return 0.0;
        };

        // Adds a paired DEPOSIT + BUY_STOCK (cash-neutral; produces a real "Buy" event).
        const auto addSynthBuy = [&](time_t when, const std::string& ticker, double shares,
                                      double per_share, const std::string& notes)
        {
            const double total = per_share * shares;
            rebuilt.addTransaction(when, total, TransactionType::DEPOSIT, notes);
            rebuilt.addTransaction(when, -total, TransactionType::BUY_STOCK,
                                   ticker, shares, notes);
        };

        // (1) Pre-flight: cover SELLs that drive running shares negative.
        {
            std::map<std::string, double> running;
            std::map<std::string, double> deficit;
            std::map<std::string, time_t> earliest;
            std::vector<Transaction> txs = rebuilt.getTransactions();
            std::sort(txs.begin(), txs.end(),
                      [](const Transaction& a, const Transaction& b){ return a.date < b.date; });
            for (const auto& tx : txs)
            {
                if (tx.stock_symbol.empty()) continue;
                const std::string& tk = tx.stock_symbol;
                if (!earliest.count(tk) || tx.date < earliest[tk]) earliest[tk] = tx.date;
                if (tx.type == TransactionType::BUY_STOCK ||
                    tx.type == TransactionType::TRANSFER_IN_ASSET)
                {
                    running[tk] += tx.shares;
                }
                else if (tx.type == TransactionType::SELL_STOCK ||
                         tx.type == TransactionType::TRANSFER_OUT_ASSET)
                {
                    running[tk] -= tx.shares;
                    if (running[tk] < 0.0 && -running[tk] > deficit[tk])
                    {
                        deficit[tk] = -running[tk];
                    }
                }
            }
            for (const auto& kv : deficit)
            {
                if (kv.second <= 1e-9) continue;
                const std::string& tk = kv.first;
                // Plant the synth at the data-window start (or earlier, if earliest
                // real tx predates the window). Anchoring it 1 day before the first
                // *recent* Plaid tx for the ticker makes the chart spike right
                // before that recent activity — but the position itself has been
                // there since long before our window.
                time_t when = fallback_synth_date;
                if (earliest.count(tk) && earliest[tk] - 86400 < when)
                {
                    when = earliest[tk] - 86400;
                }
                addSynthBuy(when, tk, kv.second, perShareBasisFromHolding(tk, when),
                            "Imported from prior history");
            }
        }

        // (2) Holdings reconciliation: positions Plaid says we own that aren't in txns.
        int holdings_imported = 0;
        for (const auto& holding : holdings)
        {
            if (!matchesAccount(holding.account_id)) continue;
            if (isCashEquivSec(holding.security_id)) continue;
            auto sec_it = sec_by_id.find(holding.security_id);
            if (sec_it == sec_by_id.end()) continue;
            const std::string ticker = sec_it->second.ticker_symbol;
            if (ticker.empty()) continue;

            double final_shares = 0.0;
            time_t earliest = now;
            bool earliest_set = false;
            for (const auto& tx : rebuilt.getTransactions())
            {
                if (tx.stock_symbol != ticker) continue;
                if (!earliest_set || tx.date < earliest) { earliest = tx.date; earliest_set = true; }
                if (tx.type == TransactionType::BUY_STOCK ||
                    tx.type == TransactionType::TRANSFER_IN_ASSET)
                {
                    final_shares += tx.shares;
                }
                else if (tx.type == TransactionType::SELL_STOCK ||
                         tx.type == TransactionType::TRANSFER_OUT_ASSET)
                {
                    final_shares -= tx.shares;
                }
            }
            const double diff = holding.quantity - final_shares;
            if (std::abs(diff) > 1e-6)
            {
                if (diff > 0.0)
                {
                    // Place the synth at the start of our data window, NOT next to the
                    // first real tx for this ticker — that real tx is often a small
                    // recent add-on to a position the user has actually held for years,
                    // and placing the synth there makes the chart show the holding
                    // "materializing" right before that add-on.
                    time_t when = fallback_synth_date;
                    if (earliest_set && earliest - 86400 < when) when = earliest - 86400;
                    addSynthBuy(when, ticker, diff, perShareBasisFromHolding(ticker, when),
                                "Imported from prior history");
                }
                else
                {
                    // Excess shares we somehow tracked but Plaid says we don't own — rare.
                    rebuilt.addTransaction(now, 0.0,
                                           TransactionType::TRANSFER_OUT_ASSET,
                                           ticker, -diff, "Reconciled from Plaid holdings");
                }
            }
            ++holdings_imported;
        }

        // Prune stock files whose ticker no longer appears anywhere in the
        // rebuilt transaction history (position dropped out of Plaid's window
        // entirely). Files for still-referenced tickers are kept so their
        // fetched price history survives the rebuild.
        {
            const auto normalizedTicker = [](const std::string& raw)
            {
                std::string normalized;
                normalized.reserve(raw.size());
                for (const char ch : raw)
                {
                    const unsigned char uch = static_cast<unsigned char>(ch);
                    if (std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.')
                    {
                        normalized.push_back(static_cast<char>(std::toupper(uch)));
                    }
                }
                return normalized;
            };

            std::set<std::string> rebuilt_tickers;
            for (const auto& tx : rebuilt.getTransactions())
            {
                if (!tx.stock_symbol.empty())
                {
                    rebuilt_tickers.insert(normalizedTicker(tx.stock_symbol));
                }
            }
            for (const auto& ticker : manager.listStocks(portfolio_name))
            {
                if (rebuilt_tickers.count(normalizedTicker(ticker)) == 0)
                {
                    manager.deleteStock(portfolio_name, ticker);
                }
            }
        }

        if (!manager.savePortfolio(portfolio_name, rebuilt))
        {
            return makeJsonResponse(500, makeErrorBody("Sync succeeded but failed to save portfolio"));
        }

        // Fetch live + historical prices for the new tickers, then recompute daily values.
        const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
        MarketDataSync::syncPortfolio(manager, portfolio_name, sync_config);
        MarketDataSync::recomputePortfolioDailyValues(manager, portfolio_name);

        conn.last_synced = now;
        manager.saveConnection(portfolio_name, conn);

        std::ostringstream out;
        out << "{"
            << "\"status\":\"ok\","
            << "\"portfolio\":" << jsonString(portfolio_name) << ","
            << "\"account_type\":\"investment\","
            << "\"transactions_imported\":" << investment_tx_imported << ","
            << "\"holdings_imported\":" << holdings_imported << ","
            << "\"available_capital\":" << jsonNumber(running_cash) << ","
            << "\"last_synced\":" << static_cast<long long>(now)
            << "}";
        return makeJsonResponse(200, out.str());
    }

    HttpResponse syncPortfolioFromConnection(PortfolioManager& manager,
                                              const std::string& portfolio_name)
    {
        // A missing/unreadable portfolio.dat is recoverable here: the sync paths
        // below rebuild the portfolio from scratch off Plaid data and only read
        // the type/currency from this object. If the file was lost (e.g. a failed
        // write), recreate a starter portfolio further down once we know the
        // account's type — otherwise a single missing file would 404 the hourly
        // sync forever and the account would silently vanish from the dashboard.
        Portfolio portfolio;
        const bool had_portfolio = manager.loadPortfolio(portfolio_name, portfolio);

        PortfolioConnection conn;
        if (!manager.loadConnection(portfolio_name, conn))
        {
            // Without a connection we have nothing to sync from, and (unlike a
            // missing portfolio.dat) nothing to rebuild it. Keep the 404.
            return makeJsonResponse(404, makeErrorBody("No connection configured for this account"));
        }

        const Plaid::Config plaid_config = Plaid::configFromEnvironment();
        if (!Plaid::isConfigured(plaid_config))
        {
            return makeJsonResponse(500, makeErrorBody("Plaid not configured on server (PLAID_CLIENT_ID / PLAID_SECRET)"));
        }

        const time_t now = std::time(nullptr);
        const time_t three_years_ago = now - (3LL * 365 * 86400);
        const std::string start_date = formatPlaidDateUTC(three_years_ago);
        const std::string end_date = formatPlaidDateUTC(now);

        // Look up the connected account's type so we know which sync path to take.
        std::vector<Plaid::AccountSummary> accounts;
        std::string ignored_inst_name, ignored_inst_id;
        std::string err;
        if (!Plaid::getAccounts(plaid_config, conn.access_token,
                                accounts, ignored_inst_name, ignored_inst_id, err))
        {
            markConnectionReauthIfNeeded(manager, portfolio_name, conn, err);
            return makeJsonResponse(502, makeErrorBody("Plaid accounts lookup failed: " + err));
        }

        std::string account_type;
        std::string account_subtype;
        for (const auto& a : accounts)
        {
            if (a.account_id == conn.account_id)
            {
                account_type = a.type;
                account_subtype = a.subtype;
                break;
            }
        }
        if (account_type.empty() && !accounts.empty())
        {
            account_type = accounts.front().type;
            account_subtype = accounts.front().subtype;
        }

        if (!had_portfolio)
        {
            // portfolio.dat was missing — synthesize a starter portfolio so the
            // rebuild below has the right type/currency to work from. The actual
            // holdings, transactions and balances all come from Plaid.
            PortfolioType recreated_type = PortfolioType::CASH;
            if (account_type == "investment")
            {
                const std::string sub = lowerCopy(account_subtype);
                if (sub.find("roth") != std::string::npos)
                    recreated_type = PortfolioType::ROTH_IRA;
                else if (sub.find("ira") != std::string::npos)
                    recreated_type = PortfolioType::TRADITIONAL_IRA;
                else
                    recreated_type = PortfolioType::BROKERAGE;
            }
            portfolio = Portfolio(recreated_type, 0.0, "USD");
            std::cerr << "[plaid] portfolio.dat missing for " << portfolio_name
                      << "; recreating as " << portfolioTypeToString(recreated_type)
                      << " from connection before sync" << std::endl;
        }

        if (account_type == "investment")
        {
            return syncInvestmentAccount(manager, portfolio_name, plaid_config, conn,
                                          start_date, end_date, portfolio, accounts);
        }
        return syncCashAccount(manager, portfolio_name, plaid_config, conn,
                                start_date, end_date, portfolio, accounts);
    }

    HttpResponse routeRequest(const HttpRequest& request, PortfolioManager& manager)
    {
        if (request.method == "OPTIONS")
        {
            return makeJsonResponse(204, "");
        }

        const std::vector<std::string> segments = splitPath(request.path);

        if (request.method == "GET" && request.path == "/api/health")
        {
            return makeJsonResponse(200, "{\"status\":\"ok\"}");
        }

        if (request.method == "GET" && request.path == "/api/portfolios")
        {
            return makeJsonResponse(200, buildPortfolioSummaryJson(manager));
        }

        if (request.method == "GET" && request.path == "/api/in-transit")
        {
            return makeJsonResponse(200, buildInTransitJson(manager));
        }

        if (request.method == "GET" && request.path == "/api/spend")
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            auto range_error = parseRangeParams(query_values, from, to);
            if (range_error.has_value())
            {
                return range_error.value();
            }
            return makeJsonResponse(200, buildSpendJson(manager, from, to));
        }

        if (request.method == "GET" && request.path == "/api/income")
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            auto range_error = parseRangeParams(query_values, from, to);
            if (range_error.has_value())
            {
                return range_error.value();
            }
            return makeJsonResponse(200, buildIncomeJson(manager, from, to));
        }

        if (request.method == "GET" && request.path == "/api/529/tags")
        {
            std::lock_guard<std::mutex> lock(g_529_mutex);
            std::vector<ExpenseTags::TagRecord> tags;
            if (!ExpenseTags::loadTags(kTagsFile, tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            std::ostringstream out;
            out << "{\"tags\":[";
            for (size_t i = 0; i < tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(tags[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/529/tag")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }

            const auto raw_key = getObjectString(body, "key");
            const auto raw_status = getObjectString(body, "status");
            if (!raw_key.has_value() || !raw_status.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("key and status are required"));
            }
            const std::string key = trim(raw_key.value());
            const std::string status = trim(raw_status.value());
            if (status != "qualified" && status != "dismissed" && status != "none")
            {
                return makeJsonResponse(400, makeErrorBody("status must be qualified, dismissed, or none"));
            }
            const auto raw_amount = getObjectNumber(body, "qualified_amount");
            return applyTagUpsert(key, status, raw_amount, kTagsFile, g_529_mutex,
                                  ensure529Dirs, manager, collectSpendTransactions);
        }

        if (request.method == "GET" && request.path == "/api/529/withdrawals")
        {
            std::lock_guard<std::mutex> lock(g_529_mutex);
            std::vector<ExpenseTags::TagRecord> withdrawals;
            if (!ExpenseTags::loadTags(k529WithdrawalsFile, withdrawals))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 withdrawals"));
            }
            std::ostringstream out;
            out << "{\"withdrawals\":[";
            for (size_t i = 0; i < withdrawals.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(withdrawals[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/529/withdrawal")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }

            const auto raw_status = getObjectString(body, "status");
            if (raw_status.has_value() && trim(raw_status.value()) == "none")
            {
                const auto raw_key = getObjectString(body, "key");
                if (!raw_key.has_value() || trim(raw_key.value()).empty())
                {
                    return makeJsonResponse(400, makeErrorBody("key is required to remove a withdrawal"));
                }
                const std::string key = trim(raw_key.value());
                std::lock_guard<std::mutex> lock(g_529_mutex);
                std::vector<ExpenseTags::TagRecord> withdrawals;
                if (!ExpenseTags::loadTags(k529WithdrawalsFile, withdrawals))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to read 529 withdrawals"));
                }
                auto existing = std::find_if(withdrawals.begin(), withdrawals.end(),
                    [&key](const ExpenseTags::TagRecord& t) { return t.key == key; });
                if (existing == withdrawals.end())
                {
                    return makeJsonResponse(404, makeErrorBody("No withdrawal for that key"));
                }
                withdrawals.erase(existing);
                if (!ExpenseTags::saveTags(k529WithdrawalsFile, withdrawals))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save 529 withdrawals"));
                }
                return makeJsonResponse(200, "{\"removed\":true}");
            }

            const auto raw_date = getObjectString(body, "date");
            const auto raw_amount = getObjectNumber(body, "amount");
            const auto raw_notes = getObjectString(body, "notes");
            if (!raw_date.has_value() || !raw_amount.has_value() || !raw_notes.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("date, amount, and notes are required"));
            }
            return createManualRecord(k529WithdrawalsFile, g_529_mutex, ensure529Dirs,
                                      raw_date.value(), raw_amount.value(),
                                      raw_notes.value(), "529 Plan", "WITHDRAWAL");
        }

        if (request.method == "GET" && request.path == "/api/tax/tags")
        {
            std::lock_guard<std::mutex> lock(g_tax_mutex);
            std::vector<ExpenseTags::TagRecord> income_tags;
            std::vector<ExpenseTags::TagRecord> deduction_tags;
            if (!ExpenseTags::loadTags(kTaxIncomeTagsFile, income_tags) ||
                !ExpenseTags::loadTags(kTaxDeductionTagsFile, deduction_tags))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read tax tags"));
            }
            std::ostringstream out;
            out << "{\"income\":[";
            for (size_t i = 0; i < income_tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(income_tags[i]);
            }
            out << "],\"deductions\":[";
            for (size_t i = 0; i < deduction_tags.size(); ++i)
            {
                if (i > 0) out << ",";
                out << serializeTagRecord(deduction_tags[i]);
            }
            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/tax/tag")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }
            // Manual income entries: pre-sync or off-platform income with no
            // Plaid transaction to key against. Creates a self-contained
            // record whose synthetic "manual-" key never matches a synced
            // transaction; totals/exports read the record's own fields so it
            // flows through everything downstream unchanged.
            const auto manual_it = body.object_value.find("manual");
            const bool is_manual = manual_it != body.object_value.end() &&
                                   manual_it->second.type == JsonType::BOOL &&
                                   manual_it->second.bool_value;
            if (is_manual)
            {
                const auto manual_kind = getObjectString(body, "kind");
                if (!manual_kind.has_value() || trim(manual_kind.value()) != "income")
                {
                    return makeJsonResponse(400, makeErrorBody("manual entries support kind income only"));
                }
                const auto raw_date = getObjectString(body, "date");
                const auto manual_amount = getObjectNumber(body, "amount");
                const auto raw_notes = getObjectString(body, "notes");
                if (!raw_date.has_value() || !manual_amount.has_value() || !raw_notes.has_value())
                {
                    return makeJsonResponse(400, makeErrorBody("date, amount, and notes are required"));
                }
                return createManualRecord(kTaxIncomeTagsFile, g_tax_mutex, ensureTaxDirs,
                                          raw_date.value(), manual_amount.value(),
                                          raw_notes.value(), "Manual", "MANUAL");
            }

            const auto raw_kind = getObjectString(body, "kind");
            const auto raw_key = getObjectString(body, "key");
            const auto raw_status = getObjectString(body, "status");
            if (!raw_kind.has_value() || !raw_key.has_value() || !raw_status.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("kind, key, and status are required"));
            }
            const std::string kind = trim(raw_kind.value());
            const std::string key = trim(raw_key.value());
            const std::string status = trim(raw_status.value());
            if (kind != "income" && kind != "deduction")
            {
                return makeJsonResponse(400, makeErrorBody("kind must be income or deduction"));
            }
            if (status != "qualified" && status != "dismissed" && status != "none")
            {
                return makeJsonResponse(400, makeErrorBody("status must be qualified, dismissed, or none"));
            }
            const auto raw_amount = getObjectNumber(body, "amount");
            const char* tags_file = (kind == "income") ? kTaxIncomeTagsFile : kTaxDeductionTagsFile;
            auto collect = (kind == "income") ? collectIncomeTransactions : collectSpendTransactions;
            return applyTagUpsert(key, status, raw_amount, tags_file, g_tax_mutex,
                                  ensureTaxDirs, manager, collect);
        }

        if (request.path == "/api/529/receipt")
        {
            return handleReceiptRoute(request, kTagsFile, kReceiptsDir, g_529_mutex);
        }

        if (request.method == "GET" &&
            (request.path == "/api/tax/export/income.csv" ||
             request.path == "/api/tax/export/deductions.csv"))
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            if (!parseYearParam(query_values, from, to))
            {
                return makeJsonResponse(400, makeErrorBody("year must be YYYY (2000-2100)"));
            }
            const bool is_income = request.path == "/api/tax/export/income.csv";
            std::lock_guard<std::mutex> lock(g_tax_mutex);
            bool ok = false;
            const auto tags = qualifiedTagsInRange(
                is_income ? kTaxIncomeTagsFile : kTaxDeductionTagsFile, from, to, ok);
            if (!ok)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read tax tags"));
            }
            HttpResponse response;
            response.status = 200;
            response.content_type = "text/csv; charset=utf-8";
            response.body = is_income
                ? buildTagCsv(tags, "date,account,source,category,deposit_amount,taxable_amount\r\n", false)
                : buildTagCsv(tags, "date,account,merchant,category,charge_amount,deductible_amount\r\n", false);
            return response;
        }

        if (request.method == "GET" && request.path == "/api/529/export.csv")
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            auto range_error = parseRangeParams(query_values, from, to);
            if (range_error.has_value())
            {
                return range_error.value();
            }

            std::lock_guard<std::mutex> lock(g_529_mutex);
            bool ok = false;
            const auto tags = qualifiedTagsInRange(kTagsFile, from, to, ok);
            if (!ok)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
            }
            HttpResponse response;
            response.status = 200;
            response.content_type = "text/csv; charset=utf-8";
            response.body = build529Csv(tags);
            return response;
        }

        if (request.method == "GET" && request.path == "/api/529/export.zip")
        {
            const auto query_values = parseQuery(request.query);
            time_t from = 0;
            time_t to = 0;
            auto range_error = parseRangeParams(query_values, from, to);
            if (range_error.has_value())
            {
                return range_error.value();
            }

            // Serialize whole-export against other exports (shared staging
            // dir), but hold g_529_mutex only while reading tags and copying
            // receipts — not across the blocking zip subprocess.
            std::lock_guard<std::mutex> export_lock(g_529_export_mutex);

            const std::filesystem::path staging = "data/529/.export_tmp";
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            std::filesystem::create_directories(staging, ec);
            if (ec)
            {
                return makeJsonResponse(500, makeErrorBody("Failed to create export staging dir"));
            }

            {
                std::lock_guard<std::mutex> lock(g_529_mutex);
                bool ok = false;
                const auto tags = qualifiedTagsInRange(kTagsFile, from, to, ok);
                if (!ok)
                {
                    std::filesystem::remove_all(staging, ec);
                    return makeJsonResponse(500, makeErrorBody("Failed to read 529 tags"));
                }

                {
                    std::ofstream csv(staging / "expenses.csv", std::ios::binary);
                    csv << build529Csv(tags);
                }

                char date_buf[16];
                for (const auto& tag : tags)
                {
                    std::tm tm_utc{};
                    gmtime_r(&tag.date, &tm_utc);
                    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
                    std::string merchant = ExpenseTags::sanitizeFilename(tag.notes);
                    if (merchant.size() > 40) merchant.resize(40);
                    if (merchant.empty()) merchant = "receipt";
                    for (const auto& receipt : tag.receipts)
                    {
                        // Stored names are sanitized at upload time; re-run
                        // sanitizeFilename as defense in depth so a tampered
                        // tags.json can't inject path components.
                        const std::string safe_receipt = ExpenseTags::sanitizeFilename(receipt);
                        if (safe_receipt.empty()) continue;
                        const std::filesystem::path src =
                            std::filesystem::path(kReceiptsDir) / tag.key / safe_receipt;

                        // Dedupe staging names: same-day same-merchant files
                        // with the same filename would otherwise collide and
                        // one copy would silently vanish from the ZIP.
                        const std::string base_name =
                            std::string(date_buf) + "_" + merchant + "_" + safe_receipt;
                        const size_t dot_pos = base_name.find_last_of('.');
                        const std::string base_stem =
                            (dot_pos != std::string::npos) ? base_name.substr(0, dot_pos) : base_name;
                        const std::string base_ext =
                            (dot_pos != std::string::npos) ? base_name.substr(dot_pos) : "";
                        std::string deduped_name = base_name;
                        int dedup_suffix = 2;
                        while (std::filesystem::exists(staging / deduped_name))
                        {
                            deduped_name = base_stem + "-" + std::to_string(dedup_suffix++) + base_ext;
                        }

                        const std::filesystem::path dst = staging / deduped_name;
                        std::filesystem::copy_file(src, dst,
                            std::filesystem::copy_options::none, ec);
                        // Missing files are skipped: the CSV still lists the name.
                    }
                }
            }

            // macOS ships /usr/bin/zip. Quote the path defensively even though
            // staging is a constant.
            const std::string cmd =
                "cd '" + staging.string() + "' && /usr/bin/zip -q -X export.zip . -r";
            if (std::system(cmd.c_str()) != 0)
            {
                std::filesystem::remove_all(staging, ec);
                return makeJsonResponse(500, makeErrorBody("zip failed"));
            }

            std::ifstream zip_file(staging / "export.zip", std::ios::binary);
            if (!zip_file.is_open())
            {
                std::filesystem::remove_all(staging, ec);
                return makeJsonResponse(500, makeErrorBody("zip output missing"));
            }
            std::ostringstream buffer;
            buffer << zip_file.rdbuf();
            zip_file.close();
            std::filesystem::remove_all(staging, ec);

            HttpResponse response;
            response.status = 200;
            response.content_type = "application/zip";
            response.body = buffer.str();
            return response;
        }

        if (request.method == "GET" && request.path == "/api/live-prices")
        {
            if (!manager.scanPortfolios())
            {
                return makeJsonResponse(500, makeErrorBody("Failed to scan portfolios"));
            }

            const std::vector<std::string> portfolio_names = manager.getPortfolioNames();
            std::map<std::string, bool> unique_tickers;
            for (const std::string& name : portfolio_names)
            {
                for (const std::string& ticker : listStocksCached(manager, name))
                {
                    unique_tickers[upperCopy(trim(ticker))] = true;
                }
            }

            std::vector<std::string> tickers;
            tickers.reserve(unique_tickers.size());
            for (const auto& entry : unique_tickers)
            {
                tickers.push_back(entry.first);
            }

            std::map<std::string, std::tuple<double, time_t, std::string, double>> quotes;
            std::string quote_error;
            if (!fetchLiveQuotesCached(tickers, quotes, quote_error))
            {
                return makeJsonResponse(502, makeErrorBody("Failed to fetch live prices: " + quote_error));
            }

            std::string aggregate_market_state = "CLOSED";
            for (const auto& quote_entry : quotes)
            {
                const std::string quote_state = std::get<2>(quote_entry.second);
                if (quote_state == "REGULAR")
                {
                    aggregate_market_state = "REGULAR";
                    break;
                }

                if (aggregate_market_state != "REGULAR" &&
                    (quote_state == "PRE" || quote_state == "POST"))
                {
                    aggregate_market_state = quote_state;
                }
            }

            std::ostringstream out;
            out << "{"
                << "\"market_state\":" << jsonString(aggregate_market_state) << ","
                << "\"portfolios\":[";

            bool first_portfolio = true;
            for (const std::string& name : portfolio_names)
            {
                Portfolio portfolio;
                if (!loadPortfolioCached(manager, name, portfolio))
                {
                    continue;
                }

                if (portfolio.getType() == PortfolioType::WATCHLIST)
                {
                    continue;
                }

                double estimated_total_value = portfolio.getAvailableCapital();
                size_t quote_count = 0;

                for (const std::string& ticker : listStocksCached(manager, name))
                {
                    StockData stock;
                    if (!loadStockDataCached(manager, name, ticker, stock))
                    {
                        continue;
                    }

                    const std::string normalized_ticker = upperCopy(trim(ticker));
                    auto quote_it = quotes.find(normalized_ticker);
                    if (quote_it != quotes.end())
                    {
                        estimated_total_value += stock.getSharesOwned() * std::get<0>(quote_it->second);
                        ++quote_count;
                        continue;
                    }

                    const auto& history = stock.getPriceHistory();
                    if (history.empty())
                    {
                        continue;
                    }

                    auto latest_it = std::max_element(
                        history.begin(),
                        history.end(),
                        [](const DailyStockPrice& lhs, const DailyStockPrice& rhs)
                        {
                            return lhs.date < rhs.date;
                        }
                    );
                    estimated_total_value += stock.getSharesOwned() * latest_it->close_price;
                }

                if (!first_portfolio)
                {
                    out << ",";
                }
                first_portfolio = false;

                out << "{"
                    << "\"name\":" << jsonString(name) << ","
                    << "\"estimated_total_value\":" << jsonNumber(estimated_total_value) << ","
                    << "\"quote_count\":" << quote_count
                    << "}";
            }

            out << "]}";
            return makeJsonResponse(200, out.str());
        }

        if (request.method == "POST" && request.path == "/api/portfolios")
        {
            JsonValue body;
            HttpResponse parse_error = parseJsonBodyObject(request, body);
            if (parse_error.status != 200)
            {
                return parse_error;
            }

            auto raw_name = getObjectString(body, "name");
            auto raw_type = getObjectString(body, "type");
            if (!raw_name.has_value() || !raw_type.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("name and type are required"));
            }

            const std::string portfolio_name = trim(raw_name.value());
            if (!isValidPortfolioName(portfolio_name))
            {
                return makeJsonResponse(400, makeErrorBody("name contains invalid characters or length"));
            }

            auto parsed_type = parsePortfolioType(raw_type.value());
            if (!parsed_type.has_value())
            {
                return makeJsonResponse(400, makeErrorBody("type must be BROKERAGE, ROTH_IRA, TRADITIONAL_IRA, WATCHLIST, CASH, CRYPTO, or DEBT"));
            }

            double initial_capital = getObjectNumber(body, "initial_capital").value_or(0.0);
            if (!isFiniteNonNegative(initial_capital))
            {
                return makeJsonResponse(400, makeErrorBody("initial_capital must be >= 0"));
            }

            if (parsed_type.value() == PortfolioType::WATCHLIST)
            {
                initial_capital = 0.0;
            }

            // Currency is meaningful for CASH accounts; everything else stays USD.
            std::string currency_code = "USD";
            auto raw_currency = getObjectString(body, "currency");
            if (raw_currency.has_value())
            {
                std::string normalized_ccy = upperCopy(trim(raw_currency.value()));
                if (parsed_type.value() == PortfolioType::CASH && !normalized_ccy.empty())
                {
                    if (!isValidCurrencyCode(normalized_ccy))
                    {
                        return makeJsonResponse(400, makeErrorBody("currency must be a 3-letter ISO 4217 code"));
                    }
                    currency_code = normalized_ccy;
                }
            }

            if (manager.scanPortfolios())
            {
                const auto& names = manager.getPortfolioNames();
                if (std::find(names.begin(), names.end(), portfolio_name) != names.end())
                {
                    return makeJsonResponse(409, makeErrorBody("Portfolio already exists"));
                }
            }

            if (!manager.createPortfolio(portfolio_name, parsed_type.value(), initial_capital, currency_code))
            {
                return makeJsonResponse(500, makeErrorBody("Failed to create portfolio"));
            }

            Portfolio created;
            if (!manager.loadPortfolio(portfolio_name, created))
            {
                return makeJsonResponse(500, makeErrorBody("Portfolio created but failed to load"));
            }

            std::ostringstream out;
            out << "{"
                << "\"status\":\"ok\","
                << "\"name\":" << jsonString(portfolio_name) << ","
                << "\"type\":" << jsonString(portfolioTypeToString(created.getType())) << ","
                << "\"currency\":" << jsonString(created.getCurrency()) << ","
                << "\"available_capital\":" << jsonNumber(created.getAvailableCapital())
                << "}";
            return makeJsonResponse(201, out.str());
        }

        if (segments.size() >= 3 && segments[0] == "api" && segments[1] == "portfolios")
        {
            const std::string portfolio_name = percentDecode(segments[2]);

            if (request.method == "GET" && segments.size() == 3)
            {
                const std::string json = buildPortfolioDetailJson(manager, portfolio_name);
                if (json.empty())
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }
                return makeJsonResponse(200, json);
            }

            if (request.method == "DELETE" && segments.size() == 3)
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Account not found"));
                }

                if (!manager.deletePortfolio(portfolio_name))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to delete account"));
                }

                std::ostringstream out;
                out << "{"
                    << "\"status\":\"ok\","
                    << "\"name\":" << jsonString(portfolio_name)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "GET" && segments.size() == 4 && segments[3] == "stocks")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }
                return makeJsonResponse(200, buildStocksJson(manager, portfolio_name));
            }

            if (request.method == "GET" && segments.size() == 4 && segments[3] == "live-prices")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                const std::vector<std::string> tickers = manager.listStocks(portfolio_name);
                std::map<std::string, std::tuple<double, time_t, std::string, double>> quotes;
                std::string quote_error;
                if (!fetchLiveQuotesCached(tickers, quotes, quote_error))
                {
                    return makeJsonResponse(502, makeErrorBody("Failed to fetch live prices: " + quote_error));
                }

                std::string aggregate_market_state = "CLOSED";

                std::ostringstream out;
                out << "{"
                    << "\"portfolio\":" << jsonString(portfolio_name) << ","
                    << "\"prices\":[";

                bool first = true;
                for (const auto& ticker : tickers)
                {
                    const std::string normalized = upperCopy(trim(ticker));
                    auto quote_it = quotes.find(normalized);
                    if (quote_it == quotes.end())
                    {
                        continue;
                    }

                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;

                    const double quote_price = std::get<0>(quote_it->second);
                    const time_t quote_as_of = std::get<1>(quote_it->second);
                    const std::string quote_state = std::get<2>(quote_it->second);
                    const double quote_open = std::get<3>(quote_it->second);

                    if (quote_state == "REGULAR")
                    {
                        aggregate_market_state = "REGULAR";
                    }
                    else if (aggregate_market_state != "REGULAR" &&
                             (quote_state == "PRE" || quote_state == "POST"))
                    {
                        aggregate_market_state = quote_state;
                    }

                    out << "{"
                        << "\"ticker\":" << jsonString(normalized) << ","
                        << "\"price\":" << jsonNumber(quote_price) << ","
                        << "\"open\":" << jsonNumber(quote_open) << ","
                        << "\"as_of\":" << static_cast<long long>(quote_as_of) << ","
                        << "\"market_state\":" << jsonString(quote_state)
                        << "}";
                }

                out << "],"
                    << "\"market_state\":" << jsonString(aggregate_market_state)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "POST" && segments.size() == 4 && segments[3] == "watchlist")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                if (portfolio.getType() != PortfolioType::WATCHLIST)
                {
                    return makeJsonResponse(409, makeErrorBody("watchlist endpoint is only available for WATCHLIST portfolios"));
                }

                JsonValue body;
                HttpResponse parse_error = parseJsonBodyObject(request, body);
                if (parse_error.status != 200)
                {
                    return parse_error;
                }

                auto ticker_value = getObjectString(body, "ticker");
                if (!ticker_value.has_value())
                {
                    return makeJsonResponse(400, makeErrorBody("ticker is required"));
                }

                const std::string ticker = upperCopy(trim(ticker_value.value()));
                if (!isValidTickerSymbol(ticker))
                {
                    return makeJsonResponse(400, makeErrorBody("Ticker contains invalid characters or length"));
                }

                bool is_invalid_ticker = false;
                const bool checked = maybeTickerExistsOnYahoo(ticker, is_invalid_ticker);
                if (checked && is_invalid_ticker)
                {
                    return makeJsonResponse(400, makeErrorBody("Ticker was not found by market data provider"));
                }

                const std::string stock_file = manager.getStockFilePath(portfolio_name, ticker);
                if (std::filesystem::exists(stock_file))
                {
                    return makeJsonResponse(200, "{\"status\":\"ok\",\"message\":\"Ticker already tracked\"}");
                }

                StockData stock;
                stock = StockData(ticker, ticker);
                stock.setTicker(ticker);
                stock.setCompanyName(ticker);
                if (!manager.saveStockData(portfolio_name, stock))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save watchlist ticker"));
                }

                {
                    std::string live_quote_error;
                    if (!persistLiveQuoteForTicker(manager, portfolio_name, ticker, live_quote_error))
                    {
                        std::cerr << "Immediate live quote persist skipped for watchlist ticker "
                                  << ticker << ": " << live_quote_error << std::endl;
                    }
                }

                std::ostringstream out;
                out << "{"
                    << "\"status\":\"ok\"," 
                    << "\"portfolio\":" << jsonString(portfolio_name) << ","
                    << "\"ticker\":" << jsonString(ticker)
                    << "}";
                return makeJsonResponse(201, out.str());
            }

            if (request.method == "DELETE" && segments.size() == 5 && segments[3] == "watchlist")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                if (portfolio.getType() != PortfolioType::WATCHLIST)
                {
                    return makeJsonResponse(409, makeErrorBody("watchlist endpoint is only available for WATCHLIST portfolios"));
                }

                const std::string ticker = upperCopy(trim(percentDecode(segments[4])));
                if (ticker.empty())
                {
                    return makeJsonResponse(400, makeErrorBody("ticker is required"));
                }

                if (!manager.deleteStock(portfolio_name, ticker))
                {
                    return makeJsonResponse(404, makeErrorBody("Ticker was not found in watchlist"));
                }

                std::ostringstream out;
                out << "{"
                    << "\"status\":\"ok\","
                    << "\"portfolio\":" << jsonString(portfolio_name) << ","
                    << "\"ticker\":" << jsonString(ticker)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "PATCH" && segments.size() == 5 && segments[3] == "watchlist")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                if (portfolio.getType() != PortfolioType::WATCHLIST)
                {
                    return makeJsonResponse(409, makeErrorBody("watchlist endpoint is only available for WATCHLIST portfolios"));
                }

                const std::string ticker = upperCopy(trim(percentDecode(segments[4])));
                if (ticker.empty())
                {
                    return makeJsonResponse(400, makeErrorBody("ticker is required"));
                }

                StockData stock;
                if (!manager.loadStockData(portfolio_name, ticker, stock))
                {
                    return makeJsonResponse(404, makeErrorBody("Ticker was not found in watchlist"));
                }

                JsonValue body;
                HttpResponse parse_error = parseJsonBodyObject(request, body);
                if (parse_error.status != 200)
                {
                    return parse_error;
                }

                bool changed = false;
                auto target_it = body.object_value.find("target_price");
                if (target_it != body.object_value.end())
                {
                    if (target_it->second.type == JsonType::NIL)
                    {
                        stock.setTargetPrice(0.0);
                        changed = true;
                    }
                    else if (target_it->second.type == JsonType::NUMBER)
                    {
                        const double value = target_it->second.number_value;
                        if (!std::isfinite(value))
                        {
                            return makeJsonResponse(400, makeErrorBody("target_price must be a finite number"));
                        }
                        stock.setTargetPrice(value > 0.0 ? value : 0.0);
                        changed = true;
                    }
                    else
                    {
                        return makeJsonResponse(400, makeErrorBody("target_price must be a number or null"));
                    }
                }

                auto notes_it = body.object_value.find("watchlist_notes");
                if (notes_it != body.object_value.end())
                {
                    if (notes_it->second.type == JsonType::NIL)
                    {
                        stock.setWatchlistNotes("");
                        changed = true;
                    }
                    else if (notes_it->second.type == JsonType::STRING)
                    {
                        std::string notes = notes_it->second.string_value;
                        if (notes.size() > 4096)
                        {
                            return makeJsonResponse(400, makeErrorBody("watchlist_notes exceeds 4096 character limit"));
                        }
                        stock.setWatchlistNotes(notes);
                        changed = true;
                    }
                    else
                    {
                        return makeJsonResponse(400, makeErrorBody("watchlist_notes must be a string or null"));
                    }
                }

                if (!changed)
                {
                    return makeJsonResponse(400, makeErrorBody("Provide target_price and/or watchlist_notes"));
                }

                if (!manager.saveStockData(portfolio_name, stock))
                {
                    return makeJsonResponse(500, makeErrorBody("Failed to save watchlist ticker"));
                }

                std::ostringstream out;
                out << "{"
                    << "\"status\":\"ok\","
                    << "\"portfolio\":" << jsonString(portfolio_name) << ","
                    << "\"ticker\":" << jsonString(ticker) << ","
                    << "\"target_price\":" << jsonNumber(stock.getTargetPrice()) << ","
                    << "\"watchlist_notes\":" << jsonString(stock.getWatchlistNotes())
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "GET" && segments.size() == 5 && segments[3] == "transactions" && segments[4] == "recent")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                int limit = 5;
                auto query_values = parseQuery(request.query);
                auto it = query_values.find("limit");
                if (it != query_values.end())
                {
                    auto maybe_limit = parsePositiveInt(it->second);
                    if (!maybe_limit.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("limit must be a positive integer"));
                    }
                    limit = maybe_limit.value();
                }

                std::ostringstream out;
                out << "{\"portfolio\":" << jsonString(portfolio_name)
                    << ",\"transactions\":" << serializeTransactions(portfolio.getTransactions(), limit)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "GET" && segments.size() == 4 && segments[3] == "transactions")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                std::ostringstream out;
                out << "{\"portfolio\":" << jsonString(portfolio_name)
                    << ",\"transactions\":" << serializeTransactions(portfolio.getTransactions(), -1)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (request.method == "POST" && segments.size() == 5 && segments[3] == "transactions")
            {
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }

                if (portfolio.getType() == PortfolioType::WATCHLIST)
                {
                    return makeJsonResponse(409, makeErrorBody("Transactions are disabled for WATCHLIST portfolios"));
                }

                if (manager.hasConnection(portfolio_name))
                {
                    return makeJsonResponse(409, makeErrorBody("Manual transactions are disabled for auto-synced accounts. Use Sync Now or Disconnect first."));
                }

                JsonValue body;
                HttpResponse parse_error = parseJsonBodyObject(request, body);
                if (parse_error.status != 200)
                {
                    return parse_error;
                }

                const std::string action = segments[4];
                const std::string notes = getObjectString(body, "notes").value_or("");
                const double date_number = getObjectNumber(body, "date").value_or(static_cast<double>(std::time(nullptr)));
                if (!std::isfinite(date_number) || date_number <= 0.0)
                {
                    return makeJsonResponse(400, makeErrorBody("date must be a valid unix timestamp"));
                }
                const time_t date = static_cast<time_t>(std::llround(date_number));

                if (action == "buy")
                {
                    auto ticker = getObjectString(body, "ticker");
                    auto shares = getObjectNumber(body, "shares");
                    auto price = getObjectNumber(body, "price_per_share");
                    if (!ticker.has_value() || !shares.has_value() || !price.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("buy requires ticker, shares, price_per_share"));
                    }

                    if (!isFinitePositive(shares.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("shares must be > 0"));
                    }

                    if (!isFiniteNonNegative(price.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("price_per_share must be >= 0"));
                    }

                    const double amount = -(shares.value() * price.value());
                    return appendStockTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::BUY_STOCK,
                        ticker.value(),
                        shares.value(),
                        price.value(),
                        amount,
                        date,
                        notes
                    );
                }

                if (action == "sell")
                {
                    auto ticker = getObjectString(body, "ticker");
                    auto shares = getObjectNumber(body, "shares");
                    auto price = getObjectNumber(body, "price_per_share");
                    if (!ticker.has_value() || !shares.has_value() || !price.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("sell requires ticker, shares, price_per_share"));
                    }

                    if (!isFinitePositive(shares.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("shares must be > 0"));
                    }

                    if (!isFiniteNonNegative(price.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("price_per_share must be >= 0"));
                    }

                    const double amount = shares.value() * price.value();
                    return appendStockTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::SELL_STOCK,
                        ticker.value(),
                        shares.value(),
                        price.value(),
                        amount,
                        date,
                        notes
                    );
                }

                if (action == "dividend")
                {
                    auto ticker = getObjectString(body, "ticker");
                    auto amount = getObjectNumber(body, "amount");
                    if (!ticker.has_value() || !amount.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("dividend requires ticker and amount"));
                    }

                    if (!isFiniteNonNegative(amount.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("amount must be >= 0"));
                    }

                    double shares = getObjectNumber(body, "shares").value_or(0.0);
                    if (shares <= 0.0)
                    {
                        Portfolio portfolio;
                        if (!manager.loadPortfolio(portfolio_name, portfolio))
                        {
                            return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                        }
                        shares = std::max(0.0, calculateSharesOwnedFromTransactions(portfolio, ticker.value()));
                    }

                    return appendStockTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::DIVIDEND,
                        ticker.value(),
                        shares,
                        0.0,
                        amount.value(),
                        date,
                        notes
                    );
                }

                if (action == "deposit")
                {
                    auto amount = getObjectNumber(body, "amount");
                    if (!amount.has_value() || !isFiniteNonNegative(amount.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("deposit requires non-negative amount"));
                    }

                    return appendCashTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::DEPOSIT,
                        amount.value(),
                        date,
                        notes
                    );
                }

                if (action == "withdrawal")
                {
                    auto amount = getObjectNumber(body, "amount");
                    if (!amount.has_value() || !isFiniteNonNegative(amount.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("withdrawal requires non-negative amount"));
                    }

                    return appendCashTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::WITHDRAWAL,
                        -amount.value(),
                        date,
                        notes
                    );
                }

                if (action == "interest")
                {
                    auto amount = getObjectNumber(body, "amount");
                    if (!amount.has_value() || !isFiniteNonNegative(amount.value()))
                    {
                        return makeJsonResponse(400, makeErrorBody("interest requires non-negative amount"));
                    }

                    return appendCashTransaction(
                        manager,
                        portfolio_name,
                        TransactionType::INTEREST,
                        amount.value(),
                        date,
                        notes
                    );
                }

                if (action == "transfer_in" || action == "transfer_out")
                {
                    auto ticker = getObjectString(body, "ticker");
                    auto shares = getObjectNumber(body, "shares");
                    if (!ticker.has_value() || !shares.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("transfer requires ticker and shares"));
                    }

                    const double cost_basis = (action == "transfer_in")
                        ? getObjectNumber(body, "cost_basis_per_share").value_or(0.0)
                        : 0.0;

                    return appendAssetTransferTransaction(
                        manager,
                        portfolio_name,
                        action == "transfer_in" ? TransactionType::TRANSFER_IN_ASSET
                                                : TransactionType::TRANSFER_OUT_ASSET,
                        ticker.value(),
                        shares.value(),
                        cost_basis,
                        date,
                        notes
                    );
                }

                return makeJsonResponse(404, makeErrorBody("Unknown transaction action"));
            }

            // -------- Auto-sync connection routes --------

            if (segments.size() == 5 && segments[3] == "connection" && segments[4] == "link-token")
            {
                if (request.method != "POST")
                {
                    return makeJsonResponse(405, makeErrorBody("Method not allowed"));
                }
                Portfolio portfolio;
                if (!manager.loadPortfolio(portfolio_name, portfolio))
                {
                    return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                }
                const Plaid::Config plaid_config = Plaid::configFromEnvironment();
                if (!Plaid::isConfigured(plaid_config))
                {
                    return makeJsonResponse(500, makeErrorBody("Plaid not configured on server (PLAID_CLIENT_ID / PLAID_SECRET)"));
                }
                std::string link_token;
                std::string err;
                if (!Plaid::createLinkToken(plaid_config, "user-" + portfolio_name, link_token, err))
                {
                    return makeJsonResponse(502, makeErrorBody("Plaid link token error: " + err));
                }
                std::ostringstream out;
                out << "{"
                    << "\"link_token\":" << jsonString(link_token) << ","
                    << "\"environment\":" << jsonString(plaid_config.environment)
                    << "}";
                return makeJsonResponse(200, out.str());
            }

            if (segments.size() == 5 && segments[3] == "connection" && segments[4] == "sync")
            {
                if (request.method != "POST")
                {
                    return makeJsonResponse(405, makeErrorBody("Method not allowed"));
                }
                return syncPortfolioFromConnection(manager, portfolio_name);
            }

            if (segments.size() == 4 && segments[3] == "connection")
            {
                if (request.method == "GET")
                {
                    return makeJsonResponse(200, buildConnectionJson(manager, portfolio_name));
                }
                if (request.method == "DELETE")
                {
                    if (!manager.hasConnection(portfolio_name))
                    {
                        return makeJsonResponse(404, makeErrorBody("No connection to delete"));
                    }
                    if (!manager.deleteConnection(portfolio_name))
                    {
                        return makeJsonResponse(500, makeErrorBody("Failed to delete connection"));
                    }
                    return makeJsonResponse(200, "{\"status\":\"ok\"}");
                }
                if (request.method == "POST")
                {
                    Portfolio portfolio;
                    if (!manager.loadPortfolio(portfolio_name, portfolio))
                    {
                        return makeJsonResponse(404, makeErrorBody("Portfolio not found"));
                    }
                    JsonValue body;
                    HttpResponse parse_error = parseJsonBodyObject(request, body);
                    if (parse_error.status != 200)
                    {
                        return parse_error;
                    }
                    auto public_token = getObjectString(body, "public_token");
                    if (!public_token.has_value() || public_token->empty())
                    {
                        return makeJsonResponse(400, makeErrorBody("public_token is required"));
                    }
                    auto requested_account = getObjectString(body, "account_id").value_or("");

                    const Plaid::Config plaid_config = Plaid::configFromEnvironment();
                    if (!Plaid::isConfigured(plaid_config))
                    {
                        return makeJsonResponse(500, makeErrorBody("Plaid not configured on server"));
                    }
                    std::string access_token;
                    std::string item_id;
                    std::string err;
                    if (!Plaid::exchangePublicToken(plaid_config, public_token.value(), access_token, item_id, err))
                    {
                        return makeJsonResponse(502, makeErrorBody("Plaid exchange failed: " + err));
                    }
                    std::vector<Plaid::AccountSummary> accounts;
                    std::string institution_name;
                    std::string institution_id;
                    if (!Plaid::getAccounts(plaid_config, access_token, accounts, institution_name, institution_id, err))
                    {
                        return makeJsonResponse(502, makeErrorBody("Plaid accounts fetch failed: " + err));
                    }
                    if (accounts.empty())
                    {
                        return makeJsonResponse(502, makeErrorBody("No accounts returned by Plaid"));
                    }
                    std::string chosen_account_id = requested_account;
                    if (chosen_account_id.empty()) chosen_account_id = accounts.front().account_id;

                    // If a prior connection exists (e.g. user is reconnecting after
                    // PLAID_CLIENT_ID was rotated and the old token is dead), preserve
                    // the original connected_at and reset last_cursor — the new item_id
                    // means Plaid's transactions/sync cursor from the old item is invalid.
                    PortfolioConnection conn;
                    PortfolioConnection prior;
                    const bool had_prior = manager.loadConnection(portfolio_name, prior);
                    conn.provider = "PLAID";
                    conn.institution_name = institution_name;
                    conn.institution_id = institution_id;
                    conn.item_id = item_id;
                    conn.access_token = access_token;
                    conn.account_id = chosen_account_id;
                    conn.connected_at = had_prior && prior.connected_at > 0
                                          ? prior.connected_at
                                          : std::time(nullptr);
                    conn.last_synced = 0;
                    conn.needs_reauth = false;
                    conn.reauth_detected_at = 0;
                    if (!manager.saveConnection(portfolio_name, conn))
                    {
                        return makeJsonResponse(500, makeErrorBody("Failed to persist connection"));
                    }

                    // Immediately sync — this wipes existing transactions and pulls fresh from Plaid.
                    HttpResponse sync_resp = syncPortfolioFromConnection(manager, portfolio_name);
                    if (sync_resp.status >= 200 && sync_resp.status < 300)
                    {
                        std::ostringstream out;
                        out << "{"
                            << "\"status\":\"ok\","
                            << "\"connection\":" << buildConnectionJson(manager, portfolio_name) << ","
                            << "\"accounts\":[";
                        for (size_t i = 0; i < accounts.size(); ++i)
                        {
                            if (i > 0) out << ",";
                            out << "{"
                                << "\"account_id\":" << jsonString(accounts[i].account_id) << ","
                                << "\"name\":" << jsonString(accounts[i].name) << ","
                                << "\"type\":" << jsonString(accounts[i].type) << ","
                                << "\"subtype\":" << jsonString(accounts[i].subtype) << ","
                                << "\"mask\":" << jsonString(accounts[i].mask)
                                << "}";
                        }
                        out << "]}";
                        return makeJsonResponse(201, out.str());
                    }
                    return sync_resp;
                }
                return makeJsonResponse(405, makeErrorBody("Method not allowed"));
            }
        }

        return makeJsonResponse(404, makeErrorBody("Endpoint not found"));
    }
}

PortfolioApiServer::PortfolioApiServer(std::string data_dir, uint16_t listen_port)
    : data_directory(std::move(data_dir)), port(listen_port)
{
}

bool PortfolioApiServer::start()
{
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        std::cerr << "Error creating socket: " << std::strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        std::cerr << "Error setting socket options: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return false;
    }

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        std::cerr << "Error binding socket on port " << port << ": " << std::strerror(errno) << std::endl;
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 64) < 0)
    {
        std::cerr << "Error listening on socket: " << std::strerror(errno) << std::endl;
        close(server_fd);
        return false;
    }

    std::cout << "Portfolio API server listening on http://localhost:" << port << std::endl;
    std::cout << "Data directory: " << data_directory << std::endl;

    // Run startup sync in the background so the API is responsive immediately.
    const std::string startup_sync_data_dir = data_directory;
    std::thread(
        [startup_sync_data_dir]()
        {
            PortfolioManager sync_manager(startup_sync_data_dir);
            const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
            std::lock_guard<std::mutex> lock(g_data_access_mutex);
            if (!MarketDataSync::syncAllPortfolios(sync_manager, sync_config))
            {
                std::cerr << "Startup market-data sync completed with errors" << std::endl;
            }
        }
    ).detach();

    // Keep daily values current by syncing once per day after market close.
    // Poll the wall clock on a short interval rather than sleeping for the full
    // gap until the next scheduled run: std::this_thread::sleep_for is backed by
    // a monotonic clock that pauses while the host is suspended (laptop asleep),
    // so a long sleep can fire hours or days after the intended wall-clock time.
    const std::string periodic_sync_data_dir = data_directory;
    std::thread(
        [periodic_sync_data_dir]()
        {
            long long last_synced_day = -1;
            time_t last_failed_attempt = 0;

            while (true)
            {
                const time_t now = std::time(nullptr);
                const long long target_day = latestExpectedDailySyncDayLocal(now);

                const bool sync_due = target_day > 0 && target_day > last_synced_day;
                const bool retry_cooldown_passed =
                    last_failed_attempt == 0 ||
                    (now - last_failed_attempt) >= (DAILY_SYNC_RETRY_MINUTES * 60);

                if (sync_due && retry_cooldown_passed)
                {
                    PortfolioManager sync_manager(periodic_sync_data_dir);
                    const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
                    std::lock_guard<std::mutex> lock(g_data_access_mutex);
                    if (MarketDataSync::syncAllPortfolios(sync_manager, sync_config))
                    {
                        last_synced_day = target_day;
                        last_failed_attempt = 0;
                    }
                    else
                    {
                        last_failed_attempt = now;
                        std::cerr << "Scheduled daily market-data sync completed with errors" << std::endl;
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(DAILY_SYNC_POLL_SECONDS));
            }
        }
    ).detach();

    // Nightly data/ snapshot with 14-day retention. Wall-clock polled for the
    // same suspend-safety reasons as the daily sync thread above.
    std::thread(
        []()
        {
            long long last_backup_day = -1;
            while (true)
            {
                const time_t now = std::time(nullptr);
                const long long current_day = static_cast<long long>(now / 86400);
                if (current_day != last_backup_day)
                {
                    runDataBackup();
                    last_backup_day = current_day;
                }
                std::this_thread::sleep_for(std::chrono::seconds(DAILY_SYNC_POLL_SECONDS));
            }
        }
    ).detach();

    // Hourly Plaid auto-pull: refresh every connected portfolio once per hour.
    // Plaid bills Transactions and Investments per Item per month (subscription),
    // so call frequency is bounded by per-Item rate limits (~30 calls/Item/day)
    // rather than cost. Hourly leaves headroom for retries and manual syncs.
    const std::string plaid_sync_data_dir = data_directory;
    std::thread(
        [plaid_sync_data_dir]()
        {
            time_t last_plaid_run = 0;
            while (true)
            {
                const time_t now = std::time(nullptr);
                if (now - last_plaid_run >= PLAID_SYNC_INTERVAL_SECONDS)
                {
                    PortfolioManager sync_manager(plaid_sync_data_dir);
                    if (sync_manager.scanPortfolios())
                    {
                        const auto& names = sync_manager.getPortfolioNames();
                        int succeeded = 0;
                        int failed = 0;
                        int skipped = 0;
                        for (const std::string& name : names)
                        {
                            if (!sync_manager.hasConnection(name))
                            {
                                ++skipped;
                                continue;
                            }
                            // Skip connections that previously surfaced a stale-token
                            // error — they need a fresh Plaid Link before they'll work
                            // again. Hammering Plaid won't change that.
                            PortfolioConnection peek;
                            if (sync_manager.loadConnection(name, peek) && peek.needs_reauth)
                            {
                                ++skipped;
                                continue;
                            }
                            std::lock_guard<std::mutex> lock(g_data_access_mutex);
                            HttpResponse resp = syncPortfolioFromConnection(sync_manager, name);
                            if (resp.status >= 200 && resp.status < 300)
                            {
                                ++succeeded;
                            }
                            else
                            {
                                ++failed;
                                std::cerr << "[plaid] hourly sync failed for "
                                          << name << ": status=" << resp.status
                                          << " body=" << resp.body << std::endl;
                            }
                        }
                        std::cerr << "[plaid] hourly sync: " << succeeded
                                  << " ok, " << failed << " failed, "
                                  << skipped << " not connected" << std::endl;
                    }
                    else
                    {
                        std::cerr << "[plaid] hourly sync: failed to scan portfolios" << std::endl;
                    }
                    last_plaid_run = now;
                }

                std::this_thread::sleep_for(std::chrono::seconds(PLAID_SYNC_POLL_SECONDS));
            }
        }
    ).detach();

    const std::string connection_data_dir = data_directory;
    while (true)
    {
        sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_address), &client_len);
        if (client_fd < 0)
        {
            std::cerr << "Error accepting client: " << std::strerror(errno) << std::endl;
            continue;
        }

        std::thread(
            [client_fd, connection_data_dir]()
            {
                PortfolioManager manager(connection_data_dir);

                auto raw_request = readHttpMessage(client_fd);
                HttpResponse response;
                if (!raw_request.has_value())
                {
                    response = makeJsonResponse(400, makeErrorBody("Failed to parse HTTP request"));
                }
                else
                {
                    auto request = parseHttpRequest(raw_request.value());
                    if (!request.has_value())
                    {
                        response = makeJsonResponse(400, makeErrorBody("Malformed HTTP request"));
                    }
                    else
                    {
                        const HttpRequest& parsed_request = request.value();
                        const bool is_read_only =
                            parsed_request.method == "GET" || parsed_request.method == "OPTIONS";

                        if (is_read_only)
                        {
                            response = routeRequest(parsed_request, manager);
                        }
                        else
                        {
                            std::lock_guard<std::mutex> lock(g_data_access_mutex);
                            response = routeRequest(parsed_request, manager);
                        }

                        applyContentNegotiation(response, parsed_request);
                    }
                }

                const std::string payload = makeHttpResponseText(response);
                sendAll(client_fd, payload);
                close(client_fd);
            }
        ).detach();
    }

    close(server_fd);
    return true;
}

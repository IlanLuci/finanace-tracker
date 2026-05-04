#include "web_server.hpp"

#include "market_data_sync.hpp"
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
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 2;
    constexpr uint32_t OLDEST_SUPPORTED_FILE_VERSION = 1;
    constexpr int DAILY_SYNC_HOUR_LOCAL = 18;
    constexpr int DAILY_SYNC_MINUTE_LOCAL = 5;
    constexpr int DAILY_SYNC_RETRY_MINUTES = 15;
    constexpr int DAILY_SYNC_POLL_SECONDS = 60;
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

    bool fetchYahooLiveQuotes(const std::vector<std::string>& symbols,
                              std::map<std::string, std::tuple<double, time_t, std::string>>& out_quotes,
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

                    out_quotes[symbol] = std::make_tuple(market_price, market_time, market_state);
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

            out_quotes[symbol] = std::make_tuple(price, market_time, market_state);
        }

        return true;
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

        std::map<std::string, std::tuple<double, time_t, std::string>> quotes;
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
                << "\"notes\":" << jsonString(tx.notes);
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

        return type <= static_cast<uint8_t>(PortfolioType::WATCHLIST);
    }

    double estimatePortfolioTotalValue(const Portfolio& portfolio, PortfolioManager& manager, const std::string& portfolio_name)
    {
        if (portfolio.getType() == PortfolioType::WATCHLIST)
        {
            return 0.0;
        }

        double total = portfolio.getAvailableCapital();
        const std::vector<std::string> tickers = manager.listStocks(portfolio_name);

        for (const std::string& ticker : tickers)
        {
            StockData stock;
            if (!manager.loadStockData(portfolio_name, ticker, stock))
            {
                continue;
            }

            const auto& prices = stock.getPriceHistory();
            if (prices.empty())
            {
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

    bool getLatestAndPreviousDistinctDayValues(const std::vector<DailyPortfolioValue>& values,
                                               double& latest_value,
                                               time_t& latest_date,
                                               double& previous_value,
                                               time_t& previous_date)
    {
        if (values.empty())
        {
            return false;
        }

        std::vector<DailyPortfolioValue> sorted_values = values;
        std::sort(
            sorted_values.begin(),
            sorted_values.end(),
            [](const DailyPortfolioValue& lhs, const DailyPortfolioValue& rhs)
            {
                return lhs.date < rhs.date;
            }
        );

        const DailyPortfolioValue& latest = sorted_values.back();
        latest_value = latest.value;
        latest_date = latest.date;

        const long long latest_bucket = dayBucketForTimestamp(latest.date);
        for (auto it = sorted_values.rbegin() + 1; it != sorted_values.rend(); ++it)
        {
            if (dayBucketForTimestamp(it->date) < latest_bucket)
            {
                previous_value = it->value;
                previous_date = it->date;
                return true;
            }
        }

        return false;
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
        const std::vector<std::string> tickers = manager.listStocks(portfolio_name);
        for (const std::string& ticker : tickers)
        {
            StockData stock;
            if (!manager.loadStockData(portfolio_name, ticker, stock))
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

        double latest_value = 0.0;
        time_t latest_date = 0;
        double previous_value = 0.0;
        time_t previous_date = 0;
        if (!getLatestAndPreviousDistinctDayValues(portfolio.getDailyValues(), latest_value, latest_date, previous_value, previous_date))
        {
            return 0.0;
        }

        const double current_total = estimatePortfolioTotalValue(portfolio, manager, portfolio_name);
        return current_total - previous_value;
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

        double latest_value = 0.0;
        time_t latest_date = 0;
        double previous_value = 0.0;
        time_t previous_date = 0;
        if (!getLatestAndPreviousDistinctDayValues(portfolio.getDailyValues(), latest_value, latest_date, previous_value, previous_date))
        {
            return 0.0;
        }

        if (std::abs(previous_value) < 1e-9)
        {
            return 0.0;
        }

        const double amount = estimatePortfolioTotalValue(portfolio, manager, portfolio_name) - previous_value;
        return (amount / std::abs(previous_value)) * 100.0;
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

            if (tx.type == TransactionType::BUY_STOCK)
            {
                shares_owned += tx.shares;
            }
            else if (tx.type == TransactionType::SELL_STOCK)
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
            if (!manager.loadPortfolio(name, portfolio))
            {
                continue;
            }

            if (!first)
            {
                out << ",";
            }
            first = false;

            out << "{"
                << "\"name\":" << jsonString(name) << ","
                << "\"type\":" << jsonString(portfolioTypeToString(portfolio.getType())) << ","
                << "\"available_capital\":" << jsonNumber(portfolio.getAvailableCapital()) << ","
                << "\"reported_total_value\":" << jsonNumber(portfolio.getCurrentPortfolioValue()) << ","
                << "\"estimated_total_value\":" << jsonNumber(estimatePortfolioTotalValue(portfolio, manager, name)) << ","
                << "\"day_change_amount\":" << jsonNumber(calculatePortfolioDayChangeAmount(portfolio, manager, name)) << ","
                << "\"day_change_percent\":" << jsonNumber(calculatePortfolioDayChangePercent(portfolio, manager, name)) << ","
                << "\"stock_count\":" << manager.listStocks(name).size() << ","
                << "\"transaction_count\":" << portfolio.getTransactions().size() << ","
                << "\"daily_values\":" << serializeDailyValues(portfolio.getDailyValues())
                << "}";
        }

        out << "]}";
        return out.str();
    }

    std::string buildPortfolioDetailJson(PortfolioManager& manager, const std::string& name)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(name, portfolio))
        {
            return "";
        }

        std::ostringstream out;
        out << "{"
            << "\"name\":" << jsonString(name) << ","
            << "\"type\":" << jsonString(portfolioTypeToString(portfolio.getType())) << ","
            << "\"available_capital\":" << jsonNumber(portfolio.getAvailableCapital()) << ","
            << "\"reported_total_value\":" << jsonNumber(portfolio.getCurrentPortfolioValue()) << ","
            << "\"estimated_total_value\":" << jsonNumber(estimatePortfolioTotalValue(portfolio, manager, name)) << ","
            << "\"day_change_amount\":" << jsonNumber(calculatePortfolioDayChangeAmount(portfolio, manager, name)) << ","
            << "\"day_change_percent\":" << jsonNumber(calculatePortfolioDayChangePercent(portfolio, manager, name)) << ","
            << "\"daily_values\":" << serializeDailyValues(portfolio.getDailyValues()) << ","
            << "\"transaction_count\":" << portfolio.getTransactions().size()
            << "}";

        return out.str();
    }

    std::string buildStocksJson(PortfolioManager& manager, const std::string& portfolio_name)
    {
        Portfolio portfolio;
        const bool has_portfolio = manager.loadPortfolio(portfolio_name, portfolio);
        std::ostringstream out;
        const std::vector<std::string> tickers = manager.listStocks(portfolio_name);

        out << "{\"portfolio\":" << jsonString(portfolio_name) << ",\"stocks\":[";

        bool first_stock = true;
        for (const auto& ticker : tickers)
        {
            StockData stock;
            if (!manager.loadStockData(portfolio_name, ticker, stock))
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
        out << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
        out << "Access-Control-Allow-Headers: Content-Type\r\n";
        out << "Content-Length: " << response.body.size() << "\r\n";
        out << "Connection: close\r\n\r\n";
        out << response.body;
        return out.str();
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

            if (data.size() > 2 * 1024 * 1024)
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
                for (const std::string& ticker : manager.listStocks(name))
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

            std::map<std::string, std::tuple<double, time_t, std::string>> quotes;
            std::string quote_error;
            if (!fetchYahooLiveQuotes(tickers, quotes, quote_error))
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
                if (!manager.loadPortfolio(name, portfolio))
                {
                    continue;
                }

                if (portfolio.getType() == PortfolioType::WATCHLIST)
                {
                    continue;
                }

                double estimated_total_value = portfolio.getAvailableCapital();
                size_t quote_count = 0;

                for (const std::string& ticker : manager.listStocks(name))
                {
                    StockData stock;
                    if (!manager.loadStockData(name, ticker, stock))
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
                return makeJsonResponse(400, makeErrorBody("type must be BROKERAGE, ROTH_IRA, TRADITIONAL_IRA, or WATCHLIST"));
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

            if (manager.scanPortfolios())
            {
                const auto& names = manager.getPortfolioNames();
                if (std::find(names.begin(), names.end(), portfolio_name) != names.end())
                {
                    return makeJsonResponse(409, makeErrorBody("Portfolio already exists"));
                }
            }

            if (!manager.createPortfolio(portfolio_name, parsed_type.value(), initial_capital))
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
                std::map<std::string, std::tuple<double, time_t, std::string>> quotes;
                std::string quote_error;
                if (!fetchYahooLiveQuotes(tickers, quotes, quote_error))
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

                return makeJsonResponse(404, makeErrorBody("Unknown transaction action"));
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

    PortfolioManager manager(data_directory);

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
            }
        }

        const std::string payload = makeHttpResponseText(response);
        sendAll(client_fd, payload);
        close(client_fd);
    }

    close(server_fd);
    return true;
}

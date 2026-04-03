#include "market_data_sync.hpp"

#include "portfolio_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr time_t SECONDS_PER_DAY = 86400;
    constexpr double EPS = 1e-9;

    bool isStockTransactionType(TransactionType type)
    {
        return type == TransactionType::BUY_STOCK ||
               type == TransactionType::SELL_STOCK ||
               type == TransactionType::DIVIDEND;
    }

    std::string normalizeTicker(const std::string& ticker)
    {
        std::string normalized;
        normalized.reserve(ticker.size());

        for (const char ch : ticker)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.')
            {
                normalized.push_back(static_cast<char>(std::toupper(uch)));
            }
        }

        return normalized;
    }

    long long dayBucket(time_t ts)
    {
        return static_cast<long long>(ts) / static_cast<long long>(SECONDS_PER_DAY);
    }

    time_t dayBucketToTimestamp(long long day)
    {
        return static_cast<time_t>(day * static_cast<long long>(SECONDS_PER_DAY) + 16 * 3600);
    }

    bool isWeekday(long long day)
    {
        const time_t ts = static_cast<time_t>(day * static_cast<long long>(SECONDS_PER_DAY));
        std::tm* local = std::localtime(&ts);
        if (local == nullptr)
        {
            return false;
        }

        return local->tm_wday >= 1 && local->tm_wday <= 5;
    }

    long long latestExpectedTradingDay(time_t now)
    {
        long long day = dayBucket(now);
        std::tm* local = std::localtime(&now);
        if (local == nullptr)
        {
            return day;
        }

        const bool before_close = (local->tm_hour < 16);
        if (before_close)
        {
            --day;
        }

        while (day > 0 && !isWeekday(day))
        {
            --day;
        }

        return day;
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

    bool parseDate(const std::string& date_text, time_t& out_ts)
    {
        if (date_text.size() != 10 || date_text[4] != '-' || date_text[7] != '-')
        {
            return false;
        }

        std::tm tm_value = {};
        try
        {
            tm_value.tm_year = std::stoi(date_text.substr(0, 4)) - 1900;
            tm_value.tm_mon = std::stoi(date_text.substr(5, 2)) - 1;
            tm_value.tm_mday = std::stoi(date_text.substr(8, 2));
        }
        catch (const std::exception&)
        {
            return false;
        }
        tm_value.tm_hour = 16;
        tm_value.tm_min = 0;
        tm_value.tm_sec = 0;

        out_ts = std::mktime(&tm_value);
        return out_ts > 0;
    }

    bool fetchFromAlphaVantage(const std::string& symbol,
                               const std::string& api_key,
                               std::string& response_body,
                               std::string& error)
    {
        auto performRequest = [&](const std::string& function_name, std::string& out_body) -> bool
        {
            const std::string url =
                "https://www.alphavantage.co/query?function=" + function_name +
                "&outputsize=compact&symbol=" + symbol + "&apikey=" + api_key;

            const std::string command =
                "curl -sS --fail --connect-timeout 15 --max-time 45 '" + shellEscapeSingleQuoted(url) + "'";

            std::array<char, 4096> buffer = {};
            FILE* pipe = popen(command.c_str(), "r");
            if (pipe == nullptr)
            {
                error = "Failed to execute curl for " + symbol;
                return false;
            }

            out_body.clear();
            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            {
                out_body += buffer.data();
            }

            const int status = pclose(pipe);
            if (status != 0)
            {
                error = "HTTP request failed for " + symbol;
                return false;
            }

            return true;
        };

        // Use the free endpoint directly to avoid spending extra requests
        // on premium-only adjusted data calls.
        if (!performRequest("TIME_SERIES_DAILY", response_body))
        {
            return false;
        }

        auto extractJsonStringValue = [](const std::string& json, const std::string& key) -> std::string
        {
            const std::string marker = "\"" + key + "\"";
            const size_t key_pos = json.find(marker);
            if (key_pos == std::string::npos)
            {
                return "";
            }

            const size_t colon = json.find(':', key_pos + marker.size());
            if (colon == std::string::npos)
            {
                return "";
            }

            const size_t quote_start = json.find('"', colon + 1);
            if (quote_start == std::string::npos)
            {
                return "";
            }

            std::string out;
            out.reserve(128);
            bool escaping = false;
            for (size_t i = quote_start + 1; i < json.size(); ++i)
            {
                const char ch = json[i];
                if (escaping)
                {
                    out.push_back(ch);
                    escaping = false;
                    continue;
                }

                if (ch == '\\')
                {
                    escaping = true;
                    continue;
                }

                if (ch == '"')
                {
                    break;
                }

                out.push_back(ch);
            }

            return out;
        };

        if (response_body.find("\"Error Message\"") != std::string::npos)
        {
            std::string message = extractJsonStringValue(response_body, "Error Message");
            if (message.empty())
            {
                message = "Alpha Vantage reported an error";
            }
            error = message + " for " + symbol;
            return false;
        }

        if (response_body.find("\"Note\"") != std::string::npos)
        {
            std::string message = extractJsonStringValue(response_body, "Note");
            if (message.empty())
            {
                message = "Alpha Vantage request note returned";
            }
            error = message;
            return false;
        }

        if (response_body.find("\"Information\"") != std::string::npos)
        {
            std::string message = extractJsonStringValue(response_body, "Information");
            if (message.empty())
            {
                message = "Alpha Vantage returned informational response";
            }
            error = message;
            return false;
        }

        return true;
    }

    bool fetchFromYahoo(const std::string& symbol,
                        std::string& response_body,
                        std::string& error)
    {
        // Yahoo Finance API: no authentication required, unlimited free tier
        const std::string url =
            "https://query1.finance.yahoo.com/v8/finance/chart/" + symbol +
            "?interval=1d&range=2y";

        // Include proper User-Agent and headers to avoid being blocked as a bot
        // Use --compressed to handle gzip/deflate automatically
        const std::string command =
            "curl -sS --compressed -w '\\n%{http_code}' --connect-timeout 15 --max-time 45 "
            "-H 'User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36' "
            "-H 'Accept: application/json' "
            "-H 'Accept-Language: en-US,en;q=0.9' "
            "-H 'DNT: 1' "
            "-H 'Connection: keep-alive' "
            "'" + shellEscapeSingleQuoted(url) + "'";

        std::array<char, 4096> buffer = {};
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
        {
            error = "Failed to execute curl for " + symbol;
            return false;
        }

        response_body.clear();
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
            response_body += buffer.data();
        }

        const int status = pclose(pipe);
        if (status != 0)
        {
            error = "curl execution failed for " + symbol;
            return false;
        }

        // Extract HTTP status code from last line of response
        const size_t last_newline = response_body.rfind('\n');
        std::string http_status_str = "500";
        if (last_newline != std::string::npos && last_newline + 1 < response_body.length())
        {
            http_status_str = response_body.substr(last_newline + 1);
            // Remove trailing whitespace
            http_status_str.erase(http_status_str.find_last_not_of(" \n\r\t") + 1);
            response_body = response_body.substr(0, last_newline);
        }

        // Safely parse HTTP status code
        int http_code = 500;
        try
        {
            http_code = std::stoi(http_status_str);
        }
        catch (const std::exception&)
        {
            error = "Failed to parse HTTP status code (got: '" + http_status_str + "') for " + symbol;
            return false;
        }

        if (http_code < 200 || http_code >= 300)
        {
            error = "HTTP " + std::to_string(http_code) + " (request failed for " + symbol + ")";
            return false;
        }

        return true;
    }

    bool parseDailyCloseSeriesFromYahoo(const std::string& json,
                                        std::map<long long, double>& prices_by_day,
                                        std::string& error)
    {
        prices_by_day.clear();

        // Parse Yahoo Finance response format:
        // {"chart": {"result": [{"timestamp": [...], "indicators": {"quote": [{"close": [...]}]}}]}}

        // Check for error indicators
        if (json.find("\"No matching Symbol.\"") != std::string::npos)
        {
            error = "Yahoo Finance: no matching symbol";
            return false;
        }

        if (json.find("\"chart\"") == std::string::npos)
        {
            error = "Yahoo Finance: malformed response (missing chart)";
            return false;
        }

        // Extract timestamp array
        const std::string ts_marker = "\"timestamp\":[";
        const size_t ts_pos = json.find(ts_marker);
        if (ts_pos == std::string::npos)
        {
            error = "Yahoo Finance: missing timestamp array";
            return false;
        }

        size_t cursor = ts_pos + ts_marker.length();
        size_t bracket = json.find(']', cursor);
        if (bracket == std::string::npos)
        {
            error = "Yahoo Finance: malformed timestamp array";
            return false;
        }

        std::vector<long long> timestamps;
        {
            const std::string ts_str = json.substr(cursor, bracket - cursor);
            size_t pos = 0;
            while (pos < ts_str.length())
            {
                // Skip whitespace and commas
                while (pos < ts_str.length() && (std::isspace(ts_str[pos]) || ts_str[pos] == ','))
                {
                    ++pos;
                }

                if (pos >= ts_str.length())
                    break;

                size_t end = pos;
                while (end < ts_str.length() && std::isdigit(ts_str[end]))
                {
                    ++end;
                }

                if (end > pos)
                {
                    try
                    {
                        timestamps.push_back(std::stoll(ts_str.substr(pos, end - pos)));
                    }
                    catch (const std::exception&)
                    {
                        // skip malformed timestamp
                    }
                }

                pos = end;
            }
        }

        if (timestamps.empty())
        {
            error = "Yahoo Finance: no timestamps parsed";
            return false;
        }

        // Extract close prices array
        const std::string close_marker = "\"close\":[";
        const size_t close_pos = json.find(close_marker);
        if (close_pos == std::string::npos)
        {
            error = "Yahoo Finance: missing close price array";
            return false;
        }

        cursor = close_pos + close_marker.length();
        bracket = json.find(']', cursor);
        if (bracket == std::string::npos)
        {
            error = "Yahoo Finance: malformed close price array";
            return false;
        }

        std::vector<double> closes;
        {
            const std::string close_str = json.substr(cursor, bracket - cursor);
            size_t pos = 0;
            while (pos < close_str.length())
            {
                // Skip whitespace and commas
                while (pos < close_str.length() && (std::isspace(close_str[pos]) || close_str[pos] == ','))
                {
                    ++pos;
                }

                if (pos >= close_str.length())
                    break;

                // Handle null values
                if (close_str.substr(pos, 4) == "null")
                {
                    closes.push_back(0.0); // placeholder for null close
                    pos += 4;
                    continue;
                }

                size_t end = pos;
                if (close_str[end] == '-' || close_str[end] == '+')
                    ++end;
                while (end < close_str.length() && (std::isdigit(close_str[end]) || close_str[end] == '.'))
                {
                    ++end;
                }

                if (end > pos)
                {
                    try
                    {
                        closes.push_back(std::stod(close_str.substr(pos, end - pos)));
                    }
                    catch (const std::exception&)
                    {
                        closes.push_back(0.0);
                    }
                }

                pos = end;
            }
        }

        if (closes.empty())
        {
            error = "Yahoo Finance: no close prices parsed";
            return false;
        }

        // Pair timestamps with close prices
        const size_t n = std::min(timestamps.size(), closes.size());
        for (size_t i = 0; i < n; ++i)
        {
            const long long ts = timestamps[i];
            const double price = closes[i];

            // Skip null prices
            if (price < EPS)
            {
                continue;
            }

            // Convert unix timestamp to day bucket
            long long day = dayBucket(static_cast<time_t>(ts));
            prices_by_day[day] = price;
        }

        if (prices_by_day.empty())
        {
            error = "Yahoo Finance: no valid price data extracted";
            return false;
        }

        return true;
    }

    bool isLikelyRateLimitError(std::string message)
    {
        std::transform(
            message.begin(),
            message.end(),
            message.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
        );

        return message.find("rate") != std::string::npos ||
               message.find("limit") != std::string::npos ||
               message.find("frequency") != std::string::npos ||
               message.find("too many") != std::string::npos;
    }

    bool parseDailyCloseSeries(const std::string& json,
                               std::map<long long, double>& prices_by_day,
                               std::string& error)
    {
        const std::string marker = "\"Time Series (Daily)\"";
        const size_t start = json.find(marker);
        if (start == std::string::npos)
        {
            if (json.find("\"Note\"") != std::string::npos)
            {
                error = "Alpha Vantage returned Note payload (likely rate limit/throttle)";
            }
            else if (json.find("\"Information\"") != std::string::npos)
            {
                error = "Alpha Vantage returned Information payload instead of time series";
            }
            else if (json.find("\"Error Message\"") != std::string::npos)
            {
                error = "Alpha Vantage returned Error Message payload";
            }
            else
            {
                error = "Missing Time Series (Daily) in response";
            }
            return false;
        }

        prices_by_day.clear();

        size_t cursor = json.find('"', start + marker.size());
        while (cursor != std::string::npos)
        {
            if (cursor + 11 >= json.size())
            {
                break;
            }

            const bool is_date_key =
                std::isdigit(static_cast<unsigned char>(json[cursor + 1])) &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 2])) &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 3])) &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 4])) &&
                json[cursor + 5] == '-' &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 6])) &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 7])) &&
                json[cursor + 8] == '-' &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 9])) &&
                std::isdigit(static_cast<unsigned char>(json[cursor + 10])) &&
                json[cursor + 11] == '"';

            if (is_date_key)
            {
                const std::string date_text = json.substr(cursor + 1, 10);

                const size_t colon = json.find(':', cursor + 12);
                if (colon == std::string::npos)
                {
                    break;
                }

                const size_t block_open = json.find('{', colon);
                if (block_open == std::string::npos)
                {
                    break;
                }

                const size_t block_close = json.find('}', block_open + 1);
                if (block_close == std::string::npos)
                {
                    break;
                }

                const size_t close_key = json.find("\"4. close\"", block_open);
                if (close_key != std::string::npos && close_key < block_close)
                {
                    const size_t value_colon = json.find(':', close_key);
                    if (value_colon == std::string::npos || value_colon > block_close)
                    {
                        cursor = block_close + 1;
                        continue;
                    }

                    size_t value_quote_start = json.find('"', value_colon + 1);
                    if (value_quote_start == std::string::npos)
                    {
                        cursor = block_close + 1;
                        continue;
                    }

                    size_t value_quote_end = json.find('"', value_quote_start + 1);
                    if (value_quote_end == std::string::npos || value_quote_end > block_close)
                    {
                        cursor = block_close + 1;
                        continue;
                    }

                    const std::string close_text = json.substr(
                        value_quote_start + 1,
                        value_quote_end - value_quote_start - 1
                    );

                    try
                    {
                        const double close_price = std::stod(close_text);
                        time_t parsed_time = 0;
                        if (parseDate(date_text, parsed_time))
                        {
                            prices_by_day[dayBucket(parsed_time)] = close_price;
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }

                cursor = block_close + 1;
                continue;
            }

            cursor = json.find('"', cursor + 1);
        }

        if (prices_by_day.empty())
        {
            error = "No daily close values found in response";
            return false;
        }

        return true;
    }

    bool shouldFetchTickerData(const StockData& stock, long long earliest_required_day)
    {
        const auto& prices = stock.getPriceHistory();
        if (prices.empty())
        {
            return true;
        }

        long long min_day = std::numeric_limits<long long>::max();
        long long max_day = std::numeric_limits<long long>::min();
        std::set<long long> known_days;
        for (const auto& point : prices)
        {
            const long long day = dayBucket(point.date);
            known_days.insert(day);
            min_day = std::min(min_day, day);
            max_day = std::max(max_day, day);
        }

        if (earliest_required_day < min_day)
        {
            return true;
        }

        for (const auto& event : stock.getEvents())
        {
            const long long event_day = dayBucket(event.date);
            if (isWeekday(event_day) && known_days.find(event_day) == known_days.end())
            {
                return true;
            }
        }

        if (max_day < latestExpectedTradingDay(std::time(nullptr)))
        {
            return true;
        }

        return false;
    }

    time_t latestPriceHistoryUpdate(const StockData& stock)
    {
        time_t latest = 0;
        for (const auto& point : stock.getPriceHistory())
        {
            latest = std::max(latest, point.last_updated);
        }
        return latest;
    }

    bool collectTickerEarliestDays(const Portfolio& portfolio,
                                   std::unordered_map<std::string, long long>& earliest_day_by_ticker)
    {
        earliest_day_by_ticker.clear();

        for (const auto& tx : portfolio.getTransactions())
        {
            if (!isStockTransactionType(tx.type))
            {
                continue;
            }

            const std::string ticker = normalizeTicker(tx.stock_symbol);
            if (ticker.empty())
            {
                continue;
            }

            const long long tx_day = dayBucket(tx.date);
            auto it = earliest_day_by_ticker.find(ticker);
            if (it == earliest_day_by_ticker.end())
            {
                earliest_day_by_ticker[ticker] = tx_day;
            }
            else
            {
                it->second = std::min(it->second, tx_day);
            }
        }

        return true;
    }

    class DailyRequestLedger
    {
    private:
        fs::path ledger_path;
        long long day_key = -1;
        size_t used_requests = 0;

        static fs::path tempPathFor(const fs::path& final_path)
        {
            return final_path.string() + ".tmp.sync";
        }

        bool save() const
        {
            try
            {
                if (!ledger_path.parent_path().empty())
                {
                    fs::create_directories(ledger_path.parent_path());
                }

                const fs::path temp_path = tempPathFor(ledger_path);
                {
                    std::ofstream out(temp_path, std::ios::trunc);
                    if (!out.is_open())
                    {
                        return false;
                    }

                    out << day_key << '\n' << used_requests << '\n';
                    if (!out.good())
                    {
                        return false;
                    }
                }

                if (fs::exists(ledger_path))
                {
                    fs::remove(ledger_path);
                }
                fs::rename(temp_path, ledger_path);
                return true;
            }
            catch (const fs::filesystem_error&)
            {
                return false;
            }
        }

    public:
        explicit DailyRequestLedger(fs::path path)
            : ledger_path(std::move(path))
        {
        }

        bool load()
        {
            day_key = dayBucket(std::time(nullptr));
            used_requests = 0;

            std::ifstream in(ledger_path);
            if (!in.is_open())
            {
                return true;
            }

            long long stored_day = -1;
            size_t stored_count = 0;
            if (!(in >> stored_day >> stored_count))
            {
                return true;
            }

            if (stored_day == day_key)
            {
                used_requests = stored_count;
            }

            return true;
        }

        bool consume(size_t limit)
        {
            const long long current_day = dayBucket(std::time(nullptr));
            if (current_day != day_key)
            {
                day_key = current_day;
                used_requests = 0;
            }

            if (used_requests >= limit)
            {
                return false;
            }

            ++used_requests;
            return save();
        }
    };

    bool updateTickerPriceHistory(PortfolioManager& manager,
                                  const std::string& portfolio_name,
                                  const std::string& ticker,
                                  const std::map<long long, double>& fetched_prices,
                                  long long earliest_needed_day)
    {
        StockData stock;
        const bool has_existing = manager.loadStockData(portfolio_name, ticker, stock);
        if (!has_existing)
        {
            stock = StockData(ticker, ticker);
        }

        if (stock.getTicker().empty())
        {
            stock.setTicker(ticker);
        }
        if (stock.getCompanyName().empty())
        {
            stock.setCompanyName(ticker);
        }

        bool changed = false;
        for (const auto& entry : fetched_prices)
        {
            if (entry.first < earliest_needed_day)
            {
                continue;
            }

            const time_t ts = dayBucketToTimestamp(entry.first);
            if (!stock.updateDailyClosePrice(ts, entry.second))
            {
                stock.addDailyClosePrice(ts, entry.second);
            }
            changed = true;
        }

        if (!changed)
        {
            return true;
        }

        return manager.saveStockData(portfolio_name, stock);
    }

    fs::path requestLedgerPathForPortfolio(PortfolioManager& manager, const std::string& portfolio_name)
    {
        return fs::path(manager.getPortfolioPath(portfolio_name)).parent_path() / ".alphavantage_budget";
    }
}

namespace MarketDataSync
{
    SyncConfig configFromEnvironment()
    {
        SyncConfig config;

        const char* alpha_api_key = std::getenv("ALPHAVANTAGE_API_KEY");
        if (alpha_api_key != nullptr)
        {
            config.alpha_vantage_api_key = alpha_api_key;
        }

        const char* max_req = std::getenv("ALPHAVANTAGE_MAX_REQUESTS_PER_RUN");
        if (max_req != nullptr)
        {
            const long parsed = std::strtol(max_req, nullptr, 10);
            if (parsed > 0)
            {
                config.max_requests_per_run = static_cast<size_t>(parsed);
            }
        }

        const char* min_wait = std::getenv("ALPHAVANTAGE_MIN_SECONDS_BETWEEN_REQUESTS");
        if (min_wait != nullptr)
        {
            const long parsed = std::strtol(min_wait, nullptr, 10);
            if (parsed > 0)
            {
                config.min_seconds_between_requests = static_cast<int>(parsed);
            }
        }

        return config;
    }

    bool recomputePortfolioDailyValues(PortfolioManager& manager, const std::string& portfolio_name)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            std::cerr << "Failed to load portfolio for recompute: " << portfolio_name << std::endl;
            return false;
        }

        std::unordered_map<std::string, double> shares_by_ticker;
        std::vector<Transaction> txs = portfolio.getTransactions();
        std::sort(
            txs.begin(),
            txs.end(),
            [](const Transaction& lhs, const Transaction& rhs)
            {
                return lhs.date < rhs.date;
            }
        );

        std::set<long long> timeline_days;
        double initial_cash = portfolio.getAvailableCapital();
        for (const auto& tx : txs)
        {
            timeline_days.insert(dayBucket(tx.date));
            initial_cash -= tx.amount;
        }

        struct PriceTrack
        {
            std::vector<DailyStockPrice> sorted_prices;
            size_t cursor = 0;
            double current_price = 0.0;
        };

        std::unordered_map<std::string, PriceTrack> price_tracks;
        for (const auto& ticker : manager.listStocks(portfolio_name))
        {
            StockData stock;
            if (!manager.loadStockData(portfolio_name, ticker, stock))
            {
                continue;
            }

            std::vector<DailyStockPrice> prices = stock.getPriceHistory();
            std::sort(
                prices.begin(),
                prices.end(),
                [](const DailyStockPrice& lhs, const DailyStockPrice& rhs)
                {
                    return lhs.date < rhs.date;
                }
            );

            if (prices.empty())
            {
                continue;
            }

            for (const auto& point : prices)
            {
                timeline_days.insert(dayBucket(point.date));
            }

            price_tracks[ticker] = PriceTrack{std::move(prices), 0, 0.0};
        }

        if (timeline_days.empty())
        {
            portfolio.clearDailyValues();
            return manager.savePortfolio(portfolio_name, portfolio);
        }

        std::vector<DailyPortfolioValue> rebuilt_values;
        rebuilt_values.reserve(timeline_days.size());

        size_t tx_cursor = 0;
        double running_cash = initial_cash;

        for (const long long day : timeline_days)
        {
            while (tx_cursor < txs.size() && dayBucket(txs[tx_cursor].date) <= day)
            {
                const Transaction& tx = txs[tx_cursor];
                running_cash += tx.amount;

                if (tx.type == TransactionType::BUY_STOCK)
                {
                    shares_by_ticker[normalizeTicker(tx.stock_symbol)] += tx.shares;
                }
                else if (tx.type == TransactionType::SELL_STOCK)
                {
                    shares_by_ticker[normalizeTicker(tx.stock_symbol)] -= tx.shares;
                }

                ++tx_cursor;
            }

            double total_value = running_cash;

            for (auto& [ticker, track] : price_tracks)
            {
                while (track.cursor < track.sorted_prices.size() &&
                       dayBucket(track.sorted_prices[track.cursor].date) <= day)
                {
                    track.current_price = track.sorted_prices[track.cursor].close_price;
                    ++track.cursor;
                }

                auto shares_it = shares_by_ticker.find(ticker);
                if (shares_it == shares_by_ticker.end() || std::abs(shares_it->second) <= EPS)
                {
                    continue;
                }

                total_value += shares_it->second * track.current_price;
            }

            rebuilt_values.emplace_back(dayBucketToTimestamp(day), total_value, std::time(nullptr));
        }

        portfolio.setDailyValues(rebuilt_values);
        return manager.savePortfolio(portfolio_name, portfolio);
    }

    bool syncPortfolio(PortfolioManager& manager, const std::string& portfolio_name, const SyncConfig& config)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            return false;
        }

        // Seed or refresh transaction-derived stock files first so later market-data
        // updates can apply to both existing and newly discovered tickers.
        if (!manager.savePortfolio(portfolio_name, portfolio))
        {
            std::cerr << "Failed to seed stock files before market-data sync for "
                      << portfolio_name << std::endl;
            return false;
        }

        std::unordered_map<std::string, long long> earliest_day_by_ticker;
        collectTickerEarliestDays(portfolio, earliest_day_by_ticker);

        DailyRequestLedger request_ledger(requestLedgerPathForPortfolio(manager, portfolio_name));
        if (!request_ledger.load())
        {
            std::cerr << "Failed to load Alpha Vantage request ledger for " << portfolio_name << std::endl;
        }

        struct SyncCandidate
        {
            std::string ticker;
            long long earliest_needed_day = 0;
            time_t price_last_updated = 0;
            StockData stock;
        };

        std::vector<SyncCandidate> ordered_candidates;
        ordered_candidates.reserve(earliest_day_by_ticker.size());

        for (const auto& [ticker, earliest_needed_day] : earliest_day_by_ticker)
        {
            StockData stock;
            if (!manager.loadStockData(portfolio_name, ticker, stock))
            {
                stock = StockData(ticker, ticker);
            }

            ordered_candidates.push_back(SyncCandidate{
                ticker,
                earliest_needed_day,
                latestPriceHistoryUpdate(stock),
                std::move(stock)
            });
        }

        std::sort(
            ordered_candidates.begin(),
            ordered_candidates.end(),
            [](const SyncCandidate& lhs, const SyncCandidate& rhs)
            {
                if (lhs.price_last_updated != rhs.price_last_updated)
                {
                    return lhs.price_last_updated < rhs.price_last_updated;
                }
                return lhs.ticker < rhs.ticker;
            }
        );

        bool yahoo_rate_limited = false;
        bool alpha_vantage_rate_limited = false;
        size_t yahoo_request_count = 0;
        size_t alpha_request_count = 0;

        for (const auto& candidate : ordered_candidates)
        {
            const std::string& ticker = candidate.ticker;
            const long long earliest_needed_day = candidate.earliest_needed_day;
            const bool needs_fetch = shouldFetchTickerData(candidate.stock, earliest_needed_day);
            if (!needs_fetch)
            {
                continue;
            }

            bool fetched_from_provider = false;

            // Try Yahoo Finance first (unlimited, free, no auth required)
            if (!yahoo_rate_limited)
            {
                // Add delay between Yahoo Finance requests (500ms) to avoid rate limiting
                if (yahoo_request_count > 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                std::string response_body;
                std::string fetch_error;
                if (!fetchFromYahoo(ticker, response_body, fetch_error))
                {
                    std::cerr << "Yahoo Finance fetch failed for " << ticker << ": " << fetch_error << std::endl;
                    
                    // If we get 429 (rate limit), stop trying Yahoo Finance
                    if (fetch_error.find("429") != std::string::npos)
                    {
                        std::cerr << "Yahoo Finance rate limit reached; switching to Alpha Vantage backup" << std::endl;
                        yahoo_rate_limited = true;
                    }
                    
                    ++yahoo_request_count;
                }
                else
                {
                    std::map<long long, double> prices_by_day;
                    std::string parse_error;
                    if (!parseDailyCloseSeriesFromYahoo(response_body, prices_by_day, parse_error))
                    {
                        std::cerr << "Failed to parse Yahoo Finance response for " << ticker
                                  << ": " << parse_error << std::endl;
                        ++yahoo_request_count;
                    }
                    else
                    {
                        if (!updateTickerPriceHistory(manager, portfolio_name, ticker, prices_by_day, earliest_needed_day))
                        {
                            std::cerr << "Failed to persist market data for " << ticker << std::endl;
                        }

                        ++yahoo_request_count;
                        fetched_from_provider = true;
                    }
                }
            }

            if (!fetched_from_provider)
            {
                if (config.alpha_vantage_api_key.empty())
                {
                    std::cerr << "Skipping Alpha Vantage backup for " << ticker
                              << " (ALPHAVANTAGE_API_KEY is not set)" << std::endl;
                    continue;
                }

                if (alpha_request_count >= config.max_requests_per_run)
                {
                    std::cerr << "Alpha Vantage backup request budget reached for this run ("
                              << config.max_requests_per_run << ")" << std::endl;
                    alpha_vantage_rate_limited = true;
                    continue;
                }

                if (!request_ledger.consume(config.max_requests_per_run))
                {
                    std::cerr << "Alpha Vantage daily request budget reached for " << portfolio_name << std::endl;
                    alpha_vantage_rate_limited = true;
                    continue;
                }

                if (alpha_request_count > 0 && config.min_seconds_between_requests > 0)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(config.min_seconds_between_requests));
                }

                std::string response_body;
                std::string fetch_error;
                if (!fetchFromAlphaVantage(ticker, config.alpha_vantage_api_key, response_body, fetch_error))
                {
                    std::cerr << "Alpha Vantage backup fetch failed for " << ticker << ": " << fetch_error << std::endl;
                    if (isLikelyRateLimitError(fetch_error))
                    {
                        alpha_vantage_rate_limited = true;
                    }

                    ++alpha_request_count;
                    continue;
                }

                std::map<long long, double> prices_by_day;
                std::string parse_error;
                if (!parseDailyCloseSeries(response_body, prices_by_day, parse_error))
                {
                    std::cerr << "Failed to parse Alpha Vantage backup response for " << ticker
                              << ": " << parse_error << std::endl;
                    if (isLikelyRateLimitError(parse_error))
                    {
                        alpha_vantage_rate_limited = true;
                    }

                    ++alpha_request_count;
                    continue;
                }

                if (!updateTickerPriceHistory(manager, portfolio_name, ticker, prices_by_day, earliest_needed_day))
                {
                    std::cerr << "Failed to persist market data for " << ticker << std::endl;
                }

                ++alpha_request_count;
                fetched_from_provider = true;
            }
        }

        if (yahoo_request_count > 0)
        {
            std::cerr << "Yahoo Finance primary provider: " << yahoo_request_count << " requests completed" << std::endl;
        }

        if (alpha_vantage_rate_limited)
        {
            std::cerr << "Alpha Vantage backup rate-limited during fallback" << std::endl;
        }

        return recomputePortfolioDailyValues(manager, portfolio_name);
    }

    bool syncAllPortfolios(PortfolioManager& manager, const SyncConfig& config)
    {

        if (!manager.scanPortfolios())
        {
            return false;
        }

        bool all_ok = true;
        for (const auto& name : manager.getPortfolioNames())
        {
            if (!syncPortfolio(manager, name, config))
            {
                all_ok = false;
            }
        }

        return all_ok;
    }
}

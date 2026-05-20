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
#include <iostream>
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
    constexpr time_t SECONDS_PER_DAY = 86400;
    constexpr double EPS = 1e-9;

    bool isStockTransactionType(TransactionType type)
    {
        return type == TransactionType::BUY_STOCK ||
               type == TransactionType::SELL_STOCK ||
               type == TransactionType::DIVIDEND ||
               type == TransactionType::TRANSFER_IN_ASSET ||
               type == TransactionType::TRANSFER_OUT_ASSET;
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

}

namespace MarketDataSync
{
    SyncConfig configFromEnvironment()
    {
        return SyncConfig{};
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

        const auto isCashNeutral = [](TransactionType t)
        {
            return t == TransactionType::TRANSFER_IN_ASSET ||
                   t == TransactionType::TRANSFER_OUT_ASSET;
        };

        std::set<long long> timeline_days;
        double initial_cash = portfolio.getAvailableCapital();
        for (const auto& tx : txs)
        {
            timeline_days.insert(dayBucket(tx.date));
            if (!isCashNeutral(tx.type))
            {
                initial_cash -= tx.amount;
            }
        }

        // CASH portfolios have no stocks; rebuild the balance series from
        // transactions alone, and always anchor a point at "today" so a freshly
        // created account shows up on the chart.
        if (portfolio.getType() == PortfolioType::CASH)
        {
            const long long today = dayBucket(std::time(nullptr));
            timeline_days.insert(today);

            std::vector<DailyPortfolioValue> rebuilt_values;
            rebuilt_values.reserve(timeline_days.size());

            size_t tx_cursor = 0;
            double running_cash = initial_cash;
            for (const long long day : timeline_days)
            {
                while (tx_cursor < txs.size() && dayBucket(txs[tx_cursor].date) <= day)
                {
                    running_cash += txs[tx_cursor].amount;
                    ++tx_cursor;
                }
                rebuilt_values.emplace_back(dayBucketToTimestamp(day), running_cash, std::time(nullptr));
            }

            portfolio.setDailyValues(rebuilt_values);
            return manager.savePortfolio(portfolio_name, portfolio);
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
                if (!isCashNeutral(tx.type))
                {
                    running_cash += tx.amount;
                }

                if (tx.type == TransactionType::BUY_STOCK ||
                    tx.type == TransactionType::TRANSFER_IN_ASSET)
                {
                    shares_by_ticker[normalizeTicker(tx.stock_symbol)] += tx.shares;
                }
                else if (tx.type == TransactionType::SELL_STOCK ||
                         tx.type == TransactionType::TRANSFER_OUT_ASSET)
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

    bool syncPortfolio(PortfolioManager& manager, const std::string& portfolio_name, const SyncConfig&)
    {
        Portfolio portfolio;
        if (!manager.loadPortfolio(portfolio_name, portfolio))
        {
            return false;
        }

        // CASH portfolios hold no tickers — skip the Yahoo Finance fetch path
        // and just rebuild the balance series from transactions.
        if (portfolio.getType() == PortfolioType::CASH)
        {
            return recomputePortfolioDailyValues(manager, portfolio_name);
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

        size_t yahoo_request_count = 0;

        for (const auto& candidate : ordered_candidates)
        {
            const std::string& ticker = candidate.ticker;
            const long long earliest_needed_day = candidate.earliest_needed_day;
            const bool needs_fetch = shouldFetchTickerData(candidate.stock, earliest_needed_day);
            if (!needs_fetch)
            {
                continue;
            }

            // Add delay between Yahoo Finance requests (500ms) to reduce throttling risk.
            if (yahoo_request_count > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::string response_body;
            std::string fetch_error;
            if (!fetchFromYahoo(ticker, response_body, fetch_error))
            {
                std::cerr << "Yahoo Finance fetch failed for " << ticker << ": " << fetch_error << std::endl;
                ++yahoo_request_count;
                continue;
            }

            std::map<long long, double> prices_by_day;
            std::string parse_error;
            if (!parseDailyCloseSeriesFromYahoo(response_body, prices_by_day, parse_error))
            {
                std::cerr << "Failed to parse Yahoo Finance response for " << ticker
                          << ": " << parse_error << std::endl;
                ++yahoo_request_count;
                continue;
            }

            if (!updateTickerPriceHistory(manager, portfolio_name, ticker, prices_by_day, earliest_needed_day))
            {
                std::cerr << "Failed to persist market data for " << ticker << std::endl;
            }

            ++yahoo_request_count;
        }

        if (yahoo_request_count > 0)
        {
            std::cerr << "Yahoo Finance primary provider: " << yahoo_request_count << " requests completed" << std::endl;
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

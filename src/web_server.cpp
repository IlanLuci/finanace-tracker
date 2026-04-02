#include "web_server.hpp"

#include "market_data_sync.hpp"
#include "portfolio_data.hpp"

#include <algorithm>
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
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 2;
    constexpr uint32_t OLDEST_SUPPORTED_FILE_VERSION = 1;

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

    std::string serializeTransaction(const Transaction& tx)
    {
        std::ostringstream out;
        out << "{"
            << "\"date\":" << static_cast<long long>(tx.date) << ","
            << "\"amount\":" << jsonNumber(tx.amount) << ","
            << "\"type\":" << jsonString(transactionTypeToString(tx.type)) << ","
            << "\"stock_symbol\":" << jsonString(tx.stock_symbol) << ","
            << "\"shares\":" << jsonNumber(tx.shares) << ","
            << "\"notes\":" << jsonString(tx.notes)
            << "}";
        return out.str();
    }

    std::string serializeTransactions(std::vector<Transaction> txs, int limit)
    {
        std::sort(
            txs.begin(),
            txs.end(),
            [](const Transaction& lhs, const Transaction& rhs)
            {
                return lhs.date > rhs.date;
            }
        );

        if (limit > 0 && static_cast<size_t>(limit) < txs.size())
        {
            txs.resize(static_cast<size_t>(limit));
        }

        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < txs.size(); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            out << serializeTransaction(txs[i]);
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

        return type <= static_cast<uint8_t>(PortfolioType::TRADITIONAL_IRA);
    }

    double estimatePortfolioTotalValue(const Portfolio& portfolio, PortfolioManager& manager, const std::string& portfolio_name)
    {
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
            << "\"daily_values\":" << serializeDailyValues(portfolio.getDailyValues()) << ","
            << "\"transaction_count\":" << portfolio.getTransactions().size()
            << "}";

        return out.str();
    }

    std::string buildStocksJson(PortfolioManager& manager, const std::string& portfolio_name)
    {
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
            if (!stock.getPriceHistory().empty())
            {
                auto latest_it = std::max_element(
                    stock.getPriceHistory().begin(),
                    stock.getPriceHistory().end(),
                    [](const DailyStockPrice& lhs, const DailyStockPrice& rhs)
                    {
                        return lhs.date < rhs.date;
                    }
                );
                latest_price = latest_it->close_price;
                latest_price_date = latest_it->date;
            }

            out << "{"
                << "\"ticker\":" << jsonString(stock.getTicker()) << ","
                << "\"company_name\":" << jsonString(stock.getCompanyName()) << ","
                << "\"shares_owned\":" << jsonNumber(stock.getSharesOwned()) << ","
                << "\"average_purchase_price\":" << jsonNumber(stock.getAveragePurchasePrice()) << ","
                << "\"last_updated\":" << static_cast<long long>(stock.getLastUpdated()) << ","
                << "\"latest_close_price\":" << jsonNumber(latest_price) << ","
                << "\"latest_close_date\":" << static_cast<long long>(latest_price_date) << ","
                << "\"position_market_value\":" << jsonNumber(stock.getSharesOwned() * latest_price) << ","
                << "\"event_count\":" << stock.getEvents().size() << ","
                << "\"recent_events\":";

            std::vector<StockEvent> sorted_events = stock.getEvents();
            std::sort(
                sorted_events.begin(),
                sorted_events.end(),
                [](const StockEvent& lhs, const StockEvent& rhs)
                {
                    return lhs.date > rhs.date;
                }
            );
            if (sorted_events.size() > 5)
            {
                sorted_events.resize(5);
            }

            out << "[";
            for (size_t i = 0; i < sorted_events.size(); ++i)
            {
                if (i > 0)
                {
                    out << ",";
                }

                const auto& event = sorted_events[i];
                out << "{"
                    << "\"date\":" << static_cast<long long>(event.date) << ","
                    << "\"type\":" << jsonString(stockEventTypeToString(event.type)) << ","
                    << "\"shares\":" << jsonNumber(event.shares) << ","
                    << "\"price_per_share\":" << jsonNumber(event.price_per_share) << ","
                    << "\"cash_amount\":" << jsonNumber(event.cash_amount) << ","
                    << "\"notes\":" << jsonString(event.notes)
                    << "}";
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
        out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
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

        if (shares <= 0.0)
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

        const double next_capital = portfolio.getAvailableCapital() + amount;
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
                JsonValue body;
                HttpResponse parse_error = parseJsonBodyObject(request, body);
                if (parse_error.status != 200)
                {
                    return parse_error;
                }

                const std::string action = segments[4];
                const std::string notes = getObjectString(body, "notes").value_or("");
                const time_t date = static_cast<time_t>(
                    std::llround(getObjectNumber(body, "date").value_or(static_cast<double>(std::time(nullptr))))
                );

                if (action == "buy")
                {
                    auto ticker = getObjectString(body, "ticker");
                    auto shares = getObjectNumber(body, "shares");
                    auto price = getObjectNumber(body, "price_per_share");
                    if (!ticker.has_value() || !shares.has_value() || !price.has_value())
                    {
                        return makeJsonResponse(400, makeErrorBody("buy requires ticker, shares, price_per_share"));
                    }
                    if (price.value() < 0.0)
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
                    if (price.value() < 0.0)
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
                    if (amount.value() < 0.0)
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
                    if (!amount.has_value() || amount.value() < 0.0)
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
                    if (!amount.has_value() || amount.value() < 0.0)
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
                    if (!amount.has_value() || amount.value() < 0.0)
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
            if (!MarketDataSync::syncAllPortfolios(sync_manager, sync_config))
            {
                std::cerr << "Startup market-data sync completed with errors" << std::endl;
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
                response = routeRequest(request.value(), manager);
            }
        }

        const std::string payload = makeHttpResponseText(response);
        sendAll(client_fd, payload);
        close(client_fd);
    }

    close(server_fd);
    return true;
}

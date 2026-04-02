#ifndef PORTFOLIO_DATA_HPP
#define PORTFOLIO_DATA_HPP

#include <string>
#include <vector>
#include <ctime>
#include <cstdint>

// Portfolio type enumeration
enum class PortfolioType : uint8_t
{
    BROKERAGE = 0,
    ROTH_IRA = 1,
    TRADITIONAL_IRA = 2
};

// Daily portfolio value record
struct DailyPortfolioValue
{
    time_t date;         // Unix timestamp for market close date
    double value;        // Portfolio value at market close
    time_t last_updated; // Unix timestamp when this record was last modified

    DailyPortfolioValue() : date(0), value(0.0), last_updated(0) {}
    DailyPortfolioValue(time_t d, double v, time_t lu) : date(d), value(v), last_updated(lu) {}
};

// Transaction record types - impacts on available cash
enum class TransactionType : uint8_t
{
    DEPOSIT = 0,           // Add cash to portfolio
    WITHDRAWAL = 1,        // Remove cash from portfolio
    BUY_STOCK = 2,         // Purchase stock (reduces cash)
    SELL_STOCK = 3,        // Sell stock (increases cash)
    DIVIDEND = 4           // Dividend payment (increases cash)
};

// Stock-specific event types
enum class StockEventType : uint8_t
{
    BUY = 0,
    SELL = 1,
    DIVIDEND = 2
};

struct Transaction
{
    time_t date;              // Unix timestamp of transaction
    double amount;            // Cash impact in dollars (+/-) or total sale/buy value
    TransactionType type;     // Transaction type
    std::string stock_symbol; // Stock ticker symbol (for BUY/SELL/DIVIDEND)
    double shares;            // Number of shares (for BUY/SELL transactions)
    std::string notes;        // Optional notes about transaction

    Transaction() 
        : date(0), amount(0.0), type(TransactionType::DEPOSIT), shares(0.0) {}
    
    Transaction(time_t d, double amt, TransactionType t, const std::string& n = "") 
        : date(d), amount(amt), type(t), shares(0.0), notes(n) {}
    
    // Constructor for stock transactions
    Transaction(time_t d, double amt, TransactionType t, 
                const std::string& symbol, double num_shares, const std::string& n = "")
        : date(d), amount(amt), type(t), stock_symbol(symbol), shares(num_shares), notes(n) {}
};

struct StockEvent
{
    time_t date;                  // Unix timestamp of event
    StockEventType type;          // BUY/SELL/DIVIDEND
    double shares;                // Shares transacted (or shares held for dividend records)
    double price_per_share;       // Price per share for BUY/SELL
    double cash_amount;           // Signed cash amount (-buy, +sell, +dividend)
    std::string notes;            // Optional notes

    StockEvent()
        : date(0), type(StockEventType::BUY), shares(0.0), price_per_share(0.0), cash_amount(0.0) {}

    StockEvent(time_t d, StockEventType t, double sh, double pps, double cash, const std::string& n = "")
        : date(d), type(t), shares(sh), price_per_share(pps), cash_amount(cash), notes(n) {}
};

struct DailyStockPrice
{
    time_t date;         // Unix timestamp for market close date
    double close_price;  // Stock close price at market close
    time_t last_updated; // Unix timestamp when this record was last modified

    DailyStockPrice() : date(0), close_price(0.0), last_updated(0) {}
    DailyStockPrice(time_t d, double cp, time_t lu) : date(d), close_price(cp), last_updated(lu) {}
};

class StockData
{
private:
    uint32_t version;
    std::string company_name;
    std::string ticker;
    double shares_owned;
    double average_purchase_price;
    time_t last_updated;
    std::vector<StockEvent> events;
    std::vector<DailyStockPrice> price_history;

public:
    StockData();
    StockData(const std::string& company, const std::string& ticker_symbol);

    uint32_t getVersion() const { return version; }
    const std::string& getCompanyName() const { return company_name; }
    const std::string& getTicker() const { return ticker; }
    double getSharesOwned() const { return shares_owned; }
    double getAveragePurchasePrice() const { return average_purchase_price; }
    time_t getLastUpdated() const { return last_updated; }
    const std::vector<StockEvent>& getEvents() const { return events; }
    const std::vector<DailyStockPrice>& getPriceHistory() const { return price_history; }

    void setCompanyName(const std::string& company) { company_name = company; }
    void setTicker(const std::string& ticker_symbol) { ticker = ticker_symbol; }

    bool recordBuy(time_t date, double shares, double price_per_share, const std::string& notes = "");
    bool recordSell(time_t date, double shares, double price_per_share, const std::string& notes = "");
    bool recordDividend(time_t date, double cash_amount, double shares_at_record = 0.0, const std::string& notes = "");
    bool addEvent(time_t date, StockEventType type, double shares, double price_per_share,
                  double cash_amount, const std::string& notes = "");
    bool rebuildFromEvents(const std::vector<StockEvent>& ordered_events);

    void addDailyClosePrice(time_t date, double close_price);
    bool updateDailyClosePrice(time_t date, double close_price, time_t updated_at = std::time(nullptr));

    bool saveToFile(const std::string& filepath) const;
    bool loadFromFile(const std::string& filepath);
};

// Main Portfolio class
class Portfolio
{
private:
    uint32_t version;
    PortfolioType type;
    double available_capital;
    std::vector<DailyPortfolioValue> daily_values;
    std::vector<Transaction> transactions;

public:
    // Constructor
    Portfolio();
    Portfolio(PortfolioType ptype, double initial_capital);

    // Getters
    uint32_t getVersion() const { return version; }
    PortfolioType getType() const { return type; }
    double getAvailableCapital() const { return available_capital; }
    const std::vector<DailyPortfolioValue>& getDailyValues() const { return daily_values; }
    const std::vector<Transaction>& getTransactions() const { return transactions; }

    // Setters
    void setAvailableCapital(double capital) { available_capital = capital; }
    void addDailyValue(time_t date, double value);
    bool updateDailyValue(time_t date, double value, time_t updated_at = std::time(nullptr));
    void setDailyValues(const std::vector<DailyPortfolioValue>& values);
    void clearDailyValues();
    void addTransaction(time_t date, double amount, TransactionType type, const std::string& notes = "");
    void addTransaction(time_t date, double amount, TransactionType type, 
                       const std::string& symbol, double shares, const std::string& notes = "");

    // File I/O
    bool saveToFile(const std::string& filepath) const;
    bool loadFromFile(const std::string& filepath);

    // Utility
    double getCurrentPortfolioValue() const;
    double getCapitalMovement(time_t start_date, time_t end_date) const;
};

// Portfolio Manager for managing multiple portfolios
class PortfolioManager
{
private:
    std::string data_directory;
    std::vector<std::string> portfolio_names;

public:
    PortfolioManager(const std::string& data_dir = "data");

    // Portfolio management
    bool createPortfolio(const std::string& name, PortfolioType type, double initial_capital);
    bool loadPortfolio(const std::string& name, Portfolio& portfolio);
    bool savePortfolio(const std::string& name, const Portfolio& portfolio);
    bool deletePortfolio(const std::string& name);

    // Stock data management per portfolio
    bool saveStockData(const std::string& portfolio_name, const StockData& stock_data);
    bool loadStockData(const std::string& portfolio_name, const std::string& ticker, StockData& stock_data);
    bool deleteStock(const std::string& portfolio_name, const std::string& ticker);
    std::vector<std::string> listStocks(const std::string& portfolio_name) const;
    
    // Discovery
    bool scanPortfolios();
    const std::vector<std::string>& getPortfolioNames() const { return portfolio_names; }
    
    // Utility
    std::string getPortfolioPath(const std::string& name) const;
    std::string getPortfolioFilePath(const std::string& name) const;
    std::string getStocksDirectoryPath(const std::string& portfolio_name) const;
    std::string getStockFilePath(const std::string& portfolio_name, const std::string& ticker) const;
};

#endif

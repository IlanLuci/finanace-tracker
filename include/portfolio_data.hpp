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
    
    // Discovery
    bool scanPortfolios();
    const std::vector<std::string>& getPortfolioNames() const { return portfolio_names; }
    
    // Utility
    std::string getPortfolioPath(const std::string& name) const;
    std::string getPortfolioFilePath(const std::string& name) const;
};

#endif

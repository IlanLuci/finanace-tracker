#include "portfolio_data.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fs = std::filesystem;
namespace
{
    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 2;
    constexpr uint32_t OLDEST_SUPPORTED_FILE_VERSION = 1;
    constexpr time_t SECONDS_PER_DAY = 86400;
    constexpr uint32_t MAX_DAILY_VALUES = 100000;
    constexpr uint32_t MAX_TRANSACTIONS = 1000000;
    constexpr uint16_t MAX_SYMBOL_LENGTH = 16;
    constexpr uint16_t MAX_NOTES_LENGTH = 4096;

    long long dayBucket(time_t ts)
    {
        return static_cast<long long>(ts) / static_cast<long long>(SECONDS_PER_DAY);
    }

    double normalizedCashAmount(double amount)
    {
        return std::abs(amount);
    }

    bool isValidPortfolioType(uint8_t type_byte)
    {
        return type_byte <= static_cast<uint8_t>(PortfolioType::TRADITIONAL_IRA);
    }

    bool isValidTransactionType(uint8_t type_byte)
    {
        return type_byte <= static_cast<uint8_t>(TransactionType::DIVIDEND);
    }

    bool readExact(std::ifstream& file, char* buffer, std::streamsize bytes)
    {
        file.read(buffer, bytes);
        return file.good();
    }
}

// ==================== Portfolio Implementation ====================

Portfolio::Portfolio() 
    : version(CURRENT_PORTFOLIO_FILE_VERSION), type(PortfolioType::BROKERAGE), available_capital(0.0)
{
}

Portfolio::Portfolio(PortfolioType ptype, double initial_capital)
    : version(CURRENT_PORTFOLIO_FILE_VERSION), type(ptype), available_capital(initial_capital)
{
}

void Portfolio::addDailyValue(time_t date, double value)
{
    const time_t now = std::time(nullptr);
    const long long target_day = dayBucket(date);

    // Keep one value per market day; update existing entry if it already exists.
    for (auto& daily_value : daily_values)
    {
        if (dayBucket(daily_value.date) == target_day)
        {
            daily_value.date = date;
            daily_value.value = value;
            daily_value.last_updated = now;
            return;
        }
    }

    daily_values.emplace_back(date, value, now);
}

bool Portfolio::updateDailyValue(time_t date, double value, time_t updated_at)
{
    const long long target_day = dayBucket(date);
    bool updated = false;

    for (auto& daily_value : daily_values)
    {
        if (dayBucket(daily_value.date) == target_day)
        {
            daily_value.date = date;
            daily_value.value = value;
            daily_value.last_updated = updated_at;
            updated = true;
        }
    }

    return updated;
}

void Portfolio::addTransaction(time_t date, double amount, TransactionType type, const std::string& notes)
{
    transactions.emplace_back(date, amount, type, notes);
}

void Portfolio::addTransaction(time_t date, double amount, TransactionType type, 
                               const std::string& symbol, double shares, const std::string& notes)
{
    transactions.emplace_back(date, amount, type, symbol, shares, notes);
}

double Portfolio::getCurrentPortfolioValue() const
{
    if (daily_values.empty())
    {
        return 0.0;
    }

    const auto latest_it = std::max_element(
        daily_values.begin(),
        daily_values.end(),
        [](const DailyPortfolioValue& a, const DailyPortfolioValue& b)
        {
            return a.date < b.date;
        }
    );

    return latest_it->value;
}

double Portfolio::getCapitalMovement(time_t start_date, time_t end_date) const
{
    double movement = 0.0;

    for (const auto& transaction : transactions)
    {
        if (transaction.date >= start_date && transaction.date <= end_date)
        {
            switch (transaction.type)
            {
                case TransactionType::DEPOSIT:
                case TransactionType::SELL_STOCK:
                case TransactionType::DIVIDEND:
                    movement += normalizedCashAmount(transaction.amount);
                    break;
                case TransactionType::WITHDRAWAL:
                case TransactionType::BUY_STOCK:
                    movement -= normalizedCashAmount(transaction.amount);
                    break;
            }
        }
    }

    return movement;
}

/*
 * Binary file format for portfolio data:
 * Header (12 bytes):
 *   - version: uint32_t (4 bytes)
 *   - type: uint8_t (1 byte)
 *   - reserved: 3 bytes (for alignment/future use)
 * Available capital: double (8 bytes)
 * Daily values:
 *   - count: uint32_t (4 bytes)
 *   - v1 array: [date (time_t, 8 bytes) + value (double, 8 bytes)] * count
 *   - v2+ array: [date (time_t, 8 bytes) + value (double, 8 bytes) + last_updated (time_t, 8 bytes)] * count
 * Transactions:
 *   - count: uint32_t (4 bytes)
 *   - array: [date (time_t, 8 bytes) + amount (double, 8 bytes) + 
 *            type (uint8_t, 1 byte) + symbol_length (uint16_t, 2 bytes) + 
 *            symbol (variable) + shares (double, 8 bytes) +
 *            notes_length (uint16_t, 2 bytes) + notes (variable)]
 */

bool Portfolio::saveToFile(const std::string& filepath) const
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file " << filepath << " for writing" << std::endl;
        return false;
    }

    // Write header
    uint32_t file_version = CURRENT_PORTFOLIO_FILE_VERSION;
    file.write(reinterpret_cast<const char*>(&file_version), sizeof(uint32_t));
    uint8_t type_byte = static_cast<uint8_t>(type);
    file.write(reinterpret_cast<const char*>(&type_byte), sizeof(uint8_t));
    uint8_t reserved[3] = {0, 0, 0};
    file.write(reinterpret_cast<const char*>(reserved), 3);

    // Write available capital
    file.write(reinterpret_cast<const char*>(&available_capital), sizeof(double));

    // Write daily values
    if (daily_values.size() > std::numeric_limits<uint32_t>::max())
    {
        std::cerr << "Error: Too many daily values to serialize" << std::endl;
        return false;
    }
    uint32_t daily_count = static_cast<uint32_t>(daily_values.size());
    file.write(reinterpret_cast<const char*>(&daily_count), sizeof(uint32_t));
    for (const auto& dv : daily_values)
    {
        file.write(reinterpret_cast<const char*>(&dv.date), sizeof(time_t));
        file.write(reinterpret_cast<const char*>(&dv.value), sizeof(double));
        file.write(reinterpret_cast<const char*>(&dv.last_updated), sizeof(time_t));
    }

    // Write transactions
    if (transactions.size() > std::numeric_limits<uint32_t>::max())
    {
        std::cerr << "Error: Too many transactions to serialize" << std::endl;
        return false;
    }
    uint32_t tx_count = static_cast<uint32_t>(transactions.size());
    file.write(reinterpret_cast<const char*>(&tx_count), sizeof(uint32_t));
    for (const auto& tx : transactions)
    {
        file.write(reinterpret_cast<const char*>(&tx.date), sizeof(time_t));
        file.write(reinterpret_cast<const char*>(&tx.amount), sizeof(double));
        uint8_t type_byte = static_cast<uint8_t>(tx.type);
        file.write(reinterpret_cast<const char*>(&type_byte), sizeof(uint8_t));
        
        // Write stock symbol
        if (tx.stock_symbol.length() > std::numeric_limits<uint16_t>::max() ||
            tx.stock_symbol.length() > MAX_SYMBOL_LENGTH)
        {
            std::cerr << "Error: Stock symbol too long for serialization" << std::endl;
            return false;
        }
        uint16_t symbol_length = static_cast<uint16_t>(tx.stock_symbol.length());
        file.write(reinterpret_cast<const char*>(&symbol_length), sizeof(uint16_t));
        if (symbol_length > 0)
        {
            file.write(tx.stock_symbol.c_str(), symbol_length);
        }
        
        // Write shares
        file.write(reinterpret_cast<const char*>(&tx.shares), sizeof(double));
        
        // Write notes
        if (tx.notes.length() > std::numeric_limits<uint16_t>::max() ||
            tx.notes.length() > MAX_NOTES_LENGTH)
        {
            std::cerr << "Error: Notes too long for serialization" << std::endl;
            return false;
        }
        uint16_t notes_length = static_cast<uint16_t>(tx.notes.length());
        file.write(reinterpret_cast<const char*>(&notes_length), sizeof(uint16_t));
        if (notes_length > 0)
        {
            file.write(tx.notes.c_str(), notes_length);
        }
    }

    file.close();
    return file.good();
}

bool Portfolio::loadFromFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file " << filepath << " for reading" << std::endl;
        return false;
    }

    // Read and validate header first into temporaries.
    uint32_t loaded_version;
    if (!readExact(file, reinterpret_cast<char*>(&loaded_version), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt or truncated file header (version)" << std::endl;
        return false;
    }
    if (loaded_version < OLDEST_SUPPORTED_FILE_VERSION || loaded_version > CURRENT_PORTFOLIO_FILE_VERSION)
    {
        std::cerr << "Error: Unsupported portfolio file version " << loaded_version << std::endl;
        return false;
    }

    uint8_t portfolio_type_byte;
    if (!readExact(file, reinterpret_cast<char*>(&portfolio_type_byte), sizeof(uint8_t)))
    {
        std::cerr << "Error: Corrupt or truncated file header (portfolio type)" << std::endl;
        return false;
    }
    if (!isValidPortfolioType(portfolio_type_byte))
    {
        std::cerr << "Error: Invalid portfolio type in file" << std::endl;
        return false;
    }

    char reserved[3];
    if (!readExact(file, reserved, 3)) // Skip reserved bytes
    {
        std::cerr << "Error: Corrupt or truncated file header (reserved bytes)" << std::endl;
        return false;
    }

    // Read available capital
    double loaded_available_capital;
    if (!readExact(file, reinterpret_cast<char*>(&loaded_available_capital), sizeof(double)))
    {
        std::cerr << "Error: Corrupt or truncated file (available capital)" << std::endl;
        return false;
    }

    // Read daily values
    uint32_t daily_count;
    if (!readExact(file, reinterpret_cast<char*>(&daily_count), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt or truncated file (daily count)" << std::endl;
        return false;
    }
    if (daily_count > MAX_DAILY_VALUES)
    {
        std::cerr << "Error: Daily count exceeds supported limit" << std::endl;
        return false;
    }

    std::vector<DailyPortfolioValue> loaded_daily_values;
    loaded_daily_values.reserve(daily_count);
    for (uint32_t i = 0; i < daily_count; ++i)
    {
        time_t date;
        double value;
        time_t last_updated;
        if (!readExact(file, reinterpret_cast<char*>(&date), sizeof(time_t)) ||
            !readExact(file, reinterpret_cast<char*>(&value), sizeof(double)))
        {
            std::cerr << "Error: Corrupt or truncated file (daily value record)" << std::endl;
            return false;
        }

        if (loaded_version >= 2)
        {
            if (!readExact(file, reinterpret_cast<char*>(&last_updated), sizeof(time_t)))
            {
                std::cerr << "Error: Corrupt or truncated file (daily value last_updated)" << std::endl;
                return false;
            }
        }
        else
        {
            // For legacy files that did not store last_updated, use market-close date.
            last_updated = date;
        }

        loaded_daily_values.emplace_back(date, value, last_updated);
    }

    // Read transactions
    uint32_t tx_count;
    if (!readExact(file, reinterpret_cast<char*>(&tx_count), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt or truncated file (transaction count)" << std::endl;
        return false;
    }
    if (tx_count > MAX_TRANSACTIONS)
    {
        std::cerr << "Error: Transaction count exceeds supported limit" << std::endl;
        return false;
    }

    std::vector<Transaction> loaded_transactions;
    loaded_transactions.reserve(tx_count);
    for (uint32_t i = 0; i < tx_count; ++i)
    {
        time_t date;
        double amount;
        uint8_t type_byte;
        uint16_t symbol_length;
        double shares;
        uint16_t notes_length;
        
        if (!readExact(file, reinterpret_cast<char*>(&date), sizeof(time_t)) ||
            !readExact(file, reinterpret_cast<char*>(&amount), sizeof(double)) ||
            !readExact(file, reinterpret_cast<char*>(&type_byte), sizeof(uint8_t)))
        {
            std::cerr << "Error: Corrupt or truncated file (transaction header)" << std::endl;
            return false;
        }
        if (!isValidTransactionType(type_byte))
        {
            std::cerr << "Error: Invalid transaction type in file" << std::endl;
            return false;
        }
        
        // Read stock symbol
        if (!readExact(file, reinterpret_cast<char*>(&symbol_length), sizeof(uint16_t)))
        {
            std::cerr << "Error: Corrupt or truncated file (symbol length)" << std::endl;
            return false;
        }
        if (symbol_length > MAX_SYMBOL_LENGTH)
        {
            std::cerr << "Error: Symbol length exceeds supported limit" << std::endl;
            return false;
        }
        std::string symbol;
        if (symbol_length > 0)
        {
            symbol.resize(symbol_length);
            if (!readExact(file, &symbol[0], symbol_length))
            {
                std::cerr << "Error: Corrupt or truncated file (symbol)" << std::endl;
                return false;
            }
        }
        
        // Read shares
        if (!readExact(file, reinterpret_cast<char*>(&shares), sizeof(double)))
        {
            std::cerr << "Error: Corrupt or truncated file (shares)" << std::endl;
            return false;
        }
        
        // Read notes
        if (!readExact(file, reinterpret_cast<char*>(&notes_length), sizeof(uint16_t)))
        {
            std::cerr << "Error: Corrupt or truncated file (notes length)" << std::endl;
            return false;
        }
        if (notes_length > MAX_NOTES_LENGTH)
        {
            std::cerr << "Error: Notes length exceeds supported limit" << std::endl;
            return false;
        }
        std::string notes;
        if (notes_length > 0)
        {
            notes.resize(notes_length);
            if (!readExact(file, &notes[0], notes_length))
            {
                std::cerr << "Error: Corrupt or truncated file (notes)" << std::endl;
                return false;
            }
        }
        
        TransactionType tx_type = static_cast<TransactionType>(type_byte);
        loaded_transactions.emplace_back(date, amount, tx_type, symbol, shares, notes);
    }

    version = loaded_version;
    type = static_cast<PortfolioType>(portfolio_type_byte);
    available_capital = loaded_available_capital;
    daily_values = std::move(loaded_daily_values);
    transactions = std::move(loaded_transactions);

    file.close();
    return true;
}

// ==================== PortfolioManager Implementation ====================

PortfolioManager::PortfolioManager(const std::string& data_dir)
    : data_directory(data_dir)
{
    // Create data directory if it doesn't exist
    if (!fs::exists(data_directory))
    {
        try
        {
            fs::create_directories(data_directory);
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error creating data directory: " << e.what() << std::endl;
        }
    }
}

bool PortfolioManager::createPortfolio(const std::string& name, PortfolioType type, double initial_capital)
{
    std::string portfolio_dir = getPortfolioPath(name);
    std::string portfolio_file = getPortfolioFilePath(name);
    
    try
    {
        // Create portfolio directory
        if (!fs::exists(portfolio_dir))
        {
            fs::create_directories(portfolio_dir);
        }

        // Prevent accidental overwrite of an existing portfolio file.
        if (fs::exists(portfolio_file))
        {
            std::cerr << "Error creating portfolio: portfolio already exists at " << portfolio_file << std::endl;
            return false;
        }
        
        // Create and save portfolio file
        Portfolio portfolio(type, initial_capital);
        return savePortfolio(name, portfolio);
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error creating portfolio: " << e.what() << std::endl;
        return false;
    }
}

bool PortfolioManager::loadPortfolio(const std::string& name, Portfolio& portfolio)
{
    std::string filepath = getPortfolioFilePath(name);
    return portfolio.loadFromFile(filepath);
}

bool PortfolioManager::savePortfolio(const std::string& name, const Portfolio& portfolio)
{
    std::string filepath = getPortfolioFilePath(name);
    return portfolio.saveToFile(filepath);
}

bool PortfolioManager::deletePortfolio(const std::string& name)
{
    std::string portfolio_dir = getPortfolioPath(name);
    
    try
    {
        if (fs::exists(portfolio_dir))
        {
            fs::remove_all(portfolio_dir);
            return true;
        }
        return false;
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error deleting portfolio: " << e.what() << std::endl;
        return false;
    }
}

bool PortfolioManager::scanPortfolios()
{
    portfolio_names.clear();
    
    try
    {
        if (!fs::exists(data_directory))
            return false;

        for (const auto& entry : fs::directory_iterator(data_directory))
        {
            if (entry.is_directory())
            {
                portfolio_names.push_back(entry.path().filename().string());
            }
        }
        
        std::sort(portfolio_names.begin(), portfolio_names.end());
        return true;
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error scanning portfolios: " << e.what() << std::endl;
        return false;
    }
}

std::string PortfolioManager::getPortfolioPath(const std::string& name) const
{
    return data_directory + "/" + name;
}

std::string PortfolioManager::getPortfolioFilePath(const std::string& name) const
{
    return getPortfolioPath(name) + "/portfolio.dat";
}

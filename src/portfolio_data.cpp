#include "portfolio_data.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;
namespace
{
    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 2;
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
    daily_values.emplace_back(date, value, std::time(nullptr));
}

bool Portfolio::updateDailyValue(time_t date, double value, time_t updated_at)
{
    for (auto& daily_value : daily_values)
    {
        if (daily_value.date == date)
        {
            daily_value.value = value;
            daily_value.last_updated = updated_at;
            return true;
        }
    }
    return false;
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
        return 0.0;
    return daily_values.back().value;
}

double Portfolio::getCapitalMovement(time_t start_date, time_t end_date) const
{
    double movement = 0.0;
    for (const auto& transaction : transactions)
    {
        if (transaction.date >= start_date && transaction.date <= end_date)
        {
            if (transaction.type == TransactionType::DEPOSIT)
                movement += transaction.amount;
            else
                movement -= transaction.amount;
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
    uint32_t daily_count = daily_values.size();
    file.write(reinterpret_cast<const char*>(&daily_count), sizeof(uint32_t));
    for (const auto& dv : daily_values)
    {
        file.write(reinterpret_cast<const char*>(&dv.date), sizeof(time_t));
        file.write(reinterpret_cast<const char*>(&dv.value), sizeof(double));
        file.write(reinterpret_cast<const char*>(&dv.last_updated), sizeof(time_t));
    }

    // Write transactions
    uint32_t tx_count = transactions.size();
    file.write(reinterpret_cast<const char*>(&tx_count), sizeof(uint32_t));
    for (const auto& tx : transactions)
    {
        file.write(reinterpret_cast<const char*>(&tx.date), sizeof(time_t));
        file.write(reinterpret_cast<const char*>(&tx.amount), sizeof(double));
        uint8_t type_byte = static_cast<uint8_t>(tx.type);
        file.write(reinterpret_cast<const char*>(&type_byte), sizeof(uint8_t));
        
        // Write stock symbol
        uint16_t symbol_length = tx.stock_symbol.length();
        file.write(reinterpret_cast<const char*>(&symbol_length), sizeof(uint16_t));
        if (symbol_length > 0)
        {
            file.write(tx.stock_symbol.c_str(), symbol_length);
        }
        
        // Write shares
        file.write(reinterpret_cast<const char*>(&tx.shares), sizeof(double));
        
        // Write notes
        uint16_t notes_length = tx.notes.length();
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

    // Read header
    file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    uint8_t type_byte;
    file.read(reinterpret_cast<char*>(&type_byte), sizeof(uint8_t));
    type = static_cast<PortfolioType>(type_byte);
    char reserved[3];
    file.read(reserved, 3); // Skip reserved bytes

    // Read available capital
    file.read(reinterpret_cast<char*>(&available_capital), sizeof(double));

    // Read daily values
    uint32_t daily_count;
    file.read(reinterpret_cast<char*>(&daily_count), sizeof(uint32_t));
    daily_values.clear();
    for (uint32_t i = 0; i < daily_count; ++i)
    {
        time_t date;
        double value;
        time_t last_updated;
        file.read(reinterpret_cast<char*>(&date), sizeof(time_t));
        file.read(reinterpret_cast<char*>(&value), sizeof(double));

        if (version >= 2)
        {
            file.read(reinterpret_cast<char*>(&last_updated), sizeof(time_t));
        }
        else
        {
            // For legacy files that did not store last_updated, use market-close date.
            last_updated = date;
        }

        daily_values.emplace_back(date, value, last_updated);
    }

    // Read transactions
    uint32_t tx_count;
    file.read(reinterpret_cast<char*>(&tx_count), sizeof(uint32_t));
    transactions.clear();
    for (uint32_t i = 0; i < tx_count; ++i)
    {
        time_t date;
        double amount;
        uint8_t type_byte;
        uint16_t symbol_length;
        double shares;
        uint16_t notes_length;
        
        file.read(reinterpret_cast<char*>(&date), sizeof(time_t));
        file.read(reinterpret_cast<char*>(&amount), sizeof(double));
        file.read(reinterpret_cast<char*>(&type_byte), sizeof(uint8_t));
        
        // Read stock symbol
        file.read(reinterpret_cast<char*>(&symbol_length), sizeof(uint16_t));
        std::string symbol;
        if (symbol_length > 0)
        {
            symbol.resize(symbol_length);
            file.read(&symbol[0], symbol_length);
        }
        
        // Read shares
        file.read(reinterpret_cast<char*>(&shares), sizeof(double));
        
        // Read notes
        file.read(reinterpret_cast<char*>(&notes_length), sizeof(uint16_t));
        std::string notes;
        if (notes_length > 0)
        {
            notes.resize(notes_length);
            file.read(&notes[0], notes_length);
        }
        
        TransactionType tx_type = static_cast<TransactionType>(type_byte);
        transactions.emplace_back(date, amount, tx_type, symbol, shares, notes);
    }

    file.close();
    return file.good();
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
    
    try
    {
        // Create portfolio directory
        if (!fs::exists(portfolio_dir))
        {
            fs::create_directories(portfolio_dir);
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

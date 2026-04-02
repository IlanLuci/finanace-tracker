#include "portfolio_data.hpp"
#include "file_utils.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
namespace
{
    bool isStockTransactionType(TransactionType tx_type);
    StockEventType toStockEventType(TransactionType tx_type);

    constexpr uint32_t CURRENT_PORTFOLIO_FILE_VERSION = 2;
    constexpr uint32_t OLDEST_SUPPORTED_FILE_VERSION = 1;
    constexpr uint32_t CURRENT_STOCK_FILE_VERSION = 1;
    constexpr uint32_t OLDEST_SUPPORTED_STOCK_FILE_VERSION = 1;
    constexpr time_t SECONDS_PER_DAY = 86400;
    constexpr uint32_t MAX_DAILY_VALUES = 100000;
    constexpr uint32_t MAX_TRANSACTIONS = 1000000;
    constexpr uint32_t MAX_STOCK_EVENTS = 1000000;
    constexpr uint32_t MAX_STOCK_PRICE_POINTS = 1000000;
    constexpr uint16_t MAX_SYMBOL_LENGTH = 16;
    constexpr uint16_t MAX_COMPANY_NAME_LENGTH = 256;
    constexpr uint16_t MAX_NOTES_LENGTH = 4096;
    constexpr const char* TEMP_SYNC_SUFFIX = ".tmp.sync";
    constexpr const char* BACKUP_SYNC_SUFFIX = ".bak.sync";

    bool hasSuffix(const std::string& value, const std::string& suffix)
    {
        if (value.size() < suffix.size())
        {
            return false;
        }

        return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string stripSuffix(const std::string& value, const std::string& suffix)
    {
        if (!hasSuffix(value, suffix))
        {
            return value;
        }

        return value.substr(0, value.size() - suffix.size());
    }

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

    bool isValidStockEventType(uint8_t type_byte)
    {
        return type_byte <= static_cast<uint8_t>(StockEventType::DIVIDEND);
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

    bool readExact(std::ifstream& file, char* buffer, std::streamsize bytes)
    {
        file.read(buffer, bytes);
        return file.good();
    }

    bool nearlyEqual(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) <= eps;
    }

    bool rebuildPositionFromEvents(const std::vector<StockEvent>& ordered_events,
                                   double& rebuilt_shares,
                                   double& rebuilt_avg_purchase_price)
    {
        std::vector<StockEvent> sorted_events = ordered_events;
        std::stable_sort(
            sorted_events.begin(),
            sorted_events.end(),
            [](const StockEvent& a, const StockEvent& b)
            {
                return a.date < b.date;
            }
        );

        rebuilt_shares = 0.0;
        rebuilt_avg_purchase_price = 0.0;

        for (const auto& event : sorted_events)
        {
            if (event.shares < 0.0 || event.price_per_share < 0.0)
            {
                return false;
            }

            if (event.type == StockEventType::BUY)
            {
                if (event.shares <= 0.0)
                {
                    return false;
                }

                const double existing_cost = rebuilt_shares * rebuilt_avg_purchase_price;
                const double buy_cost = event.shares * event.price_per_share;
                rebuilt_shares += event.shares;
                rebuilt_avg_purchase_price = (existing_cost + buy_cost) / rebuilt_shares;
            }
            else if (event.type == StockEventType::SELL)
            {
                if (event.shares <= 0.0 || event.shares > rebuilt_shares)
                {
                    return false;
                }

                rebuilt_shares -= event.shares;
                if (rebuilt_shares == 0.0)
                {
                    rebuilt_avg_purchase_price = 0.0;
                }
            }
        }

        return true;
    }

    std::vector<StockEvent> sortedEvents(std::vector<StockEvent> events)
    {
        std::stable_sort(
            events.begin(),
            events.end(),
            [](const StockEvent& a, const StockEvent& b)
            {
                return a.date < b.date;
            }
        );
        return events;
    }

    bool stockEventsEquivalent(const std::vector<StockEvent>& lhs, const std::vector<StockEvent>& rhs)
    {
        const std::vector<StockEvent> left_sorted = sortedEvents(lhs);
        const std::vector<StockEvent> right_sorted = sortedEvents(rhs);

        if (left_sorted.size() != right_sorted.size())
        {
            return false;
        }

        for (size_t i = 0; i < left_sorted.size(); ++i)
        {
            const StockEvent& a = left_sorted[i];
            const StockEvent& b = right_sorted[i];

            if (a.date != b.date ||
                a.type != b.type ||
                !nearlyEqual(a.shares, b.shares) ||
                !nearlyEqual(a.price_per_share, b.price_per_share) ||
                !nearlyEqual(a.cash_amount, b.cash_amount) ||
                a.notes != b.notes)
            {
                return false;
            }
        }

        return true;
    }

    std::vector<StockEvent> buildDerivedEventsForTicker(const Portfolio& portfolio, const std::string& normalized_ticker)
    {
        std::vector<StockEvent> derived_events;

        for (const auto& tx : portfolio.getTransactions())
        {
            if (!isStockTransactionType(tx.type))
            {
                continue;
            }

            const std::string ticker = normalizeTicker(tx.stock_symbol);
            if (ticker != normalized_ticker)
            {
                continue;
            }

            double price_per_share = 0.0;
            if (tx.shares > 0.0 && tx.type != TransactionType::DIVIDEND)
            {
                price_per_share = std::abs(tx.amount) / tx.shares;
            }

            derived_events.emplace_back(
                tx.date,
                toStockEventType(tx.type),
                tx.shares,
                price_per_share,
                tx.amount,
                tx.notes
            );
        }

        return derived_events;
    }

    std::string makeTempPath(const std::string& final_path)
    {
        return final_path + ".tmp.sync";
    }

    std::string makeBackupPath(const std::string& final_path)
    {
        return final_path + ".bak.sync";
    }

    class ScopedPortfolioLock
    {
    private:
        std::string lock_directory;
        bool acquired;

    public:
        explicit ScopedPortfolioLock(const std::string& portfolio_dir)
            : lock_directory(portfolio_dir + "/.save.lock"), acquired(false)
        {
            try
            {
                acquired = fs::create_directory(lock_directory);
                if (acquired)
                {
                    std::ofstream marker(lock_directory + "/owner.txt", std::ios::trunc);
                    if (marker.is_open())
                    {
                        marker << std::time(nullptr) << '\n';
                    }
                }
            }
            catch (const fs::filesystem_error&)
            {
                acquired = false;
            }
        }

        ~ScopedPortfolioLock()
        {
            if (!acquired)
            {
                return;
            }

            try
            {
                fs::remove_all(lock_directory);
            }
            catch (const fs::filesystem_error&)
            {
            }
        }

        bool isAcquired() const
        {
            return acquired;
        }
    };

    void recoverSyncArtifacts(const std::string& data_directory)
    {
        if (!fs::exists(data_directory))
        {
            return;
        }

        std::vector<fs::path> tmp_files;
        std::vector<fs::path> backup_files;
        std::vector<fs::path> lock_directories;

        for (const auto& entry : fs::recursive_directory_iterator(data_directory))
        {
            const fs::path path = entry.path();
            const std::string path_string = path.string();

            if (entry.is_directory() && path.filename() == ".save.lock")
            {
                lock_directories.push_back(path);
                continue;
            }

            if (!entry.is_regular_file())
            {
                continue;
            }

            if (hasSuffix(path_string, TEMP_SYNC_SUFFIX))
            {
                tmp_files.push_back(path);
            }
            else if (hasSuffix(path_string, BACKUP_SYNC_SUFFIX))
            {
                backup_files.push_back(path);
            }
        }

        for (const auto& tmp_file : tmp_files)
        {
            const std::string tmp_path = tmp_file.string();
            const std::string final_path = stripSuffix(tmp_path, TEMP_SYNC_SUFFIX);
            const std::string backup_path = makeBackupPath(final_path);

            try
            {
                if (fs::exists(final_path))
                {
                    fs::remove(tmp_path);
                    continue;
                }

                if (fs::exists(backup_path))
                {
                    fs::rename(backup_path, final_path);
                    if (fs::exists(tmp_path))
                    {
                        fs::remove(tmp_path);
                    }
                    continue;
                }

                fs::rename(tmp_path, final_path);
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Error recovering temp sync file " << tmp_path << ": " << e.what() << std::endl;
            }
        }

        for (const auto& backup_file : backup_files)
        {
            const std::string backup_path = backup_file.string();
            if (!fs::exists(backup_path))
            {
                continue;
            }

            const std::string final_path = stripSuffix(backup_path, BACKUP_SYNC_SUFFIX);
            try
            {
                if (fs::exists(final_path))
                {
                    fs::remove(backup_path);
                }
                else
                {
                    fs::rename(backup_path, final_path);
                }
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Error recovering backup sync file " << backup_path << ": " << e.what() << std::endl;
            }
        }

        for (const auto& lock_dir : lock_directories)
        {
            try
            {
                if (fs::exists(lock_dir))
                {
                    fs::remove_all(lock_dir);
                }
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Error removing stale lock directory " << lock_dir.string() << ": " << e.what() << std::endl;
            }
        }
    }

    std::unordered_map<std::string, std::string> listStockFilesByTicker(const std::string& stocks_dir)
    {
        std::unordered_map<std::string, std::string> files;
        if (!fs::exists(stocks_dir))
        {
            return files;
        }

        for (const auto& entry : fs::directory_iterator(stocks_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const fs::path path = entry.path();
            if (path.extension() != ".dat")
            {
                continue;
            }

            files[path.stem().string()] = path.string();
        }

        return files;
    }

    bool isStockTransactionType(TransactionType tx_type)
    {
        return tx_type == TransactionType::BUY_STOCK ||
               tx_type == TransactionType::SELL_STOCK ||
               tx_type == TransactionType::DIVIDEND;
    }

    StockEventType toStockEventType(TransactionType tx_type)
    {
        switch (tx_type)
        {
            case TransactionType::BUY_STOCK:
                return StockEventType::BUY;
            case TransactionType::SELL_STOCK:
                return StockEventType::SELL;
            case TransactionType::DIVIDEND:
                return StockEventType::DIVIDEND;
            case TransactionType::DEPOSIT:
            case TransactionType::WITHDRAWAL:
                return StockEventType::BUY;
        }

        return StockEventType::BUY;
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

void Portfolio::setDailyValues(const std::vector<DailyPortfolioValue>& values)
{
    daily_values = values;
}

void Portfolio::clearDailyValues()
{
    daily_values.clear();
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

// ==================== StockData Implementation ====================

StockData::StockData()
    : version(CURRENT_STOCK_FILE_VERSION), shares_owned(0.0), average_purchase_price(0.0),
      last_updated(0)
{
}

StockData::StockData(const std::string& company, const std::string& ticker_symbol)
    : version(CURRENT_STOCK_FILE_VERSION), company_name(company),
      ticker(normalizeTicker(ticker_symbol)), shares_owned(0.0), average_purchase_price(0.0),
      last_updated(std::time(nullptr))
{
}

bool StockData::recordBuy(time_t date, double shares, double price_per_share, const std::string& notes)
{
    if (shares <= 0.0 || price_per_share < 0.0)
    {
        return false;
    }

    const double existing_cost = shares_owned * average_purchase_price;
    const double buy_cost = shares * price_per_share;
    const double new_total_shares = shares_owned + shares;
    if (new_total_shares <= 0.0)
    {
        return false;
    }

    shares_owned = new_total_shares;
    average_purchase_price = (existing_cost + buy_cost) / new_total_shares;
    last_updated = std::time(nullptr);

    events.emplace_back(date, StockEventType::BUY, shares, price_per_share, -buy_cost, notes);
    return true;
}

bool StockData::recordSell(time_t date, double shares, double price_per_share, const std::string& notes)
{
    if (shares <= 0.0 || price_per_share < 0.0 || shares > shares_owned)
    {
        return false;
    }

    shares_owned -= shares;
    if (shares_owned == 0.0)
    {
        average_purchase_price = 0.0;
    }
    last_updated = std::time(nullptr);

    const double sell_value = shares * price_per_share;
    events.emplace_back(date, StockEventType::SELL, shares, price_per_share, sell_value, notes);
    return true;
}

bool StockData::recordDividend(time_t date, double cash_amount, double shares_at_record, const std::string& notes)
{
    if (cash_amount < 0.0)
    {
        return false;
    }

    const double recorded_shares = (shares_at_record > 0.0) ? shares_at_record : shares_owned;
    last_updated = std::time(nullptr);
    events.emplace_back(date, StockEventType::DIVIDEND, recorded_shares, 0.0, cash_amount, notes);
    return true;
}

bool StockData::addEvent(time_t date, StockEventType type, double shares, double price_per_share,
                         double cash_amount, const std::string& notes)
{
    if (shares < 0.0 || price_per_share < 0.0)
    {
        return false;
    }

    events.emplace_back(date, type, shares, price_per_share, cash_amount, notes);
    last_updated = std::time(nullptr);
    return true;
}

bool StockData::rebuildFromEvents(const std::vector<StockEvent>& ordered_events)
{
    double rebuilt_shares = 0.0;
    double rebuilt_avg_purchase_price = 0.0;

    if (!rebuildPositionFromEvents(ordered_events, rebuilt_shares, rebuilt_avg_purchase_price))
    {
        return false;
    }

    std::vector<StockEvent> sorted_events = ordered_events;
    std::stable_sort(
        sorted_events.begin(),
        sorted_events.end(),
        [](const StockEvent& a, const StockEvent& b)
        {
            return a.date < b.date;
        }
    );

    shares_owned = rebuilt_shares;
    average_purchase_price = rebuilt_avg_purchase_price;
    events = std::move(sorted_events);
    last_updated = std::time(nullptr);
    return true;
}

void StockData::addDailyClosePrice(time_t date, double close_price)
{
    const time_t now = std::time(nullptr);
    const long long target_day = dayBucket(date);

    for (auto& price_point : price_history)
    {
        if (dayBucket(price_point.date) == target_day)
        {
            price_point.date = date;
            price_point.close_price = close_price;
            price_point.last_updated = now;
            last_updated = now;
            return;
        }
    }

    price_history.emplace_back(date, close_price, now);
    last_updated = now;
}

bool StockData::updateDailyClosePrice(time_t date, double close_price, time_t updated_at)
{
    const long long target_day = dayBucket(date);
    bool updated = false;

    for (auto& price_point : price_history)
    {
        if (dayBucket(price_point.date) == target_day)
        {
            price_point.date = date;
            price_point.close_price = close_price;
            price_point.last_updated = updated_at;
            last_updated = updated_at;
            updated = true;
        }
    }

    return updated;
}

bool StockData::saveToFile(const std::string& filepath) const
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open stock file " << filepath << " for writing" << std::endl;
        return false;
    }

    if (ticker.size() > MAX_SYMBOL_LENGTH)
    {
        std::cerr << "Error: Ticker exceeds supported limit" << std::endl;
        return false;
    }
    if (company_name.size() > MAX_COMPANY_NAME_LENGTH)
    {
        std::cerr << "Error: Company name exceeds supported limit" << std::endl;
        return false;
    }
    if (events.size() > std::numeric_limits<uint32_t>::max())
    {
        std::cerr << "Error: Too many stock events to serialize" << std::endl;
        return false;
    }
    if (price_history.size() > std::numeric_limits<uint32_t>::max())
    {
        std::cerr << "Error: Too many stock prices to serialize" << std::endl;
        return false;
    }

    const uint32_t file_version = CURRENT_STOCK_FILE_VERSION;
    file.write(reinterpret_cast<const char*>(&file_version), sizeof(uint32_t));
    const uint8_t reserved[4] = {0, 0, 0, 0};
    file.write(reinterpret_cast<const char*>(reserved), 4);

    const uint16_t ticker_len = static_cast<uint16_t>(ticker.size());
    file.write(reinterpret_cast<const char*>(&ticker_len), sizeof(uint16_t));
    if (ticker_len > 0)
    {
        file.write(ticker.c_str(), ticker_len);
    }

    const uint16_t company_len = static_cast<uint16_t>(company_name.size());
    file.write(reinterpret_cast<const char*>(&company_len), sizeof(uint16_t));
    if (company_len > 0)
    {
        file.write(company_name.c_str(), company_len);
    }

    file.write(reinterpret_cast<const char*>(&shares_owned), sizeof(double));
    file.write(reinterpret_cast<const char*>(&average_purchase_price), sizeof(double));
    file.write(reinterpret_cast<const char*>(&last_updated), sizeof(time_t));

    const uint32_t event_count = static_cast<uint32_t>(events.size());
    file.write(reinterpret_cast<const char*>(&event_count), sizeof(uint32_t));
    for (const auto& event : events)
    {
        if (event.notes.size() > MAX_NOTES_LENGTH)
        {
            std::cerr << "Error: Stock event notes exceed supported limit" << std::endl;
            return false;
        }

        file.write(reinterpret_cast<const char*>(&event.date), sizeof(time_t));
        const uint8_t type_byte = static_cast<uint8_t>(event.type);
        file.write(reinterpret_cast<const char*>(&type_byte), sizeof(uint8_t));
        file.write(reinterpret_cast<const char*>(&event.shares), sizeof(double));
        file.write(reinterpret_cast<const char*>(&event.price_per_share), sizeof(double));
        file.write(reinterpret_cast<const char*>(&event.cash_amount), sizeof(double));

        const uint16_t notes_len = static_cast<uint16_t>(event.notes.size());
        file.write(reinterpret_cast<const char*>(&notes_len), sizeof(uint16_t));
        if (notes_len > 0)
        {
            file.write(event.notes.c_str(), notes_len);
        }
    }

    const uint32_t price_count = static_cast<uint32_t>(price_history.size());
    file.write(reinterpret_cast<const char*>(&price_count), sizeof(uint32_t));
    for (const auto& price : price_history)
    {
        file.write(reinterpret_cast<const char*>(&price.date), sizeof(time_t));
        file.write(reinterpret_cast<const char*>(&price.close_price), sizeof(double));
        file.write(reinterpret_cast<const char*>(&price.last_updated), sizeof(time_t));
    }

    file.close();
    return file.good();
}

bool StockData::loadFromFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open stock file " << filepath << " for reading" << std::endl;
        return false;
    }

    uint32_t loaded_version;
    if (!readExact(file, reinterpret_cast<char*>(&loaded_version), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt stock file header (version)" << std::endl;
        return false;
    }
    if (loaded_version < OLDEST_SUPPORTED_STOCK_FILE_VERSION || loaded_version > CURRENT_STOCK_FILE_VERSION)
    {
        std::cerr << "Error: Unsupported stock file version " << loaded_version << std::endl;
        return false;
    }

    char reserved[4];
    if (!readExact(file, reserved, 4))
    {
        std::cerr << "Error: Corrupt stock file header (reserved)" << std::endl;
        return false;
    }

    uint16_t ticker_len;
    if (!readExact(file, reinterpret_cast<char*>(&ticker_len), sizeof(uint16_t)))
    {
        std::cerr << "Error: Corrupt stock file (ticker length)" << std::endl;
        return false;
    }
    if (ticker_len > MAX_SYMBOL_LENGTH)
    {
        std::cerr << "Error: Stock ticker length exceeds supported limit" << std::endl;
        return false;
    }
    std::string loaded_ticker;
    if (ticker_len > 0)
    {
        loaded_ticker.resize(ticker_len);
        if (!readExact(file, &loaded_ticker[0], ticker_len))
        {
            std::cerr << "Error: Corrupt stock file (ticker)" << std::endl;
            return false;
        }
    }

    uint16_t company_len;
    if (!readExact(file, reinterpret_cast<char*>(&company_len), sizeof(uint16_t)))
    {
        std::cerr << "Error: Corrupt stock file (company length)" << std::endl;
        return false;
    }
    if (company_len > MAX_COMPANY_NAME_LENGTH)
    {
        std::cerr << "Error: Company name length exceeds supported limit" << std::endl;
        return false;
    }
    std::string loaded_company_name;
    if (company_len > 0)
    {
        loaded_company_name.resize(company_len);
        if (!readExact(file, &loaded_company_name[0], company_len))
        {
            std::cerr << "Error: Corrupt stock file (company name)" << std::endl;
            return false;
        }
    }

    double loaded_shares_owned;
    double loaded_average_purchase_price;
    time_t loaded_last_updated;
    if (!readExact(file, reinterpret_cast<char*>(&loaded_shares_owned), sizeof(double)) ||
        !readExact(file, reinterpret_cast<char*>(&loaded_average_purchase_price), sizeof(double)) ||
        !readExact(file, reinterpret_cast<char*>(&loaded_last_updated), sizeof(time_t)))
    {
        std::cerr << "Error: Corrupt stock file (position fields)" << std::endl;
        return false;
    }

    uint32_t event_count;
    if (!readExact(file, reinterpret_cast<char*>(&event_count), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt stock file (event count)" << std::endl;
        return false;
    }
    if (event_count > MAX_STOCK_EVENTS)
    {
        std::cerr << "Error: Event count exceeds supported limit" << std::endl;
        return false;
    }

    std::vector<StockEvent> loaded_events;
    loaded_events.reserve(event_count);
    for (uint32_t i = 0; i < event_count; ++i)
    {
        time_t date;
        uint8_t type_byte;
        double shares;
        double price_per_share;
        double cash_amount;
        uint16_t notes_len;

        if (!readExact(file, reinterpret_cast<char*>(&date), sizeof(time_t)) ||
            !readExact(file, reinterpret_cast<char*>(&type_byte), sizeof(uint8_t)) ||
            !readExact(file, reinterpret_cast<char*>(&shares), sizeof(double)) ||
            !readExact(file, reinterpret_cast<char*>(&price_per_share), sizeof(double)) ||
            !readExact(file, reinterpret_cast<char*>(&cash_amount), sizeof(double)) ||
            !readExact(file, reinterpret_cast<char*>(&notes_len), sizeof(uint16_t)))
        {
            std::cerr << "Error: Corrupt stock file (event record)" << std::endl;
            return false;
        }
        if (!isValidStockEventType(type_byte))
        {
            std::cerr << "Error: Invalid stock event type in file" << std::endl;
            return false;
        }
        if (notes_len > MAX_NOTES_LENGTH)
        {
            std::cerr << "Error: Event notes length exceeds supported limit" << std::endl;
            return false;
        }

        std::string notes;
        if (notes_len > 0)
        {
            notes.resize(notes_len);
            if (!readExact(file, &notes[0], notes_len))
            {
                std::cerr << "Error: Corrupt stock file (event notes)" << std::endl;
                return false;
            }
        }

        loaded_events.emplace_back(date, static_cast<StockEventType>(type_byte), shares,
                                   price_per_share, cash_amount, notes);
    }

    uint32_t price_count;
    if (!readExact(file, reinterpret_cast<char*>(&price_count), sizeof(uint32_t)))
    {
        std::cerr << "Error: Corrupt stock file (price count)" << std::endl;
        return false;
    }
    if (price_count > MAX_STOCK_PRICE_POINTS)
    {
        std::cerr << "Error: Price count exceeds supported limit" << std::endl;
        return false;
    }

    double rebuilt_shares = 0.0;
    double rebuilt_avg_purchase_price = 0.0;
    if (!rebuildPositionFromEvents(loaded_events, rebuilt_shares, rebuilt_avg_purchase_price))
    {
        std::cerr << "Error: Loaded stock events are internally inconsistent" << std::endl;
        return false;
    }

    if (!nearlyEqual(rebuilt_shares, loaded_shares_owned) ||
        !nearlyEqual(rebuilt_avg_purchase_price, loaded_average_purchase_price))
    {
        std::cerr << "Error: Loaded stock position does not match event history" << std::endl;
        return false;
    }

    if (normalizeTicker(loaded_ticker).empty())
    {
        std::cerr << "Error: Loaded stock ticker is empty or invalid" << std::endl;
        return false;
    }

    std::vector<DailyStockPrice> loaded_prices;
    loaded_prices.reserve(price_count);
    for (uint32_t i = 0; i < price_count; ++i)
    {
        time_t date;
        double close_price;
        time_t updated_at;
        if (!readExact(file, reinterpret_cast<char*>(&date), sizeof(time_t)) ||
            !readExact(file, reinterpret_cast<char*>(&close_price), sizeof(double)) ||
            !readExact(file, reinterpret_cast<char*>(&updated_at), sizeof(time_t)))
        {
            std::cerr << "Error: Corrupt stock file (price record)" << std::endl;
            return false;
        }

        loaded_prices.emplace_back(date, close_price, updated_at);
    }

    version = loaded_version;
    ticker = normalizeTicker(loaded_ticker);
    company_name = loaded_company_name;
    shares_owned = loaded_shares_owned;
    average_purchase_price = loaded_average_purchase_price;
    last_updated = loaded_last_updated;
    events = std::move(loaded_events);
    price_history = std::move(loaded_prices);

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

    recoverSyncArtifacts(data_directory);
}

bool PortfolioManager::createPortfolio(const std::string& name, PortfolioType type, double initial_capital)
{
    std::string portfolio_dir = getPortfolioPath(name);
    std::string portfolio_file = getPortfolioFilePath(name);
    std::string stocks_dir = getStocksDirectoryPath(name);
    
    try
    {
        // Create portfolio directory
        if (!fs::exists(portfolio_dir))
        {
            fs::create_directories(portfolio_dir);
        }
        if (!fs::exists(stocks_dir))
        {
            fs::create_directories(stocks_dir);
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
    const std::string portfolio_dir = getPortfolioPath(name);
    ScopedPortfolioLock save_lock(portfolio_dir);
    if (!save_lock.isAcquired())
    {
        std::cerr << "Error saving portfolio: another write operation is in progress for " << name << std::endl;
        return false;
    }

    const std::string filepath = getPortfolioFilePath(name);
    const std::string stocks_dir = getStocksDirectoryPath(name);
    try
    {
        if (!fs::exists(stocks_dir))
        {
            fs::create_directories(stocks_dir);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error creating stocks directory during sync: " << e.what() << std::endl;
        return false;
    }

    std::unordered_map<std::string, std::vector<StockEvent>> events_by_ticker;
    for (const auto& tx : portfolio.getTransactions())
    {
        if (!isStockTransactionType(tx.type) || tx.stock_symbol.empty())
        {
            continue;
        }

        const std::string ticker = normalizeTicker(tx.stock_symbol);
        if (ticker.empty())
        {
            std::cerr << "Error syncing stock data: invalid ticker in portfolio transaction" << std::endl;
            return false;
        }

        if (tx.type == TransactionType::BUY_STOCK || tx.type == TransactionType::SELL_STOCK)
        {
            if (tx.shares <= 0.0)
            {
                std::cerr << "Error syncing stock data: non-positive share count for " << ticker << std::endl;
                return false;
            }
        }

        double price_per_share = 0.0;
        if (tx.shares > 0.0 && tx.type != TransactionType::DIVIDEND)
        {
            price_per_share = std::abs(tx.amount) / tx.shares;
        }

        events_by_ticker[ticker].emplace_back(
            tx.date,
            toStockEventType(tx.type),
            tx.shares,
            price_per_share,
            tx.amount,
            tx.notes
        );
    }

    const std::unordered_map<std::string, std::string> existing_stock_files = listStockFilesByTicker(stocks_dir);

    struct FileStage
    {
        std::string final_path;
        std::string tmp_path;
        std::string backup_path;
        bool had_existing = false;
    };

    std::vector<FileStage> staged_replacements;
    std::vector<FileStage> staged_deletions;

    const std::string portfolio_tmp = makeTempPath(filepath);
    const std::string portfolio_backup = makeBackupPath(filepath);
    FileUtils::deleteFile(portfolio_tmp);

    if (!portfolio.saveToFile(portfolio_tmp))
    {
        return false;
    }

    staged_replacements.push_back({
        filepath,
        portfolio_tmp,
        portfolio_backup,
        fs::exists(filepath)
    });

    for (auto& [ticker, derived_events] : events_by_ticker)
    {
        StockData stock_data;
        const std::string stock_file = getStockFilePath(name, ticker);
        const std::string stock_tmp = makeTempPath(stock_file);
        const std::string stock_backup = makeBackupPath(stock_file);
        FileUtils::deleteFile(stock_tmp);

        if (fs::exists(stock_file) && !stock_data.loadFromFile(stock_file))
        {
            std::cerr << "Error syncing stock file for ticker " << ticker << std::endl;
            return false;
        }

        if (stock_data.getTicker().empty())
        {
            stock_data.setTicker(ticker);
        }
        if (stock_data.getCompanyName().empty())
        {
            stock_data.setCompanyName(ticker);
        }

        if (!stock_data.rebuildFromEvents(derived_events))
        {
            std::cerr << "Error rebuilding stock position for ticker " << ticker
                      << " from portfolio transactions" << std::endl;
            return false;
        }

        if (!stock_data.saveToFile(stock_tmp))
        {
            std::cerr << "Error staging synced stock file for ticker " << ticker << std::endl;
            return false;
        }

        staged_replacements.push_back({
            stock_file,
            stock_tmp,
            stock_backup,
            fs::exists(stock_file)
        });
    }

    std::unordered_set<std::string> active_tickers;
    for (const auto& [ticker, _] : events_by_ticker)
    {
        active_tickers.insert(ticker);
    }

    for (const auto& [ticker, stock_file] : existing_stock_files)
    {
        if (active_tickers.find(ticker) != active_tickers.end())
        {
            continue;
        }

        // Keep API-enriched or user-curated files when no matching transactions exist.
        // Only prune files that look auto-generated by sync and have no market history.
        StockData stale_candidate;
        if (!stale_candidate.loadFromFile(stock_file))
        {
            std::cerr << "Error loading stale stock file candidate " << stock_file << std::endl;
            return false;
        }

        const bool looks_auto_generated =
            stale_candidate.getCompanyName() == stale_candidate.getTicker() &&
            stale_candidate.getPriceHistory().empty();

        if (!looks_auto_generated)
        {
            continue;
        }

        staged_deletions.push_back({
            stock_file,
            "",
            makeBackupPath(stock_file),
            true
        });
    }

    bool commit_failed = false;
    std::vector<size_t> replaced_indices;
    std::vector<size_t> deleted_indices;

    // Stage 1: move existing files to backups and place new files.
    for (size_t i = 0; i < staged_replacements.size(); ++i)
    {
        auto& stage = staged_replacements[i];
        try
        {
            if (stage.had_existing)
            {
                if (fs::exists(stage.backup_path))
                {
                    fs::remove(stage.backup_path);
                }
                fs::rename(stage.final_path, stage.backup_path);
            }
            fs::rename(stage.tmp_path, stage.final_path);
            replaced_indices.push_back(i);
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error committing staged file " << stage.final_path
                      << ": " << e.what() << std::endl;
            commit_failed = true;
            break;
        }
    }

    // Stage 2: remove stale files via backup move for rollback safety.
    if (!commit_failed)
    {
        for (size_t i = 0; i < staged_deletions.size(); ++i)
        {
            auto& stage = staged_deletions[i];
            try
            {
                if (fs::exists(stage.backup_path))
                {
                    fs::remove(stage.backup_path);
                }
                if (fs::exists(stage.final_path))
                {
                    fs::rename(stage.final_path, stage.backup_path);
                    deleted_indices.push_back(i);
                }
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Error staging stale stock deletion " << stage.final_path
                          << ": " << e.what() << std::endl;
                commit_failed = true;
                break;
            }
        }
    }

    if (commit_failed)
    {
        for (auto it = deleted_indices.rbegin(); it != deleted_indices.rend(); ++it)
        {
            auto& stage = staged_deletions[*it];
            try
            {
                if (fs::exists(stage.backup_path))
                {
                    fs::rename(stage.backup_path, stage.final_path);
                }
            }
            catch (const fs::filesystem_error&)
            {
            }
        }

        for (auto it = replaced_indices.rbegin(); it != replaced_indices.rend(); ++it)
        {
            auto& stage = staged_replacements[*it];
            try
            {
                if (fs::exists(stage.final_path))
                {
                    fs::remove(stage.final_path);
                }
                if (stage.had_existing && fs::exists(stage.backup_path))
                {
                    fs::rename(stage.backup_path, stage.final_path);
                }
            }
            catch (const fs::filesystem_error&)
            {
            }
        }

        for (const auto& stage : staged_replacements)
        {
            if (!stage.tmp_path.empty())
            {
                FileUtils::deleteFile(stage.tmp_path);
            }
        }

        return false;
    }

    for (const auto& stage : staged_replacements)
    {
        FileUtils::deleteFile(stage.backup_path);
    }
    for (const auto& stage : staged_deletions)
    {
        FileUtils::deleteFile(stage.backup_path);
    }

    return true;
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

bool PortfolioManager::saveStockData(const std::string& portfolio_name, const StockData& stock_data)
{
    const std::string portfolio_file = getPortfolioFilePath(portfolio_name);
    if (!fs::exists(portfolio_file))
    {
        std::cerr << "Error saving stock data: portfolio does not exist at " << portfolio_file << std::endl;
        return false;
    }

    const std::string portfolio_dir = getPortfolioPath(portfolio_name);
    ScopedPortfolioLock save_lock(portfolio_dir);
    if (!save_lock.isAcquired())
    {
        std::cerr << "Error saving stock data: another write operation is in progress for "
                  << portfolio_name << std::endl;
        return false;
    }

    const std::string stocks_dir = getStocksDirectoryPath(portfolio_name);

    try
    {
        if (!fs::exists(stocks_dir))
        {
            fs::create_directories(stocks_dir);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error creating stocks directory: " << e.what() << std::endl;
        return false;
    }

    const std::string normalized_ticker = normalizeTicker(stock_data.getTicker());
    if (normalized_ticker.empty())
    {
        std::cerr << "Error saving stock data: ticker is empty or invalid" << std::endl;
        return false;
    }

    Portfolio portfolio_for_validation;
    if (!loadPortfolio(portfolio_name, portfolio_for_validation))
    {
        std::cerr << "Error saving stock data: failed to load portfolio for validation" << std::endl;
        return false;
    }

    const std::vector<StockEvent> derived_events = buildDerivedEventsForTicker(portfolio_for_validation, normalized_ticker);
    if (!derived_events.empty())
    {
        const std::string filepath = getStockFilePath(portfolio_name, normalized_ticker);
        StockData existing_stock;
        const bool has_existing_stock_file = fs::exists(filepath) && existing_stock.loadFromFile(filepath);

        const bool incoming_is_partial_market_update =
            stock_data.getEvents().empty() &&
            nearlyEqual(stock_data.getSharesOwned(), 0.0) &&
            nearlyEqual(stock_data.getAveragePurchasePrice(), 0.0);

        if (incoming_is_partial_market_update)
        {
            if (!has_existing_stock_file)
            {
                std::cerr << "Error saving stock data: partial update requires existing stock file for "
                          << normalized_ticker << std::endl;
                return false;
            }

            StockData merged = existing_stock;
            if (!stock_data.getCompanyName().empty())
            {
                merged.setCompanyName(stock_data.getCompanyName());
            }

            for (const auto& price_point : stock_data.getPriceHistory())
            {
                merged.addDailyClosePrice(price_point.date, price_point.close_price);
            }

            return merged.saveToFile(filepath);
        }

        double expected_shares = 0.0;
        double expected_avg_price = 0.0;
        if (!rebuildPositionFromEvents(derived_events, expected_shares, expected_avg_price))
        {
            std::cerr << "Error saving stock data: portfolio transactions for " << normalized_ticker
                      << " are internally inconsistent" << std::endl;
            return false;
        }

        if (!nearlyEqual(stock_data.getSharesOwned(), expected_shares) ||
            !nearlyEqual(stock_data.getAveragePurchasePrice(), expected_avg_price))
        {
            std::cerr << "Error saving stock data: shares/cost basis for " << normalized_ticker
                      << " must come from portfolio BUY/SELL transactions" << std::endl;
            return false;
        }

        if (!stockEventsEquivalent(stock_data.getEvents(), derived_events))
        {
            std::cerr << "Error saving stock data: event history for " << normalized_ticker
                      << " must be transaction-derived" << std::endl;
            return false;
        }
    }

    std::string filepath = getStockFilePath(portfolio_name, normalized_ticker);
    return stock_data.saveToFile(filepath);
}

bool PortfolioManager::loadStockData(const std::string& portfolio_name, const std::string& ticker, StockData& stock_data)
{
    std::string filepath = getStockFilePath(portfolio_name, ticker);
    return stock_data.loadFromFile(filepath);
}

bool PortfolioManager::deleteStock(const std::string& portfolio_name, const std::string& ticker)
{
    const std::string filepath = getStockFilePath(portfolio_name, ticker);

    try
    {
        if (fs::exists(filepath))
        {
            fs::remove(filepath);
            return true;
        }
        return false;
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error deleting stock file: " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::string> PortfolioManager::listStocks(const std::string& portfolio_name) const
{
    std::vector<std::string> tickers;
    const std::string stocks_dir = getStocksDirectoryPath(portfolio_name);

    try
    {
        if (!fs::exists(stocks_dir))
        {
            return tickers;
        }

        for (const auto& entry : fs::directory_iterator(stocks_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const fs::path path = entry.path();
            if (path.extension() == ".dat")
            {
                tickers.push_back(path.stem().string());
            }
        }

        std::sort(tickers.begin(), tickers.end());
    }
    catch (const fs::filesystem_error& e)
    {
        std::cerr << "Error listing stocks: " << e.what() << std::endl;
    }

    return tickers;
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

std::string PortfolioManager::getStocksDirectoryPath(const std::string& portfolio_name) const
{
    return getPortfolioPath(portfolio_name) + "/stocks";
}

std::string PortfolioManager::getStockFilePath(const std::string& portfolio_name, const std::string& ticker) const
{
    return getStocksDirectoryPath(portfolio_name) + "/" + normalizeTicker(ticker) + ".dat";
}

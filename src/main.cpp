#include "portfolio_data.hpp"
#include "file_utils.hpp"
#include "market_data_sync.hpp"
#include "web_server.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace
{
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

bool loadDotEnvFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(in, line))
    {
        const std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#')
        {
            continue;
        }

        const size_t eq = raw.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            continue;
        }

        std::string key = trim(raw.substr(0, eq));
        std::string value = trim(raw.substr(eq + 1));

        if (key.rfind("export ", 0) == 0)
        {
            key = trim(key.substr(7));
        }

        if (key.empty())
        {
            continue;
        }

        if (value.size() >= 2)
        {
            const char first = value.front();
            const char last = value.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
            {
                value = value.substr(1, value.size() - 2);
            }
        }

        // Keep shell-exported values if already present.
        if (std::getenv(key.c_str()) == nullptr)
        {
            setenv(key.c_str(), value.c_str(), 0);
        }
    }

    return true;
}
}

void printPortfolioInfo(const Portfolio& portfolio)
{
    std::cout << "\n=== Portfolio Information ===" << std::endl;
    
    switch (portfolio.getType())
    {
        case PortfolioType::BROKERAGE:
            std::cout << "Type: Brokerage" << std::endl;
            break;
        case PortfolioType::ROTH_IRA:
            std::cout << "Type: Roth IRA" << std::endl;
            break;
        case PortfolioType::TRADITIONAL_IRA:
            std::cout << "Type: Traditional IRA" << std::endl;
            break;
        case PortfolioType::WATCHLIST:
            std::cout << "Type: Watchlist" << std::endl;
            break;
    }
    
    std::cout << "Available Capital: " << FileUtils::formatCurrency(portfolio.getAvailableCapital()) << std::endl;
    std::cout << "Current Value: " << FileUtils::formatCurrency(portfolio.getCurrentPortfolioValue()) << std::endl;
    
    if (!portfolio.getDailyValues().empty())
    {
        const auto& daily_values = portfolio.getDailyValues();
        const auto latest_it = std::max_element(
            daily_values.begin(),
            daily_values.end(),
            [](const DailyPortfolioValue& a, const DailyPortfolioValue& b)
            {
                return a.date < b.date;
            }
        );

        std::cout << "Daily Values Recorded: " << portfolio.getDailyValues().size() << std::endl;
        std::cout << "Latest Price: " << FileUtils::formatCurrency(latest_it->value) << " on " 
                  << FileUtils::timeToString(latest_it->date) << std::endl;
        std::cout << "Latest Record Updated: "
                  << FileUtils::timeToString(latest_it->last_updated) << std::endl;
    }
    
    if (!portfolio.getTransactions().empty())
    {
        std::cout << "\nTransactions Recorded: " << portfolio.getTransactions().size() << std::endl;
        std::cout << "\nTransaction Details:" << std::endl;
        std::cout << std::string(100, '-') << std::endl;
        
        for (const auto& tx : portfolio.getTransactions())
        {
            std::string type_str;
            switch (tx.type)
            {
                case TransactionType::DEPOSIT:
                    type_str = "DEPOSIT    ";
                    break;
                case TransactionType::WITHDRAWAL:
                    type_str = "WITHDRAWAL ";
                    break;
                case TransactionType::BUY_STOCK:
                    type_str = "BUY STOCK  ";
                    break;
                case TransactionType::SELL_STOCK:
                    type_str = "SELL STOCK ";
                    break;
                case TransactionType::DIVIDEND:
                    type_str = "DIVIDEND   ";
                    break;
                case TransactionType::INTEREST:
                    type_str = "INTEREST   ";
                    break;
            }
            
            std::cout << FileUtils::timeToString(tx.date) << " | " << type_str << " | ";
            std::cout << std::setw(12) << FileUtils::formatCurrency(tx.amount);
            
            if (!tx.stock_symbol.empty())
            {
                std::cout << " | " << tx.stock_symbol;
                if (tx.shares > 0)
                {
                    std::cout << " (" << std::fixed << std::setprecision(2) << tx.shares << " shares)";
                }
            }
            
            if (!tx.notes.empty())
            {
                std::cout << " | " << tx.notes;
            }
            
            std::cout << std::endl;
        }
        std::cout << std::string(100, '-') << std::endl;
    }
    
    std::cout << std::endl;
}

int main(int argc, char *argv[])
{
    loadDotEnvFile(".env");

    bool run_server = false;
    uint16_t server_port = 8080;
    std::string data_dir = "data";

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--server")
        {
            run_server = true;
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            ++i;
            try
            {
                const int parsed_port = std::stoi(argv[i]);
                if (parsed_port < 1 || parsed_port > 65535)
                {
                    std::cerr << "Invalid --port value: must be in range 1-65535" << std::endl;
                    return 1;
                }
                server_port = static_cast<uint16_t>(parsed_port);
            }
            catch (const std::exception&)
            {
                std::cerr << "Invalid --port value: must be an integer" << std::endl;
                return 1;
            }
        }
        else if (arg == "--data-dir" && i + 1 < argc)
        {
            ++i;
            data_dir = argv[i];
        }
    }

    if (run_server)
    {
        PortfolioApiServer server(data_dir, server_port);
        return server.start() ? 0 : 1;
    }
    
    std::cout << "=== Stock Portfolio Tracker ===" << std::endl;
    std::cout << "Version 1.0" << std::endl;

    // Initialize portfolio manager
    PortfolioManager manager(data_dir);

    const MarketDataSync::SyncConfig sync_config = MarketDataSync::configFromEnvironment();
    MarketDataSync::syncAllPortfolios(manager, sync_config);

    // Create a few example portfolios
    std::cout << "\nCreating example portfolios..." << std::endl;

    // Create a brokerage account
    if (manager.createPortfolio("Brokerage_Account", PortfolioType::BROKERAGE, 50000.0))
    {
        std::cout << "✓ Created Brokerage Account with $50,000 initial capital" << std::endl;
    }

    // Create a Roth IRA
    if (manager.createPortfolio("Roth_IRA_2024", PortfolioType::ROTH_IRA, 7000.0))
    {
        std::cout << "✓ Created Roth IRA with $7,000 initial capital" << std::endl;
    }

    // Create a Traditional IRA
    if (manager.createPortfolio("Traditional_IRA", PortfolioType::TRADITIONAL_IRA, 10000.0))
    {
        std::cout << "✓ Created Traditional IRA with $10,000 initial capital" << std::endl;
    }

    // Example: Load and modify a portfolio
    std::cout << "\nLoading Brokerage Account..." << std::endl;
    Portfolio portfolio;
    
    if (manager.loadPortfolio("Brokerage_Account", portfolio))
    {
        // Add some daily values
        time_t today = FileUtils::getCurrentTime();
        portfolio.addDailyValue(today - 86400 * 2, 50000.0);  // 2 days ago
        portfolio.addDailyValue(today - 86400, 51250.0);      // yesterday
        portfolio.addDailyValue(today, 52100.0);               // today

        // Retroactive correction to yesterday's close.
        portfolio.updateDailyValue(today - 86400, 51300.0, FileUtils::getCurrentTime());

        // Add various transaction types
        portfolio.addTransaction(today - 86400 * 7, 5000.0, TransactionType::DEPOSIT, "Initial contribution");
        
        // Buy 50 shares of AAPL at $150/share
        portfolio.addTransaction(today - 86400 * 5, -7500.0, TransactionType::BUY_STOCK, "AAPL", 50.0, "Buy 50 shares @ $150");
        
        // Sell 20 shares of AAPL at $155/share
        portfolio.addTransaction(today - 86400 * 3, 3100.0, TransactionType::SELL_STOCK, "AAPL", 20.0, "Sell 20 shares @ $155");
        
        // Dividend payment from AAPL (0.25 per share, 30 shares owned)
        portfolio.addTransaction(today - 86400, 7.50, TransactionType::DIVIDEND, "AAPL", 30.0, "AAPL Q1 dividend");
        
        // Another deposit
        portfolio.addTransaction(today - 86400 * 3, 2500.0, TransactionType::DEPOSIT, "Additional deposit");

        // Update available capital
        portfolio.setAvailableCapital(45000.0);

        // Save changes
        if (manager.savePortfolio("Brokerage_Account", portfolio))
        {
            std::cout << "✓ Portfolio saved successfully" << std::endl;
        }

        printPortfolioInfo(portfolio);
    }

    // Example: Scan and list all portfolios
    std::cout << "Scanning all portfolios..." << std::endl;
    if (manager.scanPortfolios())
    {
        const auto& names = manager.getPortfolioNames();
        std::cout << "Found " << names.size() << " portfolio(s):" << std::endl;
        for (const auto& name : names)
        {
            std::cout << "  - " << name << std::endl;
        }
    }

    // Display utility examples
    std::cout << "\n=== Utility Examples ===" << std::endl;
    std::cout << "Currency formatting: " << FileUtils::formatCurrency(12345.67) << std::endl;
    std::cout << "Percentage formatting: " << FileUtils::formatPercentage(5.234) << std::endl;
    std::cout << "Current date/time: " << FileUtils::getCurrentDateString() << std::endl;

    std::cout << "\n=== Portfolio Tracking System is Ready ===" << std::endl;
    std::cout << "Portfolios are stored in the 'data/' directory as binary files." << std::endl;
    std::cout << "Each portfolio has its own folder containing 'portfolio.dat'." << std::endl;
    std::cout << "Future: Add individual stock files to each portfolio directory." << std::endl;

    return 0;
}


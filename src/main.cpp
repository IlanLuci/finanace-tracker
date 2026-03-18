#include "portfolio_data.hpp"
#include "file_utils.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

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
    (void)argc;  // Mark as intentionally unused
    (void)argv;  // Mark as intentionally unused
    
    std::cout << "=== Stock Portfolio Tracker ===" << std::endl;
    std::cout << "Version 1.0" << std::endl;

    // Initialize portfolio manager
    PortfolioManager manager("data");

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


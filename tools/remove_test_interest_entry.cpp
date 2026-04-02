#include "portfolio_data.hpp"
#include "market_data_sync.hpp"

#include <cmath>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::string portfolio_name = (argc > 1) ? argv[1] : "Vanguard_Brokeridge";
    const std::string data_dir = (argc > 2) ? argv[2] : "data";

    PortfolioManager manager(data_dir);
    Portfolio portfolio;
    if (!manager.loadPortfolio(portfolio_name, portfolio))
    {
        std::cerr << "Failed to load portfolio: " << portfolio_name << "\n";
        return 1;
    }

    Portfolio cleaned(portfolio.getType(), portfolio.getAvailableCapital());
    cleaned.setDailyValues(portfolio.getDailyValues());

    bool removed = false;
    for (const auto& tx : portfolio.getTransactions())
    {
        const bool is_test_interest =
            !removed &&
            tx.type == TransactionType::INTEREST &&
            std::abs(tx.amount - 12.34) < 1e-9 &&
            tx.notes == "Interest test";

        if (is_test_interest)
        {
            cleaned.setAvailableCapital(cleaned.getAvailableCapital() - tx.amount);
            removed = true;
            continue;
        }

        if (!tx.stock_symbol.empty())
        {
            cleaned.addTransaction(tx.date, tx.amount, tx.type, tx.stock_symbol, tx.shares, tx.notes);
        }
        else
        {
            cleaned.addTransaction(tx.date, tx.amount, tx.type, tx.notes);
        }
    }

    if (!removed)
    {
        std::cout << "No matching test interest entry found in " << portfolio_name << "\n";
        return 0;
    }

    if (!manager.savePortfolio(portfolio_name, cleaned))
    {
        std::cerr << "Failed to save cleaned portfolio\n";
        return 1;
    }

    if (!MarketDataSync::recomputePortfolioDailyValues(manager, portfolio_name))
    {
        std::cerr << "Removed transaction but failed to recompute daily values\n";
        return 1;
    }

    std::cout << "Removed one test interest entry from " << portfolio_name << "\n";
    return 0;
}

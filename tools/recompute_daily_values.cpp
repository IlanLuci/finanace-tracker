#include "portfolio_data.hpp"
#include "market_data_sync.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    std::string data_dir = "data";

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc)
        {
            data_dir = argv[++i];
        }
    }

    PortfolioManager manager(data_dir);
    if (!manager.scanPortfolios())
    {
        std::cerr << "Failed to scan portfolios in: " << data_dir << std::endl;
        return 1;
    }

    bool all_ok = true;
    for (const auto& name : manager.getPortfolioNames())
    {
        const bool ok = MarketDataSync::recomputePortfolioDailyValues(manager, name);
        std::cout << (ok ? "[OK] " : "[ERR] ") << name << std::endl;
        if (!ok)
        {
            all_ok = false;
        }
    }

    return all_ok ? 0 : 1;
}

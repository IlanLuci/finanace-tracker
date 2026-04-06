#ifndef MARKET_DATA_SYNC_HPP
#define MARKET_DATA_SYNC_HPP

#include <cstddef>
#include <string>

class PortfolioManager;

namespace MarketDataSync
{
    struct SyncConfig {};

    SyncConfig configFromEnvironment();

    bool recomputePortfolioDailyValues(PortfolioManager& manager, const std::string& portfolio_name);
    bool syncPortfolio(PortfolioManager& manager, const std::string& portfolio_name, const SyncConfig& config);
    bool syncAllPortfolios(PortfolioManager& manager, const SyncConfig& config);
}

#endif
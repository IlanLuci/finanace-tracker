#ifndef MARKET_DATA_SYNC_HPP
#define MARKET_DATA_SYNC_HPP

#include <cstddef>
#include <string>

class PortfolioManager;

namespace MarketDataSync
{
    struct SyncConfig
    {
        std::string api_key;
        size_t max_requests_per_run = 25;
        int min_seconds_between_requests = 12;
    };

    SyncConfig configFromEnvironment();

    bool recomputePortfolioDailyValues(PortfolioManager& manager, const std::string& portfolio_name);
    bool syncPortfolio(PortfolioManager& manager, const std::string& portfolio_name, const SyncConfig& config);
    bool syncAllPortfolios(PortfolioManager& manager, const SyncConfig& config);
}

#endif
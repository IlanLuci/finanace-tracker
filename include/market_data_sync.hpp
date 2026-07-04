#ifndef MARKET_DATA_SYNC_HPP
#define MARKET_DATA_SYNC_HPP

#include <cstddef>
#include <ctime>
#include <string>

class PortfolioManager;

namespace MarketDataSync
{
    struct SyncConfig {};

    SyncConfig configFromEnvironment();

    // Best-effort lookup of a ticker's daily-close price nearest to `date`.
    // Used to recover cost basis for synthesized "prior history" buys of
    // positions that are no longer held (so no live holding basis exists).
    // Returns false (and leaves out_price untouched) on any fetch/parse error.
    bool fetchHistoricalClose(const std::string& symbol, time_t date,
                              double& out_price, std::string& error);

    bool recomputePortfolioDailyValues(PortfolioManager& manager, const std::string& portfolio_name);
    bool syncPortfolio(PortfolioManager& manager, const std::string& portfolio_name, const SyncConfig& config);
    bool syncAllPortfolios(PortfolioManager& manager, const SyncConfig& config);
}

#endif
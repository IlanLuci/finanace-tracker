#include "portfolio_data.hpp"
#include "file_utils.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    std::cout << "=== Testing Portfolio Data Persistence ===" << std::endl;

    std::string data_dir = "data";
    bool keep_test_data = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc)
        {
            data_dir = argv[++i];
        }
        else if (arg == "--keep-test-data")
        {
            keep_test_data = true;
        }
        else
        {
            std::cerr << "Usage: " << argv[0] << " [--data-dir <path>] [--keep-test-data]" << std::endl;
            return 1;
        }
    }

    std::cout << "Using data dir: " << data_dir << std::endl;

    PortfolioManager manager(data_dir);
    const std::string sync_portfolio_name = "AutoSync_Test";
    const std::string recovery_portfolio_name = "Recovery_Test";

    manager.deletePortfolio(sync_portfolio_name);
    manager.deletePortfolio(recovery_portfolio_name);

    // Validate automatic stock sync from portfolio transaction history
    // using an isolated portfolio so repeated test runs stay deterministic.
    time_t now = FileUtils::getCurrentTime();

    if (!manager.createPortfolio(sync_portfolio_name, PortfolioType::BROKERAGE, 10000.0))
    {
        std::cerr << "✗ Failed to create isolated auto-sync test portfolio" << std::endl;
        return 1;
    }

    Portfolio sync_portfolio;
    if (!manager.loadPortfolio(sync_portfolio_name, sync_portfolio))
    {
        std::cerr << "✗ Failed to load isolated auto-sync test portfolio" << std::endl;
        return 1;
    }

    sync_portfolio.addTransaction(now - 86400 * 10, -7500.0, TransactionType::BUY_STOCK, "AAPL", 50.0, "Initial buy");
    sync_portfolio.addTransaction(now - 86400 * 6, 3100.0, TransactionType::SELL_STOCK, "AAPL", 20.0, "Trim position");
    sync_portfolio.addTransaction(now - 86400 * 2, 7.50, TransactionType::DIVIDEND, "AAPL", 30.0, "Quarterly dividend");

    if (!manager.savePortfolio(sync_portfolio_name, sync_portfolio))
    {
        std::cerr << "✗ Failed to save portfolio for stock auto-sync" << std::endl;
        return 1;
    }

    StockData baseline_aapl;
    if (!manager.loadStockData(sync_portfolio_name, "AAPL", baseline_aapl))
    {
        std::cerr << "✗ Failed to load stock data after auto-sync" << std::endl;
        return 1;
    }

    baseline_aapl.addDailyClosePrice(now - 86400 * 2, 154.25);
    baseline_aapl.addDailyClosePrice(now - 86400, 156.10);
    baseline_aapl.addDailyClosePrice(now, 157.42);
    baseline_aapl.setCompanyName("Apple Inc.");

    if (!manager.saveStockData(sync_portfolio_name, baseline_aapl))
    {
        std::cerr << "✗ Failed to save baseline stock metadata/price history" << std::endl;
        return 1;
    }

    if (!manager.savePortfolio(sync_portfolio_name, sync_portfolio))
    {
        std::cerr << "✗ Failed to save portfolio for second stock auto-sync" << std::endl;
        return 1;
    }

    StockData reloaded_aapl;
    if (!manager.loadStockData(sync_portfolio_name, "AAPL", reloaded_aapl))
    {
        std::cerr << "✗ Failed to load stock data" << std::endl;
        return 1;
    }

    std::cout << "\n✓ Successfully saved and loaded stock-level data" << std::endl;
    std::cout << "  Company: " << reloaded_aapl.getCompanyName() << std::endl;
    std::cout << "  Ticker: " << reloaded_aapl.getTicker() << std::endl;
    std::cout << "  Shares Owned: " << reloaded_aapl.getSharesOwned() << std::endl;
    std::cout << "  Average Purchase Price: "
              << FileUtils::formatCurrency(reloaded_aapl.getAveragePurchasePrice()) << std::endl;
    std::cout << "  Event Count: " << reloaded_aapl.getEvents().size() << std::endl;
    std::cout << "  Price Points: " << reloaded_aapl.getPriceHistory().size() << std::endl;

    const bool shares_ok = std::abs(reloaded_aapl.getSharesOwned() - 30.0) < 1e-9;
    const bool avg_cost_ok = std::abs(reloaded_aapl.getAveragePurchasePrice() - 150.0) < 1e-9;
    const bool events_ok = reloaded_aapl.getEvents().size() == 3;
    const bool prices_ok = reloaded_aapl.getPriceHistory().size() == 3;

    if (!shares_ok || !avg_cost_ok || !events_ok || !prices_ok)
    {
        std::cerr << "✗ Stock data validation failed after reload" << std::endl;
        return 1;
    }

    const auto stock_tickers = manager.listStocks(sync_portfolio_name);
    std::cout << "  Stocks in " << sync_portfolio_name << ": ";
    for (size_t i = 0; i < stock_tickers.size(); ++i)
    {
        if (i > 0)
        {
            std::cout << ", ";
        }
        std::cout << stock_tickers[i];
    }
    std::cout << std::endl;

    // Ensure stale stock files are deleted during auto-sync.
    StockData stale_stock("OLD", "OLD");
    if (!manager.saveStockData(sync_portfolio_name, stale_stock))
    {
        std::cerr << "✗ Failed to create stale stock file for cleanup test" << std::endl;
        return 1;
    }

    if (!manager.savePortfolio(sync_portfolio_name, sync_portfolio))
    {
        std::cerr << "✗ Failed to save portfolio for stale stock cleanup test" << std::endl;
        return 1;
    }

    const auto cleaned_tickers = manager.listStocks(sync_portfolio_name);
    const bool old_removed = std::find(cleaned_tickers.begin(), cleaned_tickers.end(), "OLD") == cleaned_tickers.end();
    const bool aapl_present = std::find(cleaned_tickers.begin(), cleaned_tickers.end(), "AAPL") != cleaned_tickers.end();
    if (!old_removed || !aapl_present)
    {
        std::cerr << "✗ Stale stock cleanup validation failed" << std::endl;
        return 1;
    }

    // saveStockData should reject unknown portfolios.
    if (manager.saveStockData("Does_Not_Exist", stale_stock))
    {
        std::cerr << "✗ saveStockData unexpectedly succeeded for missing portfolio" << std::endl;
        return 1;
    }

    // Transaction-derived tickers should reject direct event/position mutation.
    StockData tampered_aapl = reloaded_aapl;
    if (!tampered_aapl.recordBuy(now, 1.0, 200.0, "tamper attempt"))
    {
        std::cerr << "✗ Failed to construct tampered stock payload" << std::endl;
        return 1;
    }

    if (manager.saveStockData(sync_portfolio_name, tampered_aapl))
    {
        std::cerr << "✗ saveStockData unexpectedly allowed transaction-owned event tampering" << std::endl;
        return 1;
    }

    // Partial API market update should merge without touching transaction-owned position/events.
    StockData partial_market_update("Apple Inc.", "AAPL");
    partial_market_update.addDailyClosePrice(now + 86400, 158.33);
    if (!manager.saveStockData(sync_portfolio_name, partial_market_update))
    {
        std::cerr << "✗ Failed to apply partial market data update" << std::endl;
        return 1;
    }

    StockData merged_aapl;
    if (!manager.loadStockData(sync_portfolio_name, "AAPL", merged_aapl))
    {
        std::cerr << "✗ Failed to load merged stock data" << std::endl;
        return 1;
    }

    const bool merged_shares_ok = std::abs(merged_aapl.getSharesOwned() - 30.0) < 1e-9;
    const bool merged_events_ok = merged_aapl.getEvents().size() == 3;
    const bool merged_prices_grew = merged_aapl.getPriceHistory().size() >= 4;
    if (!merged_shares_ok || !merged_events_ok || !merged_prices_grew)
    {
        std::cerr << "✗ Partial market update merge validation failed" << std::endl;
        return 1;
    }

    // API-only stocks (no portfolio transactions) should survive portfolio save.
    StockData api_only("Watchlist Corp", "WCH");
    api_only.addDailyClosePrice(now - 86400, 42.0);
    api_only.addDailyClosePrice(now, 43.0);
    if (!manager.saveStockData(sync_portfolio_name, api_only))
    {
        std::cerr << "✗ Failed to save API-only stock data" << std::endl;
        return 1;
    }

    if (!manager.savePortfolio(sync_portfolio_name, sync_portfolio))
    {
        std::cerr << "✗ Failed to save portfolio while preserving API-only stock" << std::endl;
        return 1;
    }

    const auto final_tickers = manager.listStocks(sync_portfolio_name);
    const bool watchlist_present = std::find(final_tickers.begin(), final_tickers.end(), "WCH") != final_tickers.end();
    if (!watchlist_present)
    {
        std::cerr << "✗ API-only stock was incorrectly pruned" << std::endl;
        return 1;
    }

    // Verify startup recovery logic for .tmp/.bak artifacts and stale lock dirs.
    if (!manager.createPortfolio(recovery_portfolio_name, PortfolioType::BROKERAGE, 5000.0))
    {
        std::cerr << "✗ Failed to create recovery test portfolio" << std::endl;
        return 1;
    }

    const std::string recovery_dir = manager.getPortfolioPath(recovery_portfolio_name);
    const std::string stocks_dir = manager.getStocksDirectoryPath(recovery_portfolio_name);
    const std::string portfolio_file = manager.getPortfolioFilePath(recovery_portfolio_name);

    {
        std::ofstream portfolio_backup(portfolio_file + ".bak.sync", std::ios::binary | std::ios::trunc);
        portfolio_backup << "backup";
        portfolio_backup.close();
        FileUtils::deleteFile(portfolio_file);
    }

    {
        std::ofstream stock_tmp(stocks_dir + "/TMPONLY.dat.tmp.sync", std::ios::binary | std::ios::trunc);
        stock_tmp << "tmp-only";
        stock_tmp.close();
    }

    FileUtils::createDirectory(recovery_dir + "/.save.lock");
    {
        std::ofstream lock_marker(recovery_dir + "/.save.lock/owner.txt", std::ios::trunc);
        lock_marker << "stale";
    }

    PortfolioManager recovery_manager(data_dir);
    const bool restored_portfolio = FileUtils::fileExists(portfolio_file);
    const bool promoted_tmp = FileUtils::fileExists(stocks_dir + "/TMPONLY.dat");
    const bool stale_lock_removed = !FileUtils::directoryExists(recovery_dir + "/.save.lock");

    if (!restored_portfolio || !promoted_tmp || !stale_lock_removed)
    {
        std::cerr << "✗ Startup artifact recovery validation failed" << std::endl;
        return 1;
    }

    // Cleanup intentionally invalid promoted tmp file from recovery test.
    recovery_manager.deleteStock(recovery_portfolio_name, "TMPONLY");

    if (!keep_test_data)
    {
        manager.deletePortfolio(sync_portfolio_name);
        manager.deletePortfolio(recovery_portfolio_name);
    }

    std::cout << "\n✓ Persistence tests passed" << std::endl;
    return 0;
}

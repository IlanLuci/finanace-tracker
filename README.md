# Portfolio Tracker

A C++17 application for tracking brokerage, retirement, cash, crypto, and watchlist accounts with persistent binary storage, a multithreaded HTTP API, and a single-page web UI. Daily close prices are synced from Yahoo Finance and cached on disk.

## Screenshots

### Portfolio overview
![Portfolio overview — all accounts, total assets, and the aggregate trend chart](https://ilan-not-elon.com/assets/images/asset-tracker-portfolio.png)

### Account detail
![Account detail — per-portfolio metrics, holdings, and recent transactions](https://ilan-not-elon.com/assets/images/asset-tracker-details.png)

More screenshots and writeup: [ilan-not-elon.com](https://ilan-not-elon.com/).

## Project Structure

```
finanace-tracker/
├── include/
│   ├── portfolio_data.hpp     # Portfolio, StockData, and shared data structures
│   ├── file_utils.hpp         # File system / formatting utilities
│   ├── market_data_sync.hpp   # Yahoo Finance sync and historical recompute
│   └── web_server.hpp         # HTTP API server
├── src/
│   ├── main.cpp               # CLI entry point and --server flag handling
│   ├── portfolio_data.cpp     # Portfolio / StockData implementation
│   ├── file_utils.cpp         # File and formatting helpers
│   ├── market_data_sync.cpp   # Yahoo Finance fetch + historical recompute
│   └── web_server.cpp         # HTTP routing, live-quote cache, threading
├── web/                       # Static single-page UI (see web/README.md)
├── tools/                     # Importers and ad-hoc maintenance utilities
├── data/                      # Portfolio data storage (created at runtime)
│   └── <portfolio_name>/
│       ├── portfolio.dat      # Binary portfolio file
│       └── stocks/
│           └── <TICKER>.dat   # Per-stock events + daily close prices
└── Makefile
```

## Data Structures

### Portfolio Types
- **BROKERAGE** (0) - Standard taxable brokerage account
- **ROTH_IRA** (1) - Roth IRA retirement account
- **TRADITIONAL_IRA** (2) - Traditional IRA retirement account
- **WATCHLIST** (3) - Read-only ticker list; transactions are disabled
- **CASH** (4) - Cash account; supports a per-portfolio ISO 4217 currency
- **CRYPTO** (5) - Crypto holdings (tickers are tracked via the stock data files)

### Core Classes

#### Portfolio
Represents a single portfolio with historical data.

**Key Members:**
- `version` (uint32_t) - File format version
- `type` (PortfolioType) - Account type
- `available_capital` (double) - Cash available to invest
- `currency` (std::string) - ISO 4217 currency code (defaults to `USD`; meaningful for CASH accounts)
- `daily_values` (vector<DailyPortfolioValue>) - Daily portfolio close values
- `transactions` (vector<Transaction>) - Cash and asset transactions

**Key Methods:**
```cpp
// Constructors
Portfolio();
Portfolio(PortfolioType ptype, double initial_capital);
Portfolio(PortfolioType ptype, double initial_capital, const std::string& ccy);

// Data access
uint32_t getVersion() const;
PortfolioType getType() const;
double getAvailableCapital() const;
const std::string& getCurrency() const;
const std::vector<DailyPortfolioValue>& getDailyValues() const;
const std::vector<Transaction>& getTransactions() const;

// Data modification
void setAvailableCapital(double capital);
void setCurrency(const std::string& ccy);
void addDailyValue(time_t date, double value);
bool updateDailyValue(time_t date, double value, time_t updated_at = std::time(nullptr));
void setDailyValues(const std::vector<DailyPortfolioValue>& values);
void clearDailyValues();
void addTransaction(time_t date, double amount, TransactionType type,
                   const std::string& notes = "");
void addTransaction(time_t date, double amount, TransactionType type,
                   const std::string& symbol, double shares, const std::string& notes = "");

// Persistence
bool saveToFile(const std::string& filepath) const;
bool loadFromFile(const std::string& filepath);

// Analytics
double getCurrentPortfolioValue() const;
double getCapitalMovement(time_t start_date, time_t end_date) const;
```

#### PortfolioManager
Manages multiple portfolios on disk.

**Key Methods:**
```cpp
// Constructor
PortfolioManager(const std::string& data_dir = "data");

// Portfolio management
bool createPortfolio(const std::string& name, PortfolioType type,
                    double initial_capital, const std::string& currency = "USD");
bool loadPortfolio(const std::string& name, Portfolio& portfolio);
bool savePortfolio(const std::string& name, const Portfolio& portfolio);
bool deletePortfolio(const std::string& name);

// Per-portfolio stock data
bool saveStockData(const std::string& portfolio_name, const StockData& stock_data);
bool loadStockData(const std::string& portfolio_name, const std::string& ticker, StockData& stock_data);
bool deleteStock(const std::string& portfolio_name, const std::string& ticker);
std::vector<std::string> listStocks(const std::string& portfolio_name) const;

// Discovery
bool scanPortfolios();
const std::vector<std::string>& getPortfolioNames() const;

// Utility
std::string getPortfolioPath(const std::string& name) const;
std::string getPortfolioFilePath(const std::string& name) const;
std::string getStocksDirectoryPath(const std::string& portfolio_name) const;
std::string getStockFilePath(const std::string& portfolio_name, const std::string& ticker) const;
```

#### StockData
Per-stock record stored at `data/<portfolio_name>/stocks/<TICKER>.dat`. Tracks the running shares owned, average purchase price, an ordered event log (`BUY`, `SELL`, `DIVIDEND`), and the daily close-price history pulled from Yahoo Finance.

### Data Records

#### DailyPortfolioValue
```cpp
struct DailyPortfolioValue {
    time_t date;         // Unix timestamp of market close
    double value;        // Portfolio value at market close
    time_t last_updated; // Unix timestamp of last edit to this daily record
};
```

#### Transaction
```cpp
struct Transaction {
    time_t date;              // Unix timestamp
    double amount;            // Cash impact (signed) or cost-basis amount for asset transfers
    TransactionType type;     // See table below
    std::string stock_symbol; // Ticker for stock-related transactions
    double shares;            // Shares for stock-related transactions
    std::string notes;        // Optional transaction notes
};
```

#### TransactionType Cash Impact

| Type | Value | Cash Impact | Typical Use |
|------|-------|-------------|-------------|
| DEPOSIT | 0 | `+amount` | Add cash to portfolio |
| WITHDRAWAL | 1 | `-amount` | Remove cash from portfolio |
| BUY_STOCK | 2 | `-amount` | Purchase stock using cash |
| SELL_STOCK | 3 | `+amount` | Sell stock and receive cash |
| DIVIDEND | 4 | `+amount` | Dividend payment into cash balance |
| INTEREST | 5 | `+amount` | Interest payment into cash balance |
| TRANSFER_IN_ASSET | 6 | `0` | Receive an asset from outside (`amount` = cost basis) |
| TRANSFER_OUT_ASSET | 7 | `0` | Send an asset out of the portfolio |

## Binary File Format

Portfolio data is stored in efficient binary format at `data/{portfolio_name}/portfolio.dat`.

### File Layout (12-byte header + data sections)

| Section | Size | Type | Description |
|---------|------|------|-------------|
| Version | 4 bytes | uint32_t | Format version number (currently 2) |
| Type | 1 byte | uint8_t | Portfolio type (0-2) |
| Reserved | 3 bytes | uint8_t[3] | Padding for alignment |
| Capital | 8 bytes | double | Available capital to invest |
| Daily Count | 4 bytes | uint32_t | Number of daily value records |
| Daily Values | Variable | DailyPortfolioValue[] | Array of daily values (24 bytes each in v2+) |
| Tx Count | 4 bytes | uint32_t | Number of transaction records |
| Transactions | Variable | Transaction[] | Array of transactions (variable size) |

Backward compatibility:
- Version 1 daily records store date + value only.
- Version 2 and newer daily records store date + value + last_updated.

### Transaction Entry Format
| Field | Size | Type |
|-------|------|------|
| Date | 8 bytes | time_t |
| Amount | 8 bytes | double |
| Type | 1 byte | uint8_t (see TransactionType table above) |
| Symbol Length | 2 bytes | uint16_t |
| Symbol | Variable | char[] (stock ticker, empty for non-stock tx) |
| Shares | 8 bytes | double |
| Notes Length | 2 bytes | uint16_t |
| Notes | Variable | char[] (UTF-8 string) |

### Daily Value Entry Format

Version 1:
| Field | Size | Type |
|-------|------|------|
| Date | 8 bytes | time_t |
| Value | 8 bytes | double |

Version 2+:
| Field | Size | Type |
|-------|------|------|
| Date | 8 bytes | time_t |
| Value | 8 bytes | double |
| Last Updated | 8 bytes | time_t |

## File Utilities

The `FileUtils` namespace provides helper functions:

### Date/Time Functions
```cpp
std::string timeToString(time_t timestamp);
time_t stringToTime(const std::string& datestr);
std::string getCurrentDateString();
time_t getCurrentTime();
```

### Formatting Functions
```cpp
std::string formatCurrency(double amount);      // e.g., "$12,345.67"
std::string formatPercentage(double percentage); // e.g., "5.23%"
```

### File System Functions
```cpp
bool fileExists(const std::string& filepath);
bool directoryExists(const std::string& dirpath);
bool createDirectory(const std::string& dirpath);
std::string getFileName(const std::string& filepath);
std::string getDirectoryPath(const std::string& filepath);
std::vector<std::string> listFilesInDirectory(const std::string& dirpath);
std::vector<std::string> listDirectories(const std::string& dirpath);
bool deleteFile(const std::string& filepath);
```

## Build Instructions

### Requirements
- C++17 compatible compiler (g++, clang)
- macOS, Linux, or Windows with MSYS2/MinGW

### Building
```bash
make              # Build the project
make run          # Build and run
make clean        # Remove build artifacts
```

### Running The Web API Server
```bash
./finance_tracker --server --port 8080 --data-dir data
```

- `--server` starts HTTP API mode instead of running the sample CLI flow.
- `--port` sets the listen port (default `8080`).
- `--data-dir` points to the portfolio storage root (default `data`).

## Market Close Price Sync (Yahoo Finance)

In `--server` mode the application runs two background threads alongside the HTTP listener:
- A **startup sync** thread that scans all portfolios + tickers, fills any gaps in daily close-price history, and recomputes daily portfolio totals — so the API is responsive immediately without blocking on Yahoo.
- A **daily after-close sync** thread that polls the wall clock and re-runs the sync once per trading day. Wall-clock polling (rather than `sleep_for`) keeps the sync correct after a laptop suspend/resume.

For each ticker the sync:
- Checks whether daily close prices are missing (empty history, stale latest day, or gaps around event days).
- Fetches daily candles from Yahoo Finance (free, no API key required) for tickers that need backfill.
- Saves market-close prices into each stock file under `data/<portfolio>/stocks/<TICKER>.dat`.
- Recomputes and persists historical daily portfolio totals from transactions + close prices.

After any transaction mutation (`buy`, `sell`, `dividend`, `deposit`, `withdrawal`, `interest`, `transfer_in`, `transfer_out`), daily portfolio values are recomputed immediately. For stock mutations, a per-portfolio Yahoo Finance sync is triggered before recompute so retroactive trades update historical totals.

Yahoo Finance is the only market-data provider and requires no configuration. For `CASH` portfolios, FX rates are fetched using Yahoo's `XXXUSD=X` pair format through the same live-quote path. If Yahoo Finance is temporarily unavailable, the app logs warnings for skipped fetches and continues operating with already-saved prices.

## Web API Endpoints

Base URL example: `http://localhost:8080`

The server accepts each TCP connection on its own thread. Write requests serialize on a process-wide data mutex so concurrent reads stay fast; read-only requests (`GET`, `OPTIONS`) skip the lock. Live quotes are cached in-process to coalesce duplicate requests across portfolios.

### Health
- `GET /api/health`

### Portfolio Views
- `GET /api/portfolios`
    - Returns all portfolios with type, currency, available cash, reported value, estimated value, and counts.
- `GET /api/portfolios/{name}`
    - Returns one portfolio with daily value history and summary stats.

### Portfolio Mutations
- `POST /api/portfolios`
    - Body: `{ "name": "Brokerage_Account", "type": "BROKERAGE", "initial_capital": 1000.00, "currency": "USD" }`
    - `type` must be one of `BROKERAGE`, `ROTH_IRA`, `TRADITIONAL_IRA`, `WATCHLIST`, `CASH`, `CRYPTO`.
    - `currency` is only applied to `CASH` portfolios; other types stay `USD`. Watchlists ignore `initial_capital`.
- `DELETE /api/portfolios/{name}`
    - Permanently removes the portfolio directory and its stock files.

### Stock Views
- `GET /api/portfolios/{name}/stocks`
    - Returns all stocks with shares owned, average purchase price, latest close, market value, and recent events.

### Live Quotes
- `GET /api/live-prices`
    - Returns the aggregate market state plus each portfolio's estimated total value using the live-quote cache.
- `GET /api/portfolios/{name}/live-prices`
    - Returns per-ticker live quotes for a single portfolio.

### Watchlist
- `POST /api/portfolios/{name}/watchlist`
    - Body: `{ "ticker": "AAPL" }`. Adds a ticker to a `WATCHLIST` portfolio (validates the symbol against Yahoo).
- `DELETE /api/portfolios/{name}/watchlist/{ticker}`
    - Removes a tracked ticker from a watchlist.

### Transaction Views
- `GET /api/portfolios/{name}/transactions/recent?limit=5`
    - Returns a recent subset of transactions sorted newest-first.
- `GET /api/portfolios/{name}/transactions`
    - Returns the full transaction history sorted newest-first.

### Transaction Mutations (JSON body)
- `POST /api/portfolios/{name}/transactions/buy`
    - Body: `{ "ticker": "AAPL", "shares": 10, "price_per_share": 175.25, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/sell`
    - Body: `{ "ticker": "AAPL", "shares": 4, "price_per_share": 181.00, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/dividend`
    - Body: `{ "ticker": "AAPL", "amount": 12.50, "shares": 30, "date": 1711824000, "notes": "optional" }`
    - `shares` is optional; if omitted, current shares for that ticker are used.
- `POST /api/portfolios/{name}/transactions/deposit`
    - Body: `{ "amount": 1000.00, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/withdrawal`
    - Body: `{ "amount": 500.00, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/interest`
    - Body: `{ "amount": 12.34, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/transfer_in`
    - Body: `{ "ticker": "AAPL", "shares": 5, "cost_basis_per_share": 150.00, "date": 1711824000, "notes": "optional" }`
- `POST /api/portfolios/{name}/transactions/transfer_out`
    - Body: `{ "ticker": "AAPL", "shares": 5, "date": 1711824000, "notes": "optional" }`

Transactions are disabled for `WATCHLIST` portfolios. All writes update `available_capital` (when relevant) and persist via `PortfolioManager::savePortfolio`, refreshing the per-stock files and recomputing the daily portfolio value history.

## Web UI

`web/` contains a single-page UI (static HTML + JS + Chart.js) for browsing portfolios, drilling into stocks, and submitting transactions. See `web/README.md` for setup; the short version:

```bash
./finance_tracker --server --port 8080 --data-dir data
cd web && python3 -m http.server 5173
# Open http://localhost:5173 and point API base URL at http://localhost:8080
```

### Output
- Binary: `./finance_tracker`
- Object files: `obj/`
- Portfolios: `data/`

## Usage Example

```cpp
#include "portfolio_data.hpp"
#include "file_utils.hpp"

int main() {
    // Initialize manager
    PortfolioManager manager("data");
    
    // Create a new portfolio
    manager.createPortfolio("My_Portfolio", PortfolioType::BROKERAGE, 50000.0);
    
    // Load it
    Portfolio portfolio;
    manager.loadPortfolio("My_Portfolio", portfolio);
    
    // Add data
    time_t now = FileUtils::getCurrentTime();
    portfolio.addDailyValue(now, 51234.56);
    portfolio.addTransaction(now, 1000.0, TransactionType::DEPOSIT, "Monthly contribution");
    portfolio.addTransaction(now, -4500.0, TransactionType::BUY_STOCK, "AAPL", 30.0, "Buy 30 shares");
    portfolio.addTransaction(now, 120.0, TransactionType::DIVIDEND, "AAPL", 30.0, "Quarterly dividend");
    
    // Save changes
    manager.savePortfolio("My_Portfolio", portfolio);
    
    // Display info
    std::cout << "Current Value: " << FileUtils::formatCurrency(portfolio.getCurrentPortfolioValue()) << std::endl;
    
    return 0;
}
```

## Future Enhancements

- Performance calculations (gains/losses, return %)
- Portfolio rebalancing tools
- Tax lot tracking
- Multi-year historical analysis
- Export functionality (CSV, JSON)
- Dividend reinvestment automation

## Technical Notes

### Memory Management
- Uses modern C++ standard library (vectors, strings)
- No manual memory allocation/deallocation required
- Automatic cleanup via RAII

### Thread Safety
- The HTTP server handles each connection on its own `std::thread`.
- Writes are serialized via a process-wide `std::mutex` so file I/O stays consistent; reads run lock-free.
- Startup sync and the daily after-close sync run on dedicated detached threads and take the same mutex while persisting changes.

### Data Persistence
- Binary format provides efficient storage and fast I/O
- Version field allows for future format migrations
- All timestamps use Unix time (UTC)

### Error Handling
- File I/O errors are logged to stderr
- Methods return bool to indicate success/failure
- Exceptions are NOT thrown by portfolio code

## License

TBD

---

Built with C++17 | Last Updated: 2026-05-21

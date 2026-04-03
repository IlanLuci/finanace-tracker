# Stock Portfolio Tracker

A C++ application for tracking and managing stock portfolios with persistent binary storage. Supports multiple portfolio types (Brokerage, Roth IRA, Traditional IRA) with historical data tracking.

## Project Structure

```
finanace-tracker/
├── include/
│   ├── portfolio_data.hpp    # Portfolio classes and data structures
│   └── file_utils.hpp        # File system utilities
├── src/
│   ├── main.cpp              # Example usage and entry point
│   ├── portfolio_data.cpp    # Implementation
│   └── file_utils.cpp        # File utilities implementation
├── data/                      # Portfolio data storage (created at runtime)
│   ├── Brokerage_Account/
│   │   └── portfolio.dat      # Binary portfolio file
│   ├── Roth_IRA_2024/
│   │   └── portfolio.dat
│   └── Traditional_IRA/
│       └── portfolio.dat
└── Makefile
```

## Data Structures

### Portfolio Types
- **BROKERAGE** (0) - Standard taxable brokerage account
- **ROTH_IRA** (1) - Roth IRA retirement account
- **TRADITIONAL_IRA** (2) - Traditional IRA retirement account

### Core Classes

#### Portfolio
Represents a single portfolio with historical data.

**Key Members:**
- `version` (uint32_t) - File format version
- `type` (PortfolioType) - Account type
- `available_capital` (double) - Cash available to invest
- `daily_values` (vector<DailyPortfolioValue>) - Daily marker close values
- `transactions` (vector<Transaction>) - Deposits and withdrawals

**Key Methods:**
```cpp
// Constructors
Portfolio();
Portfolio(PortfolioType ptype, double initial_capital);

// Data access
uint32_t getVersion() const;
PortfolioType getType() const;
double getAvailableCapital() const;
const std::vector<DailyPortfolioValue>& getDailyValues() const;
const std::vector<Transaction>& getTransactions() const;

// Data modification
void setAvailableCapital(double capital);
void addDailyValue(time_t date, double value);
void addTransaction(time_t date, double amount, TransactionType type, 
                   const std::string& notes = "");

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
                    double initial_capital);
bool loadPortfolio(const std::string& name, Portfolio& portfolio);
bool savePortfolio(const std::string& name, const Portfolio& portfolio);
bool deletePortfolio(const std::string& name);

// Discovery
bool scanPortfolios();
const std::vector<std::string>& getPortfolioNames() const;

// Utility
std::string getPortfolioPath(const std::string& name) const;
std::string getPortfolioFilePath(const std::string& name) const;
```

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
    double amount;            // Cash impact in dollars (+/-)
    TransactionType type;     // DEPOSIT, WITHDRAWAL, BUY_STOCK, SELL_STOCK, DIVIDEND
    std::string stock_symbol; // Ticker for stock-related transactions
    double shares;            // Shares for stock-related transactions
    std::string notes;        // Optional transaction notes
};
```

#### TransactionType Cash Impact

| Type | Cash Impact | Typical Use |
|------|-------------|-------------|
| DEPOSIT | `+amount` | Add cash to portfolio |
| WITHDRAWAL | `-amount` | Remove cash from portfolio |
| BUY_STOCK | `-amount` | Purchase stock using cash |
| SELL_STOCK | `+amount` | Sell stock and receive cash |
| DIVIDEND | `+amount` | Dividend payment into cash balance |

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
| Type | 1 byte | uint8_t (0=DEPOSIT, 1=WITHDRAWAL, 2=BUY_STOCK, 3=SELL_STOCK, 4=DIVIDEND) |
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

## Market Close Price Sync (Yahoo Finance Primary + Alpha Vantage Backup)

On startup, the application now:
- Scans all portfolios and stock tickers found in transaction history.
- Checks whether daily close prices are missing (empty history, stale latest day, or gaps around event days).
- Fetches daily candles from Yahoo Finance (unlimited, free, no API key required) for tickers that need backfill.
- If Yahoo Finance fails for any reason, automatically falls back to Alpha Vantage for remaining tickers in that run.
- Saves market-close prices into each stock file under `data/<portfolio>/stocks/<TICKER>.dat`.
- Recomputes and persists historical daily portfolio totals from transactions + close prices.

After any API transaction mutation (`buy`, `sell`, `dividend`, `deposit`, `withdrawal`), daily portfolio values are recomputed immediately. For stock mutations, an additional per-portfolio Alpha Vantage sync is triggered before recompute so retroactive trades can update historical totals.

### Environment Variables

```bash
export ALPHAVANTAGE_API_KEY="your_api_key_here"
export ALPHAVANTAGE_MAX_REQUESTS_PER_RUN=25
export ALPHAVANTAGE_MIN_SECONDS_BETWEEN_REQUESTS=12
```

- `ALPHAVANTAGE_API_KEY`: optional backup provider key used when Yahoo Finance is unavailable or rate-limited.
- `ALPHAVANTAGE_MAX_REQUESTS_PER_RUN`: hard cap per process start for the Alpha Vantage fallback. This prevents exhausting daily quotas.
- `ALPHAVANTAGE_MIN_SECONDS_BETWEEN_REQUESTS`: delay between calls to reduce per-minute limit hits.

Yahoo Finance is the primary provider and requires no configuration. If Alpha Vantage is configured, it serves as a fallback. If no Alpha Vantage key is set and Yahoo Finance fails, the app will log a warning for skipped fetches but continue operation using already-saved prices.

## Web API Endpoints

Base URL example: `http://localhost:8080`

### Health
- `GET /api/health`

### Portfolio Views
- `GET /api/portfolios`
    - Returns all portfolios with type, available cash, reported value, estimated value, and counts.
- `GET /api/portfolios/{name}`
    - Returns one portfolio with daily value history and summary stats.

### Stock Views
- `GET /api/portfolios/{name}/stocks`
    - Returns all stocks with shares owned, average purchase price, latest close, market value, and recent events.

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

All writes update `available_capital` and persist via `PortfolioManager::savePortfolio`, preserving the existing portfolio-to-stock sync behavior.

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

### Stock Data Files
- Add individual stock data files within each portfolio directory
- Design: `data/{portfolio_name}/stocks/{stock_ticker}.dat`
- Each file contains:
  - Stock metadata (ticker, shares owned, acquisition cost)
  - Dividend records
  - Historical price data

### Features to Add
- Performance calculations (gains/losses, return %)
- Portfolio rebalancing tools
- Tax lot tracking
- Multi-year historical analysis
- Export functionality (CSV, JSON)
- Dividend tracking and reinvestment

## Technical Notes

### Memory Management
- Uses modern C++ standard library (vectors, strings)
- No manual memory allocation/deallocation required
- Automatic cleanup via RAII

### Thread Safety
- Current implementation is single-threaded
- Consider adding mutexes for concurrent access in future versions

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

Built with C++17 | Last Updated: 2026-03-18

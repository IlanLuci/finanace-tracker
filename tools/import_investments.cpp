#include "portfolio_data.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class Section {
    NONE,
    VANGUARD_BROKERAGE,
    ROBINHOOD_BROKERAGE,
    VANGUARD_ROTH
};

struct Holding {
    std::string ticker;
    double acquired_price = 0.0;
    double shares = 0.0;
    double worth = 0.0;
};

struct TargetPortfolio {
    std::string name;
    PortfolioType type;
    Section section;
    std::vector<Holding> holdings;
};

std::string trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    {
        ++start;
    }

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }

    return s.substr(start, end - start);
}

std::string toUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::vector<std::string> parseCsvRow(const std::string& line)
{
    std::vector<std::string> cols;
    std::string cell;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i)
    {
        const char ch = line[i];
        if (ch == '"')
        {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"')
            {
                cell.push_back('"');
                ++i;
            }
            else
            {
                in_quotes = !in_quotes;
            }
        }
        else if (ch == ',' && !in_quotes)
        {
            cols.push_back(trim(cell));
            cell.clear();
        }
        else
        {
            cell.push_back(ch);
        }
    }

    cols.push_back(trim(cell));
    return cols;
}

double parseMoney(const std::string& raw)
{
    const std::string s = trim(raw);
    if (s.empty())
    {
        return 0.0;
    }

    const std::string upper = toUpper(s);
    if (upper == "N/A" || upper == "#N/A")
    {
        return 0.0;
    }

    std::string cleaned;
    cleaned.reserve(s.size());
    for (char ch : s)
    {
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-')
        {
            cleaned.push_back(ch);
        }
    }

    if (cleaned.empty() || cleaned == "-" || cleaned == "." || cleaned == "-.")
    {
        return 0.0;
    }

    try
    {
        return std::stod(cleaned);
    }
    catch (...)
    {
        return 0.0;
    }
}

bool isTicker(const std::string& s)
{
    if (s.empty())
    {
        return false;
    }

    bool has_alpha = false;

    for (char ch : s)
    {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.'))
        {
            return false;
        }

        if (std::isalpha(static_cast<unsigned char>(ch)))
        {
            has_alpha = true;
        }
    }

    return has_alpha;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: import_investments <csv_path> <data_dir>\n";
        return 1;
    }

    const std::string csv_path = argv[1];
    const std::string data_dir = argv[2];

    std::ifstream in(csv_path);
    if (!in.is_open())
    {
        std::cerr << "Failed to open CSV: " << csv_path << "\n";
        return 1;
    }

    std::vector<TargetPortfolio> targets = {
        {"Vanguard_Brokeridge", PortfolioType::BROKERAGE, Section::VANGUARD_BROKERAGE, {}},
        {"Robinhood_Brokeridge", PortfolioType::BROKERAGE, Section::ROBINHOOD_BROKERAGE, {}},
        {"Vanguard_Roth_IRA", PortfolioType::ROTH_IRA, Section::VANGUARD_ROTH, {}}
    };

    Section current = Section::NONE;
    std::string line;
    while (std::getline(in, line))
    {
        const std::vector<std::string> cols = parseCsvRow(line);
        if (cols.empty())
        {
            continue;
        }

        const std::string first = cols[0];
        if (first == "Vanguard Brokerage Stocks")
        {
            current = Section::VANGUARD_BROKERAGE;
            continue;
        }
        if (first == "Vanguard Brokerage Sold" || first == "Watchlist")
        {
            current = Section::NONE;
            continue;
        }
        if (first == "Robinhood Stocks")
        {
            current = Section::ROBINHOOD_BROKERAGE;
            continue;
        }
        if (first == "Roth IRA Stocks")
        {
            current = Section::VANGUARD_ROTH;
            continue;
        }

        if (current == Section::NONE || cols.size() < 4)
        {
            continue;
        }

        // Ignore sold marker rows and non-holding summary lines.
        if (toUpper(trim(cols[0])) == "S")
        {
            continue;
        }

        const std::string ticker = cols.size() > 1 ? trim(cols[1]) : "";
        if (!isTicker(ticker) || toUpper(ticker) == "TICKER")
        {
            continue;
        }

        const double acquired_price = cols.size() > 2 ? parseMoney(cols[2]) : 0.0;
        const double shares = cols.size() > 3 ? parseMoney(cols[3]) : 0.0;
        const double worth = cols.size() > 10 ? parseMoney(cols[10]) : 0.0;
        if (shares <= 0.0)
        {
            continue;
        }

        Holding holding;
        holding.ticker = toUpper(ticker);
        holding.acquired_price = acquired_price;
        holding.shares = shares;
        holding.worth = worth;

        for (auto& target : targets)
        {
            if (target.section == current)
            {
                target.holdings.push_back(holding);
                break;
            }
        }
    }

    PortfolioManager manager(data_dir);

    const std::time_t now = std::time(nullptr);
    const std::time_t buy_date = now - 86400;

    for (const auto& target : targets)
    {
        double total_cost = 0.0;
        double total_worth = 0.0;
        for (const auto& h : target.holdings)
        {
            total_cost += (h.acquired_price * h.shares);
            total_worth += h.worth;
        }

        if (!manager.createPortfolio(target.name, target.type, total_cost))
        {
            std::cerr << "Failed to create portfolio: " << target.name << "\n";
            return 1;
        }

        Portfolio portfolio;
        if (!manager.loadPortfolio(target.name, portfolio))
        {
            std::cerr << "Failed to load created portfolio: " << target.name << "\n";
            return 1;
        }

        for (const auto& h : target.holdings)
        {
            const double amount = -(h.acquired_price * h.shares);
            portfolio.addTransaction(
                buy_date,
                amount,
                TransactionType::BUY_STOCK,
                h.ticker,
                h.shares,
                "Imported from Finances - Investments.csv"
            );
        }

        portfolio.setAvailableCapital(0.0);
        if (total_worth > 0.0)
        {
            portfolio.addDailyValue(now, total_worth);
        }

        if (!manager.savePortfolio(target.name, portfolio))
        {
            std::cerr << "Failed to save imported portfolio: " << target.name << "\n";
            return 1;
        }

        std::cout << "Created " << target.name
                  << " with " << target.holdings.size() << " holdings"
                  << ", cost=" << total_cost
                  << ", worth=" << total_worth << "\n";
    }

    if (!manager.scanPortfolios())
    {
        std::cerr << "Failed to scan portfolios after import\n";
        return 1;
    }

    std::cout << "Total portfolios: " << manager.getPortfolioNames().size() << "\n";
    for (const auto& name : manager.getPortfolioNames())
    {
        std::cout << " - " << name << "\n";
    }

    return 0;
}

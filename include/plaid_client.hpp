#ifndef PLAID_CLIENT_HPP
#define PLAID_CLIENT_HPP

#include <ctime>
#include <string>
#include <vector>

namespace Plaid
{
    struct Config
    {
        std::string client_id;
        std::string secret;
        std::string environment;   // "sandbox" | "development" | "production"
        std::string client_name;
        std::string country_codes; // e.g. "US"
        std::string language;      // e.g. "en"
    };

    struct AccountSummary
    {
        std::string account_id;
        std::string name;
        std::string official_name;
        std::string type;
        std::string subtype;
        std::string mask;
        double      current_balance;
        bool        has_balance;

        AccountSummary() : current_balance(0.0), has_balance(false) {}
    };

    struct Transaction
    {
        std::string transaction_id;
        std::string account_id;
        std::string name;
        std::string merchant_name;
        std::string iso_currency_code;
        time_t      date;             // unix midnight UTC of post date
        double      amount;           // Plaid sign convention: positive = money OUT, negative = money IN
        bool        pending;
        std::string pfc_primary;      // personal_finance_category.primary, e.g. "TRANSFER_OUT", "FOOD_AND_DRINK"
        std::string pfc_detailed;     // personal_finance_category.detailed, e.g. "FOOD_AND_DRINK_RESTAURANTS"
        // First (highest-confidence) counterparty Plaid identified, if any. Used
        // to distinguish own-account transfers (counterparty_type=financial_institution)
        // from peer-to-peer payments like Zelle to a friend (type=user/payment_app).
        std::string counterparty_name;
        std::string counterparty_type; // "financial_institution" | "user" | "payment_app" |
                                       // "merchant" | "marketplace" | "income_source" | ""

        Transaction() : date(0), amount(0.0), pending(false) {}
    };

    struct Security
    {
        std::string security_id;
        std::string ticker_symbol;
        std::string name;
        std::string type; // equity, etf, mutual fund, fixed income, cash, etc.
        bool is_cash_equivalent; // true for money-market funds, settlement cash, etc.

        Security() : is_cash_equivalent(false) {}
    };

    struct Holding
    {
        std::string account_id;
        std::string security_id;
        double      quantity;
        double      cost_basis;          // total cost basis for the position (not per-share)
        double      institution_price;   // last institution-reported price per share
        double      institution_value;   // market value
        std::string iso_currency_code;

        Holding() : quantity(0.0), cost_basis(0.0), institution_price(0.0), institution_value(0.0) {}
    };

    struct InvestmentTransaction
    {
        std::string investment_transaction_id;
        std::string account_id;
        std::string security_id;
        std::string type;        // buy, sell, cash, transfer, fee, cancel
        std::string subtype;     // buy, sell, dividend, interest, deposit, withdrawal, transfer, etc.
        double      quantity;    // signed; negative for outflow shares
        double      price;       // per share
        double      amount;      // cash impact; Plaid: + = into account, - = out of account
        double      fees;
        time_t      date;
        std::string name;
        std::string iso_currency_code;

        InvestmentTransaction()
            : quantity(0.0), price(0.0), amount(0.0), fees(0.0), date(0) {}
    };

    // Pulls config from PLAID_* env vars. Falls back to safe defaults.
    Config configFromEnvironment();

    bool isConfigured(const Config& config);

    // POST /link/token/create  → returns Plaid link_token in out_link_token
    bool createLinkToken(const Config& config,
                         const std::string& user_id,
                         std::string& out_link_token,
                         std::string& error);

    // POST /item/public_token/exchange → returns access_token + item_id
    bool exchangePublicToken(const Config& config,
                             const std::string& public_token,
                             std::string& out_access_token,
                             std::string& out_item_id,
                             std::string& error);

    // POST /accounts/get → list accounts under this item + institution info
    bool getAccounts(const Config& config,
                     const std::string& access_token,
                     std::vector<AccountSummary>& out_accounts,
                     std::string& out_institution_name,
                     std::string& out_institution_id,
                     std::string& error);

    // POST /transactions/get → returns all transactions for the item between dates.
    // dates as "YYYY-MM-DD". Will paginate internally and append all to out_transactions.
    bool getTransactions(const Config& config,
                         const std::string& access_token,
                         const std::string& start_date,
                         const std::string& end_date,
                         std::vector<Transaction>& out_transactions,
                         std::string& error);

    // POST /investments/holdings/get → current holdings + securities lookup table.
    bool getHoldings(const Config& config,
                     const std::string& access_token,
                     std::vector<Holding>& out_holdings,
                     std::vector<Security>& out_securities,
                     std::string& error);

    // POST /investments/transactions/get → investment txns + securities lookup.
    // Paginates internally.
    bool getInvestmentTransactions(const Config& config,
                                   const std::string& access_token,
                                   const std::string& start_date,
                                   const std::string& end_date,
                                   std::vector<InvestmentTransaction>& out_transactions,
                                   std::vector<Security>& out_securities,
                                   std::string& error);

    // True when the Plaid error string (typically of the form
    // "HTTP 400: {\"error_code\":\"INVALID_ACCESS_TOKEN\",...}") indicates the
    // access_token is no longer usable for this item: ITEM_LOGIN_REQUIRED (user
    // creds expired), INVALID_ACCESS_TOKEN (token issued under a different
    // client_id/env — e.g. after rotating PLAID_CLIENT_ID), or ITEM_NOT_FOUND.
    bool errorRequiresReauth(const std::string& error);
}

#endif

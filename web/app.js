const API_STORAGE_KEY = "portfolio-api-base";
const CURRENT_PORTFOLIO_KEY = "portfolio-current";
const PINNED_ACCOUNTS_KEY = "portfolio-pinned-accounts";
const ACCOUNT_FILTER_KEY = "portfolio-account-filter";

const TYPE_SORT_ORDER = ["BROKERAGE", "ROTH_IRA", "TRADITIONAL_IRA", "CRYPTO", "CASH", "WATCHLIST"];

function getPinnedAccounts() {
  try {
    const raw = localStorage.getItem(PINNED_ACCOUNTS_KEY);
    if (!raw) return new Set();
    const arr = JSON.parse(raw);
    return new Set(Array.isArray(arr) ? arr : []);
  } catch (_) {
    return new Set();
  }
}

function setPinnedAccounts(set) {
  localStorage.setItem(PINNED_ACCOUNTS_KEY, JSON.stringify(Array.from(set)));
}

function isAccountPinned(name) {
  return getPinnedAccounts().has(String(name));
}

function togglePinnedAccount(name) {
  const set = getPinnedAccounts();
  const key = String(name);
  if (set.has(key)) {
    set.delete(key);
  } else {
    set.add(key);
  }
  setPinnedAccounts(set);
}

function getAccountFilter() {
  return localStorage.getItem(ACCOUNT_FILTER_KEY) || "ALL";
}

function setAccountFilter(value) {
  if (!value || value === "ALL") {
    localStorage.removeItem(ACCOUNT_FILTER_KEY);
  } else {
    localStorage.setItem(ACCOUNT_FILTER_KEY, String(value).toUpperCase());
  }
}

function renderMonthlyActivity(portfolios, allTransactions) {
  const rows = computeMonthlyActivity(portfolios, allTransactions);
  if (rows.length === 0) {
    return `<article class="panel"><div class="panel-head"><h3>Monthly Activity</h3></div>
      <p class="sub">No monthly activity yet.</p></article>`;
  }

  const showAll = !!state.monthlyShowAll;
  const visibleCount = showAll ? rows.length : Math.min(12, rows.length);
  const visibleRows = rows.slice(0, visibleCount);

  // Group consecutive rows by year so we can insert YEAR TOTAL rows below
  // each fully-shown December (or below the oldest row in that year on screen).
  const yearTotals = new Map();
  rows.forEach((r) => {
    const t = yearTotals.get(r.year) || { cash: 0, investment: 0, total: 0 };
    t.cash += r.cashDelta;
    t.investment += r.investmentDelta;
    t.total += r.totalDelta;
    yearTotals.set(r.year, t);
  });

  const monthLabel = (year, month) => {
    const d = new Date(Date.UTC(year, month - 1, 1));
    return d.toLocaleString(undefined, { month: "short", year: "numeric", timeZone: "UTC" });
  };

  const cell = (value) => `<td class="num ${pnlCellTone(value, value !== 0)}">${signedCurrency(value)}</td>`;

  const currentYear = new Date().getUTCFullYear();
  const bodyRows = [];
  for (let i = 0; i < visibleRows.length; i += 1) {
    const r = visibleRows[i];
    bodyRows.push(
      `<tr><td>${monthLabel(r.year, r.month)}</td>${cell(r.cashDelta)}${cell(r.investmentDelta)}${cell(r.totalDelta)}</tr>`
    );
    const yearComplete = r.year < currentYear;
    const isLastInYear = (i === visibleRows.length - 1) || visibleRows[i + 1].year !== r.year;
    if (yearComplete && isLastInYear && r.month === 1) {
      const yt = yearTotals.get(r.year);
      if (yt) {
        bodyRows.push(
          `<tr class="monthly-year-total"><td><strong>${r.year} Total</strong></td>${cell(yt.cash)}${cell(yt.investment)}${cell(yt.total)}</tr>`
        );
      }
    }
  }

  const toggle = rows.length > 12
    ? `<button id="monthlyToggleBtn" class="ghost-btn" type="button">${showAll ? "Show last 12 months" : "View more"}</button>`
    : "";

  return `<article class="panel">
    <div class="panel-head">
      <h3>Monthly Activity</h3>
      ${toggle}
    </div>
    <div class="stocks-table-wrap">
      <table class="tx-table">
        <thead><tr><th>Month</th><th class="num">Cash Δ</th><th class="num">Investment Δ</th><th class="num">Total Δ</th></tr></thead>
        <tbody>${bodyRows.join("")}</tbody>
      </table>
    </div>
  </article>`;
}

function monthKey(year, month) {
  return `${year}-${String(month).padStart(2, "0")}`;
}

function monthKeyFromUnix(unixSeconds) {
  const d = new Date(safeNumber(unixSeconds) * 1000);
  return monthKey(d.getUTCFullYear(), d.getUTCMonth() + 1);
}

function lastDayValueByMonth(points) {
  const byMonth = new Map();
  (Array.isArray(points) ? points : []).forEach((p) => {
    const date = safeNumber(p?.date);
    if (date <= 0) return;
    const key = monthKeyFromUnix(date);
    const prev = byMonth.get(key);
    if (!prev || date > prev.date) {
      byMonth.set(key, { date, value: safeNumber(p?.value) });
    }
  });
  return byMonth;
}

function computeMonthlyActivity(portfolios, allTransactions) {
  const accountPortfolios = portfolios.filter((p) => !isWatchlistPortfolio(p));
  const aggregateTrend = mergeDailySeries(accountPortfolios);
  const monthEndValues = lastDayValueByMonth(aggregateTrend);

  const cashByMonth = new Map();
  accountPortfolios.forEach((p) => {
    const txs = (allTransactions || {})[p.name] || [];
    txs.forEach((tx) => {
      const type = String(tx?.type || "").toUpperCase();
      if (type !== "DEPOSIT" && type !== "WITHDRAWAL") return;
      const date = safeNumber(tx?.date);
      if (date <= 0) return;
      const key = monthKeyFromUnix(date);
      cashByMonth.set(key, (cashByMonth.get(key) || 0) + safeNumber(tx?.amount));
    });
  });

  const monthKeys = new Set([...monthEndValues.keys(), ...cashByMonth.keys()]);
  const sortedKeys = Array.from(monthKeys).sort();

  let priorEnd = 0;
  const rows = sortedKeys.map((key) => {
    const endEntry = monthEndValues.get(key);
    const endValue = endEntry ? endEntry.value : priorEnd;
    const totalDelta = endValue - priorEnd;
    const cashDelta = cashByMonth.get(key) || 0;
    const investmentDelta = totalDelta - cashDelta;
    const [yearStr, monthStr] = key.split("-");
    priorEnd = endValue;
    return {
      key,
      year: Number(yearStr),
      month: Number(monthStr),
      cashDelta,
      investmentDelta,
      totalDelta
    };
  });

  return rows.reverse(); // newest first
}

async function refreshAllTransactionsForDashboard() {
  const accountPortfolios = (state.portfolios || []).filter((p) => !isWatchlistPortfolio(p));
  const results = await Promise.all(
    accountPortfolios.map(async (p) => {
      try {
        const payload = await apiGet(`/api/portfolios/${encodeURIComponent(p.name)}/transactions`);
        return [p.name, Array.isArray(payload?.transactions) ? payload.transactions : []];
      } catch (_) {
        return [p.name, []];
      }
    })
  );
  const next = {};
  results.forEach(([name, txs]) => {
    next[name] = txs;
  });
  state.allTransactions = next;
  renderDashboard();
}

function filterAccountsByType(portfolios, filter) {
  if (!filter || filter === "ALL") return portfolios;
  const target = String(filter).toUpperCase();
  return portfolios.filter((p) => String(p.type || "").toUpperCase() === target);
}

function sortAccountsForDashboard(portfolios) {
  const pinned = getPinnedAccounts();
  const typeRank = (type) => {
    const idx = TYPE_SORT_ORDER.indexOf(String(type || "").toUpperCase());
    return idx === -1 ? TYPE_SORT_ORDER.length : idx;
  };
  return portfolios.slice().sort((a, b) => {
    const aPinned = pinned.has(String(a.name)) ? 0 : 1;
    const bPinned = pinned.has(String(b.name)) ? 0 : 1;
    if (aPinned !== bPinned) return aPinned - bPinned;
    const rankDiff = typeRank(a.type) - typeRank(b.type);
    if (rankDiff !== 0) return rankDiff;
    return portfolioDisplayName(a.name).localeCompare(portfolioDisplayName(b.name));
  });
}

const state = {
  apiBase: localStorage.getItem(API_STORAGE_KEY) || "",
  portfolios: [],
  currentPortfolio: null,
  currentStocks: [],
  recentTransactions: [],
  liveRefreshTimer: null,
  dashboardLiveRefreshTimer: null,
  liveMarketState: "UNKNOWN",
  dashboardMarketState: "UNKNOWN",
  portfolioLastUpdated: 0,
  dashboardLastUpdated: 0,
  activeView: "dashboard",
  dashboardRefreshRequestSeq: 0,
  dashboardRefreshAppliedSeq: 0,
  portfolioLiveRefreshInFlight: false,
  dashboardLiveRefreshInFlight: false,
  periods: {
    dashboard: "6M",
    portfolio: "6M"
  },
  charts: {
    dashboard: null,
    portfolio: null,
    allocation: null
  },
  stocksSort: { key: null, dir: null },
  allTransactions: {},
  monthlyShowAll: false
};

const el = {
  breadcrumbs: document.getElementById("breadcrumbs"),
  flash: document.getElementById("flash"),
  dashboardView: document.getElementById("dashboardView"),
  portfolioView: document.getElementById("portfolioView"),
  portfolioName: document.getElementById("portfolioName"),
  portfolioType: document.getElementById("portfolioType"),
  portfolioMetrics: document.getElementById("portfolioMetrics"),
  stockCount: document.getElementById("stockCount"),
  addWatchlistSymbolBtn: document.getElementById("addWatchlistSymbolBtn"),
  stocksList: document.getElementById("stocksList"),
  recentTransactions: document.getElementById("recentTransactions"),
  transactionsDialog: document.getElementById("transactionsDialog"),
  transactionsHistory: document.getElementById("transactionsHistory"),
  viewAllTransactionsBtn: document.getElementById("viewAllTransactionsBtn"),
  openTransactionDialogBtn: document.getElementById("openTransactionDialogBtn"),
  transactionDialog: document.getElementById("transactionDialog"),
  transactionForm: document.getElementById("transactionForm"),
  transactionType: document.getElementById("transactionType"),
  transactionTicker: document.getElementById("transactionTicker"),
  transactionShares: document.getElementById("transactionShares"),
  transactionPrice: document.getElementById("transactionPrice"),
  transactionAmount: document.getElementById("transactionAmount"),
  transactionDate: document.getElementById("transactionDate"),
  transactionNotes: document.getElementById("transactionNotes"),
  transactionSubmitBtn: document.getElementById("transactionSubmitBtn"),
  groupTicker: document.getElementById("groupTicker"),
  groupShares: document.getElementById("groupShares"),
  groupPrice: document.getElementById("groupPrice"),
  groupAmount: document.getElementById("groupAmount"),
  stockDialog: document.getElementById("stockDialog"),
  stockDialogTitle: document.getElementById("stockDialogTitle"),
  stockDialogBody: document.getElementById("stockDialogBody"),
  createPortfolioDialog: document.getElementById("createPortfolioDialog"),
  createPortfolioForm: document.getElementById("createPortfolioForm"),
  createPortfolioName: document.getElementById("createPortfolioName"),
  createPortfolioType: document.getElementById("createPortfolioType"),
  createPortfolioCapitalRow: document.getElementById("createPortfolioCapitalRow"),
  createPortfolioCapital: document.getElementById("createPortfolioCapital"),
  createPortfolioCurrencyRow: document.getElementById("createPortfolioCurrencyRow"),
  createPortfolioCurrency: document.getElementById("createPortfolioCurrency"),
  createPortfolioCurrencyOtherRow: document.getElementById("createPortfolioCurrencyOtherRow"),
  createPortfolioCurrencyOther: document.getElementById("createPortfolioCurrencyOther"),
  createPortfolioSubmitBtn: document.getElementById("createPortfolioSubmitBtn"),
  deleteAccountBtn: document.getElementById("deleteAccountBtn"),
  deleteAccountDialog: document.getElementById("deleteAccountDialog"),
  deleteAccountName: document.getElementById("deleteAccountName"),
  deleteAccountCancelBtn: document.getElementById("deleteAccountCancelBtn"),
  deleteAccountConfirmBtn: document.getElementById("deleteAccountConfirmBtn"),
  backToDashBtn: document.getElementById("backToDashBtn"),
  apiConfigToggle: document.getElementById("apiConfigToggle"),
  apiConfigPanel: document.getElementById("apiConfigPanel"),
  apiConfigForm: document.getElementById("apiConfigForm"),
  apiBaseInput: document.getElementById("apiBaseInput"),
  apiResetBtn: document.getElementById("apiResetBtn"),
  apiStatus: document.getElementById("apiStatus"),
  portfolioPeriodSelect: document.getElementById("portfolioPeriodSelect"),
  marketStateChip: document.getElementById("marketStateChip"),
  portfolioLastUpdatedChip: document.getElementById("portfolioLastUpdatedChip"),
  portfolioChangeChip: document.getElementById("portfolioChangeChip"),
  portfolioChart: document.getElementById("portfolioChart"),
  portfolioAllocationChart: document.getElementById("portfolioAllocationChart"),
  allocationCoverageChip: document.getElementById("allocationCoverageChip"),
  allocationEmptyState: document.getElementById("allocationEmptyState")
};

const PERIOD_OPTIONS = ["1M", "3M", "6M", "1Y", "3Y", "ALL"];

const LIVE_REFRESH_INTERVAL_MS = 15000;
const LIVE_REFRESH_REQUEST_TIMEOUT_MS = 10000;

function unixNow() {
  return Math.floor(Date.now() / 1000);
}

function fallbackMarketStateNowET() {
  const formatter = new Intl.DateTimeFormat("en-US", {
    timeZone: "America/New_York",
    weekday: "short",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false
  });

  const parts = formatter.formatToParts(new Date());
  const partValue = (type) => parts.find((part) => part.type === type)?.value || "";
  const weekday = partValue("weekday");
  const hour = safeNumber(partValue("hour"));
  const minute = safeNumber(partValue("minute"));

  const isWeekday = weekday === "Mon" || weekday === "Tue" || weekday === "Wed" || weekday === "Thu" || weekday === "Fri";
  if (!isWeekday) {
    return "CLOSED";
  }

  const totalMinutes = hour * 60 + minute;
  if (totalMinutes < 4 * 60 || totalMinutes >= 20 * 60) {
    return "CLOSED";
  }

  if (totalMinutes < 9 * 60 + 30) {
    return "PRE";
  }

  if (totalMinutes < 16 * 60) {
    return "REGULAR";
  }

  return "POST";
}

function normalizeMarketState(rawState) {
  const normalized = String(rawState || "").trim().toUpperCase().replaceAll("-", "_").replaceAll(" ", "_");
  if (!normalized) {
    return "UNKNOWN";
  }

  if (normalized === "OPEN" || normalized === "TRADING" || normalized === "OPEN_MARKET") {
    return "REGULAR";
  }

  if (normalized.includes("REGULAR")) {
    return "REGULAR";
  }

  if (normalized === "PRE" || normalized === "PREPRE" || normalized.startsWith("PRE")) {
    return "PRE";
  }

  if (normalized === "POST" || normalized === "POSTPOST" || normalized.startsWith("POST")) {
    return "POST";
  }

  if (normalized.includes("CLOSED")) {
    return "CLOSED";
  }

  return normalized;
}

function isLiveMarketSession(marketState) {
  const normalized = normalizeMarketState(marketState);
  return normalized === "REGULAR" || normalized === "PRE" || normalized === "POST";
}

function isMarketOpen(marketState) {
  return normalizeMarketState(marketState) === "REGULAR";
}

function effectiveLiveMarketState() {
  return state.liveMarketState && state.liveMarketState !== "UNKNOWN"
    ? state.liveMarketState
    : fallbackMarketStateNowET();
}

function effectiveDashboardMarketState() {
  return state.dashboardMarketState && state.dashboardMarketState !== "UNKNOWN"
    ? state.dashboardMarketState
    : fallbackMarketStateNowET();
}

function resolveMarketState(payload) {
  const entries = Array.isArray(payload?.prices) ? payload.prices : [];
  for (const entry of entries) {
    const candidate = normalizeMarketState(entry?.market_state);
    if (candidate === "REGULAR") {
      return candidate;
    }
  }
  for (const entry of entries) {
    const candidate = normalizeMarketState(entry?.market_state);
    if (candidate === "PRE" || candidate === "POST") {
      return candidate;
    }
  }

  const topLevel = normalizeMarketState(payload?.market_state);
  if (topLevel === "REGULAR" || topLevel === "PRE" || topLevel === "POST") {
    return topLevel;
  }

  if (topLevel === "CLOSED") {
    // Some providers omit per-ticker marketState while still returning fresh quotes.
    // In that case, infer session from ET clock instead of forcing CLOSED.
    if (latestAsOfFromPayload(payload) > 0 || liveQuoteCountFromPayload(payload) > 0) {
      return fallbackMarketStateNowET();
    }
    return "CLOSED";
  }

  for (const entry of entries) {
    const candidate = normalizeMarketState(entry?.market_state);
    if (candidate && candidate !== "UNKNOWN") {
      return candidate;
    }
  }

  return fallbackMarketStateNowET();
}

function latestAsOfFromPayload(payload) {
  const entries = Array.isArray(payload?.prices) ? payload.prices : [];
  let maxAsOf = 0;
  entries.forEach((entry) => {
    maxAsOf = Math.max(maxAsOf, safeNumber(entry?.as_of));
  });
  return maxAsOf > 0 ? maxAsOf : 0;
}

function liveQuoteCountFromPayload(payload) {
  const entries = Array.isArray(payload?.portfolios) ? payload.portfolios : [];
  return entries.reduce((count, entry) => count + (safeNumber(entry?.quote_count) > 0 ? 1 : 0), 0);
}

function formatUpdatedLabel(unixSeconds) {
  if (!unixSeconds || unixSeconds <= 0) {
    return "Updated: n/a";
  }

  const formatted = new Date(unixSeconds * 1000).toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  });
  return `Updated: ${formatted}`;
}

function setMarketStateChip(marketState) {
  const normalized = normalizeMarketState(marketState);
  state.liveMarketState = normalized;

  if (!el.marketStateChip) {
    return;
  }

  const toneClass = normalized === "REGULAR"
    ? "chip-positive"
    : normalized === "PRE" || normalized === "POST" || normalized === "PREPRE"
      ? "chip-neutral"
      : "chip-negative";

  el.marketStateChip.className = `chip ${toneClass}`;
  el.marketStateChip.textContent = `Market: ${typeLabel(normalized)}`;
}

function setDashboardMarketStateChip(marketState) {
  const normalized = normalizeMarketState(marketState);
  state.dashboardMarketState = normalized;

  const chip = document.getElementById("dashboardMarketStateChip");
  if (!chip) {
    return;
  }

  const toneClass = normalized === "REGULAR"
    ? "chip-positive"
    : normalized === "PRE" || normalized === "POST" || normalized === "PREPRE"
      ? "chip-neutral"
      : "chip-negative";

  chip.className = `chip ${toneClass}`;
  chip.textContent = `Market: ${typeLabel(normalized)}`;
}

function setPortfolioLastUpdatedChip(unixSeconds) {
  state.portfolioLastUpdated = safeNumber(unixSeconds);
  if (!el.portfolioLastUpdatedChip) {
    return;
  }

  if (state.portfolioLastUpdated <= 0) {
    el.portfolioLastUpdatedChip.hidden = true;
    return;
  }

  el.portfolioLastUpdatedChip.hidden = false;

  el.portfolioLastUpdatedChip.className = "chip chip-neutral";
  el.portfolioLastUpdatedChip.textContent = formatUpdatedLabel(state.portfolioLastUpdated);
}

function setDashboardLastUpdatedChip(unixSeconds) {
  state.dashboardLastUpdated = safeNumber(unixSeconds);

  const chip = document.getElementById("dashboardLastUpdatedChip");
  if (!chip) {
    return;
  }

  if (state.dashboardLastUpdated <= 0) {
    chip.hidden = true;
    return;
  }

  chip.hidden = false;

  chip.className = "chip chip-neutral";
  chip.textContent = formatUpdatedLabel(state.dashboardLastUpdated);
}

function setActiveView(view) {
  const showDashboardView = view === "dashboard";
  state.activeView = showDashboardView ? "dashboard" : "portfolio";

  el.dashboardView.classList.toggle("is-active", showDashboardView);
  el.portfolioView.classList.toggle("is-active", !showDashboardView);

  el.dashboardView.hidden = !showDashboardView;
  el.portfolioView.hidden = showDashboardView;
}

function apiUrl(path) {
  if (!state.apiBase) {
    return path;
  }
  return `${state.apiBase}${path}`;
}

async function apiGet(path) {
  const response = await fetch(apiUrl(path));
  const data = await response.json();
  if (!response.ok) {
    throw new Error(data.error || `Request failed: ${response.status}`);
  }
  return data;
}

async function apiGetWithTimeout(path, timeoutMs) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(apiUrl(path), { signal: controller.signal });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Request failed: ${response.status}`);
    }
    return data;
  } catch (error) {
    if (error?.name === "AbortError") {
      throw new Error("Live request timed out");
    }
    throw error;
  } finally {
    clearTimeout(timeout);
  }
}

async function apiPost(path, body) {
  const response = await fetch(apiUrl(path), {
    method: "POST",
    headers: {
      "Content-Type": "application/json"
    },
    body: JSON.stringify(body)
  });

  const data = await response.json();
  if (!response.ok) {
    throw new Error(data.error || `Request failed: ${response.status}`);
  }

  return data;
}

async function apiDelete(path) {
  const response = await fetch(apiUrl(path), {
    method: "DELETE"
  });

  const data = await response.json();
  if (!response.ok) {
    throw new Error(data.error || `Request failed: ${response.status}`);
  }

  return data;
}

function currency(value) {
  return new Intl.NumberFormat(undefined, {
    style: "currency",
    currency: "USD",
    maximumFractionDigits: 2
  }).format(value || 0);
}

function currencyIn(value, code) {
  const ccy = String(code || "USD").toUpperCase();
  try {
    return new Intl.NumberFormat(undefined, {
      style: "currency",
      currency: ccy,
      maximumFractionDigits: 2
    }).format(value || 0);
  } catch (_err) {
    // Unknown ISO code — fall back to a number with the code appended.
    const formatted = new Intl.NumberFormat(undefined, { maximumFractionDigits: 2 }).format(value || 0);
    return `${formatted} ${ccy}`;
  }
}

function safeNumber(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : 0;
}

function compactCurrency(value) {
  return new Intl.NumberFormat(undefined, {
    style: "currency",
    currency: "USD",
    notation: "compact",
    maximumFractionDigits: 1
  }).format(value || 0);
}

function sharesFormat(value) {
  return new Intl.NumberFormat(undefined, {
    maximumFractionDigits: 4
  }).format(value || 0);
}

function asDate(unixSeconds) {
  return new Date((unixSeconds || 0) * 1000);
}

function dateLabel(unixSeconds) {
  return asDate(unixSeconds).toLocaleDateString(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric"
  });
}

function percentage(value) {
  const sign = value > 0 ? "+" : "";
  return `${sign}${value.toFixed(2)}%`;
}

function signedCurrency(value) {
  const amount = safeNumber(value);
  const sign = amount > 0 ? "+" : "";
  return `${sign}${currency(amount)}`;
}

function changeLabel(amount, percent) {
  const changeAmount = safeNumber(amount);
  const changePercent = safeNumber(percent);
  return `${signedCurrency(changeAmount)} (${percentage(changePercent)})`;
}

function normalizeStocks(rawStocks) {
  if (!Array.isArray(rawStocks)) {
    return [];
  }

  return rawStocks
    .filter((stock) => stock && typeof stock === "object" && typeof stock.ticker === "string")
    .map((stock) => ({
      ...stock,
      ticker: String(stock.ticker),
      company_name: String(stock.company_name || ""),
      shares_owned: safeNumber(stock.shares_owned),
      average_purchase_price: safeNumber(stock.average_purchase_price),
      last_updated: safeNumber(stock.last_updated),
      latest_close_price: safeNumber(stock.latest_close_price),
      latest_close_date: safeNumber(stock.latest_close_date),
      previous_close_price: safeNumber(stock.previous_close_price),
      day_change_amount: safeNumber(stock.day_change_amount),
      day_change_percent: safeNumber(stock.day_change_percent),
      position_day_change_amount: safeNumber(stock.position_day_change_amount),
      position_market_value: safeNumber(stock.position_market_value),
      event_count: safeNumber(stock.event_count),
      recent_events: Array.isArray(stock.recent_events) ? stock.recent_events : []
    }));
}

function typeLabel(type) {
  return (type || "").replaceAll("_", " ");
}

function portfolioDisplayName(name) {
  return String(name || "")
    .replaceAll("_", " ")
    .replace(/\s+/g, " ")
    .trim();
}

function showFlash(message) {
  el.flash.textContent = message;
  el.flash.hidden = false;
}

function hideFlash() {
  el.flash.hidden = true;
  el.flash.textContent = "";
}

function setBreadcrumbs(items) {
  el.breadcrumbs.innerHTML = "";
  items.forEach((item, index) => {
    if (index > 0) {
      const sep = document.createElement("span");
      sep.textContent = "/";
      el.breadcrumbs.appendChild(sep);
    }

    if (item.onClick) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = item.label;
      button.addEventListener("click", item.onClick);
      el.breadcrumbs.appendChild(button);
    } else {
      const span = document.createElement("span");
      span.textContent = item.label;
      el.breadcrumbs.appendChild(span);
    }
  });
}

function metricCard(label, value, sub = "") {
  return `<article class="metric fade-up"><span class="label">${label}</span><strong class="value">${value}</strong>${sub ? `<div class="sub">${sub}</div>` : ""}</article>`;
}

function mergeDailySeries(portfolios) {
  // Each portfolio reports a daily value only on days it had an event
  // (a market close for brokerages, a transaction or "today" anchor for cash,
  // every day for crypto). A plain per-day sum therefore drops to a partial
  // total on days when some portfolios are silent — most visibly on weekends
  // and market holidays, when brokerages contribute nothing. To produce a
  // faithful total-asset trend we carry each portfolio's most recent value
  // forward across the union of days.
  const dayToSecondsAtClose = (day) => day * 86400 + 16 * 3600;

  const seriesPerPortfolio = (portfolios || []).map((portfolio) => {
    const points = (portfolio.daily_values || [])
      .map((point) => ({
        date: safeNumber(point?.date),
        value: safeNumber(point?.value)
      }))
      .filter((point) => point.date > 0)
      .sort((a, b) => a.date - b.date);

    // Collapse to one entry per day bucket; later entries (e.g. live-price
    // patches with the same day but a later timestamp) win.
    const byDay = new Map();
    points.forEach((point) => {
      byDay.set(Math.floor(point.date / 86400), point);
    });

    return Array.from(byDay.entries())
      .map(([day, point]) => ({ day, value: point.value }))
      .sort((a, b) => a.day - b.day);
  });

  const allDays = new Set();
  seriesPerPortfolio.forEach((series) => series.forEach((point) => allDays.add(point.day)));
  const sortedDays = Array.from(allDays).sort((a, b) => a - b);

  const cursors = seriesPerPortfolio.map(() => -1);
  const out = [];

  sortedDays.forEach((day) => {
    let total = 0;
    let anyContrib = false;
    seriesPerPortfolio.forEach((series, i) => {
      let c = cursors[i];
      while (c + 1 < series.length && series[c + 1].day <= day) {
        c += 1;
      }
      cursors[i] = c;
      if (c >= 0) {
        total += series[c].value;
        anyContrib = true;
      }
    });
    if (anyContrib) {
      out.push({ date: dayToSecondsAtClose(day), value: total });
    }
  });

  return out;
}

function daysForPeriod(period) {
  switch (period) {
    case "1M": return 30;
    case "3M": return 90;
    case "6M": return 180;
    case "1Y": return 365;
    case "3Y": return 1095;
    default: return null;
  }
}

function filterPointsByPeriod(points, period) {
  const normalized = Array.isArray(points)
    ? points
        .map((p) => ({ date: safeNumber(p?.date), value: safeNumber(p?.value) }))
        .filter((p) => p.date > 0)
        .sort((a, b) => a.date - b.date)
    : [];

  if (!normalized.length) {
    return [];
  }

  const periodDays = daysForPeriod(period);
  if (!periodDays) {
    return normalized;
  }

  const latestDate = normalized[normalized.length - 1].date;
  const cutoff = latestDate - periodDays * 86400;
  const filtered = normalized.filter((p) => p.date >= cutoff);

  return filtered.length ? filtered : [normalized[normalized.length - 1]];
}

function previousDistinctDayValue(points, referenceUnix) {
  const normalized = Array.isArray(points)
    ? points
        .map((p) => ({ date: safeNumber(p?.date), value: safeNumber(p?.value) }))
        .filter((p) => p.date > 0)
        .sort((a, b) => a.date - b.date)
    : [];

  if (!normalized.length) {
    return null;
  }

  const referenceBucket = Math.floor(safeNumber(referenceUnix) / 86400);
  for (let i = normalized.length - 1; i >= 0; i -= 1) {
    const point = normalized[i];
    const pointBucket = Math.floor(point.date / 86400);
    if (pointBucket < referenceBucket) {
      return point.value;
    }
  }

  return null;
}

function computeTrend(points) {
  if (!Array.isArray(points) || points.length < 2) {
    return { hasTrend: false, percentChange: 0 };
  }

  const startValue = safeNumber(points[0].value);
  const endValue = safeNumber(points[points.length - 1].value);
  if (Math.abs(startValue) < 1e-9) {
    return { hasTrend: false, percentChange: 0 };
  }

  const percentChange = ((endValue - startValue) / Math.abs(startValue)) * 100;
  return { hasTrend: true, percentChange };
}

function trendColor(percentChange) {
  if (percentChange > 0.0001) {
    return "#60d394";
  }
  if (percentChange < -0.0001) {
    return "#ee6055";
  }
  return "#6a7a76";
}

function trendChipClass(percentChange, hasTrend) {
  if (!hasTrend) {
    return "chip-neutral";
  }
  if (percentChange > 0.0001) {
    return "chip-positive";
  }
  if (percentChange < -0.0001) {
    return "chip-negative";
  }
  return "chip-neutral";
}

function stockPerformance(stock) {
  const averagePrice = safeNumber(stock?.average_purchase_price);
  const latestPrice = safeNumber(stock?.latest_close_price);
  const sharesOwned = safeNumber(stock?.shares_owned);
  const perShareChange = latestPrice - averagePrice;
  const totalChange = perShareChange * sharesOwned;
  const percentChange = averagePrice > 0 ? (perShareChange / averagePrice) * 100 : 0;
  const isZeroCostBasisPosition = sharesOwned > 0 && averagePrice <= 0 && latestPrice > 0;

  return {
    averagePrice,
    latestPrice,
    sharesOwned,
    perShareChange,
    totalChange,
    percentChange,
    hasBasis: averagePrice > 0 && sharesOwned > 0,
    isZeroCostBasisPosition
  };
}

function stockToneClass(totalChange, hasBasis, isZeroCostBasisPosition = false) {
  if (isZeroCostBasisPosition) {
    return "stock-card-positive";
  }

  if (!hasBasis) {
    return "stock-card-neutral";
  }
  if (totalChange > 0.0001) {
    return "stock-card-positive";
  }
  if (totalChange < -0.0001) {
    return "stock-card-negative";
  }
  return "stock-card-neutral";
}

function isWatchlistPortfolio(portfolio) {
  return String(portfolio?.type || "").toUpperCase() === "WATCHLIST";
}

function isCashPortfolio(portfolio) {
  return String(portfolio?.type || "").toUpperCase() === "CASH";
}

function isCryptoPortfolio(portfolio) {
  return String(portfolio?.type || "").toUpperCase() === "CRYPTO";
}

function periodSelectMarkup(selectId, selectedPeriod) {
  const options = PERIOD_OPTIONS
    .map((period) => `<option value="${period}"${period === selectedPeriod ? " selected" : ""}>${period}</option>`)
    .join("");
  return `<select id="${selectId}" class="graph-period-select" aria-label="Chart period">${options}</select>`;
}

function normalizePortfolios(rawPortfolios) {
  if (!Array.isArray(rawPortfolios)) {
    return [];
  }

  return rawPortfolios
    .filter((p) => p && typeof p === "object" && typeof p.name === "string" && p.name.trim() !== "")
    .map((p) => ({
      ...p,
      name: String(p.name),
      type: String(p.type || "UNKNOWN"),
      currency: String(p.currency || "USD").toUpperCase(),
      fx_to_usd: Number.isFinite(Number(p.fx_to_usd)) && Number(p.fx_to_usd) > 0 ? Number(p.fx_to_usd) : 1,
      available_capital: safeNumber(p.available_capital),
      estimated_total_value: safeNumber(p.estimated_total_value),
      reported_total_value: safeNumber(p.reported_total_value),
      day_change_amount: safeNumber(p.day_change_amount),
      day_change_percent: safeNumber(p.day_change_percent),
      stock_count: safeNumber(p.stock_count),
      transaction_count: safeNumber(p.transaction_count),
      daily_values: Array.isArray(p.daily_values)
        ? p.daily_values.map((point) => ({
            date: safeNumber(point?.date),
            value: safeNumber(point?.value),
            last_updated: safeNumber(point?.last_updated)
          }))
        : []
    }));
}

function destroyChart(name) {
  const chart = state.charts[name];
  if (chart) {
    chart.destroy();
    state.charts[name] = null;
  }
}

const themeTooltipStyle = {
  backgroundColor: "#f5f5f5",
  titleColor: "#222",
  bodyColor: "#222",
  borderColor: "#222",
  borderWidth: 1,
  cornerRadius: 0,
  displayColors: false,
  padding: 10,
  titleFont: { family: '"Noto Serif", Georgia, serif', weight: "500", size: 12 },
  bodyFont: { family: '"Noto Serif", Georgia, serif', weight: "400", size: 12 }
};

function createLineChart(canvas, points, label, color) {
  if (!canvas || !window.Chart) {
    return null;
  }

  const normalizedPoints = Array.isArray(points) ? points : [];
  const showMarkers = normalizedPoints.length <= 2;

  return new window.Chart(canvas, {
    type: "line",
    data: {
      labels: normalizedPoints.map((p) => dateLabel(p.date)),
      datasets: [
        {
          label,
          data: normalizedPoints.map((p) => p.value),
          borderColor: color,
          borderWidth: 2,
          pointRadius: showMarkers ? 3 : 0,
          pointHoverRadius: showMarkers ? 5 : 3,
          fill: true,
          tension: 0.26,
          backgroundColor: `${color}22`
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: {
        intersect: false,
        mode: "index"
      },
      plugins: {
        legend: {
          display: false
        },
        tooltip: {
          ...themeTooltipStyle,
          callbacks: {
            label: (ctx) => currency(ctx.parsed.y)
          }
        }
      },
      scales: {
        x: {
          ticks: {
            maxTicksLimit: 6,
            color: "#555"
          },
          grid: {
            display: false
          }
        },
        y: {
          ticks: {
            callback: (v) => compactCurrency(v),
            color: "#555"
          },
          grid: {
            color: "rgba(34, 34, 34, 0.08)"
          }
        }
      }
    }
  });
}

function createPieChart(canvas, labels, values, colors) {
  if (!canvas || !window.Chart) {
    return null;
  }

  return new window.Chart(canvas, {
    type: "pie",
    data: {
      labels,
      datasets: [
        {
          data: values,
          backgroundColor: colors,
          borderColor: "#222",
          borderWidth: 1,
          hoverOffset: 8
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: {
        legend: {
          position: "right",
          labels: {
            color: "#222",
            usePointStyle: true,
            boxWidth: 10,
            boxHeight: 10,
            font: {
              size: 12,
              weight: "500"
            }
          }
        },
        tooltip: {
          ...themeTooltipStyle,
          callbacks: {
            label: (ctx) => {
              const dataset = ctx.dataset?.data || [];
              const total = dataset.reduce((sum, value) => sum + safeNumber(value), 0);
              const current = safeNumber(ctx.parsed);
              const percent = total > 0 ? (current / total) * 100 : 0;
              return `${ctx.label}: ${currency(current)} (${percent.toFixed(1)}%)`;
            }
          }
        }
      }
    }
  });
}

function allocationPalette(size) {
  const themePalette = [
    "#3695c8",
    "#60d394",
    "#ee6055",
    "#f5b04d",
    "#9c89b8",
    "#1b8b68",
    "#c34b3d",
    "#2d6da3",
    "#a86bca",
    "#d97706",
    "#34a08c",
    "#8a6dbf"
  ];

  return Array.from({ length: Math.max(size, 0) }, (_, index) => themePalette[index % themePalette.length]);
}

function renderTransactionTable(transactions, options = {}) {
  const { showNotes = false } = options;
  if (!transactions.length) {
    return "<p>No transactions yet.</p>";
  }

  const rows = transactions
    .map((tx) => {
      const amountClass = tx.amount >= 0 ? "positive" : "negative";
      const symbol = tx.stock_symbol ? tx.stock_symbol : "-";
      const shares = tx.shares ? sharesFormat(tx.shares) : "-";
      const isSell = String(tx.type || "").toUpperCase() === "SELL_STOCK" || String(tx.type || "").toUpperCase() === "SELL";
      const hasProfit = isSell && Number.isFinite(tx.realized_profit);
      const profitMarkup = hasProfit
        ? ` <span class="${tx.realized_profit >= 0 ? "positive" : "negative"}">(${tx.realized_profit >= 0 ? "+" : ""}${currency(tx.realized_profit)})</span>`
        : "";
      const notesCell = showNotes ? `<td>${tx.notes || "-"}</td>` : "";
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${typeLabel(tx.type)}${profitMarkup}</td>
        <td>${symbol}</td>
        <td>${shares}</td>
        <td class="${amountClass}">${currency(tx.amount)}</td>
        ${notesCell}
      </tr>`;
    })
    .join("");

  const notesHeader = showNotes ? "<th>Notes</th>" : "";
  return `<table class="tx-table"><thead>
    <tr>
      <th>Date</th>
      <th>Type</th>
      <th>Ticker</th>
      <th>Shares</th>
      <th>Amount</th>
      ${notesHeader}
    </tr>
  </thead><tbody>${rows}</tbody></table>`;
}

function renderDashboard() {
  const portfolios = normalizePortfolios(state.portfolios);
  state.portfolios = portfolios;

  const accountPortfolios = portfolios.filter((p) => !isWatchlistPortfolio(p));

  const totalAssets = accountPortfolios.reduce((sum, p) => sum + (p.estimated_total_value || 0), 0);
  // Convert each account's native-currency cash to USD before summing so foreign
  // cash accounts contribute correctly to the dashboard total.
  const totalCash = accountPortfolios.reduce((sum, p) => {
    const fx = (p.currency && p.currency !== "USD") ? (p.fx_to_usd || 1) : 1;
    return sum + (p.available_capital || 0) * fx;
  }, 0);
  const totalStocks = accountPortfolios.reduce((sum, p) => sum + (p.stock_count || 0), 0);
  const aggregateTrend = mergeDailySeries(accountPortfolios);
  // Day Change tracks the chart: live total vs. the most recent prior-day
  // snapshot. This includes deposits/withdrawals/transfers, matching what
  // the line on the dashboard chart reflects.
  const nowTs = Math.floor(Date.now() / 1000);
  const previousDayTotal = previousDistinctDayValue(aggregateTrend, nowTs);
  const totalDayChange = (Number.isFinite(previousDayTotal) && previousDayTotal !== null)
    ? totalAssets - previousDayTotal
    : 0;
  const totalDayChangePercent = (Number.isFinite(previousDayTotal) && previousDayTotal > 0)
    ? (totalDayChange / previousDayTotal) * 100
    : 0;

  const dashboardMetrics = `<section class="metric-grid">
    ${metricCard("Total Assets", currency(totalAssets), `${accountPortfolios.length} account portfolio${accountPortfolios.length === 1 ? "" : "s"}`)}
    ${metricCard("Day Change", signedCurrency(totalDayChange), `${percentage(totalDayChangePercent)} vs previous close`)}
    ${metricCard("Total Available Cash", currency(totalCash), "Across account portfolios")}
    ${metricCard("Total Positions", String(totalStocks), "Stocks and crypto across accounts")}
  </section>`;

  const currentFilter = getAccountFilter();
  const filteredPortfolios = filterAccountsByType(portfolios, currentFilter);
  const visiblePortfolios = sortAccountsForDashboard(filteredPortfolios);

  const filterOptions = [
    { value: "ALL", label: "All Types" },
    { value: "BROKERAGE", label: "Brokerage" },
    { value: "ROTH_IRA", label: "Roth IRA" },
    { value: "TRADITIONAL_IRA", label: "Traditional IRA" },
    { value: "CRYPTO", label: "Crypto" },
    { value: "CASH", label: "Cash" },
    { value: "WATCHLIST", label: "Watchlist" }
  ];
  const filterMarkup = `<select id="accountFilterSelect" class="graph-period-select" aria-label="Filter accounts by type">${
    filterOptions
      .map((opt) => `<option value="${opt.value}"${opt.value === currentFilter ? " selected" : ""}>${opt.label}</option>`)
      .join("")
  }</select>`;

  const cards = visiblePortfolios
    .map((p, i) => {
      const animation = `style="animation-delay:${Math.min(i * 60, 360)}ms"`;
      const pinned = isAccountPinned(p.name);
      const pinIcon = pinned ? "★" : "☆";
      const pinTitle = pinned ? "Unpin" : "Pin to top";
      const header = `<div class="stock-top">
          <strong>${portfolioDisplayName(p.name)}</strong>
          <span class="card-header-right">
            <span class="chip">${typeLabel(p.type)}</span>
            <button type="button" class="pin-btn${pinned ? " is-pinned" : ""}" data-pin-name="${encodeURIComponent(p.name)}" title="${pinTitle}" aria-label="${pinTitle}">${pinIcon}</button>
          </span>
        </div>`;
      let body;
      if (isWatchlistPortfolio(p)) {
        body = `<div class="sub">Watchlist only</div>
                <div class="sub">${p.stock_count} symbols tracked</div>`;
      } else if (isCashPortfolio(p)) {
        const ccy = p.currency || "USD";
        const native = currencyIn(p.available_capital, ccy);
        const usdEquiv = (ccy !== "USD")
          ? `<div class="sub">${currency((p.available_capital || 0) * (p.fx_to_usd || 1))} USD</div>`
          : "";
        body = `<div>${native}</div>
                ${usdEquiv}
                <div class="sub">${p.transaction_count} transaction${p.transaction_count === 1 ? "" : "s"}</div>`;
      } else {
        body = `<div>${currency(p.estimated_total_value)}</div>
                <div class="sub">Day: ${changeLabel(p.day_change_amount, p.day_change_percent)}</div>
                <div class="sub">Cash: ${currency(p.available_capital)}</div>
                <div class="sub">${p.stock_count} stocks • ${p.transaction_count} transactions</div>`;
      }
      return `<article class="portfolio-card fade-up" data-name="${encodeURIComponent(p.name)}" ${animation}>
        ${header}
        ${body}
      </article>`;
    })
    .join("");

  const emptyState = currentFilter !== "ALL"
    ? `<article class="panel empty-state fade-up">
        <h3>No accounts match this filter</h3>
        <p>Pick a different type from the dropdown or choose "All Types".</p>
      </article>`
    : `<article class="panel empty-state fade-up">
        <h3>No Account Data Yet</h3>
        <p>No accounts found. Click "New Account" to create one.</p>
      </article>`;

  const chartHostId = "dashboardChart";
  el.dashboardView.innerHTML = `
    ${dashboardMetrics}
    <article class="panel chart-panel fade-up">
      <div class="panel-head">
        <h3>Total Asset Trend</h3>
        <div class="chart-tools">
          ${periodSelectMarkup("dashboardPeriodSelect", state.periods.dashboard)}
          <span id="dashboardMarketStateChip" class="chip chip-neutral">Market: n/a</span>
          <span id="dashboardLastUpdatedChip" class="chip chip-neutral">Updated: n/a</span>
          <span id="dashboardChangeChip" class="chip chip-neutral">Change: n/a</span>
        </div>
      </div>
      <canvas id="${chartHostId}" aria-label="Total Asset Trend"></canvas>
    </article>
    <section>
      <div class="panel-head">
        <h3>Accounts</h3>
        <div class="chart-tools">
          ${filterMarkup}
          <button id="openPortfolioCreateDialogBtn" class="primary-btn" type="button">New Account</button>
        </div>
      </div>
      ${cards ? `<div class="dashboard-cards">${cards}</div>` : emptyState}
    </section>
    ${renderMonthlyActivity(portfolios, state.allTransactions)}
  `;

  const openPortfolioCreateDialogBtn = document.getElementById("openPortfolioCreateDialogBtn");
  if (openPortfolioCreateDialogBtn) {
    openPortfolioCreateDialogBtn.addEventListener("click", openCreatePortfolioDialog);
  }

  const accountFilterSelect = document.getElementById("accountFilterSelect");
  if (accountFilterSelect) {
    accountFilterSelect.addEventListener("change", (event) => {
      setAccountFilter(event.target.value);
      renderDashboard();
    });
  }

  document.querySelectorAll(".portfolio-card").forEach((card) => {
    card.addEventListener("click", (event) => {
      if (event.target.closest(".pin-btn")) {
        return;
      }
      const portfolioName = decodeURIComponent(card.dataset.name || "");
      openPortfolio(portfolioName);
    });
  });

  document.querySelectorAll(".pin-btn").forEach((btn) => {
    btn.addEventListener("click", (event) => {
      event.stopPropagation();
      const name = decodeURIComponent(btn.dataset.pinName || "");
      if (!name) return;
      togglePinnedAccount(name);
      renderDashboard();
    });
  });

  const monthlyToggleBtn = document.getElementById("monthlyToggleBtn");
  if (monthlyToggleBtn) {
    monthlyToggleBtn.addEventListener("click", () => {
      state.monthlyShowAll = !state.monthlyShowAll;
      renderDashboard();
    });
  }

  destroyChart("dashboard");
  const chartCanvas = document.getElementById(chartHostId);
  const dashboardPeriodSelect = document.getElementById("dashboardPeriodSelect");
  const dashboardChangeChip = document.getElementById("dashboardChangeChip");

  const drawDashboardChart = () => {
    const filtered = filterPointsByPeriod(aggregateTrend, state.periods.dashboard);
    const trend = computeTrend(filtered);
    const color = trendColor(trend.percentChange);

    dashboardChangeChip.className = `chip ${trendChipClass(trend.percentChange, trend.hasTrend)}`;
    dashboardChangeChip.textContent = trend.hasTrend
      ? `Change: ${percentage(trend.percentChange)}`
      : "Change: n/a";

    destroyChart("dashboard");
    state.charts.dashboard = createLineChart(chartCanvas, filtered, "Total Assets", color);
  };

  dashboardPeriodSelect.addEventListener("change", (event) => {
    state.periods.dashboard = event.target.value;
    drawDashboardChart();
  });

  try {
    drawDashboardChart();
    setDashboardMarketStateChip(state.dashboardMarketState);
    setDashboardLastUpdatedChip(state.dashboardLastUpdated);
  } catch (error) {
    showFlash(`Dashboard chart could not render: ${error.message}`);
  }
}

function renderPortfolioChart() {
  if (!state.currentPortfolio) {
    return;
  }

  const points = filterPointsByPeriod(state.currentPortfolio.daily_values || [], state.periods.portfolio);
  const trend = computeTrend(points);
  const color = trendColor(trend.percentChange);

  el.portfolioChangeChip.className = `chip ${trendChipClass(trend.percentChange, trend.hasTrend)}`;
  el.portfolioChangeChip.textContent = trend.hasTrend
    ? `Change: ${percentage(trend.percentChange)}`
    : "Change: n/a";

  destroyChart("portfolio");
  state.charts.portfolio = createLineChart(
    el.portfolioChart,
    points,
    `${portfolioDisplayName(state.currentPortfolio.name)} Value`,
    color
  );
}

function renderAllocationChart(stocks, portfolio) {
  const normalizedStocks = Array.isArray(stocks) ? stocks : [];
  const points = normalizedStocks
    .map((stock) => ({
      ticker: String(stock?.ticker || "").trim(),
      value: safeNumber(stock?.position_market_value)
    }))
    .filter((stock) => stock.ticker && stock.value > 0)
    .sort((a, b) => b.value - a.value);

  const sumValue = points.reduce((sum, stock) => sum + stock.value, 0);
  const portfolioEstimate = safeNumber(portfolio?.estimated_total_value);
  const coverage = portfolioEstimate > 0 ? (sumValue / portfolioEstimate) * 100 : 0;

  if (!points.length || sumValue <= 0) {
    el.portfolioAllocationChart.hidden = true;
    el.allocationEmptyState.hidden = false;
    el.allocationCoverageChip.className = "chip chip-neutral";
    el.allocationCoverageChip.textContent = "Coverage: n/a";
    destroyChart("allocation");
    return;
  }

  el.portfolioAllocationChart.hidden = false;
  el.allocationEmptyState.hidden = true;
  el.allocationCoverageChip.className = `chip ${trendChipClass(coverage - 100, true)}`;
  el.allocationCoverageChip.textContent = `Coverage: ${coverage.toFixed(1)}%`;

  destroyChart("allocation");
  state.charts.allocation = createPieChart(
    el.portfolioAllocationChart,
    points.map((point) => point.ticker),
    points.map((point) => point.value),
    allocationPalette(points.length)
  );
}

function enrichStockRow(stock) {
  const perf = stockPerformance(stock);
  return {
    ticker: String(stock?.ticker || ""),
    company_name: String(stock?.company_name || ""),
    shares_owned: safeNumber(stock?.shares_owned),
    average_purchase_price: safeNumber(stock?.average_purchase_price),
    latest_close_price: safeNumber(stock?.latest_close_price),
    day_change_amount: safeNumber(stock?.day_change_amount),
    day_change_percent: safeNumber(stock?.day_change_percent),
    position_day_change_amount: safeNumber(stock?.position_day_change_amount),
    position_market_value: safeNumber(stock?.position_market_value),
    latest_close_date: safeNumber(stock?.latest_close_date),
    totalChange: perf.totalChange,
    percentChange: perf.percentChange,
    hasBasis: perf.hasBasis,
    isZeroCostBasisPosition: perf.isZeroCostBasisPosition,
    _stock: stock
  };
}

function sortStockRows(rows, sort) {
  if (!sort?.key) return rows;
  const dir = sort.dir === "asc" ? 1 : -1;
  return [...rows].sort((a, b) => {
    const va = a[sort.key];
    const vb = b[sort.key];
    if (typeof va === "string" || typeof vb === "string") {
      return String(va || "").localeCompare(String(vb || "")) * dir;
    }
    return ((va ?? 0) - (vb ?? 0)) * dir;
  });
}

function pnlCellTone(value, hasBasis = true) {
  if (!hasBasis) return "stock-card-neutral";
  if (value > 0.0001) return "stock-card-positive";
  if (value < -0.0001) return "stock-card-negative";
  return "stock-card-neutral";
}

function holdingsTableColumns() {
  return [
    { key: "ticker", label: "Ticker", align: "left", render: (r) => `<td class="ticker"><strong>${r.ticker}</strong></td>` },
    { key: "company_name", label: "Company", align: "left", render: (r) => `<td class="company">${r.company_name}</td>` },
    { key: "shares_owned", label: "Shares", align: "right", render: (r) => `<td class="num">${sharesFormat(r.shares_owned)}</td>` },
    { key: "average_purchase_price", label: "Avg Cost", align: "right", render: (r) => `<td class="num">${currency(r.average_purchase_price)}</td>` },
    { key: "latest_close_price", label: "Last Price", align: "right", render: (r) => `<td class="num">${currency(r.latest_close_price)}</td>` },
    { key: "day_change_amount", label: "Day Δ$", align: "right", render: (r) => `<td class="num ${pnlCellTone(r.day_change_amount)}">${signedCurrency(r.day_change_amount)}</td>` },
    { key: "day_change_percent", label: "Day Δ%", align: "right", render: (r) => `<td class="num ${pnlCellTone(r.day_change_percent)}">${percentage(r.day_change_percent)}</td>` },
    { key: "position_day_change_amount", label: "Pos Day Δ", align: "right", render: (r) => `<td class="num ${pnlCellTone(r.position_day_change_amount)}">${signedCurrency(r.position_day_change_amount)}</td>` },
    { key: "position_market_value", label: "Mkt Value", align: "right", render: (r) => `<td class="num"><strong>${currency(r.position_market_value)}</strong></td>` },
    { key: "totalChange", label: "Total P/L", align: "right", render: (r) => `<td class="num ${pnlCellTone(r.totalChange, r.hasBasis)}">${r.hasBasis ? signedCurrency(r.totalChange) : "—"}</td>` },
    { key: "percentChange", label: "Total %", align: "right", render: (r) => `<td class="num ${pnlCellTone(r.percentChange, r.hasBasis)}">${r.hasBasis ? percentage(r.percentChange) : "—"}</td>` }
  ];
}

function watchlistTableColumns() {
  return [
    { key: "ticker", label: "Ticker", align: "left", render: (r) => `<td class="ticker"><strong>${r.ticker}</strong></td>` },
    { key: "company_name", label: "Company", align: "left", render: (r) => `<td class="company">${r.company_name}</td>` },
    { key: "latest_close_price", label: "Last Price", align: "right", render: (r) => `<td class="num">${currency(r.latest_close_price)}</td>` },
    { key: "latest_close_date", label: "As Of", align: "right", render: (r) => `<td class="num">${r.latest_close_date ? dateLabel(r.latest_close_date) : "n/a"}</td>` }
  ];
}

function renderPortfolioDetail(portfolio, stocks, recentTransactions) {
  const watchlist = isWatchlistPortfolio(portfolio);
  const cash = isCashPortfolio(portfolio);
  const crypto = isCryptoPortfolio(portfolio);
  const holdingNoun = crypto ? "coin" : (watchlist ? "symbol" : "stock");
  const visibleStocks = watchlist
    ? stocks
    : stocks.filter((stock) => safeNumber(stock?.shares_owned) > 0);

  el.portfolioName.textContent = portfolioDisplayName(portfolio.name);
  el.portfolioType.textContent = typeLabel(portfolio.type);
  if (cash) {
    el.stockCount.textContent = `${portfolio.transaction_count || 0} transaction${(portfolio.transaction_count || 0) === 1 ? "" : "s"}`;
  } else {
    el.stockCount.textContent = `${visibleStocks.length} ${holdingNoun}${visibleStocks.length === 1 ? "" : "s"}`;
  }

  const stocksHeading = document.getElementById("stocksPanelHeading");
  if (stocksHeading) {
    stocksHeading.textContent = crypto ? "Crypto Holdings" : (watchlist ? "Watchlist" : "Holdings");
  }
  const allocationHeading = document.getElementById("allocationPanelHeading");
  if (allocationHeading) {
    allocationHeading.textContent = crypto ? "Crypto Allocation" : "Allocation";
  }

  el.addWatchlistSymbolBtn.hidden = !watchlist;
  el.addWatchlistSymbolBtn.onclick = watchlist ? () => addWatchlistSymbol() : null;

  if (watchlist) {
    el.portfolioMetrics.innerHTML = "";
    el.portfolioMetrics.hidden = true;
  } else if (cash) {
    el.portfolioMetrics.hidden = false;
    const ccy = portfolio.currency || "USD";
    const balanceLabel = (ccy !== "USD")
      ? `Current Balance (${ccy})`
      : "Current Balance";
    const balanceValue = currencyIn(portfolio.available_capital, ccy);
    const usdSub = (ccy !== "USD")
      ? `${currency((portfolio.available_capital || 0) * (portfolio.fx_to_usd || 1))} USD @ ${Number(portfolio.fx_to_usd || 1).toFixed(4)}`
      : "";
    el.portfolioMetrics.innerHTML = [
      metricCard(balanceLabel, balanceValue, usdSub),
      metricCard("Day Change", signedCurrency(portfolio.day_change_amount), `${percentage(portfolio.day_change_percent)} vs previous close`),
      metricCard("Transactions", String(portfolio.transaction_count || 0))
    ].join("");
  } else {
    el.portfolioMetrics.hidden = false;
    const valueDelta = (portfolio.estimated_total_value || 0) - (portfolio.available_capital || 0);
    el.portfolioMetrics.innerHTML = [
      metricCard("Total", currency(portfolio.estimated_total_value)),
      metricCard("Day Change", signedCurrency(portfolio.day_change_amount), `${percentage(portfolio.day_change_percent)} vs previous close`),
      metricCard("Available Capital", currency(portfolio.available_capital)),
      metricCard("Position Value", currency(valueDelta), `${portfolio.transaction_count} transactions`)
    ].join("");
  }

  const recentTransactionsPanel = el.recentTransactions?.closest(".panel");
  if (recentTransactionsPanel) {
    recentTransactionsPanel.hidden = watchlist;
  }

  const stocksPanel = el.stocksList?.closest(".panel");
  if (stocksPanel) {
    stocksPanel.hidden = cash;
  }

  if (!cash) {
    const columns = watchlist ? watchlistTableColumns() : holdingsTableColumns();
    if (!state.stocksSort.key || !columns.find((c) => c.key === state.stocksSort.key)) {
      state.stocksSort = watchlist
        ? { key: "ticker", dir: "asc" }
        : { key: "position_market_value", dir: "desc" };
    }

    const enriched = visibleStocks.map((stock) => enrichStockRow(stock));
    const sortedRows = sortStockRows(enriched, state.stocksSort);

    const headerHTML = columns
      .map((col) => {
        const active = state.stocksSort.key === col.key;
        const arrow = active ? (state.stocksSort.dir === "asc" ? "▲" : "▼") : "";
        const cls = [
          col.align === "right" ? "num" : "",
          active ? "sorted" : ""
        ].filter(Boolean).join(" ");
        return `<th class="${cls}" data-key="${col.key}">${col.label}<span class="sort-arrow">${arrow}</span></th>`;
      })
      .join("");

    let bodyHTML;
    if (sortedRows.length === 0) {
      bodyHTML = `<tr><td colspan="${columns.length}" class="empty">No stock data available.</td></tr>`;
    } else {
      bodyHTML = sortedRows
        .map((row) => {
          const stock = row._stock;
          const tone = watchlist
            ? "stock-card-neutral"
            : stockToneClass(row.totalChange, row.hasBasis, row.isZeroCostBasisPosition);
          const cells = columns.map((col) => col.render(row)).join("");
          return `<tr class="stock-row ${tone}" data-ticker="${stock.ticker}">${cells}</tr>`;
        })
        .join("");
    }

    el.stocksList.innerHTML = `<table class="stocks-table"><thead><tr>${headerHTML}</tr></thead><tbody>${bodyHTML}</tbody></table>`;

    el.stocksList.querySelectorAll("thead th").forEach((th) => {
      th.addEventListener("click", () => {
        const key = th.dataset.key;
        if (!key) return;
        if (state.stocksSort.key === key) {
          state.stocksSort.dir = state.stocksSort.dir === "asc" ? "desc" : "asc";
        } else {
          state.stocksSort.key = key;
          state.stocksSort.dir = (key === "ticker" || key === "company_name") ? "asc" : "desc";
        }
        renderPortfolioDetail(portfolio, stocks, recentTransactions);
      });
    });

    el.stocksList.querySelectorAll(".stock-row").forEach((row) => {
      row.addEventListener("click", () => {
        const ticker = row.dataset.ticker;
        const selected = stocks.find((s) => s.ticker === ticker);
        if (selected) {
          showStockDialog(selected);
        }
      });
    });
  }

  if (watchlist) {
    el.openTransactionDialogBtn.hidden = true;
    el.viewAllTransactionsBtn.hidden = true;
    el.recentTransactions.innerHTML = "<p>Transactions are disabled for watchlist portfolios.</p>";
  } else {
    el.openTransactionDialogBtn.hidden = false;
    el.viewAllTransactionsBtn.hidden = false;
    el.recentTransactions.innerHTML = renderTransactionTable(recentTransactions);
  }

  el.portfolioPeriodSelect.value = state.periods.portfolio;
  el.portfolioPeriodSelect.onchange = (event) => {
    state.periods.portfolio = event.target.value;
    renderPortfolioChart();
  };

  const allocationPanel = el.portfolioAllocationChart?.closest(".allocation-panel");
  const chartPanel = el.portfolioChart?.closest(".chart-panel");
  if (allocationPanel) {
    allocationPanel.hidden = watchlist || cash;
  }
  if (chartPanel) {
    chartPanel.hidden = watchlist;
  }

  if (!watchlist) {
    renderPortfolioChart();
    if (!cash) {
      renderAllocationChart(stocks, portfolio);
    }
  }
}

async function addWatchlistSymbol() {
  if (!state.currentPortfolio || !isWatchlistPortfolio(state.currentPortfolio)) {
    return;
  }

  const rawTicker = window.prompt("Add ticker to watchlist (example: AAPL)", "");
  if (!rawTicker) {
    return;
  }

  const ticker = String(rawTicker).trim().toUpperCase();
  if (!ticker) {
    return;
  }

  try {
    await apiPost(`/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/watchlist`, { ticker });
    await openPortfolio(state.currentPortfolio.name);
    showFlash(`Added ${ticker} to watchlist.`);
  } catch (error) {
    showFlash(error.message);
  }
}

async function removeWatchlistSymbol(ticker) {
  if (!state.currentPortfolio || !isWatchlistPortfolio(state.currentPortfolio)) {
    return;
  }

  const confirmed = window.confirm(`Remove ${ticker} from watchlist?`);
  if (!confirmed) {
    return;
  }

  try {
    await apiDelete(`/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/watchlist/${encodeURIComponent(ticker)}`);
    el.stockDialog.close();
    await openPortfolio(state.currentPortfolio.name);
    showFlash(`Removed ${ticker} from watchlist.`);
  } catch (error) {
    showFlash(error.message);
  }
}

function priceMapFromPayload(payload) {
  const byTicker = new Map();
  const entries = Array.isArray(payload?.prices) ? payload.prices : [];
  entries.forEach((entry) => {
    const ticker = String(entry?.ticker || "").trim().toUpperCase();
    const price = safeNumber(entry?.price);
    const open = safeNumber(entry?.open);
    const asOf = safeNumber(entry?.as_of);
    if (!ticker || price <= 0) {
      return;
    }

    byTicker.set(ticker, {
      price,
      open: open > 0 ? open : 0,
      asOf: asOf > 0 ? asOf : unixNow()
    });
  });
  return byTicker;
}

function applyLivePricesToPortfolio(portfolio, stocks, livePriceMap) {
  const nowTs = unixNow();
  const todayBucket = Math.floor(nowTs / 86400);
  const watchlist = isWatchlistPortfolio(portfolio);
  const updatedStocks = (Array.isArray(stocks) ? stocks : []).map((stock) => {
    const ticker = String(stock?.ticker || "").trim().toUpperCase();
    const live = livePriceMap.get(ticker);
    if (!live) {
      return { ...stock };
    }

    const shares = safeNumber(stock?.shares_owned);
    const livePositionValue = watchlist ? live.price : shares * live.price;
    // Day change is the move since today's regular-session open. Yahoo's quote provides
    // regularMarketOpen on every payload (including pre/post sessions, where it reflects the
    // most recent regular session's open). Fall back to the prior session close only if the
    // open is missing — that keeps the metric defined when the open isn't reported yet.
    const latestClose = safeNumber(stock?.latest_close_price);
    const latestCloseDate = safeNumber(stock?.latest_close_date);
    const previousClose = safeNumber(stock?.previous_close_price);
    const latestBucket = Math.floor(latestCloseDate / 86400);
    const priorSessionClose = (latestClose > 0 && latestBucket > 0 && latestBucket < todayBucket)
      ? latestClose
      : previousClose;
    const dayChangeBaseline = live.open > 0 ? live.open : priorSessionClose;
    const dayChangeAmount = Number.isFinite(dayChangeBaseline) && dayChangeBaseline > 0
      ? (live.price - dayChangeBaseline)
      : 0;
    const dayChangePercent = Number.isFinite(dayChangeBaseline) && dayChangeBaseline > 0
      ? (dayChangeAmount / dayChangeBaseline) * 100
      : 0;
    return {
      ...stock,
      latest_close_price: live.price,
      latest_close_date: live.asOf,
      previous_close_price: priorSessionClose,
      day_change_baseline: Number.isFinite(dayChangeBaseline) && dayChangeBaseline > 0 ? dayChangeBaseline : 0,
      day_change_amount: dayChangeAmount,
      day_change_percent: dayChangePercent,
      position_day_change_amount: watchlist ? dayChangeAmount : shares * dayChangeAmount,
      position_market_value: livePositionValue
    };
  });

  const totalPositionValue = updatedStocks.reduce(
    (sum, stock) => sum + safeNumber(stock?.position_market_value),
    0
  );

  const updatedPortfolio = {
    ...portfolio,
    estimated_total_value: watchlist
      ? totalPositionValue
      : safeNumber(portfolio?.available_capital) + totalPositionValue,
    daily_values: Array.isArray(portfolio?.daily_values) ? [...portfolio.daily_values] : []
  };

  if (watchlist) {
    const baselineTotal = updatedStocks.reduce(
      (sum, stock) => sum + safeNumber(stock?.day_change_baseline),
      0
    );
    const dayChangeAmount = updatedStocks.reduce(
      (sum, stock) => sum + safeNumber(stock?.day_change_amount),
      0
    );
    updatedPortfolio.day_change_amount = dayChangeAmount;
    updatedPortfolio.day_change_percent = baselineTotal > 0
      ? (dayChangeAmount / baselineTotal) * 100
      : 0;
  } else if (isCashPortfolio(portfolio)) {
    updatedPortfolio.day_change_amount = 0;
    updatedPortfolio.day_change_percent = 0;
  } else {
    // Market-only day change: sum each position's (shares × price delta).
    // Cash flows (deposits, withdrawals, asset transfers) are excluded so they
    // don't masquerade as market movement.
    const dayChangeAmount = updatedStocks.reduce(
      (sum, stock) => sum + safeNumber(stock?.position_day_change_amount),
      0
    );
    const previousTotal = updatedPortfolio.estimated_total_value - dayChangeAmount;
    updatedPortfolio.day_change_amount = dayChangeAmount;
    updatedPortfolio.day_change_percent = previousTotal > 0
      ? (dayChangeAmount / previousTotal) * 100
      : 0;
  }

  const existingIndex = updatedPortfolio.daily_values.findIndex(
    (point) => Math.floor(safeNumber(point?.date) / 86400) === todayBucket
  );
  const livePoint = {
    date: nowTs,
    value: updatedPortfolio.estimated_total_value,
    last_updated: nowTs
  };

  if (existingIndex >= 0) {
    updatedPortfolio.daily_values[existingIndex] = livePoint;
  } else {
    updatedPortfolio.daily_values.push(livePoint);
  }

  return {
    portfolio: updatedPortfolio,
    stocks: updatedStocks
  };
}

async function refreshPortfolioWithLivePrices(portfolioName) {
  if (!state.currentPortfolio || state.currentPortfolio.name !== portfolioName) {
    return;
  }

  if (state.portfolioLiveRefreshInFlight) {
    return;
  }

  state.portfolioLiveRefreshInFlight = true;

  try {
    const payload = await apiGetWithTimeout(
      `/api/portfolios/${encodeURIComponent(portfolioName)}/live-prices`,
      LIVE_REFRESH_REQUEST_TIMEOUT_MS
    );
    if (!state.currentPortfolio || state.currentPortfolio.name !== portfolioName) {
      return;
    }

    const marketState = resolveMarketState(payload);
    setMarketStateChip(marketState);

    const latestAsOf = latestAsOfFromPayload(payload);
    setPortfolioLastUpdatedChip(latestAsOf);

    if (!isLiveMarketSession(marketState) && latestAsOf <= 0) {
      return;
    }

    const livePriceMap = priceMapFromPayload(payload);
    if (!livePriceMap.size) {
      return;
    }

    const updated = applyLivePricesToPortfolio(state.currentPortfolio, state.currentStocks, livePriceMap);
    state.currentPortfolio = updated.portfolio;
    state.currentStocks = updated.stocks;

    state.portfolios = state.portfolios.map((portfolio) => {
      if (portfolio?.name !== portfolioName) {
        return portfolio;
      }

      return {
        ...portfolio,
        estimated_total_value: updated.portfolio.estimated_total_value,
        daily_values: updated.portfolio.daily_values
      };
    });

    renderPortfolioDetail(state.currentPortfolio, state.currentStocks, state.recentTransactions);
  } catch (_) {
    // Keep persisted snapshot visible and show a sensible market-state fallback.
    setMarketStateChip(fallbackMarketStateNowET());
    setPortfolioLastUpdatedChip(0);
  } finally {
    state.portfolioLiveRefreshInFlight = false;
  }
}

function stopLiveRefreshTimer() {
  if (state.liveRefreshTimer) {
    clearInterval(state.liveRefreshTimer);
    state.liveRefreshTimer = null;
  }
}

function stopDashboardLiveRefreshTimer() {
  if (state.dashboardLiveRefreshTimer) {
    clearInterval(state.dashboardLiveRefreshTimer);
    state.dashboardLiveRefreshTimer = null;
  }
}

async function refreshDashboardWithLivePrices() {
  if (state.dashboardLiveRefreshInFlight) {
    return;
  }

  state.dashboardLiveRefreshInFlight = true;
  const requestSeq = ++state.dashboardRefreshRequestSeq;
  const shouldRenderDashboard = !el.dashboardView.hidden && state.activeView === "dashboard";

  try {
    const payload = await apiGetWithTimeout("/api/live-prices", LIVE_REFRESH_REQUEST_TIMEOUT_MS);
    if (requestSeq < state.dashboardRefreshAppliedSeq) {
      return;
    }

    const marketState = resolveMarketState(payload);
    setDashboardMarketStateChip(marketState);

    const latestAsOf = latestAsOfFromPayload(payload);
    setDashboardLastUpdatedChip(latestAsOf);

    if (!isLiveMarketSession(marketState) && latestAsOf <= 0) {
      return;
    }

    const rows = Array.isArray(payload?.portfolios) ? payload.portfolios : [];
    const liveByName = new Map();
    rows.forEach((row) => {
      const name = String(row?.name || "").trim();
      const estimatedTotalValue = safeNumber(row?.estimated_total_value);
      if (!name || !Number.isFinite(estimatedTotalValue)) {
        return;
      }
      liveByName.set(name, estimatedTotalValue);
    });

    if (!liveByName.size) {
      return;
    }

    const nowTs = unixNow();
    const todayBucket = Math.floor(nowTs / 86400);
    state.portfolios = state.portfolios.map((portfolio) => {
      const liveEstimate = liveByName.get(portfolio?.name);
      if (!Number.isFinite(liveEstimate)) {
        return portfolio;
      }

      const dailyValues = Array.isArray(portfolio?.daily_values) ? [...portfolio.daily_values] : [];
      const index = dailyValues.findIndex((point) => Math.floor(safeNumber(point?.date) / 86400) === todayBucket);
      const livePoint = {
        date: nowTs,
        value: liveEstimate,
        last_updated: nowTs
      };

      if (index >= 0) {
        dailyValues[index] = livePoint;
      } else {
        dailyValues.push(livePoint);
      }

      const previousDayValue = previousDistinctDayValue(dailyValues, nowTs);
      let dayChangeAmount = safeNumber(portfolio?.day_change_amount);
      let dayChangePercent = safeNumber(portfolio?.day_change_percent);
      if (Number.isFinite(previousDayValue) && previousDayValue > 0) {
        dayChangeAmount = liveEstimate - previousDayValue;
        dayChangePercent = (dayChangeAmount / previousDayValue) * 100;
      }

      return {
        ...portfolio,
        estimated_total_value: liveEstimate,
        day_change_amount: dayChangeAmount,
        day_change_percent: dayChangePercent,
        daily_values: dailyValues
      };
    });

    if (shouldRenderDashboard) {
      renderDashboard();
      setActiveView("dashboard");
      setBreadcrumbs([{ label: "Assets" }]);
    }
    state.dashboardRefreshAppliedSeq = requestSeq;
  } catch (_) {
    if (!shouldRenderDashboard) {
      return;
    }
    setDashboardMarketStateChip(fallbackMarketStateNowET());
    setDashboardLastUpdatedChip(0);
  } finally {
    state.dashboardLiveRefreshInFlight = false;
  }
}

function startDashboardLiveRefreshTimer() {
  stopDashboardLiveRefreshTimer();
  state.dashboardLiveRefreshTimer = setInterval(() => {
    refreshDashboardWithLivePrices();
  }, LIVE_REFRESH_INTERVAL_MS);
}

function startLiveRefreshTimer(portfolioName) {
  stopLiveRefreshTimer();
  state.liveRefreshTimer = setInterval(() => {
    if (!state.currentPortfolio || state.currentPortfolio.name !== portfolioName) {
      stopLiveRefreshTimer();
      return;
    }

    refreshPortfolioWithLivePrices(portfolioName);
  }, LIVE_REFRESH_INTERVAL_MS);
}

function showStockDialog(stock) {
  const watchlist = isWatchlistPortfolio(state.currentPortfolio);

  if (watchlist) {
    el.stockDialogTitle.textContent = `${stock.ticker} • ${stock.company_name || "Watchlist Symbol"}`;
    el.stockDialogBody.innerHTML = `
      <article class="panel stock-dialog-summary stock-card-neutral">
        <div class="panel-head">
          <h3>Watchlist Snapshot</h3>
          <button id="removeWatchlistSymbolBtn" class="ghost-btn" type="button">Remove Symbol</button>
        </div>
        <div class="stock-dialog-grid">
          <div><span>Latest Price</span><strong>${currency(stock.latest_close_price)}</strong></div>
          <div><span>Day Change</span><strong>${changeLabel(stock.day_change_amount, stock.day_change_percent)}</strong></div>
          <div><span>As Of</span><strong>${stock.latest_close_date ? dateLabel(stock.latest_close_date) : "n/a"}</strong></div>
          <div><span>Ticker</span><strong>${stock.ticker}</strong></div>
          <div><span>Mode</span><strong>Watchlist</strong></div>
        </div>
      </article>
    `;

    const removeBtn = document.getElementById("removeWatchlistSymbolBtn");
    if (removeBtn) {
      removeBtn.addEventListener("click", () => removeWatchlistSymbol(stock.ticker));
    }

    el.stockDialog.showModal();
    return;
  }

  const performance = stockPerformance(stock);
  const toneClass = stockToneClass(
    performance.totalChange,
    performance.hasBasis,
    performance.isZeroCostBasisPosition
  );
  const events = stock.recent_events || [];

  el.stockDialogTitle.textContent = `${stock.ticker} • ${stock.company_name || "Company"}`;
  el.stockDialogBody.innerHTML = `
    <article class="panel stock-dialog-summary ${toneClass}">
      <div class="panel-head">
        <h3>Position Snapshot</h3>
        <span class="chip ${trendChipClass(performance.percentChange, performance.hasBasis)}">${performance.hasBasis ? `${performance.percentChange >= 0 ? "+" : ""}${performance.percentChange.toFixed(2)}%` : "No basis"}</span>
      </div>
      <div class="stock-dialog-grid">
        <div><span>Market Value</span><strong>${currency(stock.position_market_value)}</strong></div>
        <div><span>Unrealized P/L</span><strong>${performance.totalChange >= 0 ? "+" : ""}${currency(performance.totalChange)}</strong></div>
        <div><span>Per Share</span><strong>${performance.perShareChange >= 0 ? "+" : ""}${currency(performance.perShareChange)}</strong></div>
        <div><span>Day Change</span><strong>${changeLabel(stock.day_change_amount, stock.day_change_percent)}</strong></div>
        <div><span>Latest Close</span><strong>${currency(stock.latest_close_price)}</strong></div>
      </div>
    </article>
    <section class="metric-grid">
      ${metricCard("Shares Owned", sharesFormat(stock.shares_owned))}
      ${metricCard("Average Cost", currency(stock.average_purchase_price))}
      ${metricCard("Latest Close", currency(stock.latest_close_price), dateLabel(stock.latest_close_date))}
      ${metricCard("Day Change", signedCurrency(stock.position_day_change_amount), `${changeLabel(stock.day_change_amount, stock.day_change_percent)} per share`)}
      ${metricCard("Unrealized P/L", `${performance.totalChange >= 0 ? "+" : ""}${currency(performance.totalChange)}`, `${performance.perShareChange >= 0 ? "+" : ""}${currency(performance.perShareChange)} per share`)}
    </section>
    <article class="panel">
      <div class="panel-head">
        <h3>Recent Company Events</h3>
        <span class="chip">${events.length} entries</span>
      </div>
      <div class="dialog-table-wrap">
        ${events.length ? renderTransactionTable(events.map((e) => ({
          date: e.date,
          type: e.type === "SELL" ? "SELL_STOCK" : (e.type === "BUY" ? "BUY_STOCK" : e.type),
          stock_symbol: stock.ticker,
          shares: e.shares,
          amount: e.cash_amount,
          notes: e.notes || "",
          realized_profit: e.realized_profit
        }))) : "<p>No recent events available.</p>"}
      </div>
    </article>
  `;

  el.stockDialog.showModal();
}

async function openPortfolio(name) {
  hideFlash();
  try {
    const [portfolio, stocksPayload, recentPayload] = await Promise.all([
      apiGet(`/api/portfolios/${encodeURIComponent(name)}`),
      apiGet(`/api/portfolios/${encodeURIComponent(name)}/stocks`),
      apiGet(`/api/portfolios/${encodeURIComponent(name)}/transactions/recent?limit=8`)
    ]);

    state.currentPortfolio = portfolio;
    state.currentStocks = normalizeStocks(stocksPayload.stocks || []);
    state.recentTransactions = recentPayload.transactions || [];
    state.stocksSort = { key: null, dir: null };

    setActiveView("portfolio");

    renderPortfolioDetail(state.currentPortfolio, state.currentStocks, state.recentTransactions);
    setMarketStateChip(fallbackMarketStateNowET());
    setPortfolioLastUpdatedChip(0);

    setBreadcrumbs([
      { label: "Assets", onClick: showDashboard },
      { label: portfolioDisplayName(name) }
    ]);

    localStorage.setItem(CURRENT_PORTFOLIO_KEY, name);

    refreshPortfolioWithLivePrices(name);
    startLiveRefreshTimer(name);
    return true;
  } catch (error) {
    showFlash(error.message);
    if (state.activeView === "dashboard") {
      startDashboardLiveRefreshTimer();
      refreshDashboardWithLivePrices();
    }
    return false;
  }
}

function showDashboard() {
  stopLiveRefreshTimer();
  startDashboardLiveRefreshTimer();
  refreshDashboardWithLivePrices();
  setMarketStateChip("UNKNOWN");
  setPortfolioLastUpdatedChip(0);
  setActiveView("dashboard");
  setBreadcrumbs([{ label: "Assets" }]);
  state.currentPortfolio = null;
  localStorage.removeItem(CURRENT_PORTFOLIO_KEY);
}

function parseDateTimeLocalToUnix(value) {
  if (!value) {
    return null;
  }

  const timestampMs = Date.parse(value);
  if (!Number.isFinite(timestampMs)) {
    return null;
  }

  return Math.floor(timestampMs / 1000);
}

function updateTransactionFormVisibility() {
  const type = el.transactionType.value;
  const isBuySell = type === "buy" || type === "sell";
  const isDividend = type === "dividend";
  const isInterest = type === "interest";
  const isTransferCash = type === "deposit" || type === "withdrawal";
  const isTransferIn = type === "transfer_in_asset";
  const isTransferOut = type === "transfer_out_asset";
  const isTransferAsset = isTransferIn || isTransferOut;
  const needsTicker = isBuySell || isDividend || isTransferAsset;
  const needsShares = isBuySell || isDividend || isTransferAsset;
  const needsPrice = isBuySell || isTransferIn;
  const needsAmount = isDividend || isInterest || isTransferCash;

  const updateFieldVisibility = (el, shouldHide) => {
    if (el) {
      el.hidden = shouldHide;
      el.style.display = shouldHide ? "none" : "";
    }
  };

  updateFieldVisibility(el.groupTicker, !needsTicker);
  updateFieldVisibility(el.groupShares, !needsShares);
  updateFieldVisibility(el.groupPrice, !needsPrice);
  updateFieldVisibility(el.groupAmount, !needsAmount);

  el.transactionTicker.required = needsTicker;
  el.transactionShares.required = isBuySell || isTransferAsset;
  el.transactionPrice.required = isBuySell; // cost basis is optional on transfer_in
  el.transactionAmount.required = needsAmount;

  const priceLabel = el.groupPrice?.querySelector("label");
  if (priceLabel) {
    priceLabel.textContent = isTransferIn ? "Cost Basis Per Share (optional)" : "Price Per Share";
  }

  if (!needsTicker) {
    el.transactionTicker.value = "";
  }
  if (!needsShares) {
    el.transactionShares.value = "";
  }
  if (!needsPrice) {
    el.transactionPrice.value = "";
  }
}

function resetTransactionForm() {
  el.transactionForm.reset();
  el.transactionType.value = "buy";
  updateTransactionFormVisibility();
}

function resetCreatePortfolioForm() {
  el.createPortfolioForm.reset();
  el.createPortfolioType.value = "BROKERAGE";
  el.createPortfolioCapital.value = "0";
  el.createPortfolioCurrency.value = "USD";
  el.createPortfolioCurrencyOther.value = "";
  updateCreatePortfolioFormForType();
}

function updateCreatePortfolioFormForType() {
  const type = el.createPortfolioType.value;
  const watchlist = type === "WATCHLIST";
  const cash = type === "CASH";
  el.createPortfolioCapitalRow.hidden = watchlist;
  el.createPortfolioCapital.disabled = watchlist;
  const capitalLabel = el.createPortfolioCapitalRow.querySelector("label");
  if (capitalLabel) {
    capitalLabel.textContent = cash ? "Starting Balance" : "Initial Capital";
  }
  el.createPortfolioCurrencyRow.hidden = !cash;
  if (!cash) {
    el.createPortfolioCurrency.value = "USD";
  }
  updateCreatePortfolioCurrencyOtherVisibility();
}

function updateCreatePortfolioCurrencyOtherVisibility() {
  const cash = el.createPortfolioType.value === "CASH";
  const showOther = cash && el.createPortfolioCurrency.value === "OTHER";
  el.createPortfolioCurrencyOtherRow.hidden = !showOther;
  el.createPortfolioCurrencyOther.required = showOther;
}

function openCreatePortfolioDialog() {
  resetCreatePortfolioForm();
  el.createPortfolioDialog.showModal();
}

function openTransactionDialog() {
  if (!state.currentPortfolio) {
    showFlash("Select a portfolio first.");
    return;
  }

  if (isWatchlistPortfolio(state.currentPortfolio)) {
    showFlash("Transactions are disabled for watchlist portfolios.");
    return;
  }

  resetTransactionForm();
  const cash = isCashPortfolio(state.currentPortfolio);
  const crypto = isCryptoPortfolio(state.currentPortfolio);
  const assetNoun = crypto ? "Crypto" : "Asset";
  Array.from(el.transactionType.options).forEach((opt) => {
    const assetOnly = opt.value === "buy" || opt.value === "sell" ||
                      opt.value === "dividend" ||
                      opt.value === "transfer_in_asset" || opt.value === "transfer_out_asset";
    opt.hidden = cash && assetOnly;
    opt.disabled = cash && assetOnly;
    if (opt.value === "buy") opt.textContent = crypto ? "Buy" : "Buy Stock";
    if (opt.value === "sell") opt.textContent = crypto ? "Sell" : "Sell Stock";
    if (opt.value === "transfer_in_asset") opt.textContent = `Transfer In ${assetNoun}`;
    if (opt.value === "transfer_out_asset") opt.textContent = `Transfer Out ${assetNoun}`;
    if (opt.value === "dividend") opt.hidden = cash || crypto;
    if (opt.value === "dividend") opt.disabled = cash || crypto;
  });
  const tickerLabel = el.groupTicker?.querySelector("label");
  if (tickerLabel) {
    tickerLabel.textContent = crypto ? "Symbol" : "Ticker";
  }
  if (el.transactionTicker) {
    el.transactionTicker.placeholder = crypto ? "BTC-USD" : "AAPL";
  }
  if (cash) {
    el.transactionType.value = "deposit";
  } else if (crypto) {
    el.transactionType.value = "buy";
  }
  updateTransactionFormVisibility();
  el.transactionDialog.showModal();
}

async function submitTransactionForm(event) {
  event.preventDefault();
  if (!state.currentPortfolio) {
    showFlash("Select a portfolio first.");
    return;
  }

  if (isWatchlistPortfolio(state.currentPortfolio)) {
    showFlash("Transactions are disabled for watchlist portfolios.");
    return;
  }

  const action = el.transactionType.value;
  const payload = {
    notes: el.transactionNotes.value.trim()
  };

  const dateUnix = parseDateTimeLocalToUnix(el.transactionDate.value);
  if (dateUnix !== null) {
    payload.date = dateUnix;
  }

  if (action === "buy" || action === "sell") {
    payload.ticker = el.transactionTicker.value.trim().toUpperCase();
    payload.shares = safeNumber(el.transactionShares.value);
    payload.price_per_share = safeNumber(el.transactionPrice.value);
  } else if (action === "transfer_in_asset" || action === "transfer_out_asset") {
    payload.ticker = el.transactionTicker.value.trim().toUpperCase();
    payload.shares = safeNumber(el.transactionShares.value);
    if (action === "transfer_in_asset") {
      payload.cost_basis_per_share = safeNumber(el.transactionPrice.value);
    }
  } else if (action === "dividend") {
    payload.ticker = el.transactionTicker.value.trim().toUpperCase();
    payload.amount = safeNumber(el.transactionAmount.value);
    const shares = safeNumber(el.transactionShares.value);
    if (shares > 0) {
      payload.shares = shares;
    }
  } else if (action === "interest") {
    payload.amount = safeNumber(el.transactionAmount.value);
  } else if (action === "deposit" || action === "withdrawal") {
    payload.amount = safeNumber(el.transactionAmount.value);
  }

  const apiAction = (action === "transfer_in_asset") ? "transfer_in"
                  : (action === "transfer_out_asset") ? "transfer_out"
                  : action;

  try {
    el.transactionSubmitBtn.disabled = true;
    await apiPost(
      `/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/transactions/${apiAction}`,
      payload
    );

    const portfolioName = state.currentPortfolio.name;
    showFlash("Transaction recorded successfully.");
    resetTransactionForm();
    el.transactionDialog.close();

    // /api/portfolios serially fetches a USD FX rate for every non-USD cash
    // account, which can stall on Yahoo network failures. Refresh in the
    // background so the dialog isn't blocked on it.
    (async () => {
      try {
        const latestSummary = await apiGet("/api/portfolios");
        state.portfolios = normalizePortfolios(latestSummary.portfolios);
        refreshAllTransactionsForDashboard();
        await openPortfolio(portfolioName);
      } catch (_) {
        // best-effort; UI refreshes on next navigation
      }
    })();
  } catch (error) {
    showFlash(error.message);
  } finally {
    el.transactionSubmitBtn.disabled = false;
  }
}

async function showAllTransactions() {
  if (!state.currentPortfolio) {
    return;
  }

  if (isWatchlistPortfolio(state.currentPortfolio)) {
    showFlash("Transactions are disabled for watchlist portfolios.");
    return;
  }

  try {
    const payload = await apiGet(`/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/transactions`);
    el.transactionsHistory.innerHTML = `<div class="dialog-table-wrap">${renderTransactionTable(payload.transactions || [], { showNotes: true })}</div>`;
    el.transactionsDialog.showModal();
  } catch (error) {
    showFlash(error.message);
  }
}

async function submitCreatePortfolioForm(event) {
  event.preventDefault();

  const name = el.createPortfolioName.value.trim();
  const type = el.createPortfolioType.value;
  const isWatchlist = type === "WATCHLIST";
  const isCash = type === "CASH";
  const initialCapital = safeNumber(el.createPortfolioCapital.value);

  if (!name) {
    showFlash("Portfolio name is required.");
    return;
  }

  if (!isWatchlist && initialCapital < 0) {
    showFlash("Initial capital must be non-negative.");
    return;
  }

  let currencyCode = "USD";
  if (isCash) {
    const selected = el.createPortfolioCurrency.value;
    if (selected === "OTHER") {
      currencyCode = el.createPortfolioCurrencyOther.value.trim().toUpperCase();
    } else {
      currencyCode = selected;
    }
    if (!/^[A-Z]{3}$/.test(currencyCode)) {
      showFlash("Currency must be a 3-letter ISO code (e.g. EUR, JPY).");
      return;
    }
  }

  try {
    el.createPortfolioSubmitBtn.disabled = true;

    const body = {
      name,
      type
    };
    if (!isWatchlist) {
      body.initial_capital = initialCapital;
    }
    if (isCash) {
      body.currency = currencyCode;
    }

    const created = await apiPost("/api/portfolios", body);

    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
    renderDashboard();
    showDashboard();

    const createdName = String(created?.name || name);
    await openPortfolio(createdName);

    el.createPortfolioDialog.close();
    showFlash(`Created account ${portfolioDisplayName(createdName)}.`);
  } catch (error) {
    showFlash(error.message);
  } finally {
    el.createPortfolioSubmitBtn.disabled = false;
  }
}

function openDeleteAccountDialog() {
  if (!state.currentPortfolio) {
    showFlash("Select an account first.");
    return;
  }
  el.deleteAccountName.textContent = portfolioDisplayName(state.currentPortfolio.name);
  el.deleteAccountDialog.showModal();
}

async function confirmDeleteAccount() {
  if (!state.currentPortfolio) {
    return;
  }
  const name = state.currentPortfolio.name;
  try {
    el.deleteAccountConfirmBtn.disabled = true;
    await apiDelete(`/api/portfolios/${encodeURIComponent(name)}`);

    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
    renderDashboard();
    showDashboard();

    el.deleteAccountDialog.close();
    showFlash(`Deleted account ${portfolioDisplayName(name)}.`);
  } catch (error) {
    showFlash(error.message);
  } finally {
    el.deleteAccountConfirmBtn.disabled = false;
  }
}

async function loadDashboard() {
  hideFlash();
  try {
    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
    refreshAllTransactionsForDashboard();
    renderDashboard();
    startDashboardLiveRefreshTimer();

    const savedPortfolio = localStorage.getItem(CURRENT_PORTFOLIO_KEY);
    if (savedPortfolio) {
      const restored = await openPortfolio(savedPortfolio);
      if (restored) {
        return;
      }
      localStorage.removeItem(CURRENT_PORTFOLIO_KEY);
    }

    showDashboard();
  } catch (error) {
    showFlash(`Unable to load portfolios. ${error.message}. Check API Settings if needed.`);
    state.portfolios = [];
    renderDashboard();
    showDashboard();
  }
}

function toggleApiPanel() {
  const isHidden = el.apiConfigPanel.hidden;
  el.apiConfigPanel.hidden = !isHidden;
  el.apiConfigToggle.setAttribute("aria-expanded", String(isHidden));
}

function saveApiBase(event) {
  event.preventDefault();
  const raw = el.apiBaseInput.value.trim();
  state.apiBase = raw.replace(/\/$/, "");
  localStorage.setItem(API_STORAGE_KEY, state.apiBase);
  el.apiStatus.textContent = "Saved. Reloading data...";
  loadDashboard();
}

function resetApiBase() {
  state.apiBase = "";
  localStorage.removeItem(API_STORAGE_KEY);
  el.apiBaseInput.value = "";
  el.apiStatus.textContent = "Reset to same-origin API.";
  loadDashboard();
}

function wireEvents() {
  el.backToDashBtn.addEventListener("click", showDashboard);
  el.viewAllTransactionsBtn.addEventListener("click", showAllTransactions);
  el.openTransactionDialogBtn.addEventListener("click", openTransactionDialog);
  el.transactionType.addEventListener("change", updateTransactionFormVisibility);
  el.transactionForm.addEventListener("submit", submitTransactionForm);
  el.createPortfolioForm.addEventListener("submit", submitCreatePortfolioForm);
  el.createPortfolioType.addEventListener("change", updateCreatePortfolioFormForType);
  el.createPortfolioCurrency.addEventListener("change", updateCreatePortfolioCurrencyOtherVisibility);
  el.deleteAccountBtn.addEventListener("click", openDeleteAccountDialog);
  el.deleteAccountCancelBtn.addEventListener("click", () => el.deleteAccountDialog.close());
  el.deleteAccountConfirmBtn.addEventListener("click", confirmDeleteAccount);

  el.apiConfigToggle.addEventListener("click", toggleApiPanel);
  el.apiConfigForm.addEventListener("submit", saveApiBase);
  el.apiResetBtn.addEventListener("click", resetApiBase);
}

function init() {
  el.apiBaseInput.value = state.apiBase;
  setActiveView("dashboard");
  wireEvents();
  updateTransactionFormVisibility();
  resetCreatePortfolioForm();
  loadDashboard();
}

init();

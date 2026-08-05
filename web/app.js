const API_STORAGE_KEY = "portfolio-api-base";
const CURRENT_PORTFOLIO_KEY = "portfolio-current";
const PINNED_ACCOUNTS_KEY = "portfolio-pinned-accounts";
const ACCOUNT_FILTER_KEY = "portfolio-account-filter";
const CHART_PERIOD_KEY_PREFIX = "portfolio-chart-period:";
const VALID_CHART_PERIODS = ["1M", "3M", "6M", "1Y", "3Y", "ALL"];
const DEFAULT_CHART_PERIOD = "6M";
const DASHBOARD_SCOPE_KEY = "portfolio-dashboard-scope";
const VALID_DASHBOARD_SCOPES = ["ALL", "INVEST", "CASH"];
const DEFAULT_DASHBOARD_SCOPE = "ALL";
const API_CACHE_PREFIX = "ft.cache.";
const LAST_SYNC_KEY = "ft.last-sync";

function getChartPeriod(name) {
  const stored = localStorage.getItem(CHART_PERIOD_KEY_PREFIX + name);
  return VALID_CHART_PERIODS.includes(stored) ? stored : DEFAULT_CHART_PERIOD;
}

function setChartPeriod(name, value) {
  if (!VALID_CHART_PERIODS.includes(value)) return;
  localStorage.setItem(CHART_PERIOD_KEY_PREFIX + name, value);
}

function getDashboardScope() {
  const stored = localStorage.getItem(DASHBOARD_SCOPE_KEY);
  return VALID_DASHBOARD_SCOPES.includes(stored) ? stored : DEFAULT_DASHBOARD_SCOPE;
}

function setDashboardScope(value) {
  if (!VALID_DASHBOARD_SCOPES.includes(value)) return;
  if (value === DEFAULT_DASHBOARD_SCOPE) {
    localStorage.removeItem(DASHBOARD_SCOPE_KEY);
  } else {
    localStorage.setItem(DASHBOARD_SCOPE_KEY, value);
  }
}

const TYPE_SORT_ORDER = ["BROKERAGE", "ROTH_IRA", "TRADITIONAL_IRA", "CRYPTO", "CASH", "DEBT", "WATCHLIST"];

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
    dashboard: getChartPeriod("dashboard"),
    portfolio: getChartPeriod("portfolio")
  },
  dashboardScope: getDashboardScope(),
  charts: {
    dashboard: null,
    portfolio: null,
    allocation: null,
    spendBar: null,
    spendPie: null
  },
  spend: {
    transactions: [],
    bucket: localStorage.getItem("ft.spend.bucket") || "monthly",
    range: localStorage.getItem("ft.spend.range") || "3M",
    customFrom: localStorage.getItem("ft.spend.customFrom") || "",
    customTo: localStorage.getItem("ft.spend.customTo") || "",
    loading: false,
    tab: "analysis",
    tags529: {}
  },
  tax: {
    year: new Date().getFullYear(),
    spendTxs: [],
    incomeTxs: [],
    tags: { income: {}, deductions: {} },
    loading: false,
    depositsSearch: "",
    expensesSearch: ""
  },
  stocksSort: { key: null, dir: null },
  allTransactions: {},
  monthlyShowAll: false,
  inTransit: { total: 0, entries: [] }
};

const el = {
  breadcrumbs: document.getElementById("breadcrumbs"),
  flash: document.getElementById("flash"),
  dashboardView: document.getElementById("dashboardView"),
  portfolioView: document.getElementById("portfolioView"),
  spendView: document.getElementById("spendView"),
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
  connectAccountBtn: document.getElementById("connectAccountBtn"),
  syncAccountBtn: document.getElementById("syncAccountBtn"),
  disconnectAccountBtn: document.getElementById("disconnectAccountBtn"),
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
// When the market is closed, quotes don't move, so poll far less often.
const LIVE_REFRESH_CLOSED_INTERVAL_MS = 60000;
const LIVE_REFRESH_REQUEST_TIMEOUT_MS = 10000;

function liveRefreshIntervalMs(marketState) {
  return isLiveMarketSession(marketState)
    ? LIVE_REFRESH_INTERVAL_MS
    : LIVE_REFRESH_CLOSED_INTERVAL_MS;
}

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
  applyPortfolioLastUpdatedChip();
}

function applyPortfolioLastUpdatedChip() {
  if (!el.portfolioLastUpdatedChip) {
    return;
  }

  if (state.portfolioLiveRefreshInFlight) {
    el.portfolioLastUpdatedChip.hidden = false;
    el.portfolioLastUpdatedChip.className = "chip chip-refreshing";
    el.portfolioLastUpdatedChip.textContent = "Refreshing…";
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
  applyDashboardLastUpdatedChip();
}

function applyDashboardLastUpdatedChip() {
  const chip = document.getElementById("dashboardLastUpdatedChip");
  if (!chip) {
    return;
  }

  if (state.dashboardLiveRefreshInFlight) {
    chip.hidden = false;
    chip.className = "chip chip-refreshing";
    chip.textContent = "Refreshing…";
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
  const validViews = ["dashboard", "portfolio", "spend"];
  const active = validViews.includes(view) ? view : "dashboard";
  state.activeView = active;

  el.dashboardView.classList.toggle("is-active", active === "dashboard");
  el.portfolioView.classList.toggle("is-active", active === "portfolio");
  if (el.spendView) el.spendView.classList.toggle("is-active", active === "spend");

  el.dashboardView.hidden = active !== "dashboard";
  el.portfolioView.hidden = active !== "portfolio";
  if (el.spendView) el.spendView.hidden = active !== "spend";
}

function apiUrl(path) {
  if (!state.apiBase) {
    return path;
  }
  return `${state.apiBase}${path}`;
}

function cacheKey(path) {
  return API_CACHE_PREFIX + path;
}

function readCachedResponse(path) {
  try {
    const raw = localStorage.getItem(cacheKey(path));
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return null;
    return parsed;
  } catch (_) {
    return null;
  }
}

function writeCachedResponse(path, data) {
  try {
    localStorage.setItem(
      cacheKey(path),
      JSON.stringify({ ts: unixNow(), data })
    );
  } catch (_) {
    // Quota or serialization error — ignore; cache is best-effort.
  }
}

function getLastSyncTime() {
  const v = Number(localStorage.getItem(LAST_SYNC_KEY));
  return Number.isFinite(v) && v > 0 ? v : 0;
}

function setLastSyncTime(ts) {
  if (ts > 0) localStorage.setItem(LAST_SYNC_KEY, String(ts));
}

// fetch() throws TypeError when the network is unreachable; AbortError comes
// from our own timeout when the server can't be reached either. HTTP error
// responses (4xx/5xx with JSON body) are NOT network errors — those mean the
// server is up and the request is malformed or rejected, and should not
// degrade the UI into offline mode.
function isNetworkError(error) {
  if (!error) return false;
  if (error.name === "TypeError") return true;
  if (error.name === "AbortError") return true;
  const msg = String(error.message || "").toLowerCase();
  return (
    msg.includes("failed to fetch") ||
    msg.includes("networkerror") ||
    msg.includes("load failed") ||
    msg.includes("network request failed")
  );
}

let offlineStateCached = false;

function isOffline() {
  return offlineStateCached || !navigator.onLine;
}

function setOfflineState(value) {
  const next = Boolean(value);
  if (offlineStateCached === next) return;
  offlineStateCached = next;
  renderOfflineBanner();
}

function formatRelativeTime(unixSeconds) {
  if (!unixSeconds || unixSeconds <= 0) return "never";
  const diff = Math.max(0, unixNow() - unixSeconds);
  if (diff < 60) return "just now";
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
  const days = Math.floor(diff / 86400);
  if (days < 30) return `${days}d ago`;
  return new Date(unixSeconds * 1000).toLocaleDateString();
}

function renderOfflineBanner() {
  const banner = document.getElementById("offlineBanner");
  if (!banner) return;
  const offline = isOffline();
  if (!offline) {
    banner.hidden = true;
    return;
  }
  const ts = getLastSyncTime();
  const detail = ts > 0
    ? `Showing cached data from ${formatRelativeTime(ts)} (${new Date(ts * 1000).toLocaleString()}).`
    : "No cached data yet — connect to the server to load your portfolios.";
  banner.hidden = false;
  banner.innerHTML = `<strong>You're offline.</strong> ${detail}`;
}

const PROGRESS_BAR_SHOW_DELAY_MS = 250;
let inFlightRequests = 0;
let progressShowTimer = null;

function showProgressBar() {
  const bar = document.getElementById("globalProgressBar");
  if (bar) {
    bar.hidden = false;
  }
  document.body.setAttribute("aria-busy", "true");
}

function hideProgressBar() {
  const bar = document.getElementById("globalProgressBar");
  if (bar) {
    bar.hidden = true;
  }
  document.body.removeAttribute("aria-busy");
}

function beginRequest() {
  inFlightRequests += 1;
  if (inFlightRequests === 1 && !progressShowTimer) {
    progressShowTimer = setTimeout(() => {
      progressShowTimer = null;
      if (inFlightRequests > 0) {
        showProgressBar();
      }
    }, PROGRESS_BAR_SHOW_DELAY_MS);
  }
}

function endRequest() {
  inFlightRequests = Math.max(0, inFlightRequests - 1);
  if (inFlightRequests === 0) {
    if (progressShowTimer) {
      clearTimeout(progressShowTimer);
      progressShowTimer = null;
    }
    hideProgressBar();
  }
}

async function apiGet(path) {
  beginRequest();
  try {
    const response = await fetch(apiUrl(path));
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Request failed: ${response.status}`);
    }
    writeCachedResponse(path, data);
    setLastSyncTime(unixNow());
    setOfflineState(false);
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      const cached = readCachedResponse(path);
      if (cached) {
        return cached.data;
      }
    }
    throw error;
  } finally {
    endRequest();
  }
}

// Live-price endpoints intentionally do NOT use the offline cache — stale live
// prices would be misleading. The dashboard already has a separate fallback
// path that keeps the persisted snapshot visible when this throws.
async function apiGetWithTimeout(path, timeoutMs) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  beginRequest();

  try {
    const response = await fetch(apiUrl(path), { signal: controller.signal });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Request failed: ${response.status}`);
    }
    setOfflineState(false);
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
    }
    if (error?.name === "AbortError") {
      throw new Error("Live request timed out");
    }
    throw error;
  } finally {
    clearTimeout(timeout);
    endRequest();
  }
}

async function apiPost(path, body) {
  beginRequest();
  try {
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
    setOfflineState(false);
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      throw new Error("You're offline — this change can't be saved until you reconnect.");
    }
    throw error;
  } finally {
    endRequest();
  }
}

async function apiUploadReceiptTo(basePath, key, file) {
  beginRequest();
  try {
    const query = `key=${encodeURIComponent(key)}&filename=${encodeURIComponent(file.name)}`;
    const response = await fetch(apiUrl(`${basePath}?${query}`), {
      method: "POST",
      headers: { "Content-Type": file.type || "application/octet-stream" },
      body: file
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Upload failed: ${response.status}`);
    }
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      throw new Error("You're offline — receipts can't upload until you reconnect.");
    }
    throw error;
  } finally {
    endRequest();
  }
}

async function apiUploadReceipt(key, file) {
  return apiUploadReceiptTo("/api/529/receipt", key, file);
}

async function apiDelete(path) {
  beginRequest();
  try {
    const response = await fetch(apiUrl(path), {
      method: "DELETE"
    });

    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Request failed: ${response.status}`);
    }
    setOfflineState(false);
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      throw new Error("You're offline — this change can't be saved until you reconnect.");
    }
    throw error;
  } finally {
    endRequest();
  }
}

async function apiPatch(path, body) {
  beginRequest();
  try {
    const response = await fetch(apiUrl(path), {
      method: "PATCH",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify(body)
    });

    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || `Request failed: ${response.status}`);
    }
    setOfflineState(false);
    return data;
  } catch (error) {
    if (isNetworkError(error)) {
      setOfflineState(true);
      throw new Error("You're offline — this change can't be saved until you reconnect.");
    }
    throw error;
  } finally {
    endRequest();
  }
}

function currency(value) {
  return new Intl.NumberFormat(undefined, {
    style: "currency",
    currency: "USD",
    maximumFractionDigits: 2
  }).format(value || 0);
}

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

// Plaid PFC categories arrive as SCREAMING_SNAKE_CASE (e.g.
// "FOOD_AND_DRINK_COFFEE"). Map down to one of the ~16 primary groups so
// every transaction renders as a small set of color-coded chips rather than
// a long-tail of one-off labels.
const PFC_PRIMARIES = [
  "INCOME",
  "TRANSFER_IN",
  "TRANSFER_OUT",
  "LOAN_PAYMENTS",
  "BANK_FEES",
  "ENTERTAINMENT",
  "FOOD_AND_DRINK",
  "GENERAL_MERCHANDISE",
  "HOME_IMPROVEMENT",
  "MEDICAL",
  "PERSONAL_CARE",
  "GENERAL_SERVICES",
  "GOVERNMENT_AND_NON_PROFIT",
  "TRANSPORTATION",
  "TRAVEL",
  "RENT_AND_UTILITIES"
];

const PFC_PRIMARY_LABELS = {
  INCOME: "Income",
  TRANSFER_IN: "Transfer In",
  TRANSFER_OUT: "Transfer Out",
  LOAN_PAYMENTS: "Loan",
  BANK_FEES: "Fees",
  ENTERTAINMENT: "Entertainment",
  FOOD_AND_DRINK: "Food & Drink",
  GENERAL_MERCHANDISE: "Shopping",
  HOME_IMPROVEMENT: "Home",
  MEDICAL: "Medical",
  PERSONAL_CARE: "Personal Care",
  GENERAL_SERVICES: "Services",
  GOVERNMENT_AND_NON_PROFIT: "Gov / Non-profit",
  TRANSPORTATION: "Transport",
  TRAVEL: "Travel",
  RENT_AND_UTILITIES: "Rent & Utilities"
};

function categoryPrimary(raw) {
  if (!raw) return "";
  const upper = String(raw).toUpperCase();
  for (const p of PFC_PRIMARIES) {
    if (upper === p || upper.startsWith(p + "_")) return p;
  }
  return upper;
}

function categoryChipClass(primary) {
  return primary ? `cat-${primary.toLowerCase().replace(/_/g, "-")}` : "";
}

function categoryChip(raw) {
  if (!raw) return "";
  const primary = categoryPrimary(raw);
  // Credit-card payments are inflows that just pay down the card balance —
  // they aren't a spend category, so render no chip for them.
  if (primary === "LOAN_PAYMENTS") return "";
  const label = PFC_PRIMARY_LABELS[primary] || prettifyCategory(primary);
  return `<span class="chip chip-category ${categoryChipClass(primary)}" title="${escapeHtml(raw)}">${escapeHtml(label)}</span>`;
}

function prettifyCategory(raw) {
  if (!raw) return "";
  return String(raw)
    .toLowerCase()
    .split("_")
    .filter(Boolean)
    .map((w) => w.charAt(0).toUpperCase() + w.slice(1))
    .join(" ");
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
      target_price: safeNumber(stock.target_price),
      watchlist_notes: String(stock.watchlist_notes || ""),
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

function showFlash(message, kind = "error") {
  el.flash.textContent = message;
  el.flash.hidden = false;
  el.flash.className = `flash flash-${kind}`;
}

function hideFlash() {
  el.flash.hidden = true;
  el.flash.textContent = "";
  el.flash.className = "flash";
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
    // DEBT portfolios reduce overall totals, so flip their stored balance
    // series to a negative contribution when building the aggregate.
    const sign = isDebtPortfolio(portfolio) ? -1 : 1;
    // Foreign-currency cash accounts persist daily_values in native units, but
    // estimated_total_value (what the Total Assets card sums) is USD-converted
    // via fx_to_usd. Apply the same conversion here or the chart silently
    // diverges from the card for every non-USD cash account.
    const ccy = String(portfolio?.currency || "USD").toUpperCase();
    const fx = (isCashPortfolio(portfolio) && ccy !== "USD")
      ? (safeNumber(portfolio?.fx_to_usd) || 1)
      : 1;
    const points = (portfolio.daily_values || [])
      .map((point) => ({
        date: safeNumber(point?.date),
        value: safeNumber(point?.value) * sign * fx
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

function buildBrokerageCashSeries(portfolio) {
  // Mirror market_data_sync.cpp:recomputePortfolioDailyValues — backend doesn't
  // persist a cash-only series for brokerages, so we rebuild it from txs here.
  // initial_cash = current available_capital − Σ(non-cash-neutral tx.amount).
  const txs = (state.allTransactions || {})[portfolio.name] || [];
  const isCashNeutral = (t) => {
    const u = String(t || "").toUpperCase();
    return u === "TRANSFER_IN_ASSET" || u === "TRANSFER_OUT_ASSET";
  };

  const sorted = Array.isArray(txs)
    ? txs.slice().sort((a, b) => safeNumber(a?.date) - safeNumber(b?.date))
    : [];

  let sumNonNeutral = 0;
  sorted.forEach((tx) => {
    if (!isCashNeutral(tx?.type)) sumNonNeutral += safeNumber(tx?.amount);
  });
  const initialCash = safeNumber(portfolio.available_capital) - sumNonNeutral;

  if (!sorted.length) {
    const cap = safeNumber(portfolio.available_capital);
    if (Math.abs(cap) < 1e-9) return [];
    const todayUnix = Math.floor(Date.now() / 1000 / 86400) * 86400 + 16 * 3600;
    return [{ date: todayUnix, value: cap }];
  }

  const byDay = new Map();
  let runningCash = initialCash;
  sorted.forEach((tx) => {
    if (!isCashNeutral(tx?.type)) runningCash += safeNumber(tx?.amount);
    const day = Math.floor(safeNumber(tx?.date) / 86400);
    if (day > 0) byDay.set(day, runningCash);
  });

  const todayDay = Math.floor(Date.now() / 1000 / 86400);
  if (!byDay.has(todayDay)) byDay.set(todayDay, runningCash);

  return Array.from(byDay.entries())
    .sort((a, b) => a[0] - b[0])
    .map(([day, value]) => ({ date: day * 86400 + 16 * 3600, value }));
}

function buildBrokeragePositionsSeries(portfolio) {
  // Share value only = whole-account daily value − uninvested cash, aligned by
  // day. A brokerage/crypto account's daily_values track the total close value
  // (positions + cash); subtracting the synthesized cash series leaves just the
  // holdings. Both inputs are sparse (account value on market closes, cash on tx
  // days), so carry each forward across the union of days before subtracting.
  const totalByDay = new Map();
  (portfolio.daily_values || [])
    .map((pt) => ({ day: Math.floor(safeNumber(pt?.date) / 86400), value: safeNumber(pt?.value) }))
    .filter((pt) => pt.day > 0)
    .sort((a, b) => a.day - b.day)
    .forEach((pt) => totalByDay.set(pt.day, pt.value)); // later same-day entry wins

  const cashByDay = new Map();
  buildBrokerageCashSeries(portfolio).forEach((pt) => {
    const day = Math.floor(safeNumber(pt?.date) / 86400);
    if (day > 0) cashByDay.set(day, safeNumber(pt?.value));
  });

  const days = Array.from(new Set([...totalByDay.keys(), ...cashByDay.keys()])).sort((a, b) => a - b);
  let lastTotal = null;
  let lastCash = 0;
  const out = [];
  days.forEach((day) => {
    if (totalByDay.has(day)) lastTotal = totalByDay.get(day);
    if (cashByDay.has(day)) lastCash = cashByDay.get(day);
    if (lastTotal !== null) {
      out.push({ date: day * 86400 + 16 * 3600, value: lastTotal - lastCash });
    }
  });
  return out;
}

function portfoliosForDashboardScope(accountPortfolios, scope) {
  switch (scope) {
    case "INVEST":
      // Stocks + Crypto = share value only. Replace each account's whole-account
      // series with a positions-only one; the uninvested cash it strips out
      // shows up under the Cash view instead.
      return accountPortfolios
        .filter((p) => !isCashPortfolio(p) && !isDebtPortfolio(p))
        .map((p) => ({ ...p, daily_values: buildBrokeragePositionsSeries(p) }));
    case "CASH":
      // Cash view = standalone CASH accounts plus the uninvested cash inside
      // brokerage/crypto portfolios (synthesized from their tx history). DEBT is
      // excluded so this matches the Total Available Cash card (which also
      // ignores debt); debt only drags on the Total Assets line.
      return accountPortfolios
        .filter((p) => !isDebtPortfolio(p))
        .map((p) => {
          if (isCashPortfolio(p)) return p;
          return { ...p, daily_values: buildBrokerageCashSeries(p) };
        });
    default:
      return accountPortfolios;
  }
}

function dashboardScopeTitle(scope) {
  switch (scope) {
    case "INVEST": return "Investments Trend";
    case "CASH": return "Cash Trend";
    default: return "Total Asset Trend";
  }
}

function dashboardScopeSelectMarkup(selectId, selectedScope) {
  const options = [
    { value: "ALL", label: "Total assets" },
    { value: "INVEST", label: "Stocks + Crypto" },
    { value: "CASH", label: "Cash" }
  ]
    .map((opt) => `<option value="${opt.value}"${opt.value === selectedScope ? " selected" : ""}>${opt.label}</option>`)
    .join("");
  return `<select id="${selectId}" class="graph-period-select" aria-label="Chart scope">${options}</select>`;
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

function isDebtPortfolio(portfolio) {
  return String(portfolio?.type || "").toUpperCase() === "DEBT";
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
  footerColor: "#222",
  borderColor: "#222",
  borderWidth: 1,
  cornerRadius: 0,
  displayColors: false,
  padding: 10,
  titleFont: { family: '"Noto Serif", Georgia, serif', weight: "500", size: 12 },
  bodyFont: { family: '"Noto Serif", Georgia, serif', weight: "400", size: 12 },
  footerFont: { family: '"Noto Serif", Georgia, serif', weight: "600", size: 12 }
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
  const { showNotes = false, hideTickerShares = false } = options;
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
      let rawNotes = tx.notes || "";
      // Backend prefixes pending Plaid txs with "[PENDING] " — strip it so the
      // notes column stays clean and surface a separate "Pending" chip instead.
      const isPending = rawNotes.startsWith("[PENDING] ");
      if (isPending) rawNotes = rawNotes.slice(10);
      const isTransfer = rawNotes.startsWith("[TXFR] ");
      const cleanedNotes = isTransfer ? rawNotes.slice(7) : rawNotes;
      const pendingBadge = isPending
        ? `<span class="chip chip-pending" title="Pending at the bank — balance does not yet reflect this">Pending</span> `
        : "";
      const transferBadge = isTransfer
        ? `<span class="chip chip-transfer" title="Identified as a transfer between connected accounts">Transfer</span> `
        : "";
      const catBadge = tx.category ? categoryChip(tx.category) + " " : "";
      const tickerSharesCells = hideTickerShares ? "" : `<td>${symbol}</td><td>${shares}</td>`;
      const notesCell = showNotes
        ? `<td>${pendingBadge}${transferBadge}${catBadge}${cleanedNotes || "-"}</td>`
        : "";
      const rowClass = isPending ? ' class="tx-row-pending"' : "";
      return `<tr${rowClass}>
        <td>${dateLabel(tx.date)}</td>
        <td>${typeLabel(tx.type)}${profitMarkup}</td>
        ${tickerSharesCells}
        <td class="${amountClass}">${currency(tx.amount)}</td>
        ${notesCell}
      </tr>`;
    })
    .join("");

  const notesHeader = showNotes ? "<th>Notes</th>" : "";
  const tickerSharesHeaders = hideTickerShares ? "" : "<th>Ticker</th><th>Shares</th>";
  return `<table class="tx-table"><thead>
    <tr>
      <th>Date</th>
      <th>Type</th>
      ${tickerSharesHeaders}
      <th>Amount</th>
      ${notesHeader}
    </tr>
  </thead><tbody>${rows}</tbody></table>`;
}

function renderDashboard() {
  const portfolios = normalizePortfolios(state.portfolios);
  state.portfolios = portfolios;

  const accountPortfolios = portfolios.filter((p) => !isWatchlistPortfolio(p));

  // DEBT accounts contribute a negative estimated_total_value (set server-side),
  // so the same reduce naturally subtracts outstanding balances from totals.
  const totalAssets = accountPortfolios.reduce((sum, p) => sum + (p.estimated_total_value || 0), 0);
  // We surface unmatched transfer outflows in the subline but no longer add
  // them back to the total — the in-transit pile mixes own-account transfers
  // (where add-back is correct) with peer-to-peer payments like Zelle to a
  // friend (where add-back would mask a real outflow). Until we can reliably
  // tell them apart (Plaid counterparties), don't risk inflating Total Assets.
  const inTransitTotal = Number(state.inTransit?.total) || 0;
  // Convert each account's native-currency cash to USD before summing so foreign
  // cash accounts contribute correctly to the dashboard total. DEBT accounts
  // aren't cash and shouldn't inflate the cash metric.
  const totalCash = accountPortfolios.reduce((sum, p) => {
    if (isDebtPortfolio(p)) return sum;
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

  const accountCountSub = `${accountPortfolios.length} account${accountPortfolios.length === 1 ? "" : "s"}`;
  const totalAssetsSub = inTransitTotal > 0
    ? `${currency(inTransitTotal)} in transit (not included)`
    : accountCountSub;

  const dashboardMetrics = `<section class="metric-grid">
    ${metricCard("Total Assets", currency(totalAssets), totalAssetsSub)}
    ${metricCard("Day Change", signedCurrency(totalDayChange), `${percentage(totalDayChangePercent)} vs previous close`)}
    ${metricCard("Total Available Cash", currency(totalCash), "Across accounts")}
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
    { value: "DEBT", label: "Debt" },
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
      const syncBadge = p.is_synced
        ? `<span class="sync-badge" title="Auto-synced${p.institution_name ? " via " + escapeHtml(p.institution_name) : ""}" aria-label="Auto-synced account">⟳</span>`
        : "";
      const reauthChip = p.needs_reauth
        ? `<button type="button" class="chip reauth-chip" data-reauth-name="${encodeURIComponent(p.name)}" title="Plaid connection needs re-authentication" aria-label="Reconnect ${escapeHtml(portfolioDisplayName(p.name))}">Reconnect</button>`
        : "";
      const header = `<div class="stock-top">
          <strong>${syncBadge}${portfolioDisplayName(p.name)}</strong>
          <span class="card-header-right">
            ${reauthChip}
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
      } else if (isDebtPortfolio(p)) {
        body = `<div>${currency(p.available_capital)} owed</div>
                <div class="sub">Subtracted from totals</div>
                <div class="sub">${p.transaction_count} transaction${p.transaction_count === 1 ? "" : "s"}</div>`;
      } else {
        const holdingsLabel = isCryptoPortfolio(p) ? "coins" : "stocks";
        body = `<div>${currency(p.estimated_total_value)}</div>
                <div class="sub">Day: ${changeLabel(p.day_change_amount, p.day_change_percent)}</div>
                <div class="sub">Cash: ${currency(p.available_capital)}</div>
                <div class="sub">${p.stock_count} ${holdingsLabel} • ${p.transaction_count} transactions</div>`;
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
        <h3 id="dashboardChartTitle">${dashboardScopeTitle(state.dashboardScope)}</h3>
        <div class="chart-tools">
          ${dashboardScopeSelectMarkup("dashboardScopeSelect", state.dashboardScope)}
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
          <button id="openSpendAnalysisBtn" class="ghost-btn" type="button">Spending &amp; Tax</button>
          <button id="openPortfolioCreateDialogBtn" class="primary-btn" type="button">New Account</button>
        </div>
      </div>
      ${cards ? `<div class="dashboard-cards">${cards}</div>` : emptyState}
    </section>
    ${renderMonthlyActivity(portfolios, state.allTransactions)}
  `;

  applyDashboardLastUpdatedChip();
  if (state.dashboardMarketState && state.dashboardMarketState !== "UNKNOWN") {
    setDashboardMarketStateChip(state.dashboardMarketState);
  }

  const openPortfolioCreateDialogBtn = document.getElementById("openPortfolioCreateDialogBtn");
  if (openPortfolioCreateDialogBtn) {
    openPortfolioCreateDialogBtn.addEventListener("click", openCreatePortfolioDialog);
  }

  const openSpendAnalysisBtn = document.getElementById("openSpendAnalysisBtn");
  if (openSpendAnalysisBtn) {
    openSpendAnalysisBtn.addEventListener("click", showSpendAnalysis);
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
      if (event.target.closest(".pin-btn") || event.target.closest(".reauth-chip")) {
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

  document.querySelectorAll(".reauth-chip").forEach((btn) => {
    btn.addEventListener("click", (event) => {
      event.stopPropagation();
      const name = decodeURIComponent(btn.dataset.reauthName || "");
      if (!name) return;
      startPlaidConnect(name);
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
  const dashboardScopeSelect = document.getElementById("dashboardScopeSelect");
  const dashboardChangeChip = document.getElementById("dashboardChangeChip");
  const dashboardChartTitle = document.getElementById("dashboardChartTitle");

  const drawDashboardChart = () => {
    const scopedPortfolios = portfoliosForDashboardScope(accountPortfolios, state.dashboardScope);
    const scopedAggregate = mergeDailySeries(scopedPortfolios);
    const filtered = filterPointsByPeriod(scopedAggregate, state.periods.dashboard);
    const trend = computeTrend(filtered);
    const color = trendColor(trend.percentChange);

    dashboardChangeChip.className = `chip ${trendChipClass(trend.percentChange, trend.hasTrend)}`;
    dashboardChangeChip.textContent = trend.hasTrend
      ? `Change: ${percentage(trend.percentChange)}`
      : "Change: n/a";

    if (dashboardChartTitle) {
      dashboardChartTitle.textContent = dashboardScopeTitle(state.dashboardScope);
    }

    destroyChart("dashboard");
    state.charts.dashboard = createLineChart(chartCanvas, filtered, dashboardScopeTitle(state.dashboardScope), color);
  };

  dashboardPeriodSelect.addEventListener("change", (event) => {
    state.periods.dashboard = event.target.value;
    setChartPeriod("dashboard", state.periods.dashboard);
    drawDashboardChart();
  });

  if (dashboardScopeSelect) {
    dashboardScopeSelect.addEventListener("change", (event) => {
      const next = event.target.value;
      if (!VALID_DASHBOARD_SCOPES.includes(next)) return;
      state.dashboardScope = next;
      setDashboardScope(next);
      drawDashboardChart();
    });
  }

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
  const targetPrice = safeNumber(stock?.target_price);
  const lastPrice = safeNumber(stock?.latest_close_price);
  const targetDiff = (targetPrice > 0 && lastPrice > 0) ? (lastPrice - targetPrice) : 0;
  return {
    ticker: String(stock?.ticker || ""),
    company_name: String(stock?.company_name || ""),
    shares_owned: safeNumber(stock?.shares_owned),
    average_purchase_price: safeNumber(stock?.average_purchase_price),
    latest_close_price: lastPrice,
    day_change_amount: safeNumber(stock?.day_change_amount),
    day_change_percent: safeNumber(stock?.day_change_percent),
    position_day_change_amount: safeNumber(stock?.position_day_change_amount),
    position_market_value: safeNumber(stock?.position_market_value),
    latest_close_date: safeNumber(stock?.latest_close_date),
    target_price: targetPrice,
    target_diff: targetDiff,
    watchlist_notes: String(stock?.watchlist_notes || ""),
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

function watchlistDiffCell(stock) {
  const target = safeNumber(stock.target_price);
  const last = safeNumber(stock.latest_close_price);
  if (target <= 0 || last <= 0) {
    return `<td class="num">—</td>`;
  }
  const diff = last - target;
  const pct = (diff / target) * 100;
  const tone = pnlCellTone(diff);
  return `<td class="num ${tone}">${signedCurrency(diff)} <span class="sub">(${percentage(pct)})</span></td>`;
}

function watchlistTableColumns() {
  return [
    { key: "ticker", label: "Ticker", align: "left", render: (r) => {
        const stock = r._stock || r;
        const hasNotes = String(stock.watchlist_notes || "").trim().length > 0;
        const noteChip = hasNotes ? ` <span class="chip chip-neutral" title="Has notes">📝</span>` : "";
        return `<td class="ticker"><strong>${stock.ticker}</strong>${noteChip}</td>`;
      }
    },
    { key: "latest_close_price", label: "Last Price", align: "right", render: (r) => `<td class="num">${currency(r.latest_close_price)}</td>` },
    { key: "target_price", label: "Target", align: "right", render: (r) => `<td class="num">${safeNumber(r.target_price) > 0 ? currency(r.target_price) : "—"}</td>` },
    { key: "target_diff", label: "vs Target", align: "right", render: (r) => watchlistDiffCell(r._stock || r) },
    { key: "latest_close_date", label: "As Of", align: "right", render: (r) => `<td class="num">${r.latest_close_date ? dateLabel(r.latest_close_date) : "n/a"}</td>` }
  ];
}

function renderPortfolioDetail(portfolio, stocks, recentTransactions) {
  const watchlist = isWatchlistPortfolio(portfolio);
  const cash = isCashPortfolio(portfolio);
  const crypto = isCryptoPortfolio(portfolio);
  const debt = isDebtPortfolio(portfolio);
  const holdingNoun = crypto ? "coin" : (watchlist ? "symbol" : "stock");
  const visibleStocks = watchlist
    ? stocks
    : stocks.filter((stock) => safeNumber(stock?.shares_owned) > 0);

  el.portfolioName.textContent = portfolioDisplayName(portfolio.name);
  el.portfolioType.textContent = typeLabel(portfolio.type);
  if (cash || debt) {
    el.stockCount.textContent = `${portfolio.transaction_count || 0} transaction${(portfolio.transaction_count || 0) === 1 ? "" : "s"}`;
  } else {
    el.stockCount.textContent = `${visibleStocks.length} ${holdingNoun}${visibleStocks.length === 1 ? "" : "s"}`;
  }
  // Cash and debt accounts don't track market data, so the market-state chip
  // isn't meaningful for them.
  if (el.marketStateChip) {
    el.marketStateChip.hidden = cash || debt;
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
  } else if (debt) {
    el.portfolioMetrics.hidden = false;
    el.portfolioMetrics.innerHTML = [
      metricCard("Current Debt", currency(portfolio.available_capital), "Subtracted from Total Assets"),
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
    stocksPanel.hidden = cash || debt;
  }

  if (!cash && !debt) {
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
          state.stocksSort.dir = key === "ticker" ? "asc" : "desc";
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

  const connection = portfolio.connection || null;
  const connected = !!connection;

  el.connectAccountBtn.hidden = watchlist || connected;
  el.syncAccountBtn.hidden = !connected;
  el.disconnectAccountBtn.hidden = !connected;

  if (watchlist) {
    el.openTransactionDialogBtn.hidden = true;
    el.viewAllTransactionsBtn.hidden = true;
    el.recentTransactions.innerHTML = "<p>Transactions are disabled for watchlist portfolios.</p>";
  } else if (connected) {
    el.openTransactionDialogBtn.hidden = true;
    el.viewAllTransactionsBtn.hidden = false;
    const inst = connection.institution_name || connection.provider || "external provider";
    const lastSync = connection.last_synced > 0
      ? new Date(connection.last_synced * 1000).toLocaleString()
      : "never";
    const note = `<div class="connection-status">
      <strong>Auto-synced via ${escapeHtml(inst)}</strong>
      <span class="sub">Last sync: ${escapeHtml(lastSync)} — manual entry disabled.</span>
    </div>`;
    // For cash and debt accounts the merchant + category in `notes` is the
    // primary info per row, so show that column inline. Neither type has
    // positions, so drop the Ticker/Shares columns entirely.
    const showNotes = cash || debt;
    const hideTickerShares = cash || debt;
    el.recentTransactions.innerHTML = note + renderTransactionTable(recentTransactions, { showNotes, hideTickerShares });
  } else {
    el.openTransactionDialogBtn.hidden = false;
    el.viewAllTransactionsBtn.hidden = false;
    const showNotes = cash || debt;
    const hideTickerShares = cash || debt;
    el.recentTransactions.innerHTML = renderTransactionTable(recentTransactions, { showNotes, hideTickerShares });
  }

  el.portfolioPeriodSelect.value = state.periods.portfolio;
  el.portfolioPeriodSelect.onchange = (event) => {
    state.periods.portfolio = event.target.value;
    setChartPeriod("portfolio", state.periods.portfolio);
    renderPortfolioChart();
  };

  const allocationPanel = el.portfolioAllocationChart?.closest(".allocation-panel");
  const chartPanel = el.portfolioChart?.closest(".chart-panel");
  if (allocationPanel) {
    allocationPanel.hidden = watchlist || cash || debt;
  }
  if (chartPanel) {
    chartPanel.hidden = watchlist;
  }

  if (!watchlist) {
    renderPortfolioChart();
    if (!cash && !debt) {
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
    showFlash(`Added ${ticker} to watchlist.`, "success");
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
    showFlash(`Removed ${ticker} from watchlist.`, "success");
  } catch (error) {
    showFlash(error.message);
  }
}

async function saveWatchlistDetails(ticker) {
  if (!state.currentPortfolio || !isWatchlistPortfolio(state.currentPortfolio)) {
    return;
  }

  const priceInput = document.getElementById("watchlistTargetPrice");
  const notesInput = document.getElementById("watchlistNotes");
  const rawPrice = priceInput ? priceInput.value.trim() : "";
  const notes = notesInput ? notesInput.value : "";

  const body = { watchlist_notes: notes };
  if (rawPrice === "") {
    body.target_price = null;
  } else {
    const parsed = Number(rawPrice);
    if (!Number.isFinite(parsed) || parsed < 0) {
      showFlash("Target price must be a non-negative number.");
      return;
    }
    body.target_price = parsed;
  }

  try {
    await apiPatch(
      `/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/watchlist/${encodeURIComponent(ticker)}`,
      body
    );
    el.stockDialog.close();
    await openPortfolio(state.currentPortfolio.name);
    showFlash(`Updated ${ticker}.`, "success");
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
  applyPortfolioLastUpdatedChip();

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
    applyPortfolioLastUpdatedChip();
  }
}

function stopLiveRefreshTimer() {
  if (state.liveRefreshTimer) {
    clearTimeout(state.liveRefreshTimer);
    state.liveRefreshTimer = null;
  }
}

function stopDashboardLiveRefreshTimer() {
  if (state.dashboardLiveRefreshTimer) {
    clearTimeout(state.dashboardLiveRefreshTimer);
    state.dashboardLiveRefreshTimer = null;
  }
}

async function refreshDashboardWithLivePrices() {
  if (state.dashboardLiveRefreshInFlight) {
    return;
  }

  state.dashboardLiveRefreshInFlight = true;
  applyDashboardLastUpdatedChip();
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

      // /api/live-prices returns the cash+positions sum (positive). DEBT
      // accounts contribute negatively to the dashboard total, so flip the
      // sign for estimated_total_value while leaving daily_values (the
      // balance trajectory) positive.
      const debt = isDebtPortfolio(portfolio);
      const totalContribution = debt ? -liveEstimate : liveEstimate;

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
      if (!debt && Number.isFinite(previousDayValue) && previousDayValue > 0) {
        dayChangeAmount = liveEstimate - previousDayValue;
        dayChangePercent = (dayChangeAmount / previousDayValue) * 100;
      }

      return {
        ...portfolio,
        estimated_total_value: totalContribution,
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
    applyDashboardLastUpdatedChip();
  }
}

// Self-rescheduling timers (vs setInterval) so each tick can pick a new delay
// based on the latest market state, and so polling pauses while the tab is
// hidden. The visibilitychange handler restarts the active view's timer.
function startDashboardLiveRefreshTimer() {
  stopDashboardLiveRefreshTimer();
  if (document.hidden) {
    return;
  }
  const tick = async () => {
    await refreshDashboardWithLivePrices();
    if (state.dashboardLiveRefreshTimer === null || document.hidden) {
      return;
    }
    state.dashboardLiveRefreshTimer = setTimeout(tick, liveRefreshIntervalMs(state.dashboardMarketState));
  };
  state.dashboardLiveRefreshTimer = setTimeout(tick, liveRefreshIntervalMs(state.dashboardMarketState));
}

function startLiveRefreshTimer(portfolioName) {
  stopLiveRefreshTimer();
  if (document.hidden) {
    return;
  }
  const tick = async () => {
    if (!state.currentPortfolio || state.currentPortfolio.name !== portfolioName) {
      stopLiveRefreshTimer();
      return;
    }
    await refreshPortfolioWithLivePrices(portfolioName);
    if (state.liveRefreshTimer === null || document.hidden) {
      return;
    }
    if (!state.currentPortfolio || state.currentPortfolio.name !== portfolioName) {
      stopLiveRefreshTimer();
      return;
    }
    state.liveRefreshTimer = setTimeout(tick, liveRefreshIntervalMs(state.liveMarketState));
  };
  state.liveRefreshTimer = setTimeout(tick, liveRefreshIntervalMs(state.liveMarketState));
}

// Pause polling when the tab is backgrounded; resume (with an immediate
// refresh) the timer for whatever view is active when it returns.
function handleVisibilityChange() {
  if (document.hidden) {
    stopDashboardLiveRefreshTimer();
    stopLiveRefreshTimer();
    return;
  }

  if (state.currentPortfolio &&
      !isCashPortfolio(state.currentPortfolio) &&
      !isDebtPortfolio(state.currentPortfolio)) {
    const name = state.currentPortfolio.name;
    refreshPortfolioWithLivePrices(name);
    startLiveRefreshTimer(name);
  } else if (state.activeView === "dashboard") {
    refreshDashboardWithLivePrices();
    startDashboardLiveRefreshTimer();
  }
}

function showStockDialog(stock) {
  const watchlist = isWatchlistPortfolio(state.currentPortfolio);

  if (watchlist) {
    el.stockDialogTitle.textContent = `${stock.ticker} • ${stock.company_name || "Watchlist Symbol"}`;
    const targetPrice = safeNumber(stock.target_price);
    const lastPrice = safeNumber(stock.latest_close_price);
    const hasTarget = targetPrice > 0;
    const hasLast = lastPrice > 0;
    let diffLabel = "—";
    let diffStyle = "";
    if (hasTarget && hasLast) {
      const diff = lastPrice - targetPrice;
      const pct = (diff / targetPrice) * 100;
      if (diff > 0.0001) {
        diffStyle = "color:#1b8b68;";
      } else if (diff < -0.0001) {
        diffStyle = "color:#c34b3d;";
      }
      diffLabel = `${signedCurrency(diff)} (${percentage(pct)})`;
    }
    const targetInputValue = hasTarget ? String(targetPrice) : "";
    const notesValue = String(stock.watchlist_notes || "");

    el.stockDialogBody.innerHTML = `
      <article class="panel stock-dialog-summary stock-card-neutral">
        <div class="panel-head">
          <h3>Watchlist Snapshot</h3>
          <button id="removeWatchlistSymbolBtn" class="ghost-btn" type="button">Remove Symbol</button>
        </div>
        <div class="stock-dialog-grid">
          <div><span>Latest Price</span><strong>${currency(lastPrice)}</strong></div>
          <div><span>Target Price</span><strong>${hasTarget ? currency(targetPrice) : "—"}</strong></div>
          <div><span>vs Target</span><strong style="${diffStyle}">${diffLabel}</strong></div>
          <div><span>Day Change</span><strong>${changeLabel(stock.day_change_amount, stock.day_change_percent)}</strong></div>
          <div><span>As Of</span><strong>${stock.latest_close_date ? dateLabel(stock.latest_close_date) : "n/a"}</strong></div>
          <div><span>Ticker</span><strong>${stock.ticker}</strong></div>
        </div>
      </article>
      <article class="panel">
        <div class="panel-head">
          <h3>Target &amp; Notes</h3>
        </div>
        <form id="watchlistEditForm" class="tx-form">
          <div class="field-row">
            <label for="watchlistTargetPrice">Target Price (leave blank to clear)</label>
            <input id="watchlistTargetPrice" name="target_price" type="number" min="0" step="any" value="${escapeHtml(targetInputValue)}" placeholder="e.g. 200.00" />
          </div>
          <div class="field-row">
            <label for="watchlistNotes">Notes</label>
            <textarea id="watchlistNotes" name="watchlist_notes" rows="3" maxlength="4096" placeholder="Why are you watching this?">${escapeHtml(notesValue)}</textarea>
          </div>
          <div class="row-actions">
            <button id="watchlistSaveBtn" class="primary-btn" type="submit">Save</button>
          </div>
        </form>
      </article>
    `;

    const removeBtn = document.getElementById("removeWatchlistSymbolBtn");
    if (removeBtn) {
      removeBtn.addEventListener("click", () => removeWatchlistSymbol(stock.ticker));
    }

    const form = document.getElementById("watchlistEditForm");
    if (form) {
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        saveWatchlistDetails(stock.ticker);
      });
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

    // Cash and debt accounts have no tradeable positions, so skip the live
    // market-price refresh loop entirely — it would just be wasted requests.
    if (!isCashPortfolio(portfolio) && !isDebtPortfolio(portfolio)) {
      refreshPortfolioWithLivePrices(name);
      startLiveRefreshTimer(name);
    }
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

// ---- Spend Analysis -----------------------------------------------------

// Stable color per primary PFC so the same category looks the same across the
// stacked-bar and pie charts. Tweak palette if Plaid adds new primaries.
const SPEND_CATEGORY_COLORS = {
  FOOD_AND_DRINK:       "#d97706",
  GENERAL_MERCHANDISE:  "#7c3aed",
  TRANSPORTATION:       "#0891b2",
  TRAVEL:               "#2563eb",
  ENTERTAINMENT:        "#db2777",
  RENT_AND_UTILITIES:   "#475569",
  MEDICAL:              "#dc2626",
  PERSONAL_CARE:        "#ec4899",
  GENERAL_SERVICES:     "#65a30d",
  HOME_IMPROVEMENT:     "#92400e",
  GOVERNMENT_AND_NON_PROFIT: "#0d9488",
  BANK_FEES:            "#a16207",
  INCOME:               "#16a34a",
  OTHER:                "#6b7280"
};

function spendCategoryColor(primary) {
  return SPEND_CATEGORY_COLORS[primary] || SPEND_CATEGORY_COLORS.OTHER;
}

function spendCategoryLabel(primary) {
  return PFC_PRIMARY_LABELS[primary] || prettifyCategory(primary) || "Uncategorized";
}

function rangeToDates(range) {
  const now = new Date();
  // Snap "from" to the first of a calendar month so each N-month range shows
  // exactly N month buckets (current month + N-1 prior months), not partial slices.
  const firstOfMonth = (offset) => new Date(now.getFullYear(), now.getMonth() - offset, 1);
  const isoDate = (d) =>
    `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
  const isoLike = /^\d{4}-\d{2}-\d{2}$/;
  if (range === "CUSTOM") {
    const customFrom = state.spend.customFrom;
    const customTo = state.spend.customTo;
    const fromValid = isoLike.test(customFrom);
    const toValid = isoLike.test(customTo);
    const from = fromValid ? customFrom : isoDate(firstOfMonth(2));
    let to = toValid ? customTo : isoDate(now);
    if (fromValid && toValid && customTo < customFrom) {
      to = customFrom;
    }
    return { from, to };
  }
  let from;
  switch (range) {
    case "1M": from = firstOfMonth(0); break;
    case "3M": from = firstOfMonth(2); break;
    case "6M": from = firstOfMonth(5); break;
    case "1Y": from = firstOfMonth(11); break;
    case "YTD": from = new Date(now.getFullYear(), 0, 1); break;
    case "ALL": from = new Date(2000, 0, 1); break;
    default: from = firstOfMonth(2);
  }
  return { from: isoDate(from), to: isoDate(now) };
}

function updateSpendCustomRangeVisibility() {
  const wrap = document.getElementById("spendCustomRangeWrap");
  if (!wrap) return;
  wrap.hidden = state.spend.range !== "CUSTOM";
}

// Group transactions into period buckets keyed by their starting unix-seconds.
// Monthly = first of the month (label: month name); weekly = Monday of the week.
function bucketSpendTransactions(transactions, bucket) {
  const buckets = new Map();
  for (const tx of transactions) {
    const d = new Date((tx.date || 0) * 1000);
    if (Number.isNaN(d.getTime())) continue;
    let key;
    let label;
    if (bucket === "weekly") {
      const day = d.getDay();
      const offset = (day + 6) % 7;
      const monday = new Date(d.getFullYear(), d.getMonth(), d.getDate() - offset);
      key = Math.floor(monday.getTime() / 1000);
      label = monday.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    } else {
      const first = new Date(d.getFullYear(), d.getMonth(), 1);
      key = Math.floor(first.getTime() / 1000);
      label = first.toLocaleDateString(undefined, { month: "short" });
    }
    const bucketEntry = buckets.get(key) || { key, label, total: 0, byPrimary: {} };
    const primary = categoryPrimary(tx.category) || "OTHER";
    bucketEntry.total += tx.amount || 0;
    bucketEntry.byPrimary[primary] = (bucketEntry.byPrimary[primary] || 0) + (tx.amount || 0);
    buckets.set(key, bucketEntry);
  }
  return Array.from(buckets.values()).sort((a, b) => a.key - b.key);
}

function categoryTotals(transactions) {
  const totals = {};
  for (const tx of transactions) {
    const primary = categoryPrimary(tx.category) || "OTHER";
    totals[primary] = (totals[primary] || 0) + (tx.amount || 0);
  }
  return totals;
}

function createSpendBarChart(canvas, buckets, primariesInUse) {
  if (!canvas || !window.Chart) return null;
  const labels = buckets.map((b) => b.label);
  const datasets = primariesInUse.map((primary) => ({
    label: spendCategoryLabel(primary),
    data: buckets.map((b) => Number((b.byPrimary[primary] || 0).toFixed(2))),
    backgroundColor: spendCategoryColor(primary),
    borderWidth: 0,
    stack: "spend"
  }));
  return new window.Chart(canvas, {
    type: "bar",
    data: { labels, datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: { position: "bottom", labels: { color: "#222", usePointStyle: true, boxWidth: 10 } },
        tooltip: {
          ...themeTooltipStyle,
          filter: (item) => Math.abs(item.parsed.y || 0) >= 0.005,
          callbacks: {
            label: (ctx) => `${ctx.dataset.label}: ${currency(ctx.parsed.y)}`,
            footer: (items) => {
              const total = items.reduce((sum, it) => sum + (it.parsed.y || 0), 0);
              return `Total: ${currency(total)}`;
            }
          }
        }
      },
      scales: {
        x: { stacked: true, ticks: { color: "#555" }, grid: { display: false } },
        y: {
          stacked: true,
          ticks: { callback: (v) => compactCurrency(v), color: "#555" },
          grid: { color: "rgba(34, 34, 34, 0.08)" }
        }
      }
    }
  });
}

function renderSpendCategoryTable(totals) {
  const entries = Object.entries(totals).sort((a, b) => b[1] - a[1]);
  const grand = entries.reduce((sum, [, v]) => sum + v, 0);
  if (entries.length === 0) {
    return `<p class="muted-note" style="padding:0.75rem;">No spend in this range.</p>`;
  }
  const rows = entries
    .map(([primary, amount]) => {
      const pct = grand > 0 ? (amount / grand) * 100 : 0;
      return `<tr class="spend-cat-row" data-primary="${escapeHtml(primary)}" title="Click to see transactions">
        <td><span class="chip chip-category ${categoryChipClass(primary)}">${escapeHtml(spendCategoryLabel(primary))}</span></td>
        <td class="num">${currency(amount)}</td>
        <td class="num">${pct.toFixed(1)}%</td>
      </tr>`;
    })
    .join("");
  return `<table class="tx-table">
    <thead><tr><th>Category</th><th class="num">Total</th><th class="num">Share</th></tr></thead>
    <tbody>${rows}</tbody>
    <tfoot><tr><th>Total</th><th class="num">${currency(grand)}</th><th class="num">100.0%</th></tr></tfoot>
  </table>`;
}

function renderSpendDrilldownTable(transactions) {
  if (transactions.length === 0) {
    return `<p class="muted-note" style="padding:0.75rem;">No transactions.</p>`;
  }
  const rows = transactions
    .map((tx) => {
      const merchant = tx.notes || "—";
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${escapeHtml(merchant)}</td>
        <td>${escapeHtml(tx.account || "")}</td>
        <td class="num">${currency(tx.amount)}</td>
        <td>
          <button class="ghost-btn drilldown-529-btn" type="button" data-key="${escapeHtml(tx.key)}">529</button>
          <button class="ghost-btn drilldown-deduct-btn" type="button" data-key="${escapeHtml(tx.key)}">Deduct</button>
        </td>
      </tr>`;
    })
    .join("");
  return `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
}

function openSpendCategoryDrilldown(primary) {
  const matching = (state.spend.transactions || [])
    .filter((tx) => (categoryPrimary(tx.category) || "OTHER") === primary)
    .sort((a, b) => (b.date || 0) - (a.date || 0));

  const heading = el.transactionsDialog.querySelector("h3");
  if (heading) heading.textContent = `${spendCategoryLabel(primary)} — ${matching.length} transaction${matching.length === 1 ? "" : "s"}`;
  el.transactionsHistory.innerHTML = `<div class="dialog-table-wrap">${renderSpendDrilldownTable(matching)}</div>`;
  el.transactionsHistory.querySelectorAll(".drilldown-529-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) {
        el.transactionsDialog.close();
        setSpendTab("529");
        openQualify529Dialog(tx);
      }
    });
  });
  el.transactionsHistory.querySelectorAll(".drilldown-deduct-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) {
        el.transactionsDialog.close();
        setSpendTab("tax");
        openMarkDialog("deduction", tx);
      }
    });
  });
  el.transactionsDialog.showModal();
}

function renderSpendAnalysis() {
  const txs = state.spend.transactions || [];
  const buckets = bucketSpendTransactions(txs, state.spend.bucket);
  const totals = categoryTotals(txs);
  const primariesInUse = Object.entries(totals)
    .sort((a, b) => b[1] - a[1])
    .map(([primary]) => primary);

  const grand = primariesInUse.reduce((sum, p) => sum + totals[p], 0);
  const totalLabel = document.getElementById("spendTotalLabel");
  if (totalLabel) {
    totalLabel.textContent = `Total spend: ${currency(grand)} · ${txs.length} transactions`;
  }

  destroyChart("spendBar");
  destroyChart("spendPie");
  const barCanvas = document.getElementById("spendBarChart");
  state.charts.spendBar = createSpendBarChart(barCanvas, buckets, primariesInUse);

  const pieCanvas = document.getElementById("spendPieChart");
  state.charts.spendPie = createPieChart(
    pieCanvas,
    primariesInUse.map(spendCategoryLabel),
    primariesInUse.map((p) => Number(totals[p].toFixed(2))),
    primariesInUse.map(spendCategoryColor)
  );

  const tableHost = document.getElementById("spendCategoryTable");
  if (tableHost) {
    tableHost.innerHTML = renderSpendCategoryTable(totals);
    tableHost.querySelectorAll(".spend-cat-row").forEach((row) => {
      row.addEventListener("click", () => {
        const primary = row.dataset.primary;
        if (primary) openSpendCategoryDrilldown(primary);
      });
    });
  }
}

async function loadSpendData() {
  if (state.spend.loading) return;
  state.spend.loading = true;
  try {
    const { from, to } = rangeToDates(state.spend.range);
    const [payload, tagsPayload] = await Promise.all([
      apiGet(`/api/spend?from=${from}&to=${to}`),
      apiGet("/api/529/tags")
    ]);
    state.spend.transactions = Array.isArray(payload.transactions) ? payload.transactions : [];
    state.spend.tags529 = {};
    (Array.isArray(tagsPayload.tags) ? tagsPayload.tags : []).forEach((tag) => {
      state.spend.tags529[tag.key] = tag;
    });
    renderSpendAnalysis();
    if (state.spend.tab === "529") render529Tab();
  } catch (e) {
    showFlash(`Failed to load spend: ${e.message}`);
  } finally {
    state.spend.loading = false;
  }
}

function setSpendTab(tab) {
  state.spend.tab = tab;
  const analysisPane = document.getElementById("spendAnalysisPane");
  const pane529 = document.getElementById("spend529Pane");
  const analysisBtn = document.getElementById("spendTabAnalysis");
  const btn529 = document.getElementById("spendTab529");
  if (analysisPane) analysisPane.hidden = tab !== "analysis";
  if (pane529) pane529.hidden = tab !== "529";
  if (analysisBtn) analysisBtn.classList.toggle("is-active", tab === "analysis");
  if (btn529) btn529.classList.toggle("is-active", tab === "529");
  if (tab === "529") render529Tab();
  const taxPane = document.getElementById("spendTaxPane");
  const taxBtn = document.getElementById("spendTabTax");
  if (taxPane) taxPane.hidden = tab !== "tax";
  if (taxBtn) taxBtn.classList.toggle("is-active", tab === "tax");
  if (tab === "tax") loadTaxData();
}

const CANDIDATE_529_GENERAL = new Set([
  "GENERAL_MERCHANDISE_OFFICE_SUPPLIES",
  "GENERAL_MERCHANDISE_BOOKSTORES_AND_NEWSSTANDS",
  "GENERAL_MERCHANDISE_SUPERSTORES",
  "GENERAL_MERCHANDISE_DEPARTMENT_STORES",
  "GENERAL_MERCHANDISE_DISCOUNT_STORES",
  "GENERAL_MERCHANDISE_OTHER_GENERAL_MERCHANDISE"
]);

function isCandidate529Category(category) {
  if (!category) return false;
  if (category === "FOOD_AND_DRINK_BEER_WINE_AND_LIQUOR") return false;
  if (category.startsWith("FOOD_AND_DRINK")) return true;
  return CANDIDATE_529_GENERAL.has(category);
}

// 529 room-and-board/supplies only count during the academic year
// (Aug 15 through May 15, inclusive). Summer charges are excluded.
function isWithinAcademicYear(dateTs) {
  const d = new Date(dateTs * 1000);
  const month = d.getUTCMonth() + 1;
  const day = d.getUTCDate();
  if (month > 8 || (month === 8 && day >= 15)) return true;
  if (month < 5 || (month === 5 && day <= 15)) return true;
  return false;
}

function spendRangeBounds() {
  // Same range the /api/spend request used, as unix seconds for filtering
  // denormalized tag dates. "to" is bumped to end-of-day like the backend.
  const { from, to } = rangeToDates(state.spend.range);
  return {
    fromTs: Math.floor(new Date(`${from}T00:00:00Z`).getTime() / 1000),
    toTs: Math.floor(new Date(`${to}T00:00:00Z`).getTime() / 1000) + 86399
  };
}

async function refresh529Tags() {
  try {
    const tagsPayload = await apiGet("/api/529/tags");
    state.spend.tags529 = {};
    (Array.isArray(tagsPayload.tags) ? tagsPayload.tags : []).forEach((tag) => {
      state.spend.tags529[tag.key] = tag;
    });
    render529Tab();
  } catch (e) {
    showFlash(`Failed to refresh 529 tags: ${e.message}`);
  }
}

async function download529Export(kind) {
  const { from, to } = rangeToDates(state.spend.range);
  beginRequest();
  try {
    const response = await fetch(apiUrl(`/api/529/export.${kind}?from=${from}&to=${to}`));
    if (!response.ok) {
      let message = `Export failed: ${response.status}`;
      try {
        message = (await response.json()).error || message;
      } catch (_) { /* non-JSON error body */ }
      throw new Error(message);
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `529-expenses_${from}_to_${to}.${kind}`;
    link.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    showFlash(e.message);
  } finally {
    endRequest();
  }
}

async function downloadTaxExport(kind) {
  const year = state.tax.year;
  beginRequest();
  try {
    const response = await fetch(apiUrl(`/api/tax/export/${kind}.csv?year=${year}`));
    if (!response.ok) {
      let message = `Export failed: ${response.status}`;
      try {
        message = (await response.json()).error || message;
      } catch (_) { /* non-JSON error body */ }
      throw new Error(message);
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `tax-${kind}_${year}.csv`;
    link.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    showFlash(e.message);
  } finally {
    endRequest();
  }
}

async function saveTag529(key, status, qualifiedAmount, refresh = true) {
  const body = { key, status };
  if (status === "qualified" && qualifiedAmount != null) {
    body.qualified_amount = qualifiedAmount;
  }
  await apiPost("/api/529/tag", body);
  if (refresh) await refresh529Tags();
}

function taxYearBounds() {
  const y = state.tax.year;
  return { from: `${y}-01-01`, to: `${y}-12-31` };
}

let taxLoadSeq = 0;

async function loadTaxData() {
  const seq = ++taxLoadSeq;
  state.tax.loading = true;
  try {
    const { from, to } = taxYearBounds();
    const [spendPayload, incomePayload, tagsPayload] = await Promise.all([
      apiGet(`/api/spend?from=${from}&to=${to}`),
      apiGet(`/api/income?from=${from}&to=${to}`),
      apiGet("/api/tax/tags")
    ]);
    if (seq !== taxLoadSeq) return;
    state.tax.spendTxs = Array.isArray(spendPayload.transactions) ? spendPayload.transactions : [];
    state.tax.incomeTxs = Array.isArray(incomePayload.transactions) ? incomePayload.transactions : [];
    state.tax.tags = { income: {}, deductions: {} };
    (Array.isArray(tagsPayload.income) ? tagsPayload.income : []).forEach((tag) => {
      state.tax.tags.income[tag.key] = tag;
    });
    (Array.isArray(tagsPayload.deductions) ? tagsPayload.deductions : []).forEach((tag) => {
      state.tax.tags.deductions[tag.key] = tag;
    });
    renderTaxTab();
  } catch (e) {
    showFlash(`Failed to load tax data: ${e.message}`);
  } finally {
    if (seq === taxLoadSeq) state.tax.loading = false;
  }
}

async function refreshTaxTags() {
  try {
    const tagsPayload = await apiGet("/api/tax/tags");
    state.tax.tags = { income: {}, deductions: {} };
    (Array.isArray(tagsPayload.income) ? tagsPayload.income : []).forEach((tag) => {
      state.tax.tags.income[tag.key] = tag;
    });
    (Array.isArray(tagsPayload.deductions) ? tagsPayload.deductions : []).forEach((tag) => {
      state.tax.tags.deductions[tag.key] = tag;
    });
    renderTaxTab();
  } catch (e) {
    showFlash(`Failed to refresh tax tags: ${e.message}`);
  }
}

async function saveTaxTag(kind, key, status, amount, refresh = true) {
  const body = { kind, key, status };
  if (status === "qualified" && amount != null) {
    body.amount = amount;
  }
  await apiPost("/api/tax/tag", body);
  if (refresh) await refreshTaxTags();
}

function taxYearRecordFilter(tagMap) {
  const y = state.tax.year;
  const fromTs = Math.floor(Date.UTC(y, 0, 1) / 1000);
  const toTs = Math.floor(Date.UTC(y, 11, 31, 23, 59, 59) / 1000);
  return Object.values(tagMap)
    .filter((tag) => tag.status === "qualified" && tag.date >= fromTs && tag.date <= toTs)
    .sort((a, b) => b.date - a.date);
}

function renderTaxTab() {
  const markedIncome = taxYearRecordFilter(state.tax.tags.income);
  const markedDeductions = taxYearRecordFilter(state.tax.tags.deductions);

  const incomeTotal = markedIncome.reduce((sum, t) => sum + (t.qualified_amount || 0), 0);
  const deductionTotal = markedDeductions.reduce((sum, t) => sum + (t.qualified_amount || 0), 0);

  const incomeSummary = document.getElementById("taxSummaryIncome");
  if (incomeSummary) {
    incomeSummary.textContent = `Taxable income: ${currency(incomeTotal)} · ${markedIncome.length} marked`;
  }
  const deductionSummary = document.getElementById("taxSummaryDeductions");
  if (deductionSummary) {
    deductionSummary.textContent = `Deductible: ${currency(deductionTotal)} · ${markedDeductions.length} marked`;
  }

  const incomeTxByKey = {};
  state.tax.incomeTxs.forEach((tx) => { incomeTxByKey[tx.key] = tx; });
  const spendTxByKey = {};
  state.tax.spendTxs.forEach((tx) => { spendTxByKey[tx.key] = tx; });

  renderTaxIncomeQueue(incomeTxByKey);
  renderTaxMarkedIncome(markedIncome, incomeTxByKey);
  renderTaxDeposits(incomeTxByKey);
  renderTaxMarkedDeductions(markedDeductions, spendTxByKey);
  renderTaxExpenseBrowser(spendTxByKey);
}

function renderTaxMarkedIncome(marked, txByKey) {
  const host = document.getElementById("taxMarkedIncomeTable");
  if (!host) return;
  if (marked.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">No income marked for ${state.tax.year} yet.</p>`;
    return;
  }
  const rows = marked
    .map((tag) => {
      const orphaned = !txByKey[tag.key];
      return `<tr data-key="${escapeHtml(tag.key)}">
        <td>${dateLabel(tag.date)}</td>
        <td>${escapeHtml(tag.notes || "—")}${orphaned ? ` <span class="orphaned-badge" title="No longer in income data">orphaned</span>` : ""}</td>
        <td class="num">${currency(tag.amount)}</td>
        <td class="num">
          <input class="tax-amount-input num" type="number" step="0.01" min="0.01"
                 max="${Number.isFinite(tag.amount) ? tag.amount : 0}" value="${(tag.qualified_amount || 0).toFixed(2)}"
                 data-key="${escapeHtml(tag.key)}" data-kind="income" aria-label="Taxable amount" />
        </td>
        <td><button class="ghost-btn tax-unmark-btn" type="button" data-key="${escapeHtml(tag.key)}" data-kind="income">Unmark</button></td>
      </tr>`;
    })
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Source</th><th class="num">Deposit</th><th class="num">Taxable</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  wireTaxMarkedListEvents(host);
}

function renderTaxMarkedDeductions(marked, txByKey) {
  const host = document.getElementById("taxMarkedDeductionsTable");
  if (!host) return;
  if (marked.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">No deductions marked for ${state.tax.year} yet.</p>`;
    return;
  }
  const rows = marked
    .map((tag) => {
      const orphaned = !txByKey[tag.key];
      return `<tr data-key="${escapeHtml(tag.key)}">
        <td>${dateLabel(tag.date)}</td>
        <td>${escapeHtml(tag.notes || "—")}${orphaned ? ` <span class="orphaned-badge" title="No longer in spend data">orphaned</span>` : ""}</td>
        <td class="num">${currency(tag.amount)}</td>
        <td class="num">
          <input class="tax-amount-input num" type="number" step="0.01" min="0.01"
                 max="${Number.isFinite(tag.amount) ? tag.amount : 0}" value="${(tag.qualified_amount || 0).toFixed(2)}"
                 data-key="${escapeHtml(tag.key)}" data-kind="deduction" aria-label="Deductible amount" />
        </td>
        <td><button class="ghost-btn tax-unmark-btn" type="button" data-key="${escapeHtml(tag.key)}" data-kind="deduction">Unmark</button></td>
      </tr>`;
    })
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th class="num">Charge</th><th class="num">Deductible</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  wireTaxMarkedListEvents(host);
}

function wireTaxMarkedListEvents(host) {
  host.querySelectorAll(".tax-amount-input").forEach((input) => {
    input.addEventListener("change", async () => {
      const kind = input.dataset.kind;
      const tagMap = kind === "income" ? state.tax.tags.income : state.tax.tags.deductions;
      const tag = tagMap[input.dataset.key];
      if (!tag) return;
      const value = Number.parseFloat(input.value);
      if (!Number.isFinite(value) || value <= 0 || value > tag.amount + 0.005) {
        showFlash("Amount must be between $0.01 and the transaction amount.");
        input.value = (tag.qualified_amount || 0).toFixed(2);
        return;
      }
      try {
        await saveTaxTag(kind, input.dataset.key, "qualified", value);
      } catch (e) {
        showFlash(`Update failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll(".tax-unmark-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      try {
        await saveTaxTag(btn.dataset.kind, btn.dataset.key, "none", null);
      } catch (e) {
        showFlash(`Unmark failed: ${e.message}`);
      } finally {
        btn.disabled = false;
      }
    });
  });
}

function renderTaxIncomeQueue(incomeTxByKey) {
  const host = document.getElementById("taxIncomeQueueTable");
  if (!host) return;
  const queue = state.tax.incomeTxs
    .filter((tx) =>
      (tx.category || "").startsWith("INCOME_") &&
      !state.tax.tags.income[tx.key])
    .sort((a, b) => (b.date || 0) - (a.date || 0));
  if (queue.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">Queue is clear — no unreviewed income deposits in ${escapeHtml(String(state.tax.year))}.</p>`;
    return;
  }
  const rows = queue
    .map((tx) => `<tr>
      <td>${dateLabel(tx.date)}</td>
      <td>${escapeHtml(tx.notes || "—")}</td>
      <td>${escapeHtml(tx.account || "")}</td>
      <td class="num">${currency(tx.amount)}</td>
      <td>
        <button class="ghost-btn tax-mark-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Mark taxable</button>
        <button class="ghost-btn tax-dismiss-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✕ Dismiss</button>
      </td>
    </tr>`)
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Source</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  host.querySelectorAll(".tax-mark-income-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = incomeTxByKey[btn.dataset.key];
      if (tx) openMarkDialog("income", tx);
    });
  });
  host.querySelectorAll(".tax-dismiss-income-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTaxTag("income", btn.dataset.key, "dismissed", null);
      } catch (e) {
        showFlash(`Dismiss failed: ${e.message}`);
      }
    });
  });
}

function renderTaxDeposits(incomeTxByKey) {
  const host = document.getElementById("taxDepositsTable");
  if (!host) return;
  const needle = state.tax.depositsSearch.trim().toLowerCase();
  const rows = state.tax.incomeTxs
    .filter((tx) => !needle || (tx.notes || "").toLowerCase().includes(needle))
    .sort((a, b) => (b.date || 0) - (a.date || 0))
    .map((tx) => {
      const tag = state.tax.tags.income[tx.key];
      const marked = tag && tag.status === "qualified";
      const action = marked
        ? `<span class="muted-note">marked</span>`
        : `<button class="ghost-btn tax-mark-income-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Mark taxable</button>`;
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${escapeHtml(tx.notes || "—")}</td>
        <td>${escapeHtml(tx.account || "")}</td>
        <td class="num">${currency(tx.amount)}</td>
        <td>${action}</td>
      </tr>`;
    })
    .join("");
  host.innerHTML = rows
    ? `<table class="tx-table"><thead><tr><th>Date</th><th>Source</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
    : `<p class="muted-note" style="padding:0.75rem;">No deposits match.</p>`;
  host.querySelectorAll(".tax-mark-income-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = incomeTxByKey[btn.dataset.key];
      if (tx) openMarkDialog("income", tx);
    });
  });
}

function renderTaxExpenseBrowser(spendTxByKey) {
  const host = document.getElementById("taxExpensesTable");
  if (!host) return;
  const needle = state.tax.expensesSearch.trim().toLowerCase();
  const rows = state.tax.spendTxs
    .filter((tx) => !needle || (tx.notes || "").toLowerCase().includes(needle))
    .sort((a, b) => (b.date || 0) - (a.date || 0))
    .map((tx) => {
      const tag = state.tax.tags.deductions[tx.key];
      const marked = tag && tag.status === "qualified";
      const action = marked
        ? `<span class="muted-note">marked</span>`
        : `<button class="ghost-btn tax-deduct-expense-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Deduct</button>`;
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${escapeHtml(tx.notes || "—")}</td>
        <td>${escapeHtml(tx.account || "")}</td>
        <td class="num">${currency(tx.amount)}</td>
        <td>${action}</td>
      </tr>`;
    })
    .join("");
  host.innerHTML = rows
    ? `<table class="tx-table"><thead><tr><th>Date</th><th>Merchant</th><th>Account</th><th class="num">Amount</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
    : `<p class="muted-note" style="padding:0.75rem;">No expenses match.</p>`;
  host.querySelectorAll(".tax-deduct-expense-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = spendTxByKey[btn.dataset.key];
      if (tx) openMarkDialog("deduction", tx);
    });
  });
}

function render529Tab() {
  const { fromTs, toTs } = spendRangeBounds();
  const txByKey = {};
  (state.spend.transactions || []).forEach((tx) => { txByKey[tx.key] = tx; });

  const qualified = Object.values(state.spend.tags529)
    .filter((tag) => tag.status === "qualified" && tag.date >= fromTs && tag.date <= toTs)
    .sort((a, b) => b.date - a.date);

  const total = qualified.reduce((sum, tag) => sum + (tag.qualified_amount || 0), 0);
  const missingReceipts = qualified.filter((tag) => (tag.receipts || []).length === 0).length;

  const totalLabel = document.getElementById("total529Label");
  if (totalLabel) totalLabel.textContent = `529 qualified total: ${currency(total)}`;
  const meta = document.getElementById("summary529Meta");
  if (meta) {
    const missingNote = missingReceipts > 0
      ? ` · ⚠ ${missingReceipts} missing receipt${missingReceipts === 1 ? "" : "s"}`
      : "";
    meta.textContent = `${qualified.length} qualified charge${qualified.length === 1 ? "" : "s"} in range${missingNote}`;
  }

  render529Queue(txByKey);           // Task 9
  render529QualifiedList(qualified, txByKey); // below
}

function render529QualifiedList(qualified, txByKey) {
  const host = document.getElementById("qualified529Table");
  if (!host) return;
  if (qualified.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">Nothing qualified in this range yet.</p>`;
    return;
  }
  const rows = qualified
    .map((tag) => {
      const orphaned = !txByKey[tag.key];
      const receipts = (tag.receipts || [])
        .map((name) =>
          `<a class="receipt-chip" target="_blank"
              href="${escapeHtml(apiUrl(`/api/529/receipt?key=${encodeURIComponent(tag.key)}&filename=${encodeURIComponent(name)}`))}"
           >${escapeHtml(name)}</a>
           <button class="receipt-delete" type="button" data-key="${escapeHtml(tag.key)}"
                   data-filename="${escapeHtml(name)}" title="Delete receipt">×</button>`)
        .join(" ");
      const receiptCell = receipts ||
        `<span class="missing-receipt-badge">⚠ no receipt</span>`;
      return `<tr data-key="${escapeHtml(tag.key)}">
        <td>${dateLabel(tag.date)}</td>
        <td>${escapeHtml(tag.notes || "—")}${orphaned ? ` <span class="orphaned-badge" title="No longer in spend data">orphaned</span>` : ""}</td>
        <td class="num">${currency(tag.amount)}</td>
        <td class="num">
          <input class="qualified-amount-input num" type="number" step="0.01" min="0.01"
                 max="${Number.isFinite(tag.amount) ? tag.amount : 0}" value="${(tag.qualified_amount || 0).toFixed(2)}"
                 data-key="${escapeHtml(tag.key)}" aria-label="Qualified amount" />
        </td>
        <td class="receipt-cell">${receiptCell}
          <button class="ghost-btn receipt-upload-btn" type="button" data-key="${escapeHtml(tag.key)}">＋ Receipt</button>
        </td>
        <td><button class="ghost-btn untag-529-btn" type="button" data-key="${escapeHtml(tag.key)}">Untag</button></td>
      </tr>`;
    })
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th class="num">Charge</th><th class="num">Qualified</th><th>Receipts</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;
  wire529QualifiedListEvents(host); // Task 9/10 attach handlers; define an empty stub now
}

function wire529QualifiedListEvents(host) {
  host.querySelectorAll(".qualified-amount-input").forEach((input) => {
    input.addEventListener("change", async () => {
      const value = Number.parseFloat(input.value);
      const tag = state.spend.tags529[input.dataset.key];
      if (!tag) return;
      if (!Number.isFinite(value) || value <= 0 || value > tag.amount + 0.005) {
        showFlash("Qualified amount must be between $0.01 and the charge amount.");
        input.value = (tag.qualified_amount || 0).toFixed(2);
        return;
      }
      try {
        await saveTag529(input.dataset.key, "qualified", value);
      } catch (e) {
        showFlash(`Update failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll(".untag-529-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      try {
        await saveTag529(btn.dataset.key, "none", null);
      } catch (e) {
        showFlash(`Untag failed: ${e.message}`);
      } finally {
        btn.disabled = false;
      }
    });
  });
  host.querySelectorAll(".receipt-upload-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const fileInput = document.getElementById("receipt529FileInput");
      if (!fileInput) return;
      fileInput.dataset.key = btn.dataset.key;
      fileInput.value = "";
      fileInput.click();
    });
  });
  host.querySelectorAll(".receipt-delete").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await apiDelete(`/api/529/receipt?key=${encodeURIComponent(btn.dataset.key)}&filename=${encodeURIComponent(btn.dataset.filename)}`);
        await refresh529Tags();
      } catch (e) {
        showFlash(`Delete failed: ${e.message}`);
      }
    });
  });
  host.querySelectorAll("tr[data-key]").forEach((row) => {
    row.addEventListener("dragover", (event) => {
      event.preventDefault();
      row.classList.add("receipt-drop-active");
    });
    row.addEventListener("dragleave", () => row.classList.remove("receipt-drop-active"));
    row.addEventListener("drop", async (event) => {
      event.preventDefault();
      row.classList.remove("receipt-drop-active");
      const files = Array.from(event.dataTransfer?.files || []);
      if (files.length === 0) return;
      try {
        for (const file of files) {
          await apiUploadReceipt(row.dataset.key, file);
        }
      } catch (e) {
        showFlash(`Upload failed: ${e.message}`);
      } finally {
        await refresh529Tags();
      }
    });
  });
}

function render529Queue(txByKey) {
  const host = document.getElementById("review529Table");
  if (!host) return;
  const queue = (state.spend.transactions || [])
    .filter((tx) =>
      tx.account_type === "DEBT" &&
      isCandidate529Category(tx.category) &&
      isWithinAcademicYear(tx.date) &&
      !state.spend.tags529[tx.key])
    .sort((a, b) => (b.date || 0) - (a.date || 0));

  if (queue.length === 0) {
    host.innerHTML = `<p class="muted-note" style="padding:0.75rem;">Queue is clear — nothing to review in this range.</p>`;
    return;
  }
  const rows = queue
    .map((tx) => `<tr data-key="${escapeHtml(tx.key)}">
      <td>${dateLabel(tx.date)}</td>
      <td>${escapeHtml(tx.notes || "—")}</td>
      <td><span class="chip chip-category ${categoryChipClass(categoryPrimary(tx.category) || "OTHER")}">${escapeHtml(spendCategoryLabel(categoryPrimary(tx.category) || "OTHER"))}</span></td>
      <td class="num">${currency(tx.amount)}</td>
      <td>
        <button class="ghost-btn qualify-529-btn" type="button" data-key="${escapeHtml(tx.key)}">✓ Qualify</button>
        <button class="ghost-btn dismiss-529-btn" type="button" data-key="${escapeHtml(tx.key)}">✕ Dismiss</button>
      </td>
    </tr>`)
    .join("");
  host.innerHTML = `<table class="tx-table">
    <thead><tr><th>Date</th><th>Merchant</th><th>Category</th><th class="num">Amount</th><th></th></tr></thead>
    <tbody>${rows}</tbody>
  </table>`;

  host.querySelectorAll(".qualify-529-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tx = (state.spend.transactions || []).find((t) => t.key === btn.dataset.key);
      if (tx) openQualify529Dialog(tx);
    });
  });
  host.querySelectorAll(".dismiss-529-btn").forEach((btn) => {
    btn.addEventListener("click", async () => {
      try {
        await saveTag529(btn.dataset.key, "dismissed", null);
      } catch (e) {
        showFlash(`Dismiss failed: ${e.message}`);
      }
    });
  });
}

let qualify529Target = null;
let markDialogMode = "529";

const MARK_DIALOG_COPY = {
  "529": { title: "Qualify for 529", verb: "Qualify", showFiles: true, amountLabel: "Qualified amount" },
  "income": { title: "Mark taxable income", verb: "Mark", showFiles: false, amountLabel: "Taxable amount" },
  "deduction": { title: "Mark tax deductible", verb: "Mark", showFiles: false, amountLabel: "Deductible amount" }
};

function openMarkDialog(mode, tx) {
  markDialogMode = mode;
  qualify529Target = tx;
  const dialog = document.getElementById("qualify529Dialog");
  const title = document.getElementById("qualify529Title");
  const merchant = document.getElementById("qualify529Merchant");
  const amount = document.getElementById("qualify529Amount");
  const files = document.getElementById("qualify529Files");
  const filesWrap = document.getElementById("qualify529FilesWrap");
  if (!dialog || !amount) return;
  const copy = MARK_DIALOG_COPY[mode];
  if (title) title.textContent = copy.title;
  if (merchant) merchant.textContent = `${tx.notes || "—"} · ${dateLabel(tx.date)} · ${currency(tx.amount)}`;
  amount.value = tx.amount.toFixed(2);
  amount.max = tx.amount;
  if (files) files.value = "";
  if (filesWrap) filesWrap.hidden = !copy.showFiles;
  const amountLabel = document.getElementById("qualify529AmountLabel");
  if (amountLabel) amountLabel.textContent = copy.amountLabel;
  dialog.showModal();
}

function openQualify529Dialog(tx) {
  openMarkDialog("529", tx);
}

function wireQualify529Dialog() {
  const dialog = document.getElementById("qualify529Dialog");
  if (!dialog) return;
  const cancel = document.getElementById("qualify529Cancel");
  const save = document.getElementById("qualify529Save");
  dialog.addEventListener("close", () => { qualify529Target = null; });
  if (cancel) cancel.addEventListener("click", () => dialog.close());
  if (save) {
    save.addEventListener("click", async () => {
      if (!qualify529Target) return;
      const amountInput = document.getElementById("qualify529Amount");
      const filesInput = document.getElementById("qualify529Files");
      const qualifiedAmount = Number.parseFloat(amountInput.value);
      if (!Number.isFinite(qualifiedAmount) || qualifiedAmount <= 0 ||
          qualifiedAmount > qualify529Target.amount + 0.005) {
        showFlash("Amount must be between $0.01 and the transaction amount.");
        return;
      }
      save.disabled = true;
      try {
        const key = qualify529Target.key;
        if (markDialogMode === "529") {
          if (!isWithinAcademicYear(qualify529Target.date)) {
            showFlash("Only academic-year expenses (Aug 15 – May 15) qualify for 529.");
            return;
          }
          const files = filesInput ? Array.from(filesInput.files) : [];
          if (files.length > 0) {
            await saveTag529(key, "qualified", qualifiedAmount, false);
            try {
              for (const file of files) {
                await apiUploadReceipt(key, file);
              }
            } finally {
              await refresh529Tags();
            }
          } else {
            await saveTag529(key, "qualified", qualifiedAmount);
          }
          dialog.close();
        } else if (markDialogMode === "income") {
          await saveTaxTag("income", key, "qualified", qualifiedAmount);
          dialog.close();
        } else {
          await saveTaxTag("deduction", key, "qualified", qualifiedAmount);
          dialog.close();
        }
      } catch (e) {
        showFlash(`${MARK_DIALOG_COPY[markDialogMode].verb} failed: ${e.message}`);
      } finally {
        save.disabled = false;
      }
    });
  }
}

function wireTaxStaticControls() {
  const yearSelect = document.getElementById("taxYearSelect");
  if (yearSelect) {
    const current = new Date().getFullYear();
    for (let y = current; y >= current - 3; y--) {
      const opt = document.createElement("option");
      opt.value = String(y);
      opt.textContent = String(y);
      yearSelect.appendChild(opt);
    }
    yearSelect.value = String(state.tax.year);
    yearSelect.addEventListener("change", () => {
      state.tax.year = Number.parseInt(yearSelect.value, 10);
      loadTaxData();
    });
  }
  const depositsSearch = document.getElementById("taxDepositsSearch");
  if (depositsSearch) {
    depositsSearch.addEventListener("input", () => {
      state.tax.depositsSearch = depositsSearch.value;
      const incomeTxByKey = {};
      state.tax.incomeTxs.forEach((tx) => { incomeTxByKey[tx.key] = tx; });
      renderTaxDeposits(incomeTxByKey);
    });
  }
  const expensesSearch = document.getElementById("taxExpensesSearch");
  if (expensesSearch) {
    expensesSearch.addEventListener("input", () => {
      state.tax.expensesSearch = expensesSearch.value;
      const spendTxByKey = {};
      state.tax.spendTxs.forEach((tx) => { spendTxByKey[tx.key] = tx; });
      renderTaxExpenseBrowser(spendTxByKey);
    });
  }
}

function showSpendAnalysis() {
  stopLiveRefreshTimer();
  stopDashboardLiveRefreshTimer();
  setMarketStateChip("UNKNOWN");
  setPortfolioLastUpdatedChip(0);
  setActiveView("spend");
  setBreadcrumbs([
    { label: "Assets", onClick: showDashboard },
    { label: "Spending & Tax" }
  ]);

  const bucketSelect = document.getElementById("spendBucketSelect");
  const rangeSelect = document.getElementById("spendRangeSelect");
  if (bucketSelect) bucketSelect.value = state.spend.bucket;
  if (rangeSelect) rangeSelect.value = state.spend.range;

  const customFromInput = document.getElementById("spendCustomFrom");
  const customToInput = document.getElementById("spendCustomTo");
  if (customFromInput) customFromInput.value = state.spend.customFrom || "";
  if (customToInput) customToInput.value = state.spend.customTo || "";
  updateSpendCustomRangeVisibility();

  loadSpendData();
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
  const debt = type === "DEBT";
  el.createPortfolioCapitalRow.hidden = watchlist;
  el.createPortfolioCapital.disabled = watchlist;
  const capitalLabel = el.createPortfolioCapitalRow.querySelector("label");
  if (capitalLabel) {
    capitalLabel.textContent = debt
      ? "Current Debt Balance"
      : cash ? "Starting Balance" : "Initial Capital";
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
  const debt = isDebtPortfolio(state.currentPortfolio);
  const balanceLike = cash || debt;
  const assetNoun = crypto ? "Crypto" : "Asset";
  Array.from(el.transactionType.options).forEach((opt) => {
    const assetOnly = opt.value === "buy" || opt.value === "sell" ||
                      opt.value === "dividend" ||
                      opt.value === "transfer_in_asset" || opt.value === "transfer_out_asset";
    opt.hidden = balanceLike && assetOnly;
    opt.disabled = balanceLike && assetOnly;
    if (opt.value === "buy") opt.textContent = crypto ? "Buy" : "Buy Stock";
    if (opt.value === "sell") opt.textContent = crypto ? "Sell" : "Sell Stock";
    if (opt.value === "transfer_in_asset") opt.textContent = `Transfer In ${assetNoun}`;
    if (opt.value === "transfer_out_asset") opt.textContent = `Transfer Out ${assetNoun}`;
    if (opt.value === "dividend") opt.hidden = balanceLike || crypto;
    if (opt.value === "dividend") opt.disabled = balanceLike || crypto;
    if (opt.value === "deposit") opt.textContent = debt ? "New Charge" : "Deposit";
    if (opt.value === "withdrawal") opt.textContent = debt ? "Payment" : "Withdrawal";
    if (opt.value === "interest") opt.textContent = debt ? "Interest Charge" : "Interest";
  });
  const tickerLabel = el.groupTicker?.querySelector("label");
  if (tickerLabel) {
    tickerLabel.textContent = crypto ? "Symbol" : "Ticker";
  }
  if (el.transactionTicker) {
    el.transactionTicker.placeholder = crypto ? "BTC-USD" : "AAPL";
  }
  if (balanceLike) {
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
    showFlash("Transaction recorded. Refreshing portfolio…", "info");
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
        showFlash("Transaction recorded successfully.", "success");
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
    const hideTickerShares =
      isDebtPortfolio(state.currentPortfolio) || isCashPortfolio(state.currentPortfolio);
    el.transactionsHistory.innerHTML = `<div class="dialog-table-wrap">${renderTransactionTable(payload.transactions || [], { showNotes: true, hideTickerShares })}</div>`;
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
    showFlash(`Created account ${portfolioDisplayName(createdName)}.`, "success");
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
    showFlash(`Deleted account ${portfolioDisplayName(name)}.`, "success");
  } catch (error) {
    showFlash(error.message);
  } finally {
    el.deleteAccountConfirmBtn.disabled = false;
  }
}

async function refreshInTransit() {
  try {
    const data = await apiGet("/api/in-transit");
    state.inTransit = {
      total: Number(data?.total) || 0,
      entries: Array.isArray(data?.entries) ? data.entries : []
    };
  } catch (_) {
    state.inTransit = { total: 0, entries: [] };
  }
}

async function loadDashboard() {
  hideFlash();
  try {
    const [payload] = await Promise.all([
      apiGet("/api/portfolios"),
      refreshInTransit()
    ]);
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
    if (isNetworkError(error) || isOffline()) {
      showFlash("You're offline and no cached data is saved yet. Reconnect and reload.", "info");
    } else {
      showFlash(`Unable to load portfolios. ${error.message}. Check API Settings if needed.`);
    }
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
  el.connectAccountBtn.addEventListener("click", () => startPlaidConnect());
  el.syncAccountBtn.addEventListener("click", syncCurrentConnection);
  el.disconnectAccountBtn.addEventListener("click", disconnectCurrentConnection);

  el.apiConfigToggle.addEventListener("click", toggleApiPanel);
  el.apiConfigForm.addEventListener("submit", saveApiBase);
  el.apiResetBtn.addEventListener("click", resetApiBase);

  const backFromSpend = document.getElementById("backToDashFromSpendBtn");
  if (backFromSpend) backFromSpend.addEventListener("click", showDashboard);
  const spendTabAnalysis = document.getElementById("spendTabAnalysis");
  const spendTab529 = document.getElementById("spendTab529");
  if (spendTabAnalysis) spendTabAnalysis.addEventListener("click", () => setSpendTab("analysis"));
  if (spendTab529) spendTab529.addEventListener("click", () => setSpendTab("529"));
  const spendTabTax = document.getElementById("spendTabTax");
  if (spendTabTax) spendTabTax.addEventListener("click", () => setSpendTab("tax"));
  wireQualify529Dialog();
  wireTaxStaticControls();
  const exportCsvBtn = document.getElementById("export529CsvBtn");
  const exportZipBtn = document.getElementById("export529ZipBtn");
  if (exportCsvBtn) exportCsvBtn.addEventListener("click", () => download529Export("csv"));
  if (exportZipBtn) exportZipBtn.addEventListener("click", () => download529Export("zip"));
  const exportTaxIncomeBtn = document.getElementById("exportTaxIncomeBtn");
  const exportTaxDeductionsBtn = document.getElementById("exportTaxDeductionsBtn");
  if (exportTaxIncomeBtn) exportTaxIncomeBtn.addEventListener("click", () => downloadTaxExport("income"));
  if (exportTaxDeductionsBtn) exportTaxDeductionsBtn.addEventListener("click", () => downloadTaxExport("deductions"));
  const receiptInput = document.getElementById("receipt529FileInput");
  if (receiptInput) {
    receiptInput.addEventListener("change", async () => {
      const key = receiptInput.dataset.key;
      const files = Array.from(receiptInput.files);
      if (!key || files.length === 0) return;
      try {
        for (const file of files) {
          await apiUploadReceipt(key, file);
        }
      } catch (e) {
        showFlash(`Upload failed: ${e.message}`);
      } finally {
        await refresh529Tags();
      }
    });
  }
  const spendBucketSelect = document.getElementById("spendBucketSelect");
  if (spendBucketSelect) {
    spendBucketSelect.addEventListener("change", (event) => {
      state.spend.bucket = event.target.value;
      localStorage.setItem("ft.spend.bucket", state.spend.bucket);
      renderSpendAnalysis();
    });
  }
  const spendRangeSelect = document.getElementById("spendRangeSelect");
  if (spendRangeSelect) {
    spendRangeSelect.addEventListener("change", (event) => {
      state.spend.range = event.target.value;
      localStorage.setItem("ft.spend.range", state.spend.range);
      updateSpendCustomRangeVisibility();
      if (state.spend.range === "CUSTOM" &&
          !state.spend.customFrom && !state.spend.customTo) {
        // Defer the fetch until the user picks dates; avoids a useless empty request.
        return;
      }
      loadSpendData();
    });
  }

  const spendCustomFromInput = document.getElementById("spendCustomFrom");
  const spendCustomToInput = document.getElementById("spendCustomTo");
  const handleCustomChange = () => {
    state.spend.customFrom = spendCustomFromInput ? spendCustomFromInput.value : "";
    state.spend.customTo = spendCustomToInput ? spendCustomToInput.value : "";
    localStorage.setItem("ft.spend.customFrom", state.spend.customFrom);
    localStorage.setItem("ft.spend.customTo", state.spend.customTo);
    if (state.spend.range !== "CUSTOM") return;
    if (!state.spend.customFrom || !state.spend.customTo) return;
    loadSpendData();
  };
  if (spendCustomFromInput) spendCustomFromInput.addEventListener("change", handleCustomChange);
  if (spendCustomToInput) spendCustomToInput.addEventListener("change", handleCustomChange);
}

async function startPlaidConnect(portfolioName) {
  // Two entry points: the portfolio-detail "Connect Account" button (no arg —
  // uses the currently open portfolio) and the dashboard "Reconnect" chip
  // (passes the portfolio name directly so the dashboard doesn't need to
  // navigate into the account first).
  const name = portfolioName || state.currentPortfolio?.name;
  if (!name) return;
  if (typeof Plaid === "undefined" || !Plaid.create) {
    showFlash("Plaid Link could not load (network or ad blocker?).");
    return;
  }
  const reconnectBtn = portfolioName
    ? document.querySelector(`.reauth-chip[data-reauth-name="${encodeURIComponent(portfolioName)}"]`)
    : null;
  try {
    if (el.connectAccountBtn) el.connectAccountBtn.disabled = true;
    if (reconnectBtn) reconnectBtn.disabled = true;
    showFlash("Opening Plaid Link…", "info");
    const resp = await apiPost(`/api/portfolios/${encodeURIComponent(name)}/connection/link-token`, {});
    hideFlash();
    const handler = Plaid.create({
      token: resp.link_token,
      onSuccess: async (public_token, metadata) => {
        const accts = Array.isArray(metadata?.accounts) ? metadata.accounts : [];
        let accountId = accts[0]?.id || "";
        if (accts.length > 1) {
          const list = accts
            .map((a, i) => `${i + 1}. ${a.name || "(unnamed)"} — ${a.subtype || a.type || "?"}${a.mask ? ` …${a.mask}` : ""}`)
            .join("\n");
          const raw = window.prompt(
            `You selected ${accts.length} accounts at this institution.\n` +
            `Pick which one to link to "${portfolioDisplayName(name)}":\n\n${list}\n\n` +
            `Enter 1-${accts.length}:`,
            "1"
          );
          if (raw === null) {
            showFlash("Connection cancelled.", "info");
            return;
          }
          const idx = parseInt(raw, 10) - 1;
          if (Number.isNaN(idx) || idx < 0 || idx >= accts.length) {
            showFlash("Invalid choice — connection cancelled.");
            return;
          }
          accountId = accts[idx].id;
        }
        try {
          showFlash("Linking account and importing transactions…", "info");
          await apiPost(`/api/portfolios/${encodeURIComponent(name)}/connection`, {
            public_token,
            account_id: accountId
          });
          showFlash("Account connected and synced.", "success");
          await openPortfolio(name);
        } catch (e) {
          showFlash(`Connect failed: ${e.message}`);
        }
      },
      onExit: (err) => {
        if (err) showFlash(`Plaid Link error: ${err.display_message || err.error_message || err.error_code || "exited"}`);
      }
    });
    handler.open();
  } catch (e) {
    showFlash(`Could not start Plaid Link: ${e.message}`);
  } finally {
    if (el.connectAccountBtn) el.connectAccountBtn.disabled = false;
    if (reconnectBtn) reconnectBtn.disabled = false;
  }
}

async function syncCurrentConnection() {
  if (!state.currentPortfolio) return;
  const name = state.currentPortfolio.name;
  try {
    el.syncAccountBtn.disabled = true;
    showFlash("Syncing latest transactions…", "info");
    const resp = await apiPost(`/api/portfolios/${encodeURIComponent(name)}/connection/sync`, {});
    showFlash(`Synced ${resp.transactions_imported} transactions.`, "success");
    await openPortfolio(name);
  } catch (e) {
    showFlash(`Sync failed: ${e.message}`);
  } finally {
    el.syncAccountBtn.disabled = false;
  }
}

async function disconnectCurrentConnection() {
  if (!state.currentPortfolio) return;
  const name = state.currentPortfolio.name;
  const ok = window.confirm(`Disconnect ${portfolioDisplayName(name)}? Existing transactions stay; future syncs stop.`);
  if (!ok) return;
  try {
    el.disconnectAccountBtn.disabled = true;
    await apiDelete(`/api/portfolios/${encodeURIComponent(name)}/connection`);
    showFlash("Account disconnected.", "success");
    await openPortfolio(name);
  } catch (e) {
    showFlash(`Disconnect failed: ${e.message}`);
  } finally {
    el.disconnectAccountBtn.disabled = false;
  }
}

function handleOnline() {
  setOfflineState(false);
  // Pull a fresh snapshot now that we're back. loadDashboard() is the same
  // path used at startup, so this restores live data and (re)opens the saved
  // portfolio if any.
  loadDashboard();
}

function handleOffline() {
  setOfflineState(true);
}

function registerServiceWorker() {
  if (!("serviceWorker" in navigator)) return;
  // The SW is only useful when the app is served over http(s); file:// origins
  // cannot register one. Same-origin only — no apiBase here.
  if (!/^https?:$/.test(window.location.protocol)) return;
  navigator.serviceWorker.register("sw.js").catch(() => {
    // Best-effort — offline cache for API responses still works without the SW.
  });
}

function init() {
  el.apiBaseInput.value = state.apiBase;
  setActiveView("dashboard");
  wireEvents();
  document.addEventListener("visibilitychange", handleVisibilityChange);
  window.addEventListener("online", handleOnline);
  window.addEventListener("offline", handleOffline);
  updateTransactionFormVisibility();
  resetCreatePortfolioForm();
  renderOfflineBanner();
  registerServiceWorker();
  loadDashboard();
}

init();

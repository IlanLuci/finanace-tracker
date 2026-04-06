const API_STORAGE_KEY = "portfolio-api-base";
const CURRENT_PORTFOLIO_KEY = "portfolio-current";

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
  }
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
  createPortfolioSubmitBtn: document.getElementById("createPortfolioSubmitBtn"),
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
  const totalsByDay = new Map();

  portfolios.forEach((portfolio) => {
    (portfolio.daily_values || []).forEach((point) => {
      const key = safeNumber(point.date);
      const value = safeNumber(point.value);
      if (key <= 0) {
        return;
      }
      totalsByDay.set(key, (totalsByDay.get(key) || 0) + value);
    });
  });

  return Array.from(totalsByDay.entries())
    .map(([date, value]) => ({ date: Number(date), value }))
    .sort((a, b) => a.date - b.date);
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
    return "#1b8b68";
  }
  if (percentChange < -0.0001) {
    return "#c34b3d";
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

function stockDayChangeToneClass(dayChangeAmount) {
  if (dayChangeAmount > 0.0001) {
    return "stock-card-positive";
  }
  if (dayChangeAmount < -0.0001) {
    return "stock-card-negative";
  }
  return "stock-card-neutral";
}

function isWatchlistPortfolio(portfolio) {
  return String(portfolio?.type || "").toUpperCase() === "WATCHLIST";
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
          callbacks: {
            label: (ctx) => currency(ctx.parsed.y)
          }
        }
      },
      scales: {
        x: {
          ticks: {
            maxTicksLimit: 6
          },
          grid: {
            display: false
          }
        },
        y: {
          ticks: {
            callback: (v) => compactCurrency(v)
          },
          grid: {
            color: "rgba(70, 90, 80, 0.12)"
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
          borderColor: "rgba(15, 20, 25, 0.82)",
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
            color: "#d9e1ec",
            usePointStyle: true,
            boxWidth: 10,
            boxHeight: 10,
            font: {
              size: 12,
              weight: "600"
            }
          }
        },
        tooltip: {
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
    "#3b82f6",
    "#10b981",
    "#f59e0b",
    "#60a5fa",
    "#34d399",
    "#fbbf24",
    "#1d4ed8",
    "#059669",
    "#d97706",
    "#93c5fd",
    "#6ee7b7",
    "#fcd34d"
  ];

  return Array.from({ length: Math.max(size, 0) }, (_, index) => themePalette[index % themePalette.length]);
}

function renderTransactionTable(transactions) {
  if (!transactions.length) {
    return "<p>No transactions yet.</p>";
  }

  const rows = transactions
    .map((tx) => {
      const amountClass = tx.amount >= 0 ? "positive" : "negative";
      const symbol = tx.stock_symbol ? tx.stock_symbol : "-";
      const shares = tx.shares ? sharesFormat(tx.shares) : "-";
      const notes = tx.notes || "-";
      return `<tr>
        <td>${dateLabel(tx.date)}</td>
        <td>${typeLabel(tx.type)}</td>
        <td>${symbol}</td>
        <td>${shares}</td>
        <td class="${amountClass}">${currency(tx.amount)}</td>
        <td>${notes}</td>
      </tr>`;
    })
    .join("");

  return `<table class="tx-table"><thead>
    <tr>
      <th>Date</th>
      <th>Type</th>
      <th>Ticker</th>
      <th>Shares</th>
      <th>Amount</th>
      <th>Notes</th>
    </tr>
  </thead><tbody>${rows}</tbody></table>`;
}

function renderDashboard() {
  const portfolios = normalizePortfolios(state.portfolios);
  state.portfolios = portfolios;

  const accountPortfolios = portfolios.filter((p) => !isWatchlistPortfolio(p));

  const totalAssets = accountPortfolios.reduce((sum, p) => sum + (p.estimated_total_value || 0), 0);
  const totalCash = accountPortfolios.reduce((sum, p) => sum + (p.available_capital || 0), 0);
  const totalDayChange = accountPortfolios.reduce((sum, p) => sum + (p.day_change_amount || 0), 0);
  const previousTotalAssets = totalAssets - totalDayChange;
  const totalDayChangePercent = previousTotalAssets > 0 ? (totalDayChange / previousTotalAssets) * 100 : 0;
  const totalStocks = accountPortfolios.reduce((sum, p) => sum + (p.stock_count || 0), 0);
  const totalTransactions = accountPortfolios.reduce((sum, p) => sum + (p.transaction_count || 0), 0);
  const aggregateTrend = mergeDailySeries(accountPortfolios);

  const dashboardMetrics = `<section class="metric-grid">
    ${metricCard("Total Assets", currency(totalAssets), `${accountPortfolios.length} account portfolio${accountPortfolios.length === 1 ? "" : "s"}`)}
    ${metricCard("Day Change", signedCurrency(totalDayChange), `${percentage(totalDayChangePercent)} vs previous close`)}
    ${metricCard("Total Available Cash", currency(totalCash), "Across account portfolios")}
    ${metricCard("Total Stock Positions", String(totalStocks), "Account positions only")}
    ${metricCard("Total Transactions", String(totalTransactions), "Account transaction history")}
  </section>`;

  const cards = portfolios
    .map(
      (p, i) => `<article class="portfolio-card fade-up" data-name="${encodeURIComponent(p.name)}" style="animation-delay:${Math.min(i * 60, 360)}ms">
        <div class="stock-top">
          <strong>${portfolioDisplayName(p.name)}</strong>
          <span class="chip">${typeLabel(p.type)}</span>
        </div>
        ${isWatchlistPortfolio(p)
          ? `<div class="sub">Watchlist only</div>
             <div class="sub">Day: ${changeLabel(p.day_change_amount, p.day_change_percent)}</div>
             <div class="sub">${p.stock_count} symbols tracked</div>`
           : `<div>${currency(p.estimated_total_value)}</div>
             <div class="sub">Day: ${changeLabel(p.day_change_amount, p.day_change_percent)}</div>
             <div class="sub">Cash: ${currency(p.available_capital)}</div>
             <div class="sub">${p.stock_count} stocks • ${p.transaction_count} transactions</div>`}
      </article>`
    )
    .join("");

  const emptyState = `
    <article class="panel empty-state fade-up">
      <h3>No Portfolio Data Yet</h3>
      <p>Portfolio records were not found or could not be read. Create a portfolio in the backend, then refresh.</p>
    </article>
  `;

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
        <h3>Portfolios</h3>
        <button id="openPortfolioCreateDialogBtn" class="primary-btn" type="button">New Portfolio</button>
      </div>
      ${cards ? `<div class="dashboard-cards">${cards}</div>` : emptyState}
    </section>
  `;

  const openPortfolioCreateDialogBtn = document.getElementById("openPortfolioCreateDialogBtn");
  if (openPortfolioCreateDialogBtn) {
    openPortfolioCreateDialogBtn.addEventListener("click", openCreatePortfolioDialog);
  }

  document.querySelectorAll(".portfolio-card").forEach((card) => {
    card.addEventListener("click", () => {
      const portfolioName = decodeURIComponent(card.dataset.name || "");
      openPortfolio(portfolioName);
    });
  });

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

function renderPortfolioDetail(portfolio, stocks, recentTransactions) {
  const watchlist = isWatchlistPortfolio(portfolio);

  el.portfolioName.textContent = portfolioDisplayName(portfolio.name);
  el.portfolioType.textContent = typeLabel(portfolio.type);
  el.stockCount.textContent = watchlist
    ? `${stocks.length} symbol${stocks.length === 1 ? "" : "s"}`
    : `${stocks.length} stock${stocks.length === 1 ? "" : "s"}`;

  el.addWatchlistSymbolBtn.hidden = !watchlist;
  el.addWatchlistSymbolBtn.onclick = watchlist ? () => addWatchlistSymbol() : null;

  if (watchlist) {
    el.portfolioMetrics.innerHTML = [
      metricCard("Tracked Symbols", String(stocks.length), "No transaction ledger"),
      metricCard("Day Change", signedCurrency(portfolio.day_change_amount), `${percentage(portfolio.day_change_percent)} vs previous close`),
      metricCard("Transactions", "Disabled", "Watchlist mode"),
      metricCard("Account Totals", "Excluded", "Not included in portfolio totals"),
      metricCard("Initial Capital", "Not Applicable", "Watchlists do not hold cash")
    ].join("");
  } else {
    const valueDelta = (portfolio.estimated_total_value || 0) - (portfolio.available_capital || 0);
    el.portfolioMetrics.innerHTML = [
      metricCard("Estimated Total", currency(portfolio.estimated_total_value)),
      metricCard("Day Change", signedCurrency(portfolio.day_change_amount), `${percentage(portfolio.day_change_percent)} vs previous close`),
      metricCard("Available Capital", currency(portfolio.available_capital)),
      metricCard("Reported Total", currency(portfolio.reported_total_value)),
      metricCard("Position Value", currency(valueDelta), `${portfolio.transaction_count} transactions`)
    ].join("");
  }

  const sortedStocks = [...stocks].sort((a, b) => (b.position_market_value || 0) - (a.position_market_value || 0));
  el.stocksList.innerHTML = sortedStocks
    .map(
      (stock) => {
        const performance = stockPerformance(stock);
        const toneClass = stockToneClass(
          performance.totalChange,
          performance.hasBasis,
          performance.isZeroCostBasisPosition
        );

        if (watchlist) {
          const latestTs = safeNumber(stock.latest_close_date);
          const toneClass = stockDayChangeToneClass(stock.day_change_amount);
          return `<article class="stock-card ${toneClass} fade-up" data-ticker="${stock.ticker}">
        <div class="stock-top">
          <strong>${stock.ticker}</strong>
          <span class="stock-market-value">${currency(stock.latest_close_price)}</span>
        </div>
        <div class="stock-name">${stock.company_name || "Watchlist symbol"}</div>
        <div class="stock-stats">
          <span>Latest Price: ${currency(stock.latest_close_price)}</span>
          <span>Day Change: ${changeLabel(stock.day_change_amount, stock.day_change_percent)}</span>
          <span>As Of: ${latestTs ? dateLabel(latestTs) : "n/a"}</span>
          <span>Click for actions</span>
        </div>
      </article>`;
        }

        const perShareLabel = `${performance.perShareChange >= 0 ? "+" : ""}${currency(performance.perShareChange)} per share`;
        const totalLabel = `${performance.totalChange >= 0 ? "+" : ""}${currency(performance.totalChange)}`;

        return `<article class="stock-card ${toneClass} fade-up" data-ticker="${stock.ticker}">
        <div class="stock-top">
          <strong>${stock.ticker}</strong>
          <span class="stock-market-value">${currency(stock.position_market_value)}</span>
        </div>
        <div class="stock-name">${stock.company_name || "Company name not available"}</div>
        <div class="stock-pnl ${toneClass}">${totalLabel} <span>${performance.hasBasis ? `${performance.percentChange >= 0 ? "+" : ""}${performance.percentChange.toFixed(2)}%` : "No cost basis"}</span></div>
        <div class="stock-stats">
          <span>Shares: ${sharesFormat(stock.shares_owned)}</span>
          <span>Avg Cost: ${currency(stock.average_purchase_price)}</span>
          <span>Latest Close: ${currency(stock.latest_close_price)} on ${stock.latest_close_date ? dateLabel(stock.latest_close_date) : "n/a"}</span>
          <span>Day Change: ${changeLabel(stock.day_change_amount, stock.day_change_percent)}</span>
          <span>Position Day Change: ${signedCurrency(stock.position_day_change_amount)}</span>
          <span>${perShareLabel}</span>
        </div>
      </article>`;
      }
    )
    .join("") || "<p>No stock data available.</p>";

  document.querySelectorAll(".stock-card").forEach((card) => {
    card.addEventListener("click", () => {
      const ticker = card.dataset.ticker;
      const selected = stocks.find((s) => s.ticker === ticker);
      if (selected) {
        showStockDialog(selected);
      }
    });
  });

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
    allocationPanel.hidden = watchlist;
  }
  if (chartPanel) {
    chartPanel.hidden = watchlist;
  }

  if (!watchlist) {
    renderPortfolioChart();
    renderAllocationChart(stocks, portfolio);
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
    const asOf = safeNumber(entry?.as_of);
    if (!ticker || price <= 0) {
      return;
    }

    byTicker.set(ticker, {
      price,
      asOf: asOf > 0 ? asOf : unixNow()
    });
  });
  return byTicker;
}

function applyLivePricesToPortfolio(portfolio, stocks, livePriceMap) {
  const nowTs = unixNow();
  const watchlist = isWatchlistPortfolio(portfolio);
  const updatedStocks = (Array.isArray(stocks) ? stocks : []).map((stock) => {
    const ticker = String(stock?.ticker || "").trim().toUpperCase();
    const live = livePriceMap.get(ticker);
    if (!live) {
      return { ...stock };
    }

    const shares = safeNumber(stock?.shares_owned);
    const livePositionValue = watchlist ? live.price : shares * live.price;
    const previousClose = safeNumber(stock?.previous_close_price);
    const dayChangeAmount = Number.isFinite(previousClose) && previousClose > 0
      ? (live.price - previousClose)
      : 0;
    const dayChangePercent = Number.isFinite(previousClose) && previousClose > 0
      ? (dayChangeAmount / previousClose) * 100
      : 0;
    return {
      ...stock,
      latest_close_price: live.price,
      latest_close_date: live.asOf,
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
    const previousCloseTotal = updatedStocks.reduce(
      (sum, stock) => sum + safeNumber(stock?.previous_close_price),
      0
    );
    const dayChangeAmount = updatedStocks.reduce(
      (sum, stock) => sum + safeNumber(stock?.day_change_amount),
      0
    );
    updatedPortfolio.day_change_amount = dayChangeAmount;
    updatedPortfolio.day_change_percent = previousCloseTotal > 0
      ? (dayChangeAmount / previousCloseTotal) * 100
      : 0;
  } else {
    const previousDayValue = previousDistinctDayValue(updatedPortfolio.daily_values, nowTs);
    if (Number.isFinite(previousDayValue) && previousDayValue > 0) {
      const dayChangeAmount = updatedPortfolio.estimated_total_value - previousDayValue;
      updatedPortfolio.day_change_amount = dayChangeAmount;
      updatedPortfolio.day_change_percent = (dayChangeAmount / previousDayValue) * 100;
    }
  }

  const todayBucket = Math.floor(nowTs / 86400);
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
      setBreadcrumbs([{ label: "Dashboard" }]);
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
          type: e.type,
          stock_symbol: stock.ticker,
          shares: e.shares,
          amount: e.cash_amount,
          notes: e.notes || ""
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

    setActiveView("portfolio");

    renderPortfolioDetail(state.currentPortfolio, state.currentStocks, state.recentTransactions);
    setMarketStateChip(fallbackMarketStateNowET());
    setPortfolioLastUpdatedChip(0);

    setBreadcrumbs([
      { label: "Dashboard", onClick: showDashboard },
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
  setBreadcrumbs([{ label: "Dashboard" }]);
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
  const isTransfer = type === "deposit" || type === "withdrawal";

  // Update visibility with both hidden attribute and display property for reliability
  const updateFieldVisibility = (el, shouldHide) => {
    if (el) {
      el.hidden = shouldHide;
      if (shouldHide) {
        el.style.display = "none";
      } else {
        el.style.display = "";
      }
    }
  };

  updateFieldVisibility(el.groupTicker, isInterest || isTransfer);
  updateFieldVisibility(el.groupShares, !(isBuySell || isDividend));
  updateFieldVisibility(el.groupPrice, !isBuySell);
  updateFieldVisibility(el.groupAmount, !(isDividend || isInterest || isTransfer));

  el.transactionTicker.required = !isInterest && !isTransfer;
  el.transactionShares.required = isBuySell;
  el.transactionPrice.required = isBuySell;
  el.transactionAmount.required = isDividend || isInterest || isTransfer;

  if (isInterest || isTransfer) {
    el.transactionTicker.value = "";
    el.transactionShares.value = "";
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
  updateCreatePortfolioFormForType();
}

function updateCreatePortfolioFormForType() {
  const watchlist = el.createPortfolioType.value === "WATCHLIST";
  el.createPortfolioCapitalRow.hidden = watchlist;
  el.createPortfolioCapital.disabled = watchlist;
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

  try {
    el.transactionSubmitBtn.disabled = true;
    await apiPost(
      `/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/transactions/${action}`,
      payload
    );

    const latestSummary = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(latestSummary.portfolios);
    await openPortfolio(state.currentPortfolio.name);
    showFlash("Transaction recorded successfully.");
    resetTransactionForm();
    el.transactionDialog.close();
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
    el.transactionsHistory.innerHTML = `<div class="dialog-table-wrap">${renderTransactionTable(payload.transactions || [])}</div>`;
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
  const initialCapital = safeNumber(el.createPortfolioCapital.value);

  if (!name) {
    showFlash("Portfolio name is required.");
    return;
  }

  if (!isWatchlist && initialCapital < 0) {
    showFlash("Initial capital must be non-negative.");
    return;
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

    const created = await apiPost("/api/portfolios", body);

    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
    renderDashboard();
    showDashboard();

    const createdName = String(created?.name || name);
    await openPortfolio(createdName);

    el.createPortfolioDialog.close();
    showFlash(`Created portfolio ${portfolioDisplayName(createdName)}.`);
  } catch (error) {
    showFlash(error.message);
  } finally {
    el.createPortfolioSubmitBtn.disabled = false;
  }
}

async function loadDashboard() {
  hideFlash();
  try {
    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
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

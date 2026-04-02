const API_STORAGE_KEY = "portfolio-api-base";

const state = {
  apiBase: localStorage.getItem(API_STORAGE_KEY) || "",
  portfolios: [],
  currentPortfolio: null,
  currentStocks: [],
  recentTransactions: [],
  periods: {
    dashboard: "6M",
    portfolio: "6M"
  },
  charts: {
    dashboard: null,
    portfolio: null
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
  backToDashBtn: document.getElementById("backToDashBtn"),
  apiConfigToggle: document.getElementById("apiConfigToggle"),
  apiConfigPanel: document.getElementById("apiConfigPanel"),
  apiConfigForm: document.getElementById("apiConfigForm"),
  apiBaseInput: document.getElementById("apiBaseInput"),
  apiResetBtn: document.getElementById("apiResetBtn"),
  apiStatus: document.getElementById("apiStatus"),
  portfolioPeriodSelect: document.getElementById("portfolioPeriodSelect"),
  portfolioChangeChip: document.getElementById("portfolioChangeChip"),
  portfolioChart: document.getElementById("portfolioChart")
};

const PERIOD_OPTIONS = ["1M", "3M", "6M", "1Y", "3Y", "ALL"];

function setActiveView(view) {
  const showDashboardView = view === "dashboard";

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

  const totalAssets = portfolios.reduce((sum, p) => sum + (p.estimated_total_value || 0), 0);
  const totalCash = portfolios.reduce((sum, p) => sum + (p.available_capital || 0), 0);
  const totalStocks = portfolios.reduce((sum, p) => sum + (p.stock_count || 0), 0);
  const totalTransactions = portfolios.reduce((sum, p) => sum + (p.transaction_count || 0), 0);
  const aggregateTrend = mergeDailySeries(portfolios);

  const dashboardMetrics = `<section class="metric-grid">
    ${metricCard("Total Assets", currency(totalAssets), `${portfolios.length} portfolio${portfolios.length === 1 ? "" : "s"}`)}
    ${metricCard("Total Available Cash", currency(totalCash), "Across all accounts")}
    ${metricCard("Total Stock Positions", String(totalStocks), "Unique position records")}
    ${metricCard("Total Transactions", String(totalTransactions), "Combined history")}
  </section>`;

  const cards = portfolios
    .map(
      (p, i) => `<article class="portfolio-card fade-up" data-name="${encodeURIComponent(p.name)}" style="animation-delay:${Math.min(i * 60, 360)}ms">
        <div class="stock-top">
          <strong>${portfolioDisplayName(p.name)}</strong>
          <span class="chip">${typeLabel(p.type)}</span>
        </div>
        <div>${currency(p.estimated_total_value)}</div>
        <div class="sub">Cash: ${currency(p.available_capital)}</div>
        <div class="sub">${p.stock_count} stocks • ${p.transaction_count} transactions</div>
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
          <span id="dashboardChangeChip" class="chip chip-neutral">Change: n/a</span>
        </div>
      </div>
      <canvas id="${chartHostId}" aria-label="Total Asset Trend"></canvas>
    </article>
    <section>
      <div class="panel-head">
        <h3>Portfolios</h3>
      </div>
      ${cards ? `<div class="dashboard-cards">${cards}</div>` : emptyState}
    </section>
  `;

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

function renderPortfolioDetail(portfolio, stocks, recentTransactions) {
  el.portfolioName.textContent = portfolioDisplayName(portfolio.name);
  el.portfolioType.textContent = typeLabel(portfolio.type);
  el.stockCount.textContent = `${stocks.length} stock${stocks.length === 1 ? "" : "s"}`;

  const valueDelta = (portfolio.estimated_total_value || 0) - (portfolio.available_capital || 0);
  el.portfolioMetrics.innerHTML = [
    metricCard("Estimated Total", currency(portfolio.estimated_total_value)),
    metricCard("Available Capital", currency(portfolio.available_capital)),
    metricCard("Reported Total", currency(portfolio.reported_total_value)),
    metricCard("Position Value", currency(valueDelta), `${portfolio.transaction_count} transactions`)
  ].join("");

  const sortedStocks = [...stocks].sort((a, b) => (b.position_market_value || 0) - (a.position_market_value || 0));
  el.stocksList.innerHTML = sortedStocks
    .map(
      (stock) => `<article class="stock-card fade-up" data-ticker="${stock.ticker}">
        <div class="stock-top">
          <strong>${stock.ticker}</strong>
          <span>${currency(stock.position_market_value)}</span>
        </div>
        <div class="stock-name">${stock.company_name || "Company name not available"}</div>
        <div class="stock-stats">
          <span>Shares: ${sharesFormat(stock.shares_owned)}</span>
          <span>Avg Cost: ${currency(stock.average_purchase_price)}</span>
          <span>Latest Close: ${currency(stock.latest_close_price)} on ${dateLabel(stock.latest_close_date)}</span>
        </div>
      </article>`
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

  el.recentTransactions.innerHTML = renderTransactionTable(recentTransactions);

  el.portfolioPeriodSelect.value = state.periods.portfolio;
  el.portfolioPeriodSelect.onchange = (event) => {
    state.periods.portfolio = event.target.value;
    renderPortfolioChart();
  };

  renderPortfolioChart();
}

function showStockDialog(stock) {
  const gainPerShare = (stock.latest_close_price || 0) - (stock.average_purchase_price || 0);
  const unrealized = gainPerShare * (stock.shares_owned || 0);
  const events = stock.recent_events || [];

  el.stockDialogTitle.textContent = `${stock.ticker} • ${stock.company_name || "Company"}`;
  el.stockDialogBody.innerHTML = `
    <section class="metric-grid">
      ${metricCard("Shares Owned", sharesFormat(stock.shares_owned))}
      ${metricCard("Average Cost", currency(stock.average_purchase_price))}
      ${metricCard("Latest Close", currency(stock.latest_close_price), dateLabel(stock.latest_close_date))}
      ${metricCard("Unrealized P/L", currency(unrealized), `${gainPerShare >= 0 ? "+" : ""}${currency(gainPerShare)} per share`)}
    </section>
    <article class="panel">
      <div class="panel-head">
        <h3>Recent Company Events</h3>
        <span class="chip">${events.length} entries</span>
      </div>
      ${events.length ? renderTransactionTable(events.map((e) => ({
        date: e.date,
        type: e.type,
        stock_symbol: stock.ticker,
        shares: e.shares,
        amount: e.cash_amount,
        notes: e.notes || ""
      })) ) : "<p>No recent events available.</p>"}
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
    state.currentStocks = stocksPayload.stocks || [];
    state.recentTransactions = recentPayload.transactions || [];

    setActiveView("portfolio");

    renderPortfolioDetail(state.currentPortfolio, state.currentStocks, state.recentTransactions);

    setBreadcrumbs([
      { label: "Dashboard", onClick: showDashboard },
      { label: portfolioDisplayName(name) }
    ]);
  } catch (error) {
    showFlash(error.message);
  }
}

function showDashboard() {
  setActiveView("dashboard");
  setBreadcrumbs([{ label: "Dashboard" }]);
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

function openTransactionDialog() {
  if (!state.currentPortfolio) {
    showFlash("Select a portfolio first.");
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

  try {
    const payload = await apiGet(`/api/portfolios/${encodeURIComponent(state.currentPortfolio.name)}/transactions`);
    el.transactionsHistory.innerHTML = renderTransactionTable(payload.transactions || []);
    el.transactionsDialog.showModal();
  } catch (error) {
    showFlash(error.message);
  }
}

async function loadDashboard() {
  hideFlash();
  try {
    const payload = await apiGet("/api/portfolios");
    state.portfolios = normalizePortfolios(payload.portfolios);
    renderDashboard();
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

  el.apiConfigToggle.addEventListener("click", toggleApiPanel);
  el.apiConfigForm.addEventListener("submit", saveApiBase);
  el.apiResetBtn.addEventListener("click", resetApiBase);
}

function init() {
  el.apiBaseInput.value = state.apiBase;
  setActiveView("dashboard");
  wireEvents();
  updateTransactionFormVisibility();
  loadDashboard();
}

init();

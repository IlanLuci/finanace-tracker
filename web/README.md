# Web Interface

This folder contains a modern single-page UI for the portfolio API.

## Features

- Main dashboard with combined total assets and portfolio cards
- Portfolio drill-down with metrics and portfolio value chart
- Stock list with click-through company details
- Recent transactions and full history modal
- Configurable API base URL for same-origin or cross-origin usage

## Run

1. Start API server:
   - `./finance_tracker --server --port 8080 --data-dir data`
2. Serve this folder with any static server (example):
   - `cd web && python3 -m http.server 5173`
3. Open:
   - `http://localhost:5173`
4. In API Settings, set Base URL to:
   - `http://localhost:8080`

## Notes

- If you host this UI from the same origin as the API, leave API base URL empty.
- Charts use Chart.js from CDN.

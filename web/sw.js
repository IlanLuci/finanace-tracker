// Bump this version whenever the cached asset list changes so old caches get
// purged on activation. Versioned query strings on app.js/styles.css are part
// of each cache key, so changing those will pull a fresh copy too.
const CACHE_NAME = "finance-tracker-shell-v6";

const APP_SHELL = [
  "./",
  "./index.html",
  "./app.js",
  "./styles.css",
  "https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js",
  "https://cdn.plaid.com/link/v2/stable/link-initialize.js"
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      // Best-effort: a single CDN failure shouldn't block install of the
      // local shell (which is what makes the page openable offline).
      await Promise.all(
        APP_SHELL.map((url) =>
          cache.add(url).catch(() => undefined)
        )
      );
      self.skipWaiting();
    })()
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    (async () => {
      const keys = await caches.keys();
      await Promise.all(
        keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key))
      );
      await self.clients.claim();
    })()
  );
});

function isCacheableShellRequest(request) {
  if (request.method !== "GET") return false;
  const url = new URL(request.url);

  // API responses are cached at the app layer (localStorage) with structured
  // invalidation — don't shadow that with a SW cache that won't know when to
  // evict.
  if (url.pathname.startsWith("/api/")) return false;

  if (url.origin === self.location.origin) {
    return (
      url.pathname === "/" ||
      url.pathname.endsWith(".html") ||
      url.pathname.endsWith(".js") ||
      url.pathname.endsWith(".css") ||
      url.pathname.endsWith(".ico") ||
      url.pathname.endsWith(".png") ||
      url.pathname.endsWith(".svg")
    );
  }

  const host = url.host;
  return (
    host.includes("cdn.jsdelivr.net") ||
    host.includes("cdn.plaid.com") ||
    host.includes("fonts.googleapis.com") ||
    host.includes("fonts.gstatic.com")
  );
}

self.addEventListener("fetch", (event) => {
  const request = event.request;
  if (!isCacheableShellRequest(request)) return;

  // Versioned URLs (styles.css?v=…, app.js?v=…) ARE the cache-busting key —
  // matching them without the query would shadow newer deploys behind the
  // precached unversioned entry forever.
  const url = new URL(request.url);
  const isVersioned = url.searchParams.has("v");

  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      const cached = isVersioned
        ? await cache.match(request)
        : await cache.match(request, { ignoreSearch: true });

      const networkFetch = fetch(request)
        .then((response) => {
          if (response && response.status === 200) {
            // Opaque responses (no-cors fonts) still get cached so they
            // survive offline; we just can't introspect their bodies.
            cache.put(request, response.clone()).catch(() => undefined);
          }
          return response;
        })
        .catch(() => null);

      if (cached) {
        // Stale-while-revalidate: serve cached immediately, refresh in background.
        event.waitUntil(networkFetch);
        return cached;
      }

      const networkResponse = await networkFetch;
      if (networkResponse) return networkResponse;

      // Offline fallback for versioned assets: an older cached copy keyed
      // without the query string is better than nothing.
      if (isVersioned) {
        const anyCached = await cache.match(request, { ignoreSearch: true });
        if (anyCached) return anyCached;
      }

      // Last-ditch fallback for navigation requests: serve the shell so the
      // SPA can still boot and show cached data with the offline banner.
      if (request.mode === "navigate") {
        const shell = await cache.match("./index.html");
        if (shell) return shell;
      }

      return new Response("Offline", { status: 503, statusText: "Offline" });
    })()
  );
});

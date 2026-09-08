"use strict";
// Bridge: Sparkling Summer look (index.html) <-> anis-paperd, over WebChannel.
// The look file owns all markup, styling and animation.  This file only feeds
// it real catalog/monitor data and forwards user intent to the daemon, so the
// look can be replaced wholesale without touching daemon plumbing.

(function () {
  const A = window.anis;
  if (!A) return;
  const state = A.state;

  // Deterministic fallback gradient for entries without a usable preview.
  function hueGrad(id) {
    let h = 0;
    const s = String(id || "");
    for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) % 360;
    return ["hsl(" + h + " 45% 14%)", "hsl(" + ((h + 50) % 360) + " 65% 48%)"];
  }

  function typeOf(raw) {
    return raw.type === "scene" || raw.type === "video" || raw.type === "web"
      ? raw.type
      : "image";
  }

  // rpc_client already rewrites catalog previews to the anispaper: scheme; the
  // width hint keeps the decoded bitmap near its displayed size.
  function sized(url, w) {
    if (!url || url.indexOf("anispaper:") !== 0) return url || null;
    return url + (url.indexOf("?") >= 0 ? "&" : "?") + "w=" + w;
  }

  function toItem(raw) {
    return {
      id: raw.id,
      title: raw.title || raw.id,
      type: typeOf(raw),
      g: hueGrad(raw.id),
      pv: sized(raw.preview, 640),
      sub: typeOf(raw),
      favorite: !!raw.favorite,
      tags: Array.isArray(raw.tags) ? raw.tags : [],
      properties: {},
    };
  }

  let client = null;

  function ingestCatalog(items) {
    A.catalog.length = 0;
    for (const raw of items || []) A.catalog.push(toItem(raw));
    if (typeof A.renderLibrary === "function") A.renderLibrary();
    if (typeof A.renderWorkshop === "function") A.renderWorkshop();
    if (typeof A.renderProps === "function") A.renderProps();
    if (!A.catalog.length) return;
    if (!A.catalog.some((x) => x.id === state.selected)) {
      // Prefer the wallpaper that is actually live on the current output so
      // the UI opens showing what you have instead of the first catalog row.
      const liveId =
        state.applied && state.applied[state.out]
          ? state.applied[state.out]
          : null;
      const target =
        liveId && A.catalog.some((x) => x.id === liveId)
          ? liveId
          : A.catalog[0].id;
      A.select(target);
    } else {
      A.renderStrip();
    }
  }

  // The look ships two hard-coded output buttons.  Rebind them to the real
  // wl_output names instead of inventing markup the design did not specify.
  function ingestMonitors(list) {
    const names = (list || []).map((m) => m.name).filter(Boolean);
    if (!names.length) return;
    const row = A.$("#outRow");
    if (row) {
      row.innerHTML = "";
      names.forEach(function (name, i) {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "out" + (name === state.out || (!names.includes(state.out) && i === 0) ? " on" : "");
        btn.dataset.out = name;
        const sub = document.createElement("span");
        sub.className = "sub";
        sub.textContent = (name === state.out || (!names.includes(state.out) && i === 0)) ? "MAIN" : "STANDBY";
        btn.appendChild(document.createTextNode(name));
        btn.appendChild(sub);
        row.appendChild(btn);
      });
    }
    if (!names.includes(state.out)) A.setOut(names[0]);
    for (const name of names) {
      if (!(name in state.applied)) state.applied[name] = null;
    }
  }

  function shortOut(name) {
    if (typeof A.shortOut === "function") return A.shortOut(name);
    if (!name) return "";
    if (String(name).indexOf("HDMI") === 0) return "HDMI";
    return name;
  }

  // Live renderer rows carry the measured fps of EVERY output.  The dock
  // label is those numbers (DP-2 60 · HDMI 60); the slider stays the cap.
  function ingestRenderers(list) {
    const rows = [];
    const parts = [];
    for (const row of list || []) {
      const name = row.output;
      if (!name) continue;
      const item = A.catalog.find((x) => x.id === row.wallpaperId);
      state.applied[name] = item ? item.id : null;
      const fps = typeof row.fps === "number" ? row.fps : 0;
      const paused = !!row.gamingPaused;
      rows.push({
        id: row.wallpaperId,
        title: item ? item.title : row.wallpaperId,
        output: name,
        fps: fps,
        paused: paused,
        running: row.state === "running",
      });
      parts.push(shortOut(name) + " " + (paused ? "—" : Math.round(fps)));
    }
    if (typeof A.setMeasuredFps === "function") {
      A.setMeasuredFps(parts.join("  ") || "—", rows);
    }
    A.renderStrip();
    // Refresh ON badges on the catalog/workshop grids in place so the user
    // sees which wallpaper is live on each output.
    if (typeof A.markLive === "function") A.markLive();
  }

  window.anisOnShine = function (out, id) {
    if (client) client.applyWallpaper(id, out);
  };
  window.anisOnCurtain = function (out) {
    if (client) client.stopWallpaper(out);
  };
  window.anisOnFps = function (fps) {
    if (client) client.fpsCap = fps;
  };
  window.anisOnVol = function (vol) {
    if (client) client.volumePercent = vol;
  };
  window.anisOnGaming = function (on) {
    // The dock switch is on/off.  On means "pause when a game is running"
    // (daemon auto).  Off must be a hard off — never auto-detect Steam.
    if (client) client.setGamingMode(on ? "auto" : "off");
  };
  window.anisOnBlacklist = function (list) {
    if (client) client.setGamingBlacklist(Array.isArray(list) ? list : []);
  };
  window.anisOnProp = function (id, values) {
    if (client) client.setWallpaperProperties(id, values);
  };

  if (typeof qt === "undefined" || !qt.webChannelTransport) return;

  new QWebChannel(qt.webChannelTransport, function (channel) {
    client = channel.objects.client;
    window.rpc = client;

    const pullCatalog = function () {
      client.catalogItems(function (items) {
        ingestCatalog(items);
        ingestMonitors(client.monitors);
        ingestRenderers(client.liveRenderers);
      });
    };
    pullCatalog();

    const prevSelect = A.select;
    A.select = function (id) {
      if (typeof prevSelect === "function") prevSelect(id);
      fetchItemProps(id);
    };
    // Grid and shortcuts call the bare `select()` binding captured before
    // this override existed, which bypassed the properties fetch above and
    // left every clicked card with `properties: {}` ("no extra controls").
    // Rebind the global so all selection paths go through the wrapper.
    try { window.select = A.select; } catch (e) {}
    window.anisFetchProps = fetchItemProps;

    function fetchItemProps(id) {
      if (!client || !id || typeof client.itemProperties !== "function") return;
      const it = A.catalog.find(function (x) { return x.id === id; });
      if (!it || it._propsLoading) return;
      it._propsLoading = true;
      client.itemProperties(id, function (props) {
        it._propsLoading = false;
        const found = A.catalog.find(function (x) { return x.id === id; });
        if (found) found.properties = props && typeof props === "object" ? props : {};
        if (typeof A.renderProps === "function") A.renderProps();
      });
    }
    client.catalogChanged.connect(pullCatalog);
    client.monitorsChanged.connect(function () {
      ingestMonitors(client.monitors);
    });
    client.liveRenderersChanged.connect(function () {
      ingestRenderers(client.liveRenderers);
    });
    client.toastChanged.connect(function () {
      const msg = client.toast;
      if (msg) A.toast("DAEMON", msg);
    });
    client.gamingActiveChanged.connect(function () {
      // Active is a live pause, not the user preference.  Writing it back
      // through setGaming used to persist gamingMode=on after a Steam launch.
      if (typeof A.paintGamingActive === "function") {
        A.paintGamingActive(!!client.gamingActive);
      }
    });
    client.selectedOutputChanged.connect(function () {
      const out = client.selectedOutput;
      if (out && out !== state.out) A.setOut(out);
    });

    if (typeof client.fpsCap === "number") A.setFps(client.fpsCap);
    if (typeof client.volumePercent === "number") A.setVol(client.volumePercent);
    if (typeof A.paintGamingPref === "function") {
      A.paintGamingPref((client.gamingMode || "auto") !== "off");
    }
    if (typeof A.paintBlacklist === "function") {
      A.paintBlacklist(client.gamingBlacklist || []);
    }
    if (typeof A.paintGamingActive === "function") {
      A.paintGamingActive(!!client.gamingActive);
    }
    client.gamingModeChanged.connect(function () {
      if (typeof A.paintGamingPref === "function") {
        A.paintGamingPref((client.gamingMode || "auto") !== "off");
      }
    });
    if (client.gamingBlacklistChanged && client.gamingBlacklistChanged.connect) {
      client.gamingBlacklistChanged.connect(function () {
        if (typeof A.paintBlacklist === "function") {
          A.paintBlacklist(client.gamingBlacklist || []);
        }
      });
    }

    function workshopOwned(id, item) {
      const sid = String(id || "");
      if (!sid) return false;
      if (item && (item.installed === true || item.installed === "true")) return true;
      return A.catalog.some(function (c) {
        const cid = String(c.id || "");
        return cid === "steam:" + sid || cid === sid;
      });
    }

    function ingestWorkshop() {
      if (!A.workshop) return;
      A.workshop.length = 0;
      const items = client.workshopItems || [];
      for (let i = 0; i < items.length; i++) {
        const item = items[i];
        const type =
          item.type === "scene" || item.type === "video" || item.type === "web"
            ? item.type
            : "image";
        A.workshop.push({
          id: String(item.id),
          title: item.title || item.id,
          type: type,
          pv: item.preview || null,
          rating: String(item.rating || "everyone").toLowerCase(),
          installed: workshopOwned(item.id, item),
          g: hueGrad(item.id),
        });
      }
      if (typeof A.renderWorkshop === "function") A.renderWorkshop();
    }

    function ratingsCsv() {
      return typeof A.ratingsCsv === "function" ? A.ratingsCsv() : "everyone,questionable";
    }

    function workshopQuery() {
      const el = A.$("#wsQ");
      return ((el && el.value) || "").trim();
    }

    const runWorkshopSearch = function (page) {
      client.searchWorkshop(workshopQuery(), page || 1, ratingsCsv());
    };

    client.workshopBusyChanged.connect(function () {
      if (typeof A.renderWorkshop === "function") A.renderWorkshop();
    });
    client.workshopErrorChanged.connect(function () {
      if (typeof A.renderWorkshop === "function") A.renderWorkshop();
    });
    client.workshopItemsChanged.connect(ingestWorkshop);
    client.workshopPageChanged.connect(function () {
      if (typeof A.renderWorkshop === "function") A.renderWorkshop();
    });

    const wsGo = A.$("#wsQ");
    if (wsGo) {
      wsGo.addEventListener("keydown", function (e) {
        if (e.key !== "Enter") return;
        e.preventDefault();
        runWorkshopSearch(1);
      });
    }
    const goWs = function (delta) {
      const page = Math.max(1, (client.workshopPage || 1) + delta);
      const pages = Math.max(page, client.workshopPageCount || 1);
      if (delta > 0 && (client.workshopPage || 1) >= pages) return;
      runWorkshopSearch(page);
    };
    const wsPrev = document.getElementById("wsPrev");
    if (wsPrev) wsPrev.onclick = function () { goWs(-1); };
    const wsNext = document.getElementById("wsNext");
    if (wsNext) wsNext.onclick = function () { goWs(1); };

    const wsGrid = A.$("#wsGrid");
    if (wsGrid) {
      wsGrid.addEventListener("click", function (e) {
        const sub = e.target.closest("[data-sub]");
        if (!sub) return;
        const id = sub.dataset.sub;
        if (!id) return;
        e.preventDefault();
        e.stopPropagation();
        client.subscribeWorkshop(id);
      });
    }

    const workshopNav = document.querySelector('[data-nav="workshop"]');
    if (workshopNav) {
      workshopNav.addEventListener("click", function () {
        if (!client.workshopBusy && !(client.workshopItems && client.workshopItems.length))
          runWorkshopSearch(1);
      });
    }

    const syncDownload = function () {
      const bar = A.$("#dlBar");
      if (!bar) return;
      const on = !!client.downloadActive;
      bar.classList.toggle("on", on);
      const pct = Number(client.downloadPercent);
      bar.classList.toggle("indet", on && pct < 0);
      const fill = A.$("#dlFill");
      if (fill) fill.style.width = !on || pct < 0 ? "34%" : pct + "%";
      const lab = A.$("#dlLabel");
      if (lab) lab.textContent = client.downloadLabel || "Descargando…";
      const num = A.$("#dlPct");
      if (num) num.textContent = !on || pct < 0 ? "" : pct + "%";
    };
    if (client.downloadChanged) client.downloadChanged.connect(syncDownload);
    syncDownload();

    ingestWorkshop();
  });
})();

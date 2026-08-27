"use strict";
/* Connects the Starlight mockup to anis-paperd via Qt WebChannel. */

(function () {
  const hueGrad = (id) => {
    const hue = _hashHue(String(id || ""));
    return [`hsl(${hue} 45% 14%)`, `hsl(${(hue + 50) % 360} 65% 48%)`];
  };

  function toItem(raw) {
    const type =
      raw.type === "scene" || raw.type === "video" || raw.type === "web"
        ? raw.type
        : "static";
    const tags = Array.isArray(raw.tags) ? raw.tags : [];
    return {
      id: raw.id,
      title: raw.title || raw.id,
      type,
      g: hueGrad(raw.id),
      pv: raw.preview || null,
      favorite: !!raw.favorite,
      tags,
      rating: ratingFromTags(tags),
    };
  }

  function resetApplied(names) {
    const next = {};
    for (const name of names) next[name] = state.applied[name] || null;
    state.applied = next;
    const fpsVal = {},
      fpsDisp = {};
    for (const name of names) {
      fpsVal[name] = state.fpsVal[name] || 0;
      fpsDisp[name] = state.fpsDisp[name] || 0;
    }
    state.fpsVal = fpsVal;
    state.fpsDisp = fpsDisp;
  }

  $("#stageList").addEventListener("click", (e) => {
    const chip = e.target.closest(".out-chip");
    if (!chip || !window.rpc) return;
    window.rpc.selectedOutput = chip.dataset.out;
  });

  function renderStageChips() {
    const host = $("#stageList");
    if (!host || !state.outputs.length) return;
    host.innerHTML = state.outputs
      .map((name) => {
        const main = name === state.out;
        const live = !!state.applied[name] && !state.gaming;
        const fps = live && state.fpsDisp[name] ? state.fpsDisp[name].toFixed(1) : "—";
        return `<button class="out-chip${main ? " active" : ""}" data-out="${name}">
      <span class="dot"></span>
      <span class="oc-txt"><b>${name}</b><small class="role" id="role-${name}">${
          main ? "MAIN STAGE" : "standby"
        }</small></span>
      <em id="fps-${name}">${fps} fps</em>
    </button>`;
      })
      .join("");
  }

    function ingestCatalog(items) {
    catalog.length = 0;
    const favs = new Set();
    for (const raw of items || []) {
      const item = toItem(raw);
      catalog.push(item);
      if (raw.favorite) favs.add(item.id);
    }
    state.favs = favs;
    if (state.selected && !byId(state.selected)) state.selected = null;
    if (!state.selected && catalog.length) state.selected = catalog[0].id;
    renderGrid();
    renderRecent();
    renderStage();
    updateNowPlaying();
    setMeasured();
    if (typeof window.renderWorkshop === "function") window.renderWorkshop();
  }

  function ingestMonitors(list) {
    const names = (list || [])
      .map((m) => m && m.name)
      .filter((name) => typeof name === "string" && name.length);
    if (!names.length) return;
    state.outputs = names;
    if (!names.includes(state.out)) state.out = names[0];
    resetApplied(names);
    for (const mon of list) {
      const id = mon.currentWallpaperId;
      if (id && byId(id)) state.applied[mon.name] = byId(id);
    }
    renderStageChips();
    renderOutputs();
    renderStage();
    updateNowPlaying();
    setMeasured();
    if (window.rpc) window.rpc.selectedOutput = state.out;
  }

  function ingestRenderers(list) {
    window.liveFps = {};
    const liveIds = {};
    for (const row of list || []) {
      const name = row.output || row.name;
      if (!name) continue;
      if (typeof row.fps === "number") window.liveFps[name] = row.fps;
      if (row.wallpaperId) liveIds[name] = row.wallpaperId;
    }
    for (const [name, id] of Object.entries(liveIds)) {
      const item = byId(id);
      if (item) state.applied[name] = item;
    }
    renderStage();
    updateNowPlaying();
    setMeasured();
  }

  const origApply = applyTo;
  applyTo = function (out, id, triggerEl) {
    origApply(out, id, triggerEl);
    if (window.rpc && id)
      window.rpc.applyWallpaper(id, window.rpc.selectedOutput || out);
  };

  const origStop = stopOutput;
  stopOutput = function (out) {
    origStop(out);
    if (window.rpc) window.rpc.stopWallpaper(out);
  };

  const origLoadSteam = loadSteam;
  loadSteam = function () {
    if (window.rpc) {
      if (state.view === "workshop") {
        window.rpc.searchWorkshop(($("#q") && $("#q").value.trim()) || "", 1, ratingsCsv());
        toast("ENCORE", "Refreshing Workshop search…", "↻");
        return;
      }
      window.rpc.refreshCatalog();
      toast("ENCORE", "Refreshing catalog from anis-paperd…", "↻");
      return;
    }
    origLoadSteam();
  };

  const origGaming = $("#gamingSw").onclick;
  $("#gamingSw").onclick = function () {
    origGaming.call(this);
    if (window.rpc) window.rpc.setGamingMode(state.gaming ? "on" : "off");
  };

  $("#vol").addEventListener("input", () => {
    if (window.rpc) window.rpc.volumePercent = state.vol;
  });
  $("#fps").addEventListener("input", () => {
    if (window.rpc) window.rpc.fpsCap = state.fps;
  });
  $("#setVol").addEventListener("input", () => {
    if (window.rpc) window.rpc.volumePercent = state.vol;
  });
  $("#setFps").addEventListener("input", () => {
    if (window.rpc) window.rpc.fpsCap = state.fps;
  });

  $("#grid").addEventListener("click", (e) => {
    const card = e.target.closest(".card");
    if (!card || !window.rpc) return;
    if (e.target.closest("[data-fav]") || e.target.closest("[data-quick]")) return;
    window.rpc.selectedId = card.dataset.id;
  });

  function connectRpc(client) {
    window.rpc = client;
    const pullCatalog = () => {
      client.catalogItems((items) => {
        ingestCatalog(items);
        ingestMonitors(client.monitors);
      });
    };
    client.catalogChanged.connect(pullCatalog);
    client.monitorsChanged.connect(() => ingestMonitors(client.monitors));
    client.toastChanged.connect(() => {
      const msg = client.toast;
      if (!msg) return;
      if (/Rescan|programado/i.test(msg)) return;
      toast("DAEMON", msg, "●");
    });
    client.onlineChanged.connect(() => {
      if (!client.online)
        swapStatus(`<b style="color:var(--muted)">○</b> curtain up soon · daemon offline`);
    });
    client.liveRenderersChanged.connect(() => ingestRenderers(client.liveRenderers));
    client.volumePercentChanged.connect(() => {
      const vol = client.volumePercent;
      if (typeof vol !== "number") return;
      state.vol = vol;
      $("#vol").value = vol;
      $("#volV").textContent = vol + "%";
      $("#setVol").value = vol;
      $("#outVol").textContent = vol + "%";
    });
    client.fpsCapChanged.connect(() => {
      const fps = client.fpsCap;
      if (typeof fps !== "number") return;
      state.fps = fps;
      $("#fps").value = fps;
      $("#fpsV").textContent = fps;
      $("#setFps").value = fps;
      $("#outFps").textContent = fps;
    });
    client.gamingActiveChanged.connect(() => {
      const on = !!client.gamingActive;
      if (on === state.gaming) return;
      state.gaming = on;
      $("#gamingSw").classList.toggle("on", on);
      applyDimClass();
      renderOutputs();
      renderStage();
      updateNowPlaying();
      setMeasured();
    });

    const syncDownload = () => {
      const bar = $("#dlBar");
      if (!bar) return;
      const on = !!client.downloadActive;
      bar.classList.toggle("on", on);
      const pct = Number(client.downloadPercent);
      bar.classList.toggle("indet", on && pct < 0);
      const fill = $("#dlFill");
      if (fill) fill.style.width = !on || pct < 0 ? "34%" : pct + "%";
      const lab = $("#dlLabel");
      if (lab) lab.textContent = client.downloadLabel || "Descargando…";
      const num = $("#dlPct");
      if (num) num.textContent = !on || pct < 0 ? "" : pct + "%";
      const qr = $("#dlQr");
      if (qr) {
        const url = client.steamAuthQrUrl || "";
        if (url) qr.src = url;
        qr.classList.toggle("hidden", !on || !url);
      }
    };
    client.downloadChanged.connect(syncDownload);
    client.steamAuthNeededChanged.connect(syncDownload);
    syncDownload();
    const dlCancel = $("#dlCancel");
    if (dlCancel) dlCancel.onclick = () => client.cancelSteamAuth();

    const syncSteamAuth = () => {
      const modal = $("#steamAuth");
      if (modal) {
        modal.classList.remove("on");
        modal.hidden = true;
      }
    };
    client.steamAuthNeededChanged.connect(syncSteamAuth);
    syncSteamAuth();
    const authGo = $("#steamAuthGo");
    if (authGo) authGo.onclick = () => {};
    const authCancel = $("#steamAuthCancel");
    if (authCancel) authCancel.onclick = () => client.cancelSteamAuth();

    pullCatalog();
    ingestMonitors(client.monitors);
    ingestRenderers(client.liveRenderers);
    if (typeof client.volumePercent === "number") {
      state.vol = client.volumePercent;
      $("#vol").value = state.vol;
      $("#volV").textContent = state.vol + "%";
    }
    if (typeof client.fpsCap === "number") {
      state.fps = client.fpsCap;
      $("#fps").value = state.fps;
      $("#fpsV").textContent = state.fps;
    }
    if (client.selectedId) state.selected = client.selectedId;
    if (client.selectedOutput && state.outputs.includes(client.selectedOutput))
      state.out = client.selectedOutput;
    renderGrid();
    renderStage();
    updateNowPlaying();
    setMeasured();
    moveRail();

    function esc(s) {
      return String(s ?? "").replace(/[&<>"']/g, (c) =>
        ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])
      );
    }

    function paintWorkshopPagers() {
      const page = Math.max(1, client.workshopPage || 1);
      const pages = Math.max(page, client.workshopPageCount || 1);
      const html = `PAGE ${page} / ${pages}<small>Steam Workshop</small>`;
      const top = $("#wsPageLab"),
        bot = $("#wsPageLab2");
      if (top) top.innerHTML = html;
      if (bot) bot.innerHTML = html;
      const atStart = page <= 1 || client.workshopBusy;
      const atEnd = page >= pages || client.workshopBusy;
      ["wsPrev", "wsPrev2"].forEach((id) => {
        const b = document.getElementById(id);
        if (b) b.disabled = atStart;
      });
      ["wsNext", "wsNext2"].forEach((id) => {
        const b = document.getElementById(id);
        if (b) b.disabled = atEnd;
      });
    }
    function renderWorkshop() {
      const status = $("#wsStatus");
      const grid = $("#wsGrid");
      if (!status || !grid) return;
      paintWorkshopPagers();
      if (client.workshopBusy) {
        status.textContent = "Searching Wallpaper Engine Workshop…";
        return;
      }
      if (client.workshopError) {
        status.textContent = client.workshopError;
        if (!(client.workshopItems && client.workshopItems.length)) grid.innerHTML = "";
        return;
      }
      const items = client.workshopItems || [];
      const page = Math.max(1, client.workshopPage || 1);
      const pages = Math.max(page, client.workshopPageCount || 1);
      status.textContent = items.length
        ? `${items.length} wallpapers · page ${page} / ${pages}`
        : "Search Wallpaper Engine Workshop.";
      grid.innerHTML = items
        .map((item, i) => {
          const type = item.type || "static";
          const rating = (item.rating || "everyone").toLowerCase();
          const owned = workshopOwned(item.id, item);
          const preview = previewMarkup(
            { id: item.id, g: hueGrad(item.id), pv: item.preview },
            false
          );
          const btn = owned
            ? `<button class="ws-owned" type="button" disabled>★ SUSCRIPTO</button>`
            : `<button data-sub="${esc(item.id)}">★ SUBSCRIBE</button>`;
          return `<div class="card${owned ? " ws-have" : ""}" data-wsid="${esc(item.id)}" style="animation-delay:${Math.min(i * 38, 420)}ms">
      <div class="cfollow"></div>
      <div class="thumb">${preview}</div>
      <span class="tbadge tb-${type}">${({ scene: "SCENE", video: "VIDEO", web: "WEB", static: "IMAGE" })[type]}</span>
      <span class="rbadge rb-${esc(rating)}">${esc(rating.toUpperCase())}</span>
      <div class="apply-hover">${btn}</div>
      <div class="meta"><h4>${esc(item.title || item.id)}</h4><p>WORKSHOP · ${esc(item.id)}</p></div>
    </div>`;
        })
        .join("");
      bindThumbs(grid);
    }

    function workshopOwned(id, item) {
      const sid = String(id || "");
      if (!sid) return false;
      if (item && (item.installed === true || item.installed === "true")) return true;
      return catalog.some((c) => {
        const cid = String(c.id || "");
        return cid === "steam:" + sid || cid === sid;
      });
    }
    window.renderWorkshop = renderWorkshop;

    function workshopQuery() {
      return (($("#q") && $("#q").value) || "").trim();
    }
    const runWorkshopSearch = (page) => {
      client.searchWorkshop(workshopQuery(), page || 1, ratingsCsv());
    };

    client.workshopBusyChanged.connect(renderWorkshop);
    client.workshopErrorChanged.connect(renderWorkshop);
    client.workshopItemsChanged.connect(renderWorkshop);
    client.workshopPageChanged.connect(renderWorkshop);
    const wsGo = $("#wsGo");
    if (wsGo) wsGo.onclick = () => runWorkshopSearch(1);
    const goWs = (delta) => {
      const page = Math.max(1, (client.workshopPage || 1) + delta);
      const pages = Math.max(page, client.workshopPageCount || 1);
      if (delta > 0 && (client.workshopPage || 1) >= pages) return;
      runWorkshopSearch(page);
    };
    ["wsPrev", "wsPrev2"].forEach((id) => {
      const b = document.getElementById(id);
      if (b) b.onclick = () => goWs(-1);
    });
    ["wsNext", "wsNext2"].forEach((id) => {
      const b = document.getElementById(id);
      if (b) b.onclick = () => goWs(1);
    });
    const bindWsSearchBox = (el) => {
      if (!el) return;
      el.addEventListener("keydown", (e) => {
        if (e.key !== "Enter") return;
        if (el.id === "q" && state.view !== "workshop") return;
        e.preventDefault();
        runWorkshopSearch(1);
      });
    };
    bindWsSearchBox($("#q"));
    $("#wsGrid").addEventListener("click", (e) => {
      const card = e.target.closest("[data-wsid]");
      if (!card || card.classList.contains("ws-have")) return;
      const sub = e.target.closest("[data-sub]");
      const id = (sub && sub.dataset.sub) || card.dataset.wsid;
      if (!id) return;
      client.subscribeWorkshop(id);
    });
    const workshopNav = document.querySelector('[data-view="workshop"]');
    if (workshopNav) {
      workshopNav.addEventListener("click", () => {
        if (!client.workshopBusy && !(client.workshopItems && client.workshopItems.length))
          runWorkshopSearch(1);
      });
    }
    renderWorkshop();
  }

  function start() {
    if (typeof QWebChannel !== "function" || typeof qt === "undefined" || !qt.webChannelTransport) {
      toast("STANDALONE", "Mockup without daemon bridge — UI only.", "✧");
      return;
    }
    new QWebChannel(qt.webChannelTransport, (channel) => {
      connectRpc(channel.objects.client);
    });
  }

  start();
})();

import {
  type CSSProperties,
  type KeyboardEvent,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState
} from "react";
import type {
  CatalogItem,
  ConnectionEvent,
  DaemonEvent,
  DaemonStatus,
  Monitor,
  PreviewFrame,
  RendererStatus,
  Settings
} from "../shared/types";

type NavView = "installed" | "discover" | "favorites" | "folders";

const emptySettings: Settings = {
  customFolders: [],
  favorites: [],
  fpsCap: 30,
  defaultVolume: 1,
  retryQuota: 3,
  wallpaper: { scaleMode: "cover" }
};

function normalizeSettings(value: unknown): Settings {
  const source =
    typeof value === "object" && value !== null && !Array.isArray(value)
      ? (value as Record<string, unknown>)
      : {};
  const strings = (candidate: unknown) =>
    Array.isArray(candidate)
      ? candidate.filter((entry): entry is string => typeof entry === "string")
      : [];
  const wallpaper =
    typeof source.wallpaper === "object" && source.wallpaper !== null && !Array.isArray(source.wallpaper)
      ? (source.wallpaper as Record<string, unknown>)
      : {};
  const scaleMode =
    wallpaper.scaleMode === "fit" || wallpaper.scaleMode === "stretch" || wallpaper.scaleMode === "cover"
      ? wallpaper.scaleMode
      : emptySettings.wallpaper.scaleMode;
  return {
    customFolders: strings(source.customFolders),
    // F4 daemons do not yet expose this F5 field. Keep the UI mount-safe while
    // the user restarts or upgrades anis-paperd.
    favorites: strings(source.favorites),
    fpsCap: typeof source.fpsCap === "number" ? source.fpsCap : emptySettings.fpsCap,
    defaultVolume:
      typeof source.defaultVolume === "number" ? source.defaultVolume : emptySettings.defaultVolume,
    retryQuota: typeof source.retryQuota === "number" ? source.retryQuota : emptySettings.retryQuota,
    wallpaper: { scaleMode }
  };
}

const navItems: Array<{ id: NavView; label: string; icon: string }> = [
  { id: "installed", label: "Instaladas", icon: "▣" },
  { id: "discover", label: "Descubrir", icon: "⌁" },
  { id: "favorites", label: "Favoritas", icon: "★" },
  { id: "folders", label: "Carpetas", icon: "▱" }
];

// This output is intentionally not a physical wl_output. RendererManager accepts
// it as an isolated offscreen stream, so selecting a card never changes the
// user's desktop just to obtain a live preview.
const UI_PREVIEW_OUTPUT = "__anispaper-ui-preview__";

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : "Ocurrió un error inesperado.";
}

function typeLabel(type: string): string {
  const normalized = type.toLowerCase();
  if (normalized === "video") return "VIDEO";
  if (normalized === "web") return "WEB";
  if (normalized === "scene") return "SCENE";
  if (normalized === "application") return "APP";
  return "OTRO";
}

function dataUrl(frame: PreviewFrame | null): string | null {
  return frame ? `data:${frame.mimeType};base64,${frame.data}` : null;
}

function rendererFor(status: DaemonStatus | null, output: string): RendererStatus | undefined {
  return status?.renderers?.find((renderer) => renderer.output === output);
}

function Thumbnail({ path, title, className = "" }: { path?: string; title: string; className?: string }) {
  const [source, setSource] = useState("");

  useEffect(() => {
    let alive = true;
    setSource("");
    if (!path) return;
    void window.anispaper
      .thumbnail(path)
      .then((result) => {
        if (alive) setSource(result);
      })
      .catch(() => undefined);
    return () => {
      alive = false;
    };
  }, [path]);

  if (source) return <img className={className} src={source} alt={`Miniatura de ${title}`} />;
  return (
    <div className={`thumbnail-fallback ${className}`} aria-label={`Sin miniatura para ${title}`}>
      <span>ANIS</span>
      <small>STAR</small>
    </div>
  );
}

function CatalogCard({
  item,
  selected,
  favorite,
  safeMode,
  onSelect,
  onToggleFavorite
}: {
  item: CatalogItem;
  selected: boolean;
  favorite: boolean;
  safeMode: boolean;
  onSelect: (item: CatalogItem) => void;
  onToggleFavorite: (id: string) => void;
}) {
  const activateWithKeyboard = (event: KeyboardEvent<HTMLElement>) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      onSelect(item);
    }
  };
  return (
    <article
      className={`catalog-card chamfer ${selected ? "selected" : ""}`}
      tabIndex={0}
      role="button"
      aria-label={`Seleccionar ${item.title}`}
      onClick={() => onSelect(item)}
      onKeyDown={activateWithKeyboard}
    >
      <Thumbnail path={item.preview} title={item.title} className="card-thumb" />
      <div className="card-scanline" />
      <div className="card-meta">
        <span className={`type-badge type-${item.type.toLowerCase()}`}>{typeLabel(item.type)}</span>
        {safeMode && <span className="safe-badge">⚠ SAFE</span>}
        <button
          className={`favorite-button ${favorite ? "is-favorite" : ""}`}
          title={favorite ? "Quitar de favoritas" : "Añadir a favoritas"}
          aria-label={favorite ? "Quitar de favoritas" : "Añadir a favoritas"}
          onClick={(event) => {
            event.stopPropagation();
            onToggleFavorite(item.id);
          }}
        >
          ★
        </button>
      </div>
      <div className="card-copy">
        <h3 title={item.title}>{item.title || "Sin título"}</h3>
        <p>{item.tags?.slice(0, 2).join(" · ") || item.source || "catálogo local"}</p>
      </div>
    </article>
  );
}

function VirtualCatalogGrid({
  items,
  selectedId,
  favorites,
  safeModeIds,
  onSelect,
  onToggleFavorite
}: {
  items: CatalogItem[];
  selectedId?: string;
  favorites: Set<string>;
  safeModeIds: Set<string>;
  onSelect: (item: CatalogItem) => void;
  onToggleFavorite: (id: string) => void;
}) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [width, setWidth] = useState(640);
  const rowHeight = 236;
  const columnWidth = 194;
  const columns = Math.max(1, Math.floor((width - 22) / columnWidth));
  const rows = Math.ceil(items.length / columns);
  const viewportHeight = hostRef.current?.clientHeight ?? 620;
  const firstRow = Math.max(0, Math.floor(scrollTop / rowHeight) - 1);
  const lastRow = Math.min(rows, Math.ceil((scrollTop + viewportHeight) / rowHeight) + 2);

  useLayoutEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    const resize = () => setWidth(host.clientWidth);
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(host);
    return () => observer.disconnect();
  }, []);

  useEffect(() => setScrollTop(0), [items.length, columns]);

  if (items.length === 0) {
    return (
      <div className="empty-grid chamfer">
        <div className="empty-star">★</div>
        <h2>No hay wallpapers para mostrar</h2>
        <p>Conectá el daemon o añadí una carpeta para que ANIS STAR empiece a brillar.</p>
      </div>
    );
  }

  const visibleRows = [];
  for (let row = firstRow; row < lastRow; row += 1) {
    const start = row * columns;
    const rowItems = items.slice(start, start + columns);
    visibleRows.push(
      <div
        className="virtual-row"
        key={row}
        style={{
          top: row * rowHeight,
          gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))`
        }}
      >
        {rowItems.map((item) => (
          <CatalogCard
            key={item.id}
            item={item}
            selected={item.id === selectedId}
            favorite={favorites.has(item.id)}
            safeMode={safeModeIds.has(item.id)}
            onSelect={onSelect}
            onToggleFavorite={onToggleFavorite}
          />
        ))}
      </div>
    );
  }

  return (
    <div className="catalog-scroll" ref={hostRef} onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}>
      <div className="virtual-space" style={{ height: rows * rowHeight }}>
        {visibleRows}
      </div>
    </div>
  );
}

function Toast({ message, onDismiss }: { message: string | null; onDismiss: () => void }) {
  useEffect(() => {
    if (!message) return;
    const timer = window.setTimeout(onDismiss, 5_200);
    return () => window.clearTimeout(timer);
  }, [message, onDismiss]);
  if (!message) return null;
  return (
    <div className="toast chamfer" role="status">
      <span>✦</span>
      <p>{message}</p>
      <button onClick={onDismiss} aria-label="Cerrar aviso">
        ×
      </button>
    </div>
  );
}

function SettingsPanel({
  settings,
  onClose,
  onSave,
  onRefresh
}: {
  settings: Settings;
  onClose: () => void;
  onSave: (patch: Record<string, unknown>) => void;
  onRefresh: () => void;
}) {
  const [fps, setFps] = useState(settings.fpsCap);
  const [scaleMode, setScaleMode] = useState(settings.wallpaper.scaleMode);
  return (
    <div className="settings-popover chamfer" role="dialog" aria-label="Configuración">
      <header>
        <span>CONFIGURACIÓN</span>
        <button onClick={onClose} aria-label="Cerrar configuración">×</button>
      </header>
      <label>
        FPS máximo
        <input
          type="number"
          min="1"
          max="60"
          value={fps}
          onChange={(event) => setFps(Math.max(1, Math.min(60, Number(event.target.value) || 30)))}
        />
      </label>
      <label>
        Escalado del wallpaper
        <select value={scaleMode} onChange={(event) => setScaleMode(event.target.value as Settings["wallpaper"]["scaleMode"])}>
          <option value="cover">Cover · recorta</option>
          <option value="fit">Fit · bandas</option>
          <option value="stretch">Stretch · libre</option>
        </select>
      </label>
      <div className="settings-actions">
        <button className="secondary-button" onClick={onRefresh}>↻ Catálogo</button>
        <button
          className="primary-button"
          onClick={() => onSave({ fpsCap: fps, wallpaper: { scaleMode } })}
        >
          Guardar
        </button>
      </div>
    </div>
  );
}

export default function App() {
  const [catalog, setCatalog] = useState<CatalogItem[]>([]);
  const [monitors, setMonitors] = useState<Monitor[]>([]);
  const [settings, setSettings] = useState<Settings>(emptySettings);
  const [status, setStatus] = useState<DaemonStatus | null>(null);
  const [online, setOnline] = useState(false);
  const [view, setView] = useState<NavView>("installed");
  const [query, setQuery] = useState("");
  const [selectedId, setSelectedId] = useState<string>();
  const [selectedOutput, setSelectedOutput] = useState("");
  const [preview, setPreview] = useState<PreviewFrame | null>(null);
  const [previewBusy, setPreviewBusy] = useState(false);
  const [toast, setToast] = useState<string | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const previewRequest = useRef(0);

  const showToast = useCallback((message: string) => setToast(message), []);

  const loadCatalog = useCallback(async () => {
    const items = await window.anispaper.rpc<CatalogItem[]>("catalog.list");
    setCatalog(Array.isArray(items) ? items : []);
    setSelectedId((current) => (current && items.some((item) => item.id === current) ? current : items[0]?.id));
  }, []);

  const loadEverything = useCallback(
    async (quiet = false) => {
      try {
        const [items, outputs, daemonSettings, daemonStatus] = await Promise.all([
          window.anispaper.rpc<CatalogItem[]>("catalog.list"),
          window.anispaper.rpc<Monitor[]>("monitor.list"),
          window.anispaper.rpc<Settings>("settings.get"),
          window.anispaper.rpc<DaemonStatus>("status.get")
        ]);
        setCatalog(Array.isArray(items) ? items : []);
        setMonitors(Array.isArray(outputs) ? outputs : []);
        setSettings(normalizeSettings(daemonSettings));
        setStatus(daemonStatus ?? null);
        setSelectedId((current) => (current && items.some((item) => item.id === current) ? current : items[0]?.id));
        setSelectedOutput((current) => current || outputs[0]?.name || "");
        setOnline(true);
      } catch (error) {
        setOnline(false);
        if (!quiet) showToast(errorMessage(error));
      }
    },
    [showToast]
  );

  const loadStatus = useCallback(async () => {
    try {
      const result = await window.anispaper.rpc<DaemonStatus>("status.get");
      setStatus(result);
      setOnline(true);
    } catch {
      setOnline(false);
    }
  }, []);

  useEffect(() => {
    void loadEverything(true);
    // The socket can fail before the renderer has registered its IPC listener.
    // A delayed probe guarantees an explicit Spanish notice for a cold daemon.
    const initialDaemonCheck = window.setTimeout(() => {
      void window.anispaper
        .rpc<DaemonStatus>("status.get")
        .then((result) => {
          setStatus(result);
          setOnline(true);
        })
        .catch(() => showToast("Daemon no está corriendo. Verificá anispaper.service."));
    }, 1_600);
    const unsubscribe = window.anispaper.onEvent((event: DaemonEvent | ConnectionEvent) => {
      if ("online" in event) {
        setOnline(event.online);
        if (event.online) {
          void loadEverything(true);
        } else if (event.message) {
          showToast("Daemon no está corriendo. Verificá anispaper.service.");
        }
        return;
      }
      if (event.method === "catalog.changed") {
        void loadCatalog().catch((error) => showToast(errorMessage(error)));
      }
      if (event.method.startsWith("wallpaper.")) {
        void loadStatus();
      }
    });
    const poll = window.setInterval(() => void loadStatus(), 3_000);
    return () => {
      unsubscribe();
      window.clearTimeout(initialDaemonCheck);
      window.clearInterval(poll);
    };
  }, [loadCatalog, loadEverything, loadStatus, showToast]);

  useEffect(
    () => () => {
      // Best effort only: the daemon also cleans this isolated renderer when it
      // is replaced. Do not keep a hidden preview child after closing the UI.
      void window.anispaper.rpc("wallpaper.stop", { output: UI_PREVIEW_OUTPUT }).catch(() => undefined);
    },
    []
  );

  const selected = catalog.find((item) => item.id === selectedId) ?? null;
  const selectedRenderer = rendererFor(status, selectedOutput);
  const previewRenderer = rendererFor(status, UI_PREVIEW_OUTPUT);
  const selectedRendererId = selectedRenderer?.wallpaperId ?? selectedRenderer?.id;
  const previewRendererId = previewRenderer?.wallpaperId ?? previewRenderer?.id;
  const liveOutput =
    selected && selectedRendererId === selected.id
      ? selectedOutput
      : selected && previewRendererId === selected.id
        ? UI_PREVIEW_OUTPUT
        : "";
  const liveRenderer = liveOutput === selectedOutput ? selectedRenderer : previewRenderer;

  useEffect(() => {
    if (!liveOutput || !liveRenderer) {
      setPreview(null);
      return;
    }
    let alive = true;
    let inFlight = false;
    const nextFrame = async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        const frame = await window.anispaper.rpc<PreviewFrame>("preview.frame", { output: liveOutput });
        if (alive) {
          setPreview(frame);
          setPreviewBusy(false);
        }
      } catch {
        if (alive) setPreviewBusy(false);
      } finally {
        inFlight = false;
      }
    };
    setPreviewBusy(true);
    void nextFrame();
    const timer = window.setInterval(() => void nextFrame(), 67);
    return () => {
      alive = false;
      window.clearInterval(timer);
    };
  }, [liveOutput, liveRenderer, status]);

  const favoriteIds = useMemo(() => new Set(settings.favorites), [settings.favorites]);
  const safeModeIds = useMemo(
    () => new Set((status?.renderers ?? []).filter((renderer) => renderer.safeMode).map((renderer) => renderer.wallpaperId ?? renderer.id ?? "")),
    [status]
  );
  const filteredCatalog = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return catalog.filter((item) => {
      const matchesView =
        view === "favorites"
          ? favoriteIds.has(item.id)
          : view === "folders"
            ? item.source === "custom"
            : true;
      const haystack = [item.title, item.type, item.source, ...(item.tags ?? [])].join(" ").toLocaleLowerCase();
      return matchesView && (!needle || haystack.includes(needle));
    });
  }, [catalog, favoriteIds, query, view]);

  const toggleFavorite = useCallback(
    async (id: string) => {
      const next = settings.favorites.includes(id)
        ? settings.favorites.filter((favorite) => favorite !== id)
        : [...settings.favorites, id];
      try {
        const saved = await window.anispaper.rpc<Settings>("settings.set", { favorites: next });
        setSettings(normalizeSettings(saved));
      } catch (error) {
        showToast(`No se pudo actualizar favoritas: ${errorMessage(error)}`);
      }
    },
    [settings.favorites, showToast]
  );

  const addFolder = useCallback(async () => {
    const folder = await window.anispaper.chooseFolder();
    if (!folder) return;
    try {
      await window.anispaper.rpc("catalog.addFolder", { path: folder });
      await loadEverything(true);
      showToast("Carpeta añadida al catálogo.");
    } catch (error) {
      showToast(`No se pudo añadir la carpeta: ${errorMessage(error)}`);
    }
  }, [loadEverything, showToast]);

  const selectCatalogItem = useCallback(
    async (item: CatalogItem) => {
      const request = ++previewRequest.current;
      setSelectedId(item.id);
      setPreview(null);
      setPreviewBusy(true);
      try {
        await window.anispaper.rpc("wallpaper.apply", { id: item.id, output: UI_PREVIEW_OUTPUT });
        if (request !== previewRequest.current) return;
        await loadStatus();
      } catch (error) {
        if (request === previewRequest.current) {
          setPreviewBusy(false);
          showToast(`No se pudo abrir el preview vivo: ${errorMessage(error)}`);
        }
      }
    },
    [loadStatus, showToast]
  );

  const applyWallpaper = useCallback(async () => {
    if (!selected || !selectedOutput) {
      showToast("Elegí un wallpaper y un monitor antes de aplicar.");
      return;
    }
    try {
      await window.anispaper.rpc("wallpaper.apply", { id: selected.id, output: selectedOutput });
      await window.anispaper.rpc("wallpaper.stop", { output: UI_PREVIEW_OUTPUT });
      await loadStatus();
      showToast(`Aplicado en ${selectedOutput}.`);
    } catch (error) {
      showToast(`No se pudo iniciar el renderer: ${errorMessage(error)}`);
    }
  }, [loadStatus, selected, selectedOutput, showToast]);

  const stopWallpaper = useCallback(async () => {
    if (!selectedOutput) return;
    try {
      await window.anispaper.rpc("wallpaper.stop", { output: selectedOutput });
      setPreview(null);
      await loadStatus();
      showToast(`Renderer detenido en ${selectedOutput}.`);
    } catch (error) {
      showToast(`No se pudo detener el renderer: ${errorMessage(error)}`);
    }
  }, [loadStatus, selectedOutput, showToast]);

  const saveCurrentPreview = useCallback(async () => {
    if (!preview || !selected) {
      showToast("Todavía no hay un frame para capturar.");
      return;
    }
    try {
      const result = await window.anispaper.savePreview(preview.data, `${selected.title || "anispaper"}-sddm`);
      if (!result.canceled) showToast(`Preview JPEG guardado${result.path ? ` en ${result.path}` : ""}. F6 lo instala en SDDM.`);
    } catch (error) {
      showToast(`No se pudo guardar el preview: ${errorMessage(error)}`);
    }
  }, [preview, selected, showToast]);

  const saveSettings = useCallback(
    async (patch: Record<string, unknown>) => {
      try {
        const saved = await window.anispaper.rpc<Settings>("settings.set", patch);
        setSettings(normalizeSettings(saved));
        setSettingsOpen(false);
        showToast("Configuración guardada.");
      } catch (error) {
        showToast(`No se pudo guardar: ${errorMessage(error)}`);
      }
    },
    [showToast]
  );

  const saveVolume = useCallback(async () => {
    try {
      const saved = await window.anispaper.rpc<Settings>("settings.set", { defaultVolume: settings.defaultVolume });
      setSettings(normalizeSettings(saved));
    } catch (error) {
      showToast(`No se pudo guardar el volumen: ${errorMessage(error)}`);
    }
  }, [settings.defaultVolume, showToast]);

  const previewSource = dataUrl(preview);
  const previewStyle = {
    "--preview-fit": settings.wallpaper.scaleMode === "fit" ? "contain" : settings.wallpaper.scaleMode === "stretch" ? "fill" : "cover"
  } as CSSProperties;

  return (
    <main className="app-shell">
      <header className="topbar">
        <div className="brand" aria-label="AnisPaper">
          <span>★</span>
          <strong>AnisPaper</strong>
          <small>ANIS STAR</small>
        </div>
        <label className="search-box chamfer">
          <span>⌕</span>
          <input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Buscar título o tags…" aria-label="Buscar wallpapers" />
          {query && <button onClick={() => setQuery("")} aria-label="Limpiar búsqueda">×</button>}
        </label>
        <label className="monitor-select chamfer">
          <span>◫</span>
          <select value={selectedOutput} onChange={(event) => setSelectedOutput(event.target.value)} aria-label="Monitor de destino">
            {monitors.length === 0 && <option value="">Sin monitores</option>}
            {monitors.map((monitor) => (
              <option key={monitor.name} value={monitor.name}>
                {monitor.name}{monitor.renderSize ? ` · ${monitor.renderSize.width}×${monitor.renderSize.height}` : ""}
              </option>
            ))}
          </select>
        </label>
        <button className="settings-button chamfer" onClick={() => setSettingsOpen((open) => !open)} aria-label="Configuración">⚙</button>
        {settingsOpen && <SettingsPanel settings={settings} onClose={() => setSettingsOpen(false)} onSave={saveSettings} onRefresh={() => void window.anispaper.rpc("catalog.refresh").then(() => showToast("Rescan programado.")).catch((error) => showToast(errorMessage(error)))} />}
      </header>

      <section className="workspace">
        <nav className="left-nav" aria-label="Biblioteca">
          <div className="nav-caption">BIBLIOTECA</div>
          {navItems.map((entry) => (
            <button key={entry.id} className={view === entry.id ? "active" : ""} onClick={() => setView(entry.id)}>
              <span>{entry.icon}</span>{entry.label}
              {entry.id === "installed" && <small>{catalog.length}</small>}
              {entry.id === "favorites" && <small>{settings.favorites.length}</small>}
            </button>
          ))}
          <div className="nav-spacer" />
          <button className="add-folder chamfer" onClick={() => void addFolder()}>
            <span>＋</span> Añadir carpeta
          </button>
          <p className="nav-hint">Las carpetas se guardan en el daemon y se actualizan solas.</p>
        </nav>

        <section className="catalog-panel">
          <div className="panel-heading">
            <div>
              <p>{view === "favorites" ? "COLECCIÓN" : view === "folders" ? "CARPETAS" : "INSTALADAS"}</p>
              <h1>{filteredCatalog.length.toLocaleString("es-AR")} wallpapers</h1>
            </div>
            <button className="small-action" onClick={() => void loadCatalog().catch((error) => showToast(errorMessage(error)))}>↻ Actualizar</button>
          </div>
          <VirtualCatalogGrid
            items={filteredCatalog}
            selectedId={selected?.id}
            favorites={favoriteIds}
            safeModeIds={safeModeIds}
            onSelect={(item) => void selectCatalogItem(item)}
            onToggleFavorite={(id) => void toggleFavorite(id)}
          />
        </section>

        <aside className="preview-panel">
          <div className="preview-heading">
            <span className={online ? "online-dot" : "offline-dot"} />
            <p>PREVIEW VIVO</p>
            {previewBusy && <small>SYNC</small>}
          </div>
          <div className="live-frame chamfer" style={previewStyle}>
            {previewSource ? (
              <img src={previewSource} alt="Frame vivo del renderer" />
            ) : selected ? (
              <Thumbnail path={selected.preview} title={selected.title} className="preview-fallback" />
            ) : (
              <div className="no-selection"><span>★</span><p>Elegí un wallpaper</p></div>
            )}
            {selectedRenderer?.safeMode && <div className="safe-overlay">⚠ MODO SEGURO</div>}
          </div>
          <div className="selected-copy">
            <span className={`type-badge type-${selected?.type.toLowerCase() ?? "unknown"}`}>{selected ? typeLabel(selected.type) : "LISTO"}</span>
            <h2>{selected?.title ?? "Sin selección"}</h2>
            <p>{liveRenderer ? `${liveRenderer.renderer} · ${Math.round(liveRenderer.fps ?? 0)} fps` : "Seleccioná una card para iniciar su preview aislado."}</p>
          </div>
          <div className="playback-controls">
            <button className="control-play" onClick={() => void applyWallpaper()} title="Aplicar e iniciar renderer">▶</button>
            <button className="control-pause" disabled title="El contrato actual del daemon no expone pausa/reanudar">Ⅱ</button>
            <button className="control-stop" onClick={() => void stopWallpaper()} title="Detener renderer">■</button>
            <label className="volume-control">
              <span>VOL {Math.round(settings.defaultVolume * 100)}%</span>
              <input
                type="range"
                min="0"
                max="1"
                step="0.01"
                value={settings.defaultVolume}
                onChange={(event) => setSettings((current) => ({ ...current, defaultVolume: Number(event.target.value) }))}
                onPointerUp={() => void saveVolume()}
                onKeyUp={() => void saveVolume()}
                aria-label="Volumen por defecto"
              />
            </label>
          </div>
          <button className="apply-button chamfer" onClick={() => void applyWallpaper()} disabled={!selected || !selectedOutput}>
            <span>★</span> Aplicar a {selectedOutput || "monitor"}
          </button>
          <button className="snapshot-button chamfer" onClick={() => void saveCurrentPreview()} disabled={!preview}>
            <span>📸</span> Capturar para SDDM
          </button>
          {selected?.id.startsWith("steam:") && (
            <button className="steam-link" onClick={() => void window.anispaper.openSteam(selected.id).catch((error) => showToast(errorMessage(error)))}>Abrir en Steam ↗</button>
          )}
          <div className="preview-note">Preview a ~15 fps. Pausa y velocidad quedan deshabilitadas hasta que el daemon exponga esos controles.</div>
        </aside>
      </section>

      <footer className="statusbar">
        <span className={online ? "status-good" : "status-bad"}>{online ? "● daemon conectado" : "● daemon desconectado"}</span>
        <span>Wayland · Bridge SHM</span>
        <span>{liveRenderer ? `${Math.round(liveRenderer.fps ?? 0)} fps` : "0 fps"}</span>
        <span>{status?.watchdog?.count ?? 0} crashes</span>
        <strong>plasma intacto 😌</strong>
      </footer>
      <Toast message={toast} onDismiss={() => setToast(null)} />
    </main>
  );
}

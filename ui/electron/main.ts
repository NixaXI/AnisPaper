import { app, BrowserWindow, dialog, ipcMain, shell } from "electron";
import net from "node:net";
import path from "node:path";
import { readFile, stat, writeFile } from "node:fs/promises";
import type { DaemonEvent, JsonRecord } from "../shared/types";

const MAX_LINE_BYTES = 4 * 1024 * 1024;
const MAX_PREVIEW_BYTES = 8 * 1024 * 1024;
const RPC_TIMEOUT_MS = 8_000;

const rpcMethods = new Set([
  "catalog.list",
  "catalog.refresh",
  "catalog.addFolder",
  "monitor.list",
  "events.subscribe",
  "settings.get",
  "settings.set",
  "status.get",
  "wallpaper.apply",
  "wallpaper.stop",
  "preview.frame",
  "sddm.snapshot",
  "sddm.installTheme",
  "steam.install"
]);

// steamcmd / pkexec pueden tardar minutos; el resto responde en segundos.
const RPC_LONG_TIMEOUTS: Record<string, number> = {
  "steam.install": 15 * 60_000,
  "sddm.installTheme": 200_000
};

interface PendingRequest {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
  timer: NodeJS.Timeout;
}

class RpcError extends Error {
  constructor(message: string, readonly code?: number) {
    super(message);
    this.name = "RpcError";
  }
}

function isRecord(value: unknown): value is JsonRecord {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function daemonSocketPath(): string {
  const runtimeDir = process.env.XDG_RUNTIME_DIR;
  if (runtimeDir && path.isAbsolute(runtimeDir)) {
    return path.join(runtimeDir, "anispaper.sock");
  }
  const uid = typeof process.getuid === "function" ? process.getuid() : 1000;
  return path.join("/run/user", String(uid), "anispaper.sock");
}

class DaemonClient {
  private socket: net.Socket | null = null;
  private pending = new Map<string, PendingRequest>();
  private buffer = Buffer.alloc(0);
  private nextId = 1;
  private connected = false;
  private connecting = false;
  private retryTimer: NodeJS.Timeout | null = null;
  private stopped = false;
  private readonly previewPaths = new Set<string>();

  start(): void {
    this.stopped = false;
    this.connect();
  }

  stop(): void {
    this.stopped = true;
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    const socket = this.socket;
    this.socket = null;
    this.connected = false;
    socket?.destroy();
    this.rejectPending("La aplicación se está cerrando.");
  }

  allowsPreview(filePath: string): boolean {
    return this.previewPaths.has(filePath);
  }

  call<T>(method: string, params: JsonRecord = {}): Promise<T> {
    const socket = this.socket;
    if (!this.connected || !socket || socket.destroyed) {
      return Promise.reject(
        new RpcError("Daemon no está corriendo. Iniciá anispaper.service y reintentá.")
      );
    }
    const id = String(this.nextId++);
    const line = Buffer.from(
      `${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`,
      "utf8"
    );
    if (line.length > MAX_LINE_BYTES) {
      return Promise.reject(new RpcError("La solicitud es demasiado grande."));
    }
    return new Promise<T>((resolve, reject) => {
      const timeoutMs = RPC_LONG_TIMEOUTS[method] ?? RPC_TIMEOUT_MS;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new RpcError(`El daemon no respondió a ${method}.`));
      }, timeoutMs);
      this.pending.set(id, { resolve: (value) => resolve(value as T), reject, timer });
      socket.write(line, (error) => {
        if (!error) return;
        const pending = this.pending.get(id);
        if (!pending) return;
        clearTimeout(pending.timer);
        this.pending.delete(id);
        reject(new RpcError(`No se pudo enviar ${method}: ${error.message}`));
      });
    });
  }

  private connect(): void {
    if (this.stopped || this.connecting || this.socket) return;
    this.connecting = true;
    const socket = net.connect({ path: daemonSocketPath() });
    this.socket = socket;

    socket.once("connect", () => {
      if (this.socket !== socket) return;
      this.connecting = false;
      this.connected = true;
      this.buffer = Buffer.alloc(0);
      this.broadcast({ online: true });
      void this.call("events.subscribe").catch((error: Error) => {
        this.broadcast({ online: true, message: `No se pudo suscribir a eventos: ${error.message}` });
      });
    });
    socket.on("data", (chunk: Buffer) => this.onData(socket, chunk));
    socket.once("error", (error) => this.finish(socket, error.message));
    socket.once("close", () => this.finish(socket, "Conexión con anis-paperd cerrada."));
  }

  private onData(socket: net.Socket, chunk: Buffer): void {
    if (this.socket !== socket) return;
    this.buffer = Buffer.concat([this.buffer, chunk]);
    if (this.buffer.length > MAX_LINE_BYTES * 2) {
      socket.destroy(new Error("Respuesta JSON-RPC demasiado grande."));
      return;
    }
    for (;;) {
      const end = this.buffer.indexOf(0x0a);
      if (end < 0) return;
      const line = this.buffer.subarray(0, end);
      this.buffer = this.buffer.subarray(end + 1);
      if (line.length === 0) continue;
      if (line.length > MAX_LINE_BYTES) {
        socket.destroy(new Error("Línea JSON-RPC demasiado grande."));
        return;
      }
      let message: unknown;
      try {
        message = JSON.parse(line.toString("utf8"));
      } catch {
        continue;
      }
      if (!isRecord(message)) continue;
      if (typeof message.method === "string") {
        const event: DaemonEvent = {
          method: message.method,
          params: isRecord(message.params) ? message.params : undefined
        };
        this.broadcast(event);
        continue;
      }
      if (!("id" in message)) continue;
      const pending = this.pending.get(String(message.id));
      if (!pending) continue;
      clearTimeout(pending.timer);
      this.pending.delete(String(message.id));
      if (isRecord(message.error)) {
        pending.reject(
          new RpcError(
            typeof message.error.message === "string"
              ? message.error.message
              : "Error JSON-RPC",
            typeof message.error.code === "number" ? message.error.code : undefined
          )
        );
      } else {
        this.rememberPreviewPaths(message.result);
        pending.resolve(message.result);
      }
    }
  }

  private rememberPreviewPaths(result: unknown): void {
    if (!Array.isArray(result)) return;
    for (const value of result) {
      if (!isRecord(value) || typeof value.preview !== "string" || value.preview.length === 0) {
        continue;
      }
      this.previewPaths.add(value.preview);
    }
    if (this.previewPaths.size > 20_000) this.previewPaths.clear();
  }

  private finish(socket: net.Socket, message: string): void {
    if (this.socket !== socket) return;
    this.socket = null;
    if (!socket.destroyed) socket.destroy();
    this.buffer = Buffer.alloc(0);
    const wasConnected = this.connected;
    this.connected = false;
    this.connecting = false;
    this.rejectPending(message);
    if (wasConnected || !this.stopped) {
      this.broadcast({ online: false, message });
    }
    if (!this.stopped) {
      this.retryTimer = setTimeout(() => {
        this.retryTimer = null;
        this.connect();
      }, 1_500);
    }
  }

  private rejectPending(message: string): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(new RpcError(message));
    }
    this.pending.clear();
  }

  private broadcast(event: DaemonEvent | { online: boolean; message?: string }): void {
    for (const window of BrowserWindow.getAllWindows()) {
      window.webContents.send("anispaper:event", event);
    }
  }
}

const daemon = new DaemonClient();

async function thumbnailDataUrl(filePath: unknown): Promise<string> {
  if (typeof filePath !== "string" || !daemon.allowsPreview(filePath)) {
    throw new Error("Miniatura no autorizada por el catálogo.");
  }
  const extension = path.extname(filePath).toLowerCase();
  const mime =
    extension === ".png"
      ? "image/png"
      : extension === ".webp"
        ? "image/webp"
        : extension === ".gif"
          ? "image/gif"
          : extension === ".jpg" || extension === ".jpeg"
            ? "image/jpeg"
            : "";
  if (!mime) return "";
  const info = await stat(filePath);
  if (!info.isFile() || info.size <= 0 || info.size > MAX_PREVIEW_BYTES) return "";
  const bytes = await readFile(filePath);
  return `data:${mime};base64,${bytes.toString("base64")}`;
}

async function savePreview(data: unknown, suggestedName: unknown) {
  if (typeof data !== "string" || data.length === 0 || data.length > MAX_PREVIEW_BYTES * 2) {
    throw new Error("El frame de preview no es válido.");
  }
  const bytes = Buffer.from(data, "base64");
  if (bytes.length < 4 || bytes.length > MAX_PREVIEW_BYTES || bytes[0] !== 0xff || bytes[1] !== 0xd8) {
    throw new Error("El preview no contiene una imagen JPEG válida.");
  }
  const fallbackName = "anispaper-preview.jpg";
  const safeName =
    typeof suggestedName === "string" && /^[a-zA-Z0-9._ -]{1,100}$/.test(suggestedName)
      ? suggestedName.replace(/\.[^.]+$/, "") + ".jpg"
      : fallbackName;
  const result = await dialog.showSaveDialog({
    title: "Guardar preview para SDDM",
    defaultPath: path.join(app.getPath("pictures"), safeName),
    filters: [{ name: "Imagen JPEG", extensions: ["jpg", "jpeg"] }]
  });
  if (result.canceled || !result.filePath) return { canceled: true };
  await writeFile(result.filePath, bytes, { mode: 0o600 });
  return { canceled: false, path: result.filePath };
}

function createWindow(): void {
  const window = new BrowserWindow({
    width: 1480,
    height: 920,
    minWidth: 1040,
    minHeight: 650,
    backgroundColor: "#0A0D14",
    title: "AnisPaper",
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  });
  const devUrl = process.env.VITE_DEV_SERVER_URL;
  window.once("ready-to-show", () => {
    console.log("[AnisPaper] Ventana lista para mostrar");
    window.show();
    window.focus();
  });
  if (devUrl) {
    void window.loadURL(devUrl);
  } else {
    void window.loadFile(path.join(__dirname, "../../dist/index.html"));
  }
}

app.whenReady().then(() => {
  console.log("[AnisPaper] Electron listo");
  ipcMain.handle("anispaper:rpc", async (_event, request: unknown) => {
    if (!isRecord(request) || typeof request.method !== "string" || !rpcMethods.has(request.method)) {
      throw new Error("Método JSON-RPC no permitido por la UI.");
    }
    const params = request.params === undefined ? {} : request.params;
    if (!isRecord(params)) throw new Error("Los parámetros RPC deben ser un objeto.");
    return daemon.call(request.method, params);
  });
  ipcMain.handle("anispaper:chooseFolder", async () => {
    const result = await dialog.showOpenDialog({
      title: "Añadir carpeta de wallpapers",
      properties: ["openDirectory"]
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle("anispaper:thumbnail", (_event, filePath: unknown) => thumbnailDataUrl(filePath));
  ipcMain.handle("anispaper:savePreview", (_event, data: unknown, name: unknown) => savePreview(data, name));
  ipcMain.handle("anispaper:openSteam", async (_event, id: unknown) => {
    const normalized = typeof id === "string" ? id.replace(/^steam:/, "") : "";
    if (!/^\d+$/.test(normalized)) throw new Error("Este wallpaper no tiene un ID público de Steam.");
    await shell.openExternal(`https://steamcommunity.com/sharedfiles/filedetails/?id=${normalized}`);
  });
  console.log("[AnisPaper] Creando ventana...");
  createWindow();
  daemon.start();
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
app.on("before-quit", () => daemon.stop());

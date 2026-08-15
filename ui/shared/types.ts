export type JsonRecord = Record<string, unknown>;

export interface CatalogItem {
  id: string;
  title: string;
  type: string;
  file?: string;
  preview?: string;
  tags?: string[];
  properties?: JsonRecord;
  source?: string;
  root?: string;
}

export interface Monitor {
  name: string;
  geometry?: { x: number; y: number; width: number; height: number };
  physicalSize?: { width: number; height: number };
  renderSize?: { width: number; height: number };
  bufferScale?: number;
  currentWallpaperId?: string;
}

export interface Settings {
  customFolders: string[];
  favorites: string[];
  fpsCap: number;
  defaultVolume: number;
  retryQuota: number;
  gamingMode: "auto" | "on" | "off";
  wallpaper: { scaleMode: "cover" | "fit" | "stretch" };
}

export interface PreviewFrame {
  mimeType: "image/jpeg";
  data: string;
  width: number;
  height: number;
  safeMode: boolean;
}

export interface RendererStatus {
  id?: string;
  wallpaperId?: string;
  output: string;
  renderer: string;
  fps?: number;
  crashes?: number;
  safeMode?: boolean;
  state?: string;
  fallback?: boolean;
  hasFrame?: boolean;
  sceneNativeSupported?: boolean;
  badge?: string;
}

export interface DaemonStatus {
  phase?: string;
  catalog?: { items: number; scanning: boolean; generation: number };
  renderers?: RendererStatus[];
  watchdog?: { count: number; safeMode: boolean; backoffSeconds?: number[] };
  gaming?: { mode?: "auto" | "on" | "off"; active?: boolean };
  socket?: { path: string; connections: number };
}

export interface DaemonEvent {
  method: string;
  params?: JsonRecord;
}

export interface ConnectionEvent {
  online: boolean;
  message?: string;
}

export interface SavePreviewResult {
  canceled: boolean;
  path?: string;
}

export interface SddmSnapshotResult {
  path: string;
  width: number;
  height: number;
  output?: string;
  manager?: string;
  managerLabel?: string;
  managerUnit?: string;
  scope?: string;
  lockScreenChanged?: boolean;
}

export interface SddmInstallResult {
  theme: string;
  installed: boolean;
  manager?: string;
  managerLabel?: string;
  managerUnit?: string;
  route?: string;
  scope?: string;
  staged?: string;
}

export interface SteamInstallResult {
  id: string;
  installed: boolean;
  path?: string;
  linked?: string;
}

export interface AnisPaperApi {
  rpc<T>(method: string, params?: JsonRecord): Promise<T>;
  chooseFolder(): Promise<string | null>;
  thumbnail(path: string): Promise<string>;
  savePreview(data: string, suggestedName: string): Promise<SavePreviewResult>;
  openSteam(id: string): Promise<void>;
  onEvent(callback: (event: DaemonEvent | ConnectionEvent) => void): () => void;
}

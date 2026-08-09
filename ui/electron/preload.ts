import { contextBridge, ipcRenderer } from "electron";
import type { AnisPaperApi, ConnectionEvent, DaemonEvent, JsonRecord } from "../shared/types";

const api: AnisPaperApi = {
  rpc<T>(method: string, params: JsonRecord = {}): Promise<T> {
    return ipcRenderer.invoke("anispaper:rpc", { method, params }) as Promise<T>;
  },
  chooseFolder(): Promise<string | null> {
    return ipcRenderer.invoke("anispaper:chooseFolder") as Promise<string | null>;
  },
  thumbnail(filePath: string): Promise<string> {
    return ipcRenderer.invoke("anispaper:thumbnail", filePath) as Promise<string>;
  },
  savePreview(data: string, suggestedName: string) {
    return ipcRenderer.invoke("anispaper:savePreview", data, suggestedName);
  },
  openSteam(id: string): Promise<void> {
    return ipcRenderer.invoke("anispaper:openSteam", id) as Promise<void>;
  },
  onEvent(callback: (event: DaemonEvent | ConnectionEvent) => void): () => void {
    const listener = (_event: Electron.IpcRendererEvent, payload: DaemonEvent | ConnectionEvent) => {
      callback(payload);
    };
    ipcRenderer.on("anispaper:event", listener);
    return () => ipcRenderer.removeListener("anispaper:event", listener);
  }
};

contextBridge.exposeInMainWorld("anispaper", api);

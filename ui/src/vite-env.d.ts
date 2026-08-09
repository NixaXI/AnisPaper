/// <reference types="vite/client" />

import type { AnisPaperApi } from "../shared/types";

declare global {
  interface Window {
    anispaper: AnisPaperApi;
  }
}

export {};

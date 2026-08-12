# DP-2 Runtime Diagnostic Report

## PHASE
Evidence preservation and static trace of `renderer unavailable`; safe UI flood mitigation applied. Live runtime capture and DP-2/HDMI validation remain pending because this session has no process/journal/RPC execution facility.

## FOUND
- Persistent display configuration is already confirmed correct: DP-2 → containment 1; HDMI-A-1 → containment 2; KScreen sees both; DP-2 uses `Output=DP-2`, `ScaleMode=cover`.
- The active problem is runtime, not a demonstrated containment/mapping defect.
- `-32001 "renderer unavailable"` has multiple backend sources:
  1. `RendererManager::apply`: unsupported type, unavailable non-Scene file, or failure creating the output SHM bridge.
  2. `IsolatedRenderer::start`: unsupported renderer type, absent Scene engine/project directory, or unavailable source.
  3. `preview.frame` in `src/daemon/main.cpp`: no frame exists for the requested output, or JPEG encoding fails.
- The observed Electron stack at `DaemonClient.onData` is consistent with a rejected `preview.frame` response. It does not by itself prove Electron/Chromium caused the renderer failure.
- UI preview polling runs every 67 ms. The main-process client already cached frames and tracked backoff, but during backoff it generated a fresh locally rejected `RpcError(-32001)` for each poll. Electron logs rejected IPC handlers, explaining the massive repeated error output after one backend failure.
- Renderer lifecycle already isolates children per output, preserves a static bridge frame during bounded 1/3/9-second recovery, and enters safe mode after three crashes. `lastFrame()` returns the bridge snapshot while the renderer is absent, specifically to avoid transient `-32001` during recovery.
- Existing integration coverage asserts independent DP-2 and HDMI-A-1 renderer/bridge maps and verifies stopping DP-2 does not remove HDMI-A-1.

## EVIDENCE
- Full structured preservation of the temporary context: `artifacts/dp2-debug-current/pasted-context-160e959b.txt`.
- Relevant trace:
  - `ui/src/App.tsx`: live preview interval = 67 ms.
  - `ui/electron/main.ts`: preview request cache/in-flight/backoff and JSON-RPC rejection path.
  - `src/daemon/main.cpp`: `preview.frame` emits `-32001` only when `lastFrame()` is null or JPEG encoding fails; `wallpaper.apply` forwards renderer apply failures.
  - `src/renderers/renderer_manager.cpp`: output lookup, bridge ownership, startup timeout, crash recovery, safe mode, status and watchdog.
  - `src/renderers/isolated_renderer.cpp`: child launch, stderr forwarding, exit diagnostics, Scene SHM open/validation and frame publication.
  - `tests/f4_integration.py`: DP-2/HDMI-A-1 isolation assertions.

## ROOT CAUSE
- **Confirmed for the error flood:** local preview backoff rejected every 67 ms UI poll, creating repeated Electron IPC handler errors even though only occasional daemon retries were desired.
- **Not yet established for DP-2 renderer loss:** the first renderer stderr/exit/signal/asset/shader/SHM/watchdog event must be captured from the still-broken live session before any Plasma restart. The current evidence cannot safely distinguish never-started, crashed, missing-SHM, stalled-frame, manager-lookup, watchdog, or stale-assignment cases.

## FIX
- Updated `ui/electron/main.ts` so an unavailable preview creates one delayed in-flight probe per output. Pollers share that promise during the 750 ms backoff instead of receiving a newly rejected `-32001` every 67 ms.
- Kept `-32001` semantically intact; no errors are converted to success.
- Did not modify containments, monitor mapping, Plasma/SDDM configuration, watchdog behavior, resolution/FPS, or the FrameBridge `frameNo=0` write-in-progress invariant.
- Did not restart or kill Plasma and did not continue login.

## VALIDATION
- Static review confirms the change preserves one bounded retry probe per output and retains the original daemon error if recovery still fails.
- No build/runtime test was executed in this session because no terminal/process execution tool is available.
- Live DP-2 and HDMI-A-1 behavior is therefore **not yet validated**.

## NEXT
Before any Plasma reload:
1. Capture PIDs for plasmashell, anis-paperd and all scene-engine children.
2. Capture `status.get`, `monitor.list`, assignments, renderer state per output, SHM names/headers/frame numbers, watchdog state and recent journal.
3. Reproduce exactly one DP-2 apply and correlate wallpaper ID/output/RPC with RendererManager expected/actual entry and the first preceding child error.
4. Apply only the minimal renderer lifecycle fix supported by that first error.
5. Build/typecheck/test; validate one known-good and one failing Scene on DP-2 while HDMI-A-1 remains animated and independent.
6. Run the all-Scene harness, then performance work; defer SDDM until runtime stability is proven.

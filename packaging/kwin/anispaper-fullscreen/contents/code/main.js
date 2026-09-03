/*
    AnisPaper — per-output occlusion watcher for KWin (Plasma 6, Wayland).

    Why this lives inside the compositor: KWin does not expose
    zwlr_foreign_toplevel_management nor org_kde_plasma_window_management to
    ordinary clients, and the /KWin D-Bus object only offers queryWindowInfo,
    which is interactive (it waits for the user to click a window).  A KWin
    script is the only supported way for an external process to learn about
    window state on this compositor.

    It reports the NAMES OF THE OUTPUTS whose wallpaper is currently covered by
    a fullscreen (or maximized, when configured) window.  Per-output on purpose:
    a fullscreen video on one screen must not freeze the wallpaper the user is
    still looking at on the other.  The daemon owns the policy; pausing
    renderers is already implemented for Gaming Mode.

    Debug with:
      journalctl -b -f | grep -i anispaper-fullscreen
*/

function log(message) {
    // console.info is the only reliable sink from KWin scripts.
    console.info("anispaper-fullscreen: " + message);
}

// Maximized counts as "covering the screen" only when the user opted in; a
// maximized window still shows panels and is a normal working state, unlike
// fullscreen video or games.
const includeMaximized = readConfig("IncludeMaximized", false);

let lastReported = "\u0000";  // sentinel: no report sent yet
const tracked = new Map();

function windowCovers(window) {
    if (!window || !window.normalWindow) return false;
    // Skip anything not actually presented on its output.
    if (window.minimized || window.skipTaskbar) return false;
    if (window.fullScreen) return true;
    // maximizeMode 3 == horizontally + vertically maximized.
    return includeMaximized && window.maximizeMode === 3;
}

// The output a window sits on.  KWin's Window.output is an Output object whose
// `name` matches the wl_output name the daemon keys its bridges by (DP-2,
// HDMI-A-1); anything else is unusable for per-output matching and is skipped
// rather than guessed.
function outputNameOf(window) {
    const output = window.output;
    if (!output || typeof output.name !== "string") return "";
    return output.name;
}

function coveredOutputs() {
    const names = [];
    for (const window of workspace.windowList()) {
        if (!windowCovers(window)) continue;
        const name = outputNameOf(window);
        if (name.length > 0 && names.indexOf(name) < 0) names.push(name);
    }
    names.sort();
    return names;
}

function report() {
    const names = coveredOutputs();
    // Compare as a joined string: the daemon only cares about the set, and this
    // keeps a repeated identical state from crossing D-Bus on every signal.
    const key = names.join(",");
    if (key === lastReported) return;
    lastReported = key;
    log("covered=[" + key + "]");
    // A failed call must never break the compositor script, so errors are
    // swallowed deliberately.
    try {
        callDBus("org.anispaper.Daemon", "/Gaming", "org.anispaper.Gaming",
                 "setCoveredOutputs", names);
    } catch (error) {
        log("dbus call failed: " + error);
    }
}

function track(window) {
    if (!window || tracked.has(window)) return;
    if (!window.normalWindow) return;
    tracked.set(window, true);
    // fullScreenChanged is the signal that matters; outputChanged catches a
    // covering window dragged to the other screen, which changes WHICH output
    // is occluded without changing whether one is.
    if (window.fullScreenChanged) window.fullScreenChanged.connect(report);
    if (window.maximizedChanged) window.maximizedChanged.connect(report);
    if (window.minimizedChanged) window.minimizedChanged.connect(report);
    if (window.outputChanged) window.outputChanged.connect(report);
    if (window.closed) {
        window.closed.connect(() => {
            tracked.delete(window);
            report();
        });
    }
}

function main() {
    workspace.windowList().forEach(track);
    workspace.windowAdded.connect((window) => {
        track(window);
        report();
    });
    workspace.windowRemoved.connect(report);
    // A monitor being added or removed renames/renumbers outputs, so the
    // previous report can name an output that no longer exists.
    if (workspace.screensChanged) workspace.screensChanged.connect(report);
    log("loaded (includeMaximized=" + includeMaximized + ")");
    report();
}

main();

import { chmod, cp, mkdtemp, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const uiRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const electronRoot = path.join(uiRoot, "node_modules", "electron");
const sourceDist = path.join(electronRoot, "dist");
const cli = path.join(electronRoot, "cli.js");
const executable = process.platform === "win32" ? "electron.exe" : "electron";

if (!existsSync(sourceDist) || !existsSync(cli)) {
  throw new Error("Electron no está instalado. Ejecutá npm i dentro de ui/.");
}

const temporaryDirectory = await mkdtemp(path.join(tmpdir(), "anispaper-electron-"));
const runtimeDist = path.join(temporaryDirectory, "dist");
const forwardedArguments = process.argv.slice(2);
const waylandAmdCompatibilityFlags = [
  "--disable-gpu",
  "--disable-software-rasterizer",
  "--use-gl=desktop",
  "--ozone-platform=x11"
];
const needsWaylandAmdWorkaround =
  Boolean(process.env.WAYLAND_DISPLAY) || process.env.XDG_SESSION_TYPE === "wayland";

function hasFlag(flag) {
  const name = flag.split("=", 1)[0];
  return forwardedArguments.some((argument) => argument === name || argument.startsWith(`${name}=`));
}

const electronArguments = [
  ...(needsWaylandAmdWorkaround ? waylandAmdCompatibilityFlags : []).filter(
    (flag) => !hasFlag(flag)
  ),
  ...forwardedArguments
];
let electronChild;

function forwardTermination(signal) {
  if (electronChild && !electronChild.killed) electronChild.kill(signal);
}

process.once("SIGINT", () => forwardTermination("SIGINT"));
process.once("SIGTERM", () => forwardTermination("SIGTERM"));

function runElectron() {
  return new Promise((resolve, reject) => {
    console.log(`[AnisPaper] Electron flags: ${electronArguments.filter((argument) => argument.startsWith("--")).join(" ")}`);
    // Codex/the shell can run the launcher with this Node-mode switch set.
    // It makes the Electron binary parse Chromium flags as Node flags and
    // exit before main.ts gets a chance to start.
    const childEnvironment = { ...process.env, ELECTRON_OVERRIDE_DIST_PATH: runtimeDist };
    delete childEnvironment.ELECTRON_RUN_AS_NODE;
    const child = spawn(process.execPath, [cli, ...electronArguments], {
      cwd: uiRoot,
      env: childEnvironment,
      stdio: "inherit"
    });
    electronChild = child;
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      electronChild = undefined;
      resolve(code ?? (signal ? 1 : 0));
    });
  });
}

try {
  // See vite-with-esbuild.mjs: this workspace is on a mount where extracted
  // npm binaries lose +x. Electron needs its whole dist tree beside its binary.
  await cp(sourceDist, runtimeDist, { recursive: true });
  if (process.platform !== "win32") await chmod(path.join(runtimeDist, executable), 0o700);
  const exitCode = await runElectron();
  process.exitCode = Number(exitCode);
} finally {
  await rm(temporaryDirectory, { recursive: true, force: true });
}

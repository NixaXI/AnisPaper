import { chmod, copyFile, mkdtemp, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const uiRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const supported = {
  "linux:x64": ["@esbuild/linux-x64", "bin/esbuild"],
  "linux:arm64": ["@esbuild/linux-arm64", "bin/esbuild"],
  "darwin:x64": ["@esbuild/darwin-x64", "bin/esbuild"],
  "darwin:arm64": ["@esbuild/darwin-arm64", "bin/esbuild"],
  "win32:x64": ["@esbuild/win32-x64", "esbuild.exe"]
};
const selected = supported[`${process.platform}:${process.arch}`];

if (!selected) {
  throw new Error(`No hay runner local de esbuild para ${process.platform}/${process.arch}.`);
}

const source = path.join(uiRoot, "node_modules", selected[0], selected[1]);
const vite = path.join(uiRoot, "node_modules", "vite", "bin", "vite.js");
if (!existsSync(source) || !existsSync(vite)) {
  throw new Error("Faltan dependencias de UI. Ejecutá npm i dentro de ui/.");
}

const temporaryDirectory = await mkdtemp(path.join(tmpdir(), "anispaper-esbuild-"));
const binary = path.join(temporaryDirectory, path.basename(source));

function runNode(script, args, env) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], { cwd: uiRoot, env, stdio: "inherit" });
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve(code ?? (signal ? 1 : 0)));
  });
}

try {
  // The workspace lives on a filesystem that does not preserve executable bits
  // for npm archives. esbuild itself is the only native helper Vite needs.
  await copyFile(source, binary);
  await chmod(binary, 0o700);
  const exitCode = await runNode(vite, process.argv.slice(2), {
    ...process.env,
    ESBUILD_BINARY_PATH: binary
  });
  process.exitCode = Number(exitCode);
} finally {
  await rm(temporaryDirectory, { recursive: true, force: true });
}

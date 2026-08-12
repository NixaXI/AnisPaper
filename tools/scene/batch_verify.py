#!/usr/bin/env python3
"""Verify catalogued Scene items without touching a connected output.

The harness drives the already-running daemon and assigns every item to a
synthetic output.  It records a bounded lifecycle trace, reads only the Scene
child's own SHM header, captures bounded service diagnostics, and stops the
synthetic output before moving to the next catalog item.
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter
from typing import Any


SYNTHETIC_PREFIX = "__anispaper-scene-verify-"
RESULTS = ("PASS", "CRASH", "TIMEOUT", "UNSUPPORTED", "INVALID", "OTHER_FAILURE")
CSV_COLUMNS = (
    "workshop_id",
    "path",
    "type",
    "result",
    "startup_ms",
    "first_frame_ms",
    "fps",
    "renderer_exit",
    "signal",
    "shm_created",
    "frames_advanced",
    "plasmashell_alive",
    "visual_check",
    "error_summary",
    "log_path",
    "catalog_id",
    "output",
    "apply_ms",
    "pid",
    "width",
    "height",
    "stride",
    "pixel_format",
    "buffers",
    "preview_jpeg",
    "daemon_rpc_alive",
    "cleanup_entry_gone",
    "cleanup_residual_pids",
    "cleanup_residual_shm",
    "transport_fps",
    "daemon_fps",
    "transport_window_ms",
    "transport_sample_count",
    "post_frame_sampling_ms",
)
SCENE_HEADER = struct.Struct("<4sIQQIIIIII4I")
MAX_LOG_BYTES = 256 * 1024
MAX_STATUS_TRANSITIONS = 32
MIN_TRANSPORT_WINDOW_SECONDS = 0.5
POST_FRAME_SAMPLE_SECONDS = 0.75
PROTECTED_OUTPUTS = {"HDMI-A-1", "DP-2"}


class RpcError(RuntimeError):
    def __init__(self, error: dict[str, Any]):
        self.code = error.get("code")
        self.message = str(error.get("message", "JSON-RPC error"))
        super().__init__(f"{self.code}: {self.message}")


class Rpc:
    def __init__(self, path: pathlib.Path, timeout: float = 5.0) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(max(0.1, timeout))
        self.sock.connect(str(path))
        self.buffer = b""
        self.next_id = 1

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self) -> "Rpc":
        return self

    def __exit__(self, _exc_type: Any, _exc: Any, _traceback: Any) -> None:
        self.close()

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        timeout: float = 10.0,
    ) -> Any:
        request_id = self.next_id
        self.next_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params or {},
        }
        deadline = time.monotonic() + max(0.05, timeout)
        self.sock.settimeout(max(0.05, deadline - time.monotonic()))
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        while True:
            while b"\n" not in self.buffer:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"{method}: JSON-RPC timeout")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed its JSON-RPC socket")
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            if not line:
                continue
            response = json.loads(line.decode("utf-8"))
            if response.get("id") != request_id:
                continue
            if "error" in response:
                error = response["error"]
                if not isinstance(error, dict):
                    raise RuntimeError(f"malformed JSON-RPC error: {error!r}")
                raise RpcError(error)
            return response.get("result")


def fail(message: str) -> None:
    raise RuntimeError(message)


def finite_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    if number != number or number in (float("inf"), float("-inf")):
        return None
    return number


def renderer_for(status: dict[str, Any], output: str) -> dict[str, Any] | None:
    renderers = status.get("renderers", [])
    if not isinstance(renderers, list):
        return None
    for entry in renderers:
        if isinstance(entry, dict) and entry.get("output") == output:
            return entry
    return None


def renderer_outputs(status: dict[str, Any]) -> set[str]:
    renderers = status.get("renderers", [])
    if not isinstance(renderers, list):
        return set()
    return {
        str(entry["output"])
        for entry in renderers
        if isinstance(entry, dict) and isinstance(entry.get("output"), str)
    }


def atomic_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", dir=path.parent, prefix=path.name + ".", delete=False
        ) as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            temporary = pathlib.Path(stream.name)
        os.replace(temporary, path)
    except BaseException:
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass
        raise


def csv_value(value: Any) -> Any:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list, tuple)):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    return value


def atomic_csv(path: pathlib.Path, items: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="",
            dir=path.parent,
            prefix=path.name + ".",
            delete=False,
        ) as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS, extrasaction="ignore")
            writer.writeheader()
            for item in items:
                writer.writerow({column: csv_value(item.get(column)) for column in CSV_COLUMNS})
            stream.flush()
            os.fsync(stream.fileno())
            temporary = pathlib.Path(stream.name)
        os.replace(temporary, path)
    except BaseException:
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass
        raise


def atomic_text(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", dir=path.parent, prefix=path.name + ".", delete=False
        ) as stream:
            stream.write(text)
            if not text.endswith("\n"):
                stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            temporary = pathlib.Path(stream.name)
        os.replace(temporary, path)
    except BaseException:
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass
        raise


def check_synthetic_output(output: str, connected: set[str], active: set[str]) -> None:
    if not output.startswith(SYNTHETIC_PREFIX):
        fail(f"refusing non-synthetic verification output: {output!r}")
    if output in connected:
        fail(f"refusing connected output: {output!r}")
    if output in active:
        fail(f"refusing already-active output: {output!r}")
    if output in PROTECTED_OUTPUTS:
        fail(f"refusing protected desktop output: {output!r}")


def workshop_id(item: dict[str, Any]) -> str:
    catalog_id = str(item.get("id", ""))
    if catalog_id.startswith("steam:"):
        return catalog_id.split(":", 1)[1]
    return catalog_id


def item_path(item: dict[str, Any]) -> str:
    root = item.get("root")
    if isinstance(root, str) and root:
        return root
    file_path = item.get("file")
    return file_path if isinstance(file_path, str) else ""


def usable_file(path: pathlib.Path | None) -> tuple[bool, str]:
    if path is None:
        return False, "not declared"
    try:
        if not path.is_file():
            return False, "missing or not a regular file"
        size = path.stat().st_size
    except OSError as exc:
        return False, f"stat failed: {exc}"
    if size <= 0:
        return False, "empty file"
    return True, f"{size} bytes"


def content_validation(item: dict[str, Any]) -> dict[str, Any]:
    declared_text = item.get("file") if isinstance(item.get("file"), str) else ""
    root_text = item.get("root") if isinstance(item.get("root"), str) else ""
    declared = pathlib.Path(declared_text) if declared_text else None
    package = pathlib.Path(root_text) / "scene.pkg" if root_text else None
    declared_ok, declared_detail = usable_file(declared)
    package_ok, package_detail = usable_file(package)
    invalid_evidence = ""
    if not declared_ok and not package_ok:
        invalid_evidence = (
            "declared Scene manifest is absent/unusable "
            f"({declared_detail}) and no usable scene.pkg exists ({package_detail})"
        )
    return {
        "declaredPath": declared_text or None,
        "declaredUsable": declared_ok,
        "declaredDetail": declared_detail,
        "scenePkgPath": str(package) if package is not None else None,
        "scenePkgUsable": package_ok,
        "scenePkgDetail": package_detail,
        "invalidEvidence": invalid_evidence or None,
    }


def proc_start_time(pid: int) -> str | None:
    try:
        text = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return None
    closing = text.rfind(")")
    if closing < 0:
        return None
    fields = text[closing + 2 :].split()
    return fields[19] if len(fields) > 19 else None


def process_identity_alive(pid: int, start_time: str | None) -> bool:
    if start_time is None:
        return False
    return proc_start_time(pid) == start_time


def plasmashell_baseline() -> dict[int, str]:
    result: dict[int, str] = {}
    proc = pathlib.Path("/proc")
    try:
        entries = list(proc.iterdir())
    except OSError:
        return result
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            name = (entry / "comm").read_text(encoding="utf-8").strip()
        except (OSError, UnicodeError):
            continue
        if name != "plasmashell":
            continue
        pid = int(entry.name)
        identity = proc_start_time(pid)
        if identity is not None:
            result[pid] = identity
    return result


def plasmashell_alive(baseline: dict[int, str]) -> bool | None:
    if not baseline:
        return None
    return all(process_identity_alive(pid, identity) for pid, identity in baseline.items())


def read_scene_shm(pid: int) -> dict[str, Any]:
    path = pathlib.Path(f"/dev/shm/anispaper-scene-{pid}")
    observation: dict[str, Any] = {
        "pid": pid,
        "path": str(path),
        "created": False,
        "headerValid": False,
    }
    try:
        with path.open("rb") as stream:
            payload = stream.read(SCENE_HEADER.size)
            file_size = os.fstat(stream.fileno()).st_size
        observation["created"] = True
        observation["size"] = file_size
    except FileNotFoundError:
        return observation
    except OSError as exc:
        observation["created"] = path.exists()
        observation["error"] = str(exc)
        return observation
    if len(payload) != SCENE_HEADER.size:
        observation["error"] = f"short Scene SHM header: {len(payload)}/{SCENE_HEADER.size} bytes"
        return observation
    (
        magic,
        version,
        frame_no,
        timestamp_ns,
        write_index,
        width,
        height,
        stride,
        format_code,
        buffers,
        *_reserved,
    ) = SCENE_HEADER.unpack(payload)
    expected_size = SCENE_HEADER.size + stride * height * buffers
    valid = (
        magic == b"ANST"
        and version == 1
        and width > 0
        and height > 0
        and stride >= width * 4
        and buffers > 0
        and expected_size <= file_size
    )
    observation.update(
        {
            "magic": magic.decode("ascii", errors="replace"),
            "version": version,
            "frameNo": frame_no,
            "timestampNs": timestamp_ns,
            "writeIndex": write_index,
            "width": width,
            "height": height,
            "stride": stride,
            "format": format_code,
            "pixelFormat": "RGBA8888" if format_code == 1 else None,
            "buffers": buffers,
            "expectedSize": expected_size,
            "headerValid": valid,
        }
    )
    if not valid:
        observation["error"] = "Scene SHM header/layout validation failed"
    return observation


def status_transition(entry: dict[str, Any], elapsed_ms: float) -> dict[str, Any]:
    bridge: dict[str, Any] = {}
    bridge_value = entry.get("bridge")
    if isinstance(bridge_value, dict):
        bridge = bridge_value
    return {
        "atMs": round(elapsed_ms, 3),
        "pid": entry.get("pid"),
        "renderer": entry.get("renderer"),
        "state": entry.get("state"),
        "crashes": entry.get("crashes"),
        "safeMode": entry.get("safeMode"),
        "fallback": entry.get("fallback"),
        "hasFrame": entry.get("hasFrame"),
        "fps": entry.get("fps"),
        "error": entry.get("error"),
        "sceneNativeSupported": entry.get("sceneNativeSupported"),
        "badge": entry.get("badge"),
        "bridgeFrameNo": bridge.get("frameNo"),
    }


def transition_key(transition: dict[str, Any]) -> str:
    stable = dict(transition)
    stable.pop("atMs", None)
    stable.pop("fps", None)
    stable.pop("bridgeFrameNo", None)
    return json.dumps(stable, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def run_bounded(command: list[str], timeout: float) -> dict[str, Any]:
    result: dict[str, Any] = {"command": command, "returncode": None, "output": "", "error": None}
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            timeout=max(0.1, timeout),
            check=False,
        )
        output = completed.stdout
        if completed.stderr:
            output += ("\n" if output and not output.endswith("\n") else "") + completed.stderr
        encoded = output.encode("utf-8", errors="replace")
        if len(encoded) > MAX_LOG_BYTES:
            encoded = encoded[-MAX_LOG_BYTES:]
            output = "[output truncated to final 256 KiB]\n" + encoded.decode(
                "utf-8", errors="replace"
            )
        result.update(returncode=completed.returncode, output=output)
        if completed.returncode not in (0, 1):
            result["error"] = f"command exited {completed.returncode}"
    except FileNotFoundError as exc:
        result["error"] = f"command unavailable: {exc}"
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", errors="replace")
        result.update(output=str(partial)[-MAX_LOG_BYTES:], error=f"command timed out after {timeout}s")
    except OSError as exc:
        result["error"] = f"command failed: {exc}"
    return result


def capture_journal(started: float, finished: float) -> dict[str, Any]:
    return run_bounded(
        [
            "journalctl",
            "--user",
            "-u",
            "anispaper.service",
            f"--since=@{started:.6f}",
            f"--until=@{finished:.6f}",
            "--no-pager",
            "--output=short-iso",
            "--lines=400",
        ],
        timeout=5.0,
    )


def capture_coredumps(started: float, finished: float, pids: list[int]) -> dict[str, Any]:
    if not pids:
        return {
            "command": [],
            "returncode": None,
            "output": "",
            "error": "not run: no observed renderer PID with crash evidence",
        }
    matches = [f"COREDUMP_PID={pid}" for pid in sorted(set(pids))]
    return run_bounded(
        [
            "coredumpctl",
            "--no-pager",
            "--no-legend",
            f"--since=@{started:.6f}",
            f"--until=@{finished:.6f}",
            "--json=short",
            "list",
            *matches,
        ],
        timeout=4.0,
    )


def journal_field(value: Any) -> str | None:
    if isinstance(value, list):
        value = value[0] if value else None
    if value is None:
        return None
    return str(value)


def parse_coredump_evidence(output: str, observed_pids: set[int]) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    renderer_exit: str | None = None
    signal: str | None = None
    for line in output.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(value, dict):
            continue
        pid_text = journal_field(value.get("COREDUMP_PID") or value.get("_PID"))
        try:
            pid = int(pid_text) if pid_text is not None else None
        except ValueError:
            pid = None
        if pid is None or pid not in observed_pids:
            continue
        explicit_signal = journal_field(
            value.get("COREDUMP_SIGNAL_NAME") or value.get("COREDUMP_SIGNAL")
        )
        explicit_exit = journal_field(value.get("COREDUMP_EXIT_STATUS"))
        records.append({"pid": pid, "signal": explicit_signal, "exitStatus": explicit_exit})
        if explicit_signal:
            signal = explicit_signal
        if explicit_exit:
            renderer_exit = explicit_exit
    return {"records": records, "rendererExit": renderer_exit, "signal": signal}


def extract_renderer_exit(text: str) -> str | None:
    matches = re.findall(r"renderer child exit=(-?\d+)\s+status=(?:crash|normal)", text, re.I)
    return matches[-1] if matches else None


def explicit_invalid_error(text: str) -> str | None:
    patterns = (
        r"INVALID_WALLPAPER:\s*[^\n]+",
        r"(?:failed|unable) to parse[^\n]*(?:scene|project|json|input|pkg)",
        r"(?:scene|project|json|input|pkg)[^\n]*(?:parse error|malformed|invalid syntax)",
        r"(?:parse error|malformed json|invalid json|invalid project|invalid scene input)",
        r"unexpected (?:end|token)[^\n]*(?:json|scene|project)",
    )
    for pattern in patterns:
        match = re.search(pattern, text, re.I)
        if match:
            return match.group(0).strip()
    return None


def explicit_unsupported(entry: dict[str, Any] | None) -> str | None:
    if not entry:
        return None
    if entry.get("sceneNativeSupported") is False:
        return "status.get reported sceneNativeSupported=false"
    if entry.get("renderer") == "scene-static":
        return "status.get reported renderer=scene-static"
    badge = entry.get("badge")
    if isinstance(badge, str) and re.search(r"unsupported|sin soporte|not supported", badge, re.I):
        return f"status.get badge: {badge}"
    return None


def pid_residuals(identities: dict[int, str | None]) -> list[int]:
    return sorted(
        pid
        for pid, identity in identities.items()
        if identity is not None and process_identity_alive(pid, identity)
    )


def shm_residuals(paths: set[str]) -> list[str]:
    return sorted(path for path in paths if pathlib.Path(path).exists())


def cleanup_output(
    socket_path: pathlib.Path,
    output: str,
    identities: dict[int, str | None],
    shm_paths: set[str],
    timeout: float = 4.0,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "attempted": False,
        "stopped": None,
        "entryGone": None,
        "daemonAlive": False,
        "residualPids": [],
        "residualShm": [],
        "error": None,
    }
    if not output.startswith(SYNTHETIC_PREFIX):
        result["error"] = f"refusing cleanup for non-synthetic output {output!r}"
        return result
    deadline = time.monotonic() + max(0.5, timeout)
    rpc: Rpc | None = None
    try:
        rpc = Rpc(socket_path, timeout=min(2.0, timeout))
        result["attempted"] = True
        remaining = max(0.05, deadline - time.monotonic())
        stopped = rpc.call("wallpaper.stop", {"output": output}, timeout=remaining)
        if isinstance(stopped, dict):
            result["stopped"] = bool(stopped.get("stopped"))
        while time.monotonic() < deadline:
            remaining = max(0.05, deadline - time.monotonic())
            status = rpc.call("status.get", timeout=min(1.0, remaining))
            if not isinstance(status, dict):
                raise RuntimeError("status.get returned a non-object during cleanup")
            result["daemonAlive"] = True
            if renderer_for(status, output) is None:
                result["entryGone"] = True
                break
            time.sleep(min(0.10, max(0.0, deadline - time.monotonic())))
        if result["entryGone"] is None:
            result["entryGone"] = False
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if rpc is not None:
            rpc.close()

    while time.monotonic() < deadline:
        residual_pids = pid_residuals(identities)
        residual_shm = shm_residuals(shm_paths)
        if not residual_pids and not residual_shm:
            break
        time.sleep(min(0.10, max(0.0, deadline - time.monotonic())))
    result["residualPids"] = pid_residuals(identities)
    result["residualShm"] = shm_residuals(shm_paths)
    return result


def valid_preview(preview: Any) -> tuple[bool, dict[str, Any]]:
    details: dict[str, Any] = {}
    if not isinstance(preview, dict):
        return False, {"error": "preview.frame returned a non-object"}
    details.update(
        mimeType=preview.get("mimeType"),
        width=preview.get("width"),
        height=preview.get("height"),
        safeMode=preview.get("safeMode"),
    )
    encoded = preview.get("data")
    if not isinstance(encoded, str):
        details["error"] = "preview.frame omitted base64 data"
        return False, details
    try:
        data = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as exc:
        details["error"] = f"invalid preview base64: {exc}"
        return False, details
    details["bytes"] = len(data)
    jpeg = data.startswith(b"\xff\xd8") and data.endswith(b"\xff\xd9")
    mime_ok = preview.get("mimeType") in (None, "image/jpeg")
    if not jpeg or not mime_ok:
        details["error"] = "preview.frame returned a non-JPEG payload"
        return False, details
    return True, details


def transport_measurement(
    samples_by_pid: dict[int, list[tuple[float, int]]], scene_started_mono: float
) -> dict[str, Any] | None:
    best: dict[str, Any] | None = None
    for pid, samples in samples_by_pid.items():
        if len(samples) < 2:
            continue
        first_time, first_frame = samples[0]
        last: tuple[float, int] | None = None
        for sample_time, frame_no in samples[1:]:
            if sample_time > first_time and frame_no > first_frame:
                last = (sample_time, frame_no)
        if last is None:
            continue
        last_time, last_frame = last
        window_seconds = last_time - first_time
        delta_frames = last_frame - first_frame
        if window_seconds <= 0.0 or delta_frames <= 0:
            continue
        candidate = {
            "pid": pid,
            "firstFrameNo": first_frame,
            "lastFrameNo": last_frame,
            "deltaFrames": delta_frames,
            "firstAtMs": round((first_time - scene_started_mono) * 1000.0, 3),
            "lastAtMs": round((last_time - scene_started_mono) * 1000.0, 3),
            "windowMs": round(window_seconds * 1000.0, 3),
            "sampleCount": len(samples),
            "fps": round(delta_frames / window_seconds, 3),
            "qualified": window_seconds >= MIN_TRANSPORT_WINDOW_SECONDS,
        }
        if best is None or candidate["windowMs"] > best["windowMs"]:
            best = candidate
    return best


def lifecycle_missing(record: dict[str, Any]) -> list[str]:
    missing: list[str] = []
    if not record.get("apply_succeeded"):
        missing.append("apply")
    if not record.get("pid"):
        missing.append("startup PID")
    if not record.get("shm_created"):
        missing.append("Scene SHM")
    if not record.get("shm_header_valid"):
        missing.append("valid Scene SHM header")
    if record.get("first_frame_ms") is None:
        missing.append("first native frame")
    if not record.get("frames_advanced"):
        missing.append("advancing frameNo")
    if not record.get("preview_jpeg"):
        missing.append("JPEG preview")
    if record.get("transport_fps") is None:
        missing.append("representative transport FPS over >=0.5s")
    return missing


def build_error_summary(
    record: dict[str, Any],
    invalid_evidence: str | None,
    unsupported_evidence: str | None,
    crash_evidence: str | None,
    timed_out: bool,
    runtime_errors: list[str],
) -> str:
    result = record["result"]
    parts: list[str] = []
    if result == "INVALID" and invalid_evidence:
        parts.append(invalid_evidence)
    elif result == "UNSUPPORTED" and unsupported_evidence:
        parts.append(unsupported_evidence)
    elif result == "CRASH" and crash_evidence:
        parts.append(crash_evidence)
    elif result == "TIMEOUT" or timed_out:
        missing = lifecycle_missing(record)
        parts.append("timed out waiting for " + ", ".join(missing or ["usable lifecycle evidence"]))
    parts.extend(runtime_errors[-2:])
    preview = record.get("preview")
    if result == "OTHER_FAILURE" and isinstance(preview, dict) and preview.get("error"):
        parts.append(str(preview["error"]))
    cleanup = record.get("cleanup")
    if isinstance(cleanup, dict):
        if cleanup.get("error"):
            parts.append("cleanup: " + str(cleanup["error"]))
        if cleanup.get("entryGone") is not True:
            parts.append("cleanup entry disappearance was not verified")
        if cleanup.get("residualPids"):
            parts.append("renderer PID remains: " + ",".join(map(str, cleanup["residualPids"])))
        if cleanup.get("residualShm"):
            parts.append("Scene SHM remains: " + ",".join(map(str, cleanup["residualShm"])))
    if record.get("plasmashell_alive") is False:
        parts.append("a baseline plasmashell process is no longer alive")
    if record.get("daemon_rpc_alive") is False:
        parts.append("daemon RPC liveness was not confirmed after the Scene")
    if record.get("log_write_error"):
        parts.append("log write failed: " + str(record["log_write_error"]))
    unique: list[str] = []
    for part in parts:
        clean = " ".join(part.split())
        if clean and clean not in unique:
            unique.append(clean)
    return "; ".join(unique)[:2000]


def scene_log_text(
    record: dict[str, Any], journal: dict[str, Any], coredump: dict[str, Any]
) -> str:
    metadata = {
        "catalog_id": record.get("catalog_id"),
        "workshop_id": record.get("workshop_id"),
        "path": record.get("path"),
        "output": record.get("output"),
        "started_at_unix": record.get("started_at_unix"),
        "finished_at_unix": record.get("finished_at_unix"),
    }
    lifecycle = {
        "apply": {
            "succeeded": record.get("apply_succeeded", False),
            "response": record.get("apply_response"),
            "rpc_error": record.get("apply_rpc_error"),
            "apply_ms": record.get("apply_ms"),
            "startup_ms": record.get("startup_ms"),
            "first_frame_ms": record.get("first_frame_ms"),
        },
        "renderer_status_first": record.get("renderer_status_first"),
        "renderer_status_last": record.get("renderer_status_last"),
        "status_transitions": record.get("status_transitions", []),
        "observed_pids": record.get("observed_pids", []),
        "scene_shm": record.get("scene_shm", {}),
        "frame_ranges": record.get("frame_ranges", {}),
        "watchdog_before": record.get("watchdog_before"),
        "watchdog_last": record.get("watchdog_last"),
        "preview": record.get("preview"),
        "fps": record.get("fps"),
        "transport_fps": record.get("transport_fps"),
        "daemon_fps": record.get("daemon_fps"),
        "transport_measurement": record.get("transport_measurement"),
        "post_frame_sampling_ms": record.get("post_frame_sampling_ms"),
        "cleanup": record.get("cleanup"),
        "runtime_errors": record.get("runtime_errors", []),
        "result": record.get("result"),
        "error_summary": record.get("error_summary"),
    }
    sections = [
        "# harness\n" + json.dumps(metadata, ensure_ascii=False, indent=2, sort_keys=True),
        "# lifecycle\n" + json.dumps(lifecycle, ensure_ascii=False, indent=2, sort_keys=True),
    ]
    for title, captured in (("journalctl", journal), ("coredumpctl", coredump)):
        command = json.dumps(captured.get("command", []), ensure_ascii=False)
        header = (
            f"# {title}\ncommand={command}\nreturncode={captured.get('returncode')}\n"
            f"error={captured.get('error')}"
        )
        output = str(captured.get("output", "")) or "(no output)"
        sections.append(header + "\n" + output.rstrip())
    return "\n\n".join(sections) + "\n"


def verify_scene(
    socket_path: pathlib.Path,
    item: dict[str, Any],
    output: str,
    connected: set[str],
    timeout: float,
    log_path: pathlib.Path,
    plasma_baseline: dict[int, str],
    owned_outputs: set[str],
) -> dict[str, Any]:
    scene_started_wall = time.time()
    scene_started_mono = time.monotonic()
    deadline = scene_started_mono + timeout
    record: dict[str, Any] = {
        "id": str(item.get("id", "")),
        "catalog_id": str(item.get("id", "")),
        "workshop_id": workshop_id(item),
        "path": item_path(item),
        "type": "scene",
        "output": output,
        "result": "OTHER_FAILURE",
        "status": "OTHER_FAILURE",
        "startup_ms": None,
        "first_frame_ms": None,
        "fps": None,
        "transport_fps": None,
        "daemon_fps": None,
        "transport_window_ms": None,
        "transport_sample_count": 0,
        "post_frame_sampling_ms": None,
        "renderer_exit": None,
        "signal": None,
        "shm_created": False,
        "shm_header_valid": False,
        "frames_advanced": False,
        "plasmashell_alive": None,
        "visual_check": "NOT_RUN",
        "error_summary": "",
        "log_path": str(log_path),
        "apply_ms": None,
        "pid": None,
        "width": None,
        "height": None,
        "stride": None,
        "pixel_format": None,
        "buffers": None,
        "preview_jpeg": False,
        "daemon_rpc_alive": False,
        "cleanup_entry_gone": None,
        "cleanup_residual_pids": [],
        "cleanup_residual_shm": [],
        "started_at_unix": scene_started_wall,
        "content_validation": content_validation(item),
        "status_samples": 0,
        "status_transitions": [],
        "observed_pids": [],
        "scene_shm": {},
    }
    runtime_errors: list[str] = []
    identities: dict[int, str | None] = {}
    shm_paths: set[str] = set()
    frame_ranges: dict[int, list[int]] = {}
    last_transport_observation: dict[int, tuple[float, int]] = {}
    transport_samples: dict[int, list[tuple[float, int]]] = {}
    max_daemon_fps = 0.0
    max_crashes = 0
    fallback_observed = False
    timed_out = False
    preview_attempted = False
    unsupported_evidence: str | None = None
    last_transition_key: str | None = None
    first_native_frame_mono: float | None = None
    sampling_end_mono: float | None = None
    owns_output = False
    rpc: Rpc | None = None

    def observe_status(
        status: dict[str, Any], measure_transport: bool = True
    ) -> dict[str, Any] | None:
        nonlocal max_daemon_fps, max_crashes, fallback_observed, unsupported_evidence
        nonlocal last_transition_key
        entry = renderer_for(status, output)
        watchdog = status.get("watchdog")
        if record.get("watchdog_before") is None and isinstance(watchdog, dict):
            record["watchdog_before"] = watchdog
        if isinstance(watchdog, dict):
            record["watchdog_last"] = watchdog
        if entry is None:
            return None
        record["status_samples"] += 1
        if "renderer_status_first" not in record:
            record["renderer_status_first"] = entry
        record["renderer_status_last"] = entry
        transition = status_transition(entry, (time.monotonic() - scene_started_mono) * 1000.0)
        key = transition_key(transition)
        if key != last_transition_key:
            if len(record["status_transitions"]) < MAX_STATUS_TRANSITIONS:
                record["status_transitions"].append(transition)
            else:
                record["status_transitions_truncated"] = True
            last_transition_key = key

        crashes = entry.get("crashes")
        if isinstance(crashes, int) and not isinstance(crashes, bool):
            max_crashes = max(max_crashes, crashes)
        fallback_observed = fallback_observed or bool(entry.get("fallback") or entry.get("safeMode"))
        daemon_fps = finite_number(entry.get("fps"))
        if daemon_fps is not None:
            max_daemon_fps = max(max_daemon_fps, daemon_fps)
        pid_value = entry.get("pid")
        if isinstance(pid_value, int) and not isinstance(pid_value, bool) and pid_value > 0:
            if pid_value not in identities:
                identities[pid_value] = proc_start_time(pid_value)
                record["observed_pids"].append(pid_value)
                if record["pid"] is None:
                    record["pid"] = pid_value
                    record["startup_ms"] = round(
                        (time.monotonic() - scene_started_mono) * 1000.0, 3
                    )
            shm_path = f"/dev/shm/anispaper-scene-{pid_value}"
            shm_paths.add(shm_path)
            observation = read_scene_shm(pid_value)
            record["scene_shm"][str(pid_value)] = observation
            if observation.get("created"):
                record["shm_created"] = True
            if observation.get("headerValid"):
                record["shm_header_valid"] = True
                record["width"] = observation.get("width")
                record["height"] = observation.get("height")
                record["stride"] = observation.get("stride")
                record["pixel_format"] = observation.get("pixelFormat")
                record["pixel_format_code"] = observation.get("format")
                record["buffers"] = observation.get("buffers")
                frame_no = observation.get("frameNo")
                if isinstance(frame_no, int) and frame_no > 0:
                    observed_mono = time.monotonic()
                    bounds = frame_ranges.setdefault(pid_value, [frame_no, frame_no])
                    bounds[0] = min(bounds[0], frame_no)
                    bounds[1] = max(bounds[1], frame_no)
                    record["frames_advanced"] = any(high > low for low, high in frame_ranges.values())
                    last_transport_observation[pid_value] = (observed_mono, frame_no)
                    if (
                        measure_transport
                        and first_native_frame_mono is not None
                        and observed_mono <= deadline
                    ):
                        samples = transport_samples.setdefault(pid_value, [])
                        if not samples or (
                            observed_mono > samples[-1][0] and frame_no >= samples[-1][1]
                        ):
                            samples.append((observed_mono, frame_no))
        unsupported_evidence = unsupported_evidence or explicit_unsupported(entry)
        return entry

    try:
        rpc = Rpc(socket_path, timeout=min(5.0, timeout))
        before = rpc.call("status.get", timeout=min(3.0, timeout))
        if not isinstance(before, dict):
            raise RuntimeError("status.get returned a non-object before apply")
        check_synthetic_output(output, connected, renderer_outputs(before))
        record["watchdog_before"] = before.get("watchdog")
        owned_outputs.add(output)
        owns_output = True

        apply_started = time.monotonic()
        remaining = max(0.05, timeout - (apply_started - scene_started_mono))
        apply_result = rpc.call(
            "wallpaper.apply", {"id": record["catalog_id"], "output": output}, timeout=remaining
        )
        record["apply_ms"] = round((time.monotonic() - apply_started) * 1000.0, 3)
        record["apply_succeeded"] = True
        record["apply_response"] = apply_result

        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            status = rpc.call("status.get", timeout=min(2.0, max(0.05, remaining)))
            if not isinstance(status, dict):
                raise RuntimeError("status.get returned a non-object")
            entry = observe_status(status)
            if time.monotonic() >= deadline:
                timed_out = True
                break
            if entry is not None:
                native_frame = (
                    bool(entry.get("hasFrame"))
                    and not bool(entry.get("fallback") or entry.get("safeMode"))
                    and entry.get("renderer") != "scene-static"
                    and entry.get("sceneNativeSupported") is not False
                )
                if native_frame and first_native_frame_mono is None:
                    pid_value = entry.get("pid")
                    baseline = (
                        last_transport_observation.get(pid_value)
                        if isinstance(pid_value, int) and not isinstance(pid_value, bool)
                        else None
                    )
                    first_native_frame_mono = baseline[0] if baseline else time.monotonic()
                    if baseline is not None and isinstance(pid_value, int):
                        transport_samples[pid_value] = [baseline]
                    record["first_frame_ms"] = round(
                        (first_native_frame_mono - scene_started_mono) * 1000.0, 3
                    )
                if native_frame and not preview_attempted:
                    preview_attempted = True
                    try:
                        preview = rpc.call(
                            "preview.frame",
                            {"output": output},
                            timeout=min(4.0, max(0.05, deadline - time.monotonic())),
                        )
                        preview_ok, preview_details = valid_preview(preview)
                        record["preview_jpeg"] = preview_ok
                        record["preview"] = preview_details
                    except Exception as exc:
                        record["preview"] = {"error": str(exc)}
                measurement = transport_measurement(transport_samples, scene_started_mono)
                transport_qualified = bool(measurement and measurement["qualified"])
                post_frame_sampled = bool(
                    first_native_frame_mono is not None
                    and time.monotonic() - first_native_frame_mono >= POST_FRAME_SAMPLE_SECONDS
                )
                complete = (
                    record["pid"] is not None
                    and record["shm_created"]
                    and record["shm_header_valid"]
                    and record["first_frame_ms"] is not None
                    and record["frames_advanced"]
                    and record["preview_jpeg"]
                    and transport_qualified
                    and post_frame_sampled
                    and max_crashes == 0
                    and not fallback_observed
                )
                explicit_preview_failure = (
                    preview_attempted
                    and not record["preview_jpeg"]
                    and isinstance(record.get("preview"), dict)
                    and bool(record["preview"].get("error"))
                )
                otherwise_complete = (
                    record["pid"] is not None
                    and record["shm_created"]
                    and record["shm_header_valid"]
                    and record["first_frame_ms"] is not None
                    and record["frames_advanced"]
                    and transport_qualified
                    and post_frame_sampled
                )
                failure_sampling_complete = first_native_frame_mono is None or post_frame_sampled
                if complete:
                    break
                if (unsupported_evidence or max_crashes > 0) and failure_sampling_complete:
                    break
                if explicit_preview_failure and otherwise_complete:
                    break
            time.sleep(min(0.10, max(0.0, deadline - time.monotonic())))
        else:
            timed_out = True
        sampling_end_mono = min(time.monotonic(), deadline)

        try:
            status = rpc.call("status.get", timeout=1.5)
            if isinstance(status, dict):
                record["daemon_rpc_alive"] = True
                observe_status(status, measure_transport=False)
        except Exception as exc:
            runtime_errors.append("post-run status.get: " + str(exc))
    except TimeoutError as exc:
        timed_out = True
        runtime_errors.append(str(exc))
    except RpcError as exc:
        runtime_errors.append(f"JSON-RPC {exc}")
        record["apply_rpc_error"] = {"code": exc.code, "message": exc.message}
    except Exception as exc:
        runtime_errors.append(str(exc))
    finally:
        if sampling_end_mono is None:
            sampling_end_mono = min(time.monotonic(), scene_started_mono + timeout)
        if rpc is not None:
            rpc.close()
        if owns_output:
            cleanup = cleanup_output(socket_path, output, identities, shm_paths)
        else:
            cleanup = {
                "attempted": False,
                "stopped": None,
                "entryGone": None,
                "daemonAlive": False,
                "residualPids": pid_residuals(identities),
                "residualShm": shm_residuals(shm_paths),
                "error": "output ownership guard did not pass; cleanup was not attempted",
            }
        record["cleanup"] = cleanup
        record["cleanup_entry_gone"] = cleanup.get("entryGone")
        record["cleanup_residual_pids"] = cleanup.get("residualPids", [])
        record["cleanup_residual_shm"] = cleanup.get("residualShm", [])
        record["daemon_rpc_alive"] = bool(
            record.get("daemon_rpc_alive") or cleanup.get("daemonAlive")
        )
        record["plasmashell_alive"] = plasmashell_alive(plasma_baseline)
        record["finished_at_unix"] = time.time()

    measurement = transport_measurement(transport_samples, scene_started_mono)
    transport_qualified = bool(measurement and measurement["qualified"])
    record["transport_measurement"] = measurement
    record["transport_fps"] = measurement["fps"] if transport_qualified and measurement else None
    record["daemon_fps"] = round(max_daemon_fps, 3) if max_daemon_fps > 0.0 else None
    record["fps"] = record["transport_fps"]
    record["transport_window_ms"] = measurement["windowMs"] if measurement else None
    record["transport_sample_count"] = sum(len(samples) for samples in transport_samples.values())
    if first_native_frame_mono is not None and sampling_end_mono is not None:
        record["post_frame_sampling_ms"] = round(
            max(0.0, sampling_end_mono - first_native_frame_mono) * 1000.0, 3
        )
    record["max_crashes"] = max_crashes
    record["fallback_observed"] = fallback_observed
    record["timed_out"] = timed_out
    record["runtime_errors"] = runtime_errors
    record["frame_ranges"] = {
        str(pid): {"first": bounds[0], "last": bounds[1]} for pid, bounds in frame_ranges.items()
    }

    status_errors = [
        str(transition.get("error"))
        for transition in record["status_transitions"]
        if transition.get("error")
    ]
    pre_log_text = "\n".join(runtime_errors + status_errors)
    crash_evidence: str | None = None
    if max_crashes > 0:
        crash_evidence = status_errors[-1] if status_errors else f"status.get reported crashes={max_crashes}"
    elif re.search(r"renderer child exit=|status=crash|segmentation fault|core dumped", pre_log_text, re.I):
        crash_evidence = "renderer exit/crash reported by status or RPC"

    journal = capture_journal(scene_started_wall, record["finished_at_unix"])
    coredump = capture_coredumps(
        scene_started_wall,
        record["finished_at_unix"],
        record["observed_pids"] if crash_evidence else [],
    )
    coredump_evidence = parse_coredump_evidence(
        str(coredump.get("output", "")), set(record["observed_pids"])
    )
    record["coredump_evidence"] = coredump_evidence["records"]
    record["renderer_exit"] = coredump_evidence["rendererExit"]
    record["signal"] = coredump_evidence["signal"]
    evidence_text = "\n".join(
        [pre_log_text, str(journal.get("output", "")), str(coredump.get("output", ""))]
    )
    if record["renderer_exit"] is None:
        record["renderer_exit"] = extract_renderer_exit(evidence_text)
    if coredump_evidence["records"]:
        crash_evidence = crash_evidence or "coredumpctl reported a core for an observed renderer PID"
    elif crash_evidence is None and re.search(
        r"renderer child exit=|status=crash|segmentation fault|core dumped", evidence_text, re.I
    ):
        crash_evidence = "renderer exit/crash reported in bounded service diagnostics"

    parse_evidence = explicit_invalid_error(evidence_text)
    declared_invalid = record["content_validation"].get("invalidEvidence")
    invalid_evidence = parse_evidence or declared_invalid
    cleanup = record["cleanup"]
    cleanup_ok = (
        cleanup.get("entryGone") is True
        and not cleanup.get("error")
        and not cleanup.get("residualPids")
        and not cleanup.get("residualShm")
    )
    lifecycle_ok = not lifecycle_missing(record) and max_crashes == 0 and not fallback_observed
    safety_ok = (
        record["daemon_rpc_alive"]
        and record["plasmashell_alive"] is not False
        and cleanup_ok
    )
    explicit_non_timeout_failure = bool(
        runtime_errors
        or (
            preview_attempted
            and isinstance(record.get("preview"), dict)
            and record["preview"].get("error")
        )
    )

    if unsupported_evidence:
        record["result"] = "UNSUPPORTED"
    elif invalid_evidence and not lifecycle_ok:
        record["result"] = "INVALID"
    elif crash_evidence:
        record["result"] = "CRASH"
    elif lifecycle_ok and safety_ok:
        record["result"] = "PASS"
    elif timed_out and not explicit_non_timeout_failure:
        record["result"] = "TIMEOUT"
    else:
        record["result"] = "OTHER_FAILURE"
    record["status"] = record["result"]
    record["error_summary"] = build_error_summary(
        record,
        str(invalid_evidence) if invalid_evidence else None,
        unsupported_evidence,
        crash_evidence,
        timed_out,
        runtime_errors,
    )
    record["reason"] = record["error_summary"] or "native Scene lifecycle verified"

    try:
        atomic_text(log_path, scene_log_text(record, journal, coredump))
    except OSError as exc:
        record["log_write_error"] = str(exc)
        if record["result"] == "PASS":
            record["result"] = "OTHER_FAILURE"
            record["status"] = record["result"]
        record["error_summary"] = build_error_summary(
            record,
            str(invalid_evidence) if invalid_evidence else None,
            unsupported_evidence,
            crash_evidence,
            timed_out,
            runtime_errors,
        )
        record["reason"] = record["error_summary"]
    return record


def final_cleanup_sweep(
    socket_path: pathlib.Path, owned_outputs: set[str], timeout: float = 10.0
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "checked": sorted(owned_outputs),
        "activeBefore": [],
        "stopResults": [],
        "remaining": [],
        "daemonAlive": False,
        "error": None,
    }
    if not owned_outputs:
        result["daemonAlive"] = True
        return result
    deadline = time.monotonic() + max(1.0, timeout)
    rpc: Rpc | None = None
    try:
        rpc = Rpc(socket_path, timeout=min(3.0, timeout))
        status = rpc.call("status.get", timeout=min(3.0, timeout))
        if not isinstance(status, dict):
            raise RuntimeError("status.get returned a non-object in final cleanup sweep")
        result["daemonAlive"] = True
        active = sorted(renderer_outputs(status) & owned_outputs)
        result["activeBefore"] = active
        for output in active:
            if not output.startswith(SYNTHETIC_PREFIX):
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("final cleanup sweep deadline exhausted")
            try:
                stopped = rpc.call(
                    "wallpaper.stop", {"output": output}, timeout=min(2.0, remaining)
                )
                result["stopResults"].append({"output": output, "response": stopped})
            except Exception as exc:
                result["stopResults"].append({"output": output, "error": str(exc)})
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("final cleanup verification deadline exhausted")
        final_status = rpc.call("status.get", timeout=min(3.0, remaining))
        if not isinstance(final_status, dict):
            raise RuntimeError("status.get returned a non-object after final cleanup sweep")
        result["remaining"] = sorted(renderer_outputs(final_status) & owned_outputs)
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if rpc is not None:
            rpc.close()
    return result


def safe_log_name(index: int, item: dict[str, Any]) -> str:
    identifier = workshop_id(item) or str(item.get("id", "scene"))
    identifier = re.sub(r"[^A-Za-z0-9_.-]+", "_", identifier).strip("._") or "scene"
    return f"{index:04d}-{identifier[:80]}.log"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    parser.add_argument(
        "--socket", type=pathlib.Path, default=pathlib.Path(runtime) / "anispaper.sock"
    )
    parser.add_argument(
        "--summary", type=pathlib.Path, required=True, help="JSON report destination (atomic)"
    )
    parser.add_argument(
        "--csv",
        type=pathlib.Path,
        help="CSV report destination (default: --summary basename with .csv)",
    )
    parser.add_argument(
        "--logs-dir",
        type=pathlib.Path,
        help="per-Scene log directory (default: sibling <summary-stem>-logs)",
    )
    parser.add_argument(
        "--sample", type=int, default=0, help="verify at most N catalog scene items (0 means all)"
    )
    parser.add_argument(
        "--id",
        dest="ids",
        action="append",
        default=[],
        help="verify this scene id (repeatable; overrides --sample selection)",
    )
    parser.add_argument(
        "--timeout", type=float, default=12.0, help="maximum seconds per scene, 1..60 (default: 12)"
    )
    args = parser.parse_args()
    if args.sample < 0:
        parser.error("--sample must be non-negative")
    if not 1.0 <= args.timeout <= 60.0:
        parser.error("--timeout must be between 1 and 60 seconds")
    args.csv = args.csv or args.summary.with_suffix(".csv")
    args.logs_dir = args.logs_dir or args.summary.parent / f"{args.summary.stem}-logs"
    if args.csv == args.summary:
        parser.error("--summary and --csv must be different paths")
    if args.logs_dir in (args.summary, args.csv):
        parser.error("--logs-dir must be a directory distinct from report files")
    return args


def main() -> int:
    args = parse_args()
    started = time.time()
    run_token = f"{os.getpid()}-{int(started * 1000):x}"
    owned_outputs: set[str] = set()
    plasma_baseline = plasmashell_baseline()
    summary: dict[str, Any] = {
        "schema": 2,
        "startedAtUnix": started,
        "socket": str(args.socket),
        "summaryPath": str(args.summary),
        "csvPath": str(args.csv),
        "logsDirectory": str(args.logs_dir),
        "syntheticOutputPrefix": SYNTHETIC_PREFIX,
        "runToken": run_token,
        "protectedOutputs": sorted(PROTECTED_OUTPUTS),
        "perSceneTimeoutSeconds": args.timeout,
        "plasmashellBaselinePids": sorted(plasma_baseline),
        "items": [],
    }
    fatal_exit = 0
    json_write_error: str | None = None
    try:
        with Rpc(args.socket) as rpc:
            monitors = rpc.call("monitor.list")
            if not isinstance(monitors, list):
                fail("monitor.list returned a non-array")
            connected = {
                str(m["name"])
                for m in monitors
                if isinstance(m, dict) and isinstance(m.get("name"), str)
            }
            initial_status = rpc.call("status.get")
            if not isinstance(initial_status, dict):
                fail("status.get returned a non-object")
            active = renderer_outputs(initial_status)
            catalog = rpc.call("catalog.list")
            if not isinstance(catalog, list):
                fail("catalog.list returned a non-array")

        scenes = [
            item for item in catalog if isinstance(item, dict) and item.get("type") == "scene"
        ]
        scenes.sort(key=lambda item: str(item.get("id", "")))
        summary["catalogSceneCountTotal"] = len(scenes)
        if args.ids:
            requested = set(args.ids)
            scenes = [item for item in scenes if item.get("id") in requested]
            missing = requested - {item.get("id") for item in scenes}
            if missing:
                fail(
                    "requested scene ids are absent or not type=scene: "
                    + ", ".join(sorted(map(str, missing)))
                )
        elif args.sample:
            scenes = scenes[: args.sample]
        summary["catalogSceneCountSelected"] = len(scenes)
        summary["connectedOutputsAtStart"] = sorted(connected)
        summary["activeOutputsAtStart"] = sorted(active)

        for index, item in enumerate(scenes, start=1):
            output = f"{SYNTHETIC_PREFIX}{run_token}-{index:04d}"
            log_path = args.logs_dir / safe_log_name(index, item)
            attempt_started = time.time()
            try:
                record = verify_scene(
                    args.socket,
                    item,
                    output,
                    connected,
                    args.timeout,
                    log_path,
                    plasma_baseline,
                    owned_outputs,
                )
            except Exception as exc:
                attempt_finished = time.time()
                cleanup = (
                    cleanup_output(args.socket, output, {}, set())
                    if output in owned_outputs
                    else {
                        "attempted": False,
                        "stopped": None,
                        "entryGone": None,
                        "daemonAlive": False,
                        "residualPids": [],
                        "residualShm": [],
                        "error": "output ownership was not established; cleanup was not attempted",
                    }
                )
                record = {
                    "id": str(item.get("id", "")),
                    "catalog_id": str(item.get("id", "")),
                    "workshop_id": workshop_id(item),
                    "path": item_path(item),
                    "type": "scene",
                    "output": output,
                    "result": "OTHER_FAILURE",
                    "status": "OTHER_FAILURE",
                    "visual_check": "NOT_RUN",
                    "error_summary": f"unexpected per-Scene harness error: {exc}",
                    "reason": f"unexpected per-Scene harness error: {exc}",
                    "log_path": str(log_path),
                    "cleanup": cleanup,
                    "cleanup_entry_gone": cleanup.get("entryGone"),
                    "cleanup_residual_pids": cleanup.get("residualPids", []),
                    "cleanup_residual_shm": cleanup.get("residualShm", []),
                    "plasmashell_alive": plasmashell_alive(plasma_baseline),
                    "daemon_rpc_alive": cleanup.get("daemonAlive", False),
                    "started_at_unix": attempt_started,
                    "finished_at_unix": attempt_finished,
                    "observed_pids": [],
                }
                journal = capture_journal(attempt_started, attempt_finished)
                coredump = capture_coredumps(attempt_started, attempt_finished, [])
                try:
                    atomic_text(log_path, scene_log_text(record, journal, coredump))
                except OSError as log_exc:
                    record["log_write_error"] = str(log_exc)
                    record["error_summary"] = (
                        str(record["error_summary"]) + f"; log write failed: {log_exc}"
                    )
                    record["reason"] = record["error_summary"]
            summary["items"].append(record)
            print(
                f"{index}/{len(scenes)} {record['catalog_id']}: "
                f"{record['result']} - {record['reason']}",
                flush=True,
            )
    except KeyboardInterrupt:
        summary["fatalError"] = "interrupted"
        fatal_exit = 130
    except BaseException as exc:
        summary["fatalError"] = str(exc)
        fatal_exit = 2
    finally:
        sweep = final_cleanup_sweep(args.socket, owned_outputs)
        summary["finalCleanupSweep"] = sweep
        summary["syntheticOutputsRemaining"] = sweep.get("remaining", [])
        if sweep.get("remaining"):
            remaining = set(sweep["remaining"])
            for record in summary["items"]:
                if record.get("output") in remaining and record.get("result") == "PASS":
                    record["result"] = "OTHER_FAILURE"
                    record["status"] = "OTHER_FAILURE"
                    record["error_summary"] = "synthetic output remained active after final cleanup sweep"
                    record["reason"] = record["error_summary"]
        summary["finishedAtUnix"] = time.time()
        counts = Counter(item.get("result") for item in summary["items"])
        summary["counts"] = {name: counts.get(name, 0) for name in RESULTS}

        report_errors: list[str] = []
        try:
            atomic_csv(args.csv, summary["items"])
        except BaseException as exc:
            report_errors.append(f"CSV write failed: {exc}")
        if report_errors:
            summary["reportErrors"] = report_errors
        try:
            atomic_json(args.summary, summary)
        except BaseException as exc:
            json_write_error = f"JSON write failed: {exc}"
            report_errors.append(json_write_error)
            print("scene batch verifier report failure: " + "; ".join(report_errors), file=sys.stderr)

    if json_write_error:
        return 2
    if summary.get("reportErrors"):
        print("scene batch verifier report failure: " + "; ".join(summary["reportErrors"]), file=sys.stderr)
        return 2
    if "fatalError" in summary:
        print(f"scene batch verifier failed: {summary['fatalError']}", file=sys.stderr)
        return fatal_exit or 2
    print(
        json.dumps(
            {
                "summary": str(args.summary),
                "csv": str(args.csv),
                "logsDir": str(args.logs_dir),
                "counts": summary["counts"],
            },
            ensure_ascii=False,
        )
    )
    return 0 if all(item.get("result") == "PASS" for item in summary["items"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Queue a Wallpaper Engine workshop item in the running Steam client's download manager."""
from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from ctypes import (
    POINTER,
    c_bool,
    c_char_p,
    c_int,
    c_uint32,
    c_uint64,
    c_void_p,
    create_string_buffer,
)

APP_ID = "431960"
SUBSCRIBED = 1
INSTALLED = 4
NEEDS_UPDATE = 8
DOWNLOADING = 16
PENDING = 32


def steam_root() -> str:
    home = os.path.expanduser("~")
    for path in (
        os.path.join(home, ".local/share/Steam"),
        os.path.join(home, ".steam/steam"),
    ):
        if os.path.isdir(path):
            return path
    return os.path.join(home, ".local/share/Steam")


def load_api():
    root = steam_root()
    linux64 = os.path.join(root, "linux64")
    api_path = os.path.join(root, "steamrt64", "libsteam_api.so")
    if not os.path.isfile(api_path):
        raise SystemExit("no steam_api")
    os.environ["LD_LIBRARY_PATH"] = linux64 + ":" + os.environ.get("LD_LIBRARY_PATH", "")
    ctypes.CDLL(os.path.join(linux64, "steamclient.so"), mode=ctypes.RTLD_GLOBAL)
    return ctypes.CDLL(api_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--id", required=True)
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()
    pub_id = int(args.id)

    work = os.path.join(
        os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")),
        "anispaper",
        "steam-ugc",
    )
    os.makedirs(work, exist_ok=True)
    os.chdir(work)
    with open("steam_appid.txt", "w", encoding="ascii") as fh:
        fh.write(APP_ID + "\n")
    os.environ["SteamAppId"] = APP_ID
    os.environ["SteamGameId"] = APP_ID

    api = load_api()
    api.SteamAPI_IsSteamRunning.restype = c_bool
    if not api.SteamAPI_IsSteamRunning():
        print("FAIL steam not running", flush=True)
        return 2

    err = create_string_buffer(1024)
    api.SteamAPI_InitFlat.argtypes = [c_char_p]
    api.SteamAPI_InitFlat.restype = c_int
    rc = api.SteamAPI_InitFlat(err)
    if rc != 0:
        print(f"FAIL init {rc} {err.value[:120]!r}", flush=True)
        return 3

    api.SteamAPI_SteamUGC_v021.restype = c_void_p
    ugc = api.SteamAPI_SteamUGC_v021()
    if not ugc:
        print("FAIL no ugc", flush=True)
        return 4

    pub = c_uint64(pub_id)
    api.SteamAPI_ISteamUGC_SubscribeItem.argtypes = [c_void_p, c_uint64]
    api.SteamAPI_ISteamUGC_SubscribeItem.restype = c_uint64
    api.SteamAPI_ISteamUGC_DownloadItem.argtypes = [c_void_p, c_uint64, c_bool]
    api.SteamAPI_ISteamUGC_DownloadItem.restype = c_bool
    api.SteamAPI_ISteamUGC_GetItemState.argtypes = [c_void_p, c_uint64]
    api.SteamAPI_ISteamUGC_GetItemState.restype = c_uint32
    api.SteamAPI_ISteamUGC_GetItemDownloadInfo.argtypes = [
        c_void_p,
        c_uint64,
        POINTER(c_uint64),
        POINTER(c_uint64),
    ]
    api.SteamAPI_ISteamUGC_GetItemDownloadInfo.restype = c_bool
    api.SteamAPI_RunCallbacks.restype = None

    api.SteamAPI_ISteamUGC_SubscribeItem(ugc, pub)
    queued = api.SteamAPI_ISteamUGC_DownloadItem(ugc, pub, True)
    print(f"QUEUED {int(queued)}", flush=True)

    deadline = time.time() + max(30, args.timeout)
    last = ""
    while time.time() < deadline:
        api.SteamAPI_RunCallbacks()
        state = api.SteamAPI_ISteamUGC_GetItemState(ugc, pub)
        downloaded = c_uint64(0)
        total = c_uint64(0)
        api.SteamAPI_ISteamUGC_GetItemDownloadInfo(ugc, pub, downloaded, total)
        line = f"STATE {state} BYTES {downloaded.value} {total.value}"
        if line != last:
            print(line, flush=True)
            last = line
        installed = bool(state & INSTALLED) and not (state & NEEDS_UPDATE)
        if installed:
            print("OK", flush=True)
            return 0
        time.sleep(0.4)

    print("FAIL timeout", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())

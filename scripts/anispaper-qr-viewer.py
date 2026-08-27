#!/usr/bin/env python3
"""Always-on-top Steam QR viewer. Reloads steam-qr.png when it changes."""
from __future__ import annotations

import sys
import time
from pathlib import Path

png = Path(sys.argv[1] if len(sys.argv) > 1 else "").expanduser()
if not png:
    sys.exit(1)

try:
    import tkinter as tk
    from PIL import Image, ImageTk
except Exception:
    sys.exit(0)


def main() -> None:
    root = tk.Tk()
    root.title("AnisPaper · Steam QR")
    root.attributes("-topmost", True)
    root.resizable(False, False)
    hint = tk.Label(
        root,
        text="Escaneá con Steam Mobile · cuenta de Wallpaper Engine",
        font=("sans-serif", 11),
        pady=8,
    )
    hint.pack()
    lbl = tk.Label(root, bg="white")
    lbl.pack(padx=12, pady=12)
    state: dict = {"mtime": None, "photo": None}

    def tick() -> None:
        try:
            if png.is_file():
                m = png.stat().st_mtime
                if m != state["mtime"] and png.stat().st_size > 200:
                    img = Image.open(png)
                    img.thumbnail((420, 420))
                    state["photo"] = ImageTk.PhotoImage(img)
                    lbl.configure(image=state["photo"])
                    state["mtime"] = m
        except Exception:
            pass
        root.after(700, tick)

    tick()
    root.mainloop()


if __name__ == "__main__":
    main()
    # keep process around if tk exits immediately
    time.sleep(0)

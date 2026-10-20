"""Headless Chrome screenshots. Each call starts one browser that exits when done."""
import os
import subprocess

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def shot(html, png, w, h, scale=1, budget=4000):
    html, png = os.path.abspath(html), os.path.abspath(png)
    if os.path.exists(png):
        os.remove(png)
    subprocess.run([
        CHROME, "--headless=new", "--hide-scrollbars", "--mute-audio", "--disable-gpu",
        "--no-first-run", "--no-default-browser-check",
        f"--force-device-scale-factor={scale}", f"--virtual-time-budget={budget}",
        f"--screenshot={png}", f"--window-size={w},{h}", "file://" + html,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    ok = os.path.exists(png) and os.path.getsize(png) > 0
    print(("ok " if ok else "FAILED ") + png)
    return ok

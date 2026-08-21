#!/usr/bin/env python3
"""Bridge between KWin and the EGSS wallpaper.

    ./egss.py windows        (or run this directly)

KWin scripts can see every window -- which on a Wayland session is the only way,
since the X11 client list holds XWayland clients and nothing else. What they
cannot do is open a file or a socket. The one way out of the sandbox is
`callDBus`, so this owns a D-Bus name for the script to call, and writes what
arrives to a file the wallpaper polls.

A file rather than a socket because the wallpaper is a game loop: it wants to
read the current state when it happens to look, not to service a connection.
Written to XDG_RUNTIME_DIR and replaced by an atomic rename, so a reader never
sees half a list.

Stop it with Ctrl-C; the KWin script is unloaded on the way out.
"""

import os
import signal
import subprocess
import sys
import tempfile

import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

ROOT = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(ROOT, "kwin", "egss-windows.js")
SCRIPT_NAME = "egss-windows"

BUS_NAME = "org.egss.Wallpaper"
OBJECT_PATH = "/Windows"
INTERFACE = "org.egss.Windows"


def state_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
    return os.path.join(runtime, "egss-windows")


class Windows(dbus.service.Object):
    """Receives the geometry the KWin script sends and writes it out."""

    def __init__(self, bus, path):
        super().__init__(bus, path)
        self.path = state_path()
        self.last = None

    @dbus.service.method(INTERFACE, in_signature="s", out_signature="")
    def Update(self, payload):
        payload = str(payload)

        # A drag fires frameGeometryChanged continuously and most of those
        # arrive with the geometry unchanged; writing only on a real change
        # keeps the wallpaper's mtime check meaningful.
        if payload == self.last:
            return

        self.last = payload

        # Atomic: the reader either sees the old list or the new one.
        directory = os.path.dirname(self.path)
        handle, temporary = tempfile.mkstemp(dir=directory)

        with os.fdopen(handle, "w") as out:
            out.write(payload)

        os.replace(temporary, self.path)


def kwin(method, *arguments):
    """Call a method on KWin's scripting interface."""
    command = ["busctl", "--user", "call", "org.kde.KWin", "/Scripting",
               "org.kde.kwin.Scripting", method]
    command += list(arguments)

    return subprocess.run(command, capture_output=True, text=True)


def load_script():
    # Unloaded first: loading the same script twice leaves two copies
    # connected to every window's signals, and the reports double up.
    kwin("unloadScript", "s", SCRIPT_NAME)

    result = kwin("loadScript", "ss", SCRIPT, SCRIPT_NAME)

    if result.returncode != 0:
        sys.exit("Could not load the KWin script:\n" + result.stderr.strip())

    # loadScript returns the script's id; it does not start it.
    kwin("start")

    return result.stdout.strip()


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

    bus = dbus.SessionBus()

    # Claimed before the script is loaded, or its first report lands on nobody.
    name = dbus.service.BusName(BUS_NAME, bus, do_not_queue=True)
    windows = Windows(bus, OBJECT_PATH)

    print(f"Listening on {BUS_NAME}{OBJECT_PATH}")
    print(f"Writing      {windows.path}")

    identifier = load_script()
    print(f"KWin script  {SCRIPT_NAME} ({identifier})")
    print("\nMove a window to see it update. Ctrl-C to stop.\n")

    loop = GLib.MainLoop()

    def stop(*_):
        loop.quit()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    try:
        loop.run()
    finally:
        kwin("unloadScript", "s", SCRIPT_NAME)

        if os.path.exists(windows.path):
            os.remove(windows.path)

        print("Stopped; KWin script unloaded.")


if __name__ == "__main__":
    main()

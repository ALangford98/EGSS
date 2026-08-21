// Reports the geometry of every ordinary window to the EGSS wallpaper.
//
// KWin scripts run inside the compositor and are the only way to see Wayland
// windows: on a Wayland session the X11 client list holds XWayland clients and
// nothing else, which on this desk is two windows out of twenty.
//
// They also cannot open files or sockets, so the way out is `callDBus` to a
// service somebody else owns -- `tools/egss-windows.py`, which writes what
// arrives here to a file the wallpaper reads.
//
// **Geometry here is logical, not physical.** KWin works in scaled coordinates:
// a 3840x2160 monitor at scale 1.5 is 2560x1440 to a script, while the
// wallpaper window is an XWayland client living in physical pixels. So each
// window is reported with the output it is on and that output's own logical
// rectangle and scale, and the mapping back to physical pixels happens in the
// wallpaper, which knows the physical layout from XRandR.

function describe(window) {
    if (!window.normalWindow) return null;      // skip docks, the desktop, menus
    if (window.minimized || window.hidden) return null;
    if (window.skipTaskbar) return null;
    if (!window.output) return null;

    var geometry = window.frameGeometry;
    var output = window.output.geometry;

    // window rect | output rect | output name.
    //
    // No scale field: `window.output.scale` is not exposed by KWin 6's script
    // API and came out empty, and the reader derives the same number anyway
    // from this output's logical size against its physical one.
    return [Math.round(geometry.x), Math.round(geometry.y),
            Math.round(geometry.width), Math.round(geometry.height),
            Math.round(output.x), Math.round(output.y),
            Math.round(output.width), Math.round(output.height),
            window.output.name].join(",");
}

function report() {
    var windows = workspace.windowList();
    var lines = [];

    for (var i = 0; i < windows.length; i++) {
        var described = describe(windows[i]);
        if (described) lines.push(described);
    }

    callDBus("org.egss.Wallpaper", "/Windows", "org.egss.Windows",
             "Update", lines.join(";"));
}

function watch(window) {
    // Every signal that can move or hide a window. Moving one is the case the
    // wallpaper exists to react to, so frameGeometryChanged is the important
    // one -- it fires continuously through a drag.
    window.frameGeometryChanged.connect(report);
    window.minimizedChanged.connect(report);
    window.outputChanged.connect(report);
    window.desktopsChanged.connect(report);
}

var existing = workspace.windowList();
for (var i = 0; i < existing.length; i++)
    watch(existing[i]);

workspace.windowAdded.connect(function (window) { watch(window); report(); });
workspace.windowRemoved.connect(report);
workspace.currentDesktopChanged.connect(report);

report();

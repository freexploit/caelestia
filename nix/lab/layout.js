var ids = activities();
var baseline = "11111111-1111-4111-8111-111111111111";
var target = "22222222-2222-4222-8222-222222222222";
if (ids.indexOf(baseline) < 0 || ids.indexOf(target) < 0)
    throw new Error("Sanitized lab Activity identities are missing");
if (panels().length === 0) {
    for (var screen = 0; screen < screenCount; screen++) {
        var panel = new Panel;
        panel.screen = screen;
        panel.location = "top";
        panel.height = screen === 0 ? 36 : 32;
        panel.hiding = "none";
        panel.addWidget("org.kde.plasma.kickoff");
        panel.addWidget("org.kde.plasma.icontasks");
        panel.addWidget("org.kde.plasma.panelspacer");
        panel.addWidget("org.kde.plasma.digitalclock");
        var colorizer = panel.addWidget("luisbocanegra.panel.colorizer");
        colorizer.currentConfigGroup = ["General"];
        colorizer.writeConfig("enableDBusService", true);
        colorizer.writeConfig("presetAutoloading", '{"enabled":false}');
    }
}
var baselineDesktops = desktopsForActivity(baseline);
if (baselineDesktops.length === 0)
    throw new Error("No baseline desktops were created");
var desktop = baselineDesktops[0];
if (!desktop.widgets().some(function(w) { return w.type === "org.caelestia.notification-presenter"; }))
    desktop.addWidget("org.caelestia.notification-presenter");

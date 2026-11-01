{ inputs, package, tracker, modules }:
{ config, lib, pkgs, ... }:
let
  python = pkgs.python3.withPackages (p: [ p.dbus-python p.pygobject3 p.pyside6 ]);
  helpers = pkgs.runCommand "caelestia-lab-helpers" { nativeBuildInputs = [ pkgs.makeWrapper ]; } ''
    mkdir -p $out/bin $out/lib/caelestia-lab
    cp ${./observe.py} $out/lib/caelestia-lab/observe.py
    cp ${./clients.py} $out/lib/caelestia-lab/clients.py
    cp ${./suite.py} $out/lib/caelestia-lab/suite.py
    makeWrapper ${python}/bin/python $out/bin/lab-observe --add-flags $out/lib/caelestia-lab/observe.py
    makeWrapper ${python}/bin/python $out/bin/lab-client --add-flags $out/lib/caelestia-lab/clients.py
    makeWrapper ${python}/bin/python $out/bin/lab-suite --add-flags $out/lib/caelestia-lab/suite.py
  '';
  fixture = import ./fixture.nix;
in {
  imports = [ inputs.home-manager.nixosModules.home-manager ];
  system.stateVersion = "26.05";
  networking.hostName = "caelestia-activity-lab";
  networking.firewall.enable = true;
  services.desktopManager.plasma6.enable = true;
  services.displayManager.sddm = { enable = true; wayland.enable = true; };
  services.displayManager.defaultSession = "plasma";
  services.displayManager.autoLogin = { enable = true; user = "lab"; };
  services.displayManager.sddm.autoLogin.relogin = true;
  services.dbus.enable = true;
  services.pipewire = { enable = true; pulse.enable = true; };
  security.rtkit.enable = true;
  hardware.graphics.enable = true;
  users.users.lab = {
    isNormalUser = true;
    uid = 1000;
    password = "lab";
    extraGroups = [ "video" "audio" ];
  };
  environment.systemPackages = [
    package tracker helpers python pkgs.plasma-panel-colorizer
    pkgs.kdePackages.konsole pkgs.kdePackages.kscreen pkgs.kdePackages.spectacle
    pkgs.kdePackages.qttools pkgs.kdePackages.kconfig pkgs.kdePackages.kwin
    pkgs.quickshell pkgs.jq pkgs.dbus pkgs.wayland-utils pkgs.util-linux
  ];
  environment.sessionVariables = {
    QT_PLUGIN_PATH = "${tracker}/lib/qt-6/plugins";
    LIBGL_ALWAYS_SOFTWARE = "1";
    CAELESTIA_SHELL_CONFIG = "${package}/share/caelestia/shell/shell.qml";
  };
  home-manager = {
    useGlobalPkgs = true;
    useUserPackages = true;
    users.lab = { lib, ... }: {
      imports = modules;
      home.stateVersion = "26.05";
      programs.caelestia-kde = {
        enable = true;
        inherit package;
        autostart = false;
        settings.general.apps.terminal = [ "konsole" ];
        activities = {
          enable = true;
          baselineActivity = fixture.baseline;
          activity = fixture.caelestia;
        };
        baseline = {
          enable = true;
          packages = [ pkgs.plasma-panel-colorizer ];
          restoreLayout = ./layout.js;
          settings = {
            kwin.virtualDesktops = {
              number = 9;
              rows = 3;
              names = map (n: "Lab ${toString n}") (lib.range 1 9);
            };
            configFile = {
              kactivitymanagerdrc = {
                activities = { "${fixture.baseline}" = "Lab Baseline"; "${fixture.caelestia}" = "Lab Caelestia"; };
                activities-descriptions = { "${fixture.baseline}" = "Sanitized baseline"; "${fixture.caelestia}" = "Managed shell"; };
              };
              kwinrc = {
                ModifierOnlyShortcuts.Meta = "org.kde.plasmashell,/PlasmaShell,org.kde.PlasmaShell,activateLauncherMenu";
                Plugins.caelestia_workspace_trackerEnabled = true;
                "Script-krohnkite".LabPreservationSentinel = "must-survive";
              };
              kdeglobals.General.ColorScheme = "BreezeDark";
              kscreenlockerrc.Daemon.Autolock = false;
              plasmanotifyrc.Notifications = { PopupPosition = "TopRight"; LowPriorityPopups = true; };
              plasmashellrc.PlasmaShell.containmentCreationDelay = 0;
            };
            shortcuts.kwin."Window Close" = [ "Alt+F4" ];
            shortcuts.plasmashell."activate application launcher" = [ "Meta+Space" "Alt+F1" ];
          };
        };
      };
      home.activation.labInitialActivity = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        mkdir -p "$HOME/.local/state"
        if [ ! -e "$HOME/.local/state/kactivitymanagerdstaterc" ]; then
          printf '[main]\ncurrentActivity=${fixture.baseline}\n' > "$HOME/.local/state/kactivitymanagerdstaterc"
        fi
      '';
      systemd.user.services.caelestia-activities.Unit.After = [ "plasma-plasmashell.service" ];
    };
  };
  environment.etc."caelestia-lab/fixture.json".text = builtins.toJSON fixture;
  environment.etc."caelestia-lab/environment.json".text = builtins.toJSON {
    nixpkgs = inputs.nixpkgs.rev or "locked";
    plasma = pkgs.kdePackages.plasma-workspace.version;
    kwin = pkgs.kdePackages.kwin.version;
    colorizer = pkgs.plasma-panel-colorizer.version;
    graphics = "QEMU virtio-gpu-pci max_outputs=2, software composition; physical GPU/docking/performance unverified";
  };
  virtualisation = {
    memorySize = 6144;
    cores = 4;
    diskSize = 16384;
    useNixStoreImage = true;
    mountHostNixStore = false;
    useHostCerts = false;
    restrictNetwork = true;
    sharedDirectories = lib.mkForce { };
    qemu = {
      networkingOptions = lib.mkForce [ "-nic none" ];
      options = [ "-vga none" "-device virtio-gpu-pci,id=labgpu,max_outputs=2" ];
    };
  };
}

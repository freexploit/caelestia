{ trackerPackage }:
{ config, lib, ... }:
let cfg = config.programs.caelestia-kde.workspace-tracker;
in {
  options.programs.caelestia-kde.workspace-tracker = {
    enable = lib.mkEnableOption "KWin discovery of the matching Caelestia workspace tracker";
    package = lib.mkOption {
      type = lib.types.package;
      default = trackerPackage;
      description = "Tracker built against the same KWin as the system Plasma session.";
    };
  };
  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
    environment.sessionVariables.QT_PLUGIN_PATH = [ "${cfg.package}/lib/qt-6/plugins" ];
  };
}

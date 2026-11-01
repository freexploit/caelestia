{ defaultModule, presenterPackage }:
{ config, lib, pkgs, ... }:
let
  shell = config.programs.caelestia-kde;
  cfg = shell.activities;
  json = pkgs.formats.json { };
  coordinatorConfig = json.generate "caelestia-activities.json" {
    inherit (cfg) baselineActivity activity createActivity trackerRequired startupTimeout;
    shellCommand = [ (toString shell.launcher) ];
  };
  command = "${shell.package}/bin/caelestia-activities";
in {
  imports = [ defaultModule ];
  options.programs.caelestia-kde.activities = {
    enable = lib.mkEnableOption "Activity-scoped Caelestia coexistence";
    baselineActivity = lib.mkOption {
      type = lib.types.str;
      description = "UUID of the existing Plasma Activity; never recreated or deleted.";
    };
    activity = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Existing Caelestia Activity UUID, mutually exclusive with createActivity.";
    };
    createActivity = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = "Explicit name for a create-once Activity; its resolved UUID is persisted.";
    };
    trackerRequired = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Require discovery and verified loading of the matching KWin tracker.";
    };
    startupTimeout = lib.mkOption {
      type = lib.types.ints.positive;
      default = 30;
      description = "Seconds allowed for acknowledged shell startup before recovery.";
    };
  };
  config = lib.mkIf cfg.enable {
    assertions = [
      { assertion = shell.enable; message = "Enable programs.caelestia-kde for Activity integration."; }
      { assertion = (cfg.activity != null) != (cfg.createActivity != null);
        message = "Set exactly one of activities.activity or activities.createActivity."; }
      { assertion = cfg.activity == null || cfg.activity != cfg.baselineActivity;
        message = "Baseline and Caelestia must use distinct Activity UUIDs."; }
    ];
    programs.caelestia-kde.activityManaged = true;
    home.packages = [ pkgs.plasma-panel-colorizer presenterPackage ];
    home.sessionVariables.CAELESTIA_ACTIVITY_MANAGED = "1";
    systemd.user.services.caelestia-activities = {
      Unit = {
        Description = "Caelestia Activity coexistence coordinator";
        PartOf = [ "graphical-session.target" ];
        After = [ "graphical-session.target" "plasma-plasmashell.service" "plasma-kactivitymanagerd.service" ];
      };
      Service = {
        Type = "exec";
        ExecStart = "${command} --config ${coordinatorConfig}";
        ExecStopPost = "${command} --recover --config ${coordinatorConfig}";
        Environment = [ "CAELESTIA_ACTIVITY_MANAGED=1" ];
        Restart = "no";
        TimeoutStopSec = 20;
        KillMode = "mixed";
        Slice = "session.slice";
      };
      Install.WantedBy = [ "graphical-session.target" ];
    };
  };
}

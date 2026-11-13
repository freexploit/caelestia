{ package }:
{ config, lib, pkgs, ... }:
let
  cfg = config.programs.caelestia-kde;
  json = pkgs.formats.json { };
  defaults = json.generate "caelestia-defaults.json" cfg.settings;
  tokens = json.generate "caelestia-token-defaults.json" cfg.tokens;
  launcher = pkgs.writeShellScript "caelestia-managed-launch" ''
    export CAELESTIA_DEFAULTS_FILE=${defaults}
    export CAELESTIA_TOKENS_DEFAULTS_FILE=${tokens}
    exec ${cfg.package}/bin/caelestia-shell
  '';
in {
  options.programs.caelestia-kde = {
    enable = lib.mkEnableOption "the managed Caelestia Plasma shell";
    package = lib.mkOption {
      type = lib.types.package;
      default = package;
      description = "Caelestia package built with the target session's Qt set.";
    };
    settings = lib.mkOption {
      type = json.type;
      default = { };
      description = "Declarative shell defaults beneath writable Nexus overrides.";
    };
    tokens = lib.mkOption {
      type = json.type;
      default = { };
      description = "Declarative token defaults beneath writable overrides.";
    };
    autostart = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Start the standalone managed shell on login. Leave false for Activity coordination.";
    };
    launcher = lib.mkOption {
      type = lib.types.path;
      readOnly = true;
      default = launcher;
      internal = true;
    };
    activityManaged = lib.mkOption {
      type = lib.types.bool;
      default = false;
      internal = true;
      description = "Whether the Activity coordinator owns shell startup.";
    };
  };
  config = lib.mkIf cfg.enable {
    assertions = [{
      assertion = !(cfg.autostart && cfg.activityManaged);
      message = "Caelestia standalone autostart and Activity coordination cannot both own startup.";
    }];
    home.packages = [ cfg.package ];
    fonts.fontconfig.enable = true;
    systemd.user.services.caelestia-shell = {
      Unit = {
        Description = "Managed Caelestia Plasma shell";
        PartOf = [ "graphical-session.target" ];
        After = [ "graphical-session.target" ];
      };
      Service = {
        Type = "exec";
        ExecStart = launcher;
        Restart = "no";
        TimeoutStopSec = 10;
        KillMode = "control-group";
        Slice = "session.slice";
      };
      Install.WantedBy = lib.optional cfg.autostart "graphical-session.target";
    };
  };
}

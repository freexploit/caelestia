{ plasmaModule }:
{ config, lib, ... }:
let cfg = config.programs.caelestia-kde.baseline;
in {
  imports = [ plasmaModule ];
  options.programs.caelestia-kde.baseline = {
    enable = lib.mkEnableOption "reviewed Plasma baseline configuration";
    settings = lib.mkOption {
      type = lib.types.attrs;
      default = { };
      description = "Reviewed programs.plasma configuration, excluding automatic panel rebuilding.";
    };
    packages = lib.mkOption {
      type = lib.types.listOf lib.types.package;
      default = [ ];
      description = "Pinned widgets, themes, fonts, and other reviewed baseline dependencies.";
    };
    restoreLayout = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = "Explicit reviewed Plasma layout JavaScript to restore at login; validate in the lab before live use.";
    };
  };
  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = !(cfg.settings.overrideConfig or false);
        message = "Preservation mode does not permit blanket plasma-manager overrideConfig resets.";
      }
      {
        assertion = (cfg.settings.panels or [ ]) == [ ];
        message = "Use the explicit reviewed restoreLayout boundary rather than plasma-manager's destructive panel rebuilding.";
      }
      {
        assertion = (cfg.settings.resetFiles or [ ]) == [ ];
        message = "Preservation mode does not permit explicit KDE file resets.";
      }
    ];
    home.packages = cfg.packages;
    programs.plasma = lib.mkMerge [
      cfg.settings
      { enable = true; overrideConfig = lib.mkForce false; }
      (lib.mkIf (cfg.restoreLayout != null) {
        startup.desktopScript.caelestia-reviewed-baseline = {
          text = builtins.readFile cfg.restoreLayout;
          runAlways = false;
        };
      })
    ];
  };
}

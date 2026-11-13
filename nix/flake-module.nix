{ inputs, self, ... }:
{
  perSystem = { pkgs, system, ... }:
    let
      matugen = pkgs.callPackage ./packages/matugen.nix { };
      shell = pkgs.callPackage ./packages/caelestia-kde.nix {
        inherit matugen;
        rc2nix = inputs.plasma-manager.packages.${system}.rc2nix;
      };
      tracker = pkgs.callPackage ./packages/workspace-tracker.nix { };
      presenter = pkgs.callPackage ./packages/notification-presenter.nix { };
      lab = import ./lab {
        inherit inputs system tracker;
        package = shell;
        homeModules = self.homeManagerModules;
      };
    in {
      packages = {
        default = shell;
        caelestia-kde = shell;
        workspace-tracker = tracker;
        notification-presenter = presenter;
        inherit matugen;
      };
      apps.activity-lab = lab.app;
      checks.activity-coexistence = lab.test;
      checks.wayland-boot = lab.bootTest;
      packages.activity-lab = lab.vm;
    };
  flake.homeManagerModules = {
    default = { pkgs, ... }: {
      imports = [ (import ./home-manager/default.nix {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.caelestia-kde;
      }) ];
    };
    plasma-baseline = import ./home-manager/plasma-baseline.nix {
      plasmaModule = inputs.plasma-manager.homeModules.plasma-manager;
    };
    activities = { pkgs, ... }: {
      imports = [ (import ./home-manager/activities.nix {
        defaultModule = self.homeManagerModules.default;
        presenterPackage = self.packages.${pkgs.stdenv.hostPlatform.system}.notification-presenter;
      }) ];
    };
  };
  flake.nixosModules.workspace-tracker = { pkgs, ... }: {
    imports = [ (import ./nixos/workspace-tracker.nix {
      trackerPackage = pkgs.callPackage ./packages/workspace-tracker.nix { };
    }) ];
  };
}

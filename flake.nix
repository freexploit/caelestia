{
  description = "Caelestia for Plasma: managed Home Manager integration and an isolated Activity lab";

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts/024633cd702b10285db5cb19b40ad48d2399ba60";
    nixpkgs.url = "github:NixOS/nixpkgs/718f670e845328e9c2cecdd832835712b2cbd065";
    home-manager = {
      url = "github:nix-community/home-manager/24f2190587513b9c2f73db112a17844ae912d965";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    plasma-manager = {
      url = "github:nix-community/plasma-manager/a19a2a029fa180911bd89c554dca1616e10f4c1d";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.home-manager.follows = "home-manager";
    };
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [ ./nix/flake-module.nix ];
      systems = [ "x86_64-linux" "aarch64-linux" ];
    };
}

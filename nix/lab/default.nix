{ inputs, system, package, tracker, homeModules }:
let
  pkgs = import inputs.nixpkgs { inherit system; };
  inherit (pkgs) lib;
  modules = if builtins.isList homeModules then homeModules else [ homeModules.activities homeModules.plasma-baseline ];
  guestModule = import ./guest.nix { inherit inputs package tracker modules; };
  makeTest = strict: pkgs.testers.runNixOSTest {
    name = if strict then "caelestia-activity-coexistence" else "caelestia-wayland-boot";
    nodes.machine = guestModule;
    globalTimeout = 3600;
    testScript = ''
      STRICT = ${if strict then "True" else "False"}
      ${builtins.readFile ./test-script.py}
    '';
  };
  vm = (inputs.nixpkgs.lib.nixosSystem {
    inherit system;
    modules = [
      "${inputs.nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
      guestModule
    ];
  }).config.system.build.vm;
  runner = pkgs.writeShellApplication {
    name = "activity-lab";
    runtimeInputs = [ pkgs.coreutils pkgs.socat ];
    text = ''
      umask 077
      state=$(mktemp -d "''${TMPDIR:-/tmp}/caelestia-activity-lab.XXXXXXXX")
      trap 'rm -rf -- "$state"' EXIT
      export TMPDIR="$state"
      export NIX_DISK_IMAGE="$state/guest.qcow2"
      unset DBUS_SESSION_BUS_ADDRESS SSH_AUTH_SOCK GPG_AGENT_INFO PULSE_SERVER PIPEWIRE_REMOTE
      unset SHARED_DIR QEMU_OPTS QEMU_NET_OPTS QEMU_KERNEL_PARAMS
      monitor="$state/monitor"
      printf 'Disposable guest state: %s\n' "$state"
      printf 'Only virtual graphics are exercised; physical GPU/docking/performance remain unverified.\n'
      printf 'Guest console: lab (password lab); lab-observe ready/state; lab-suite commands are guest-only.\n'
      if [ "$#" -eq 2 ] && [ "$1" = --reviewed-baseline ]; then
        source=$(realpath -- "$2")
        test -f "$source"
        printf 'Importing explicitly reviewed local module after boot, outside public store inputs: %s\n' "$source"
        cp -- "$source" "$state/reviewed-baseline.nix"
        chmod 600 "$state/reviewed-baseline.nix"
        # A read-only virtual FAT disk is local to this run; it is never a public check input.
        mkdir "$state/reviewed"
        cp -- "$source" "$state/reviewed/baseline.nix"
        extra=(-drive "file=fat:ro:$state/reviewed,format=raw,if=virtio,readonly=on")
      elif [ "$#" -eq 0 ]; then
        extra=()
      else
        echo 'Usage: activity-lab [--reviewed-baseline /absolute/reviewed-baseline.nix]' >&2
        exit 2
      fi
      ${vm}/bin/run-caelestia-activity-lab-vm -monitor "unix:$monitor,server=on,wait=off" "''${extra[@]}"
    '';
  };
in {
  inherit vm guestModule;
  test = makeTest true;
  bootTest = makeTest false;
  app = { type = "app"; program = lib.getExe runner; };
}

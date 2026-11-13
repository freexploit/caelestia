{ lib, stdenv, cmake, ninja, pkg-config, makeWrapper, fetchzip,
  kdePackages, qt6, quickshell, matugen, python3, bash, coreutils,
  gnugrep, gnused, findutils, gawk, jq, git, glib, systemd, util-linux,
  ffmpeg, imagemagick, libnotify, wl-clipboard, cliphist, brightnessctl,
  ddcutil, networkmanager, power-profiles-daemon, pciutils, procps,
  libqalculate, pipewire, aubio, libcava, lm_sensors, protobuf, openssl,
  wayland, libx11, vulkan-headers, spirv-tools, sysprof, fftw, libpulseaudio, rc2nix
}:
let
  sources = import ./sources.nix { inherit fetchzip; };
  python = python3.withPackages (ps: [ ps.dbus-next ]);
  runtime = [ bash coreutils gnugrep gnused findutils gawk jq git glib systemd
    util-linux ffmpeg imagemagick libnotify wl-clipboard cliphist brightnessctl
    ddcutil networkmanager power-profiles-daemon pciutils procps python
    matugen quickshell kdePackages.qttools kdePackages.kconfig
    kdePackages.plasma-workspace kdePackages.kscreen kdePackages.spectacle ];
in stdenv.mkDerivation {
  pname = "caelestia-kde";
  version = "2.6.3";
  src = lib.cleanSourceWith {
    src = ../..;
    filter = path: type:
      let name = baseNameOf path; in
      !(builtins.elem name [ ".git" ".codegraph" ".omp" "openspec" "result" "flake.lock" ])
      && !(lib.hasPrefix "result-" name);
  };
  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper spirv-tools
    qt6.qtshadertools qt6.qttools kdePackages.extra-cmake-modules ];
  buildInputs = [ qt6.qtbase qt6.qtdeclarative qt6.qtwayland qt6.qtmultimedia
    qt6.qt5compat qt6.qtsvg qt6.qtimageformats kdePackages.kwindowsystem
    kdePackages.kguiaddons kdePackages.networkmanager-qt kdePackages.pulseaudio-qt
    kdePackages.kglobalaccel kdePackages.kconfig kdePackages.kcoreaddons
    kdePackages.kpipewire libqalculate pipewire aubio libcava lm_sensors
    protobuf openssl wayland libx11 vulkan-headers sysprof fftw libpulseaudio ];
  dontWrapQtApps = true;
  cmakeDir = "../shell";
  cmakeBuildType = "Release";
  cmakeFlags = [
    "-DVERSION=2.6.3" "-DGIT_REVISION=nix-managed" "-DDISTRIBUTOR=Nix"
    "-DINSTALL_BINDIR=bin" "-DINSTALL_LIBDIR=lib/caelestia"
    "-DINSTALL_DATADIR=share/caelestia" "-DINSTALL_QMLDIR=${qt6.qtbase.qtQmlPrefix}"
    "-DINSTALL_QSCONFDIR=share/caelestia/shell" "-DCAELESTIA_OFFLINE=ON"
    "-DCAELESTIA_M3SHAPES_SOURCE_DIR=${sources.m3shapes}" "-DREQUIRE_CAVA=ON"
  ];
  postUnpack = ''
    cp -r ${sources.dots}/. "$sourceRoot/src/dots/"
    chmod -R u+w "$sourceRoot/src/dots"
  '';
  postInstall = ''
    mkdir -p "$out/share/caelestia/shell/assets/icons" "$out/lib/caelestia" "$out/libexec/caelestia"
    cp -r ${sources.icons} "$out/share/caelestia/shell/assets/icons/yet-another-monochrome-icon-set"
    if [ -d "$src/src/lib" ]; then
      cp -r "$src/src/lib/". "$out/lib/caelestia/"
    fi
    makeWrapper ${kdePackages.qttools}/bin/qdbus "$out/libexec/caelestia/qdbus6"
    makeWrapper ${quickshell}/bin/quickshell "$out/bin/caelestia-shell" \
      --set CAELESTIA_MANAGED 1 \
      --set CAELESTIA_BIN_DIR "$out/bin" \
      --set CAELESTIA_LIB_DIR "$out/lib/caelestia" \
      --set CAELESTIA_CAPTURE_LIB "$out/lib/caelestia" \
      --set CAELESTIA_RC2NIX "${rc2nix}/bin/rc2nix" \
      --set CAELESTIA_DATA_DIR "$out/share/caelestia" \
      --set CAELESTIA_SHELL_CONFIG "$out/share/caelestia/shell/shell.qml" \
      --prefix PATH : "$out/bin:$out/libexec/caelestia:${lib.makeBinPath runtime}" \
      --prefix QML2_IMPORT_PATH : "$out/${qt6.qtbase.qtQmlPrefix}:${quickshell}/${qt6.qtbase.qtQmlPrefix}:${kdePackages.kpipewire}/${qt6.qtbase.qtQmlPrefix}:${qt6.qt5compat}/${qt6.qtbase.qtQmlPrefix}:${qt6.qtmultimedia}/${qt6.qtbase.qtQmlPrefix}" \
      --set QS_NO_RELOAD_POPUP 1 --set QS_DISABLE_CRASH_HANDLER 1 \
      --add-flags "-n -p $out/share/caelestia/shell/shell.qml"
    for command in "$out"/bin/caelestia*; do
      [ "$command" = "$out/bin/caelestia-shell" ] && continue
      wrapProgram "$command" \
        --set CAELESTIA_MANAGED 1 \
        --set CAELESTIA_INSTALL_KIND package \
        --set CAELESTIA_BIN_DIR "$out/bin" \
        --set CAELESTIA_LIB_DIR "$out/lib/caelestia" \
        --set CAELESTIA_DATA_DIR "$out/share/caelestia" \
        --set CAELESTIA_SHELL_CONFIG "$out/share/caelestia/shell/shell.qml" \
        --set CAELESTIA_CAPTURE_LIB "$out/lib/caelestia" \
        --set CAELESTIA_RC2NIX "${rc2nix}/bin/rc2nix" \
        --prefix QML2_IMPORT_PATH : "$out/${qt6.qtbase.qtQmlPrefix}" \
        --prefix PATH : "$out/bin:$out/libexec/caelestia:${lib.makeBinPath runtime}"
    done
  '';
  meta = {
    description = "Caelestia shell for Plasma with a non-invasive managed runtime";
    homepage = "https://github.com/ladybug-me/caelestia-kde";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "caelestia";
  };
}

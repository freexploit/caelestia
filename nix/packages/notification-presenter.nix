{ lib, stdenv, cmake, ninja, kdePackages }:
stdenv.mkDerivation {
  pname = "caelestia-notification-presenter";
  version = "1.0.0-plasma-${kdePackages.plasma-workspace.version}";
  src = ../../plasma/notification-presenter;
  nativeBuildInputs = [ cmake ninja kdePackages.extra-cmake-modules ];
  buildInputs = with kdePackages; [ qtbase qtdeclarative libplasma plasma-workspace kconfig kcoreaddons ki18n ];
  dontWrapQtApps = true;
  meta = {
    description = "Scoped Plasma notification presentation companion for Caelestia";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
  };
}

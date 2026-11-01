{ lib, stdenv, cmake, ninja, kdePackages }:
stdenv.mkDerivation {
  pname = "caelestia-workspace-tracker";
  version = "2.6.3-kwin-${kdePackages.kwin.version}";
  src = ../../shell/kwin-effects/workspace-tracker;
  nativeBuildInputs = [ cmake ninja kdePackages.extra-cmake-modules ];
  buildInputs = with kdePackages; [ qtbase kcoreaddons kwin ];
  dontWrapQtApps = true;
  meta = {
    description = "Caelestia workspace tracker built against the selected KWin";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
  };
}

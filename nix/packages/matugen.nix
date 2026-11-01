{ lib, rustPlatform, fetchzip }:
rustPlatform.buildRustPackage {
  pname = "matugen";
  version = "4.2.0";
  src = fetchzip {
    url = "https://github.com/InioX/matugen/archive/refs/tags/v4.2.0.tar.gz";
    hash = "sha256-VbzUY/0Q44vNYa+HB5qpctSpVsnPB+aYsFsx77Ggc7I=";
  };
  cargoHash = "sha256-jmeyg1HWlRL8bdMhjqUVcd9TR6XtwP5aRGJAx4FYshw=";
  meta = {
    description = "Material You palette generator required by Caelestia";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
    mainProgram = "matugen";
  };
}

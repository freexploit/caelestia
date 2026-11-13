{ fetchzip }:
{
  m3shapes = fetchzip {
    url = "https://github.com/soramanew/m3shapes/archive/bdc327b29f95394a732baf3c9b19658ba23755b6.tar.gz";
    hash = "sha256-kfHyzZaPHgqZML48OA+5JwBOsLdQJ2ci/aGPShvUB4Y=";
  };
  dots = fetchzip {
    url = "https://github.com/caelestia-dots/caelestia/archive/639dd6e55b8d0e95aec12499720fb6c434548022.tar.gz";
    hash = "sha256-2J4lgLSEQotdziRzkWqBY5x/OOCWF4+z2wr8KdCukEo=";
  };
  icons = fetchzip {
    url = "https://bitbucket.org/dirn-typo/yet-another-monochrome-icon-set/get/227359bea788d55d202d9aad5f2daf63fe644cc3.tar.gz";
    hash = "sha256-auY9G706HBivoGJCiUvDKouTnu2AvpJOcPVw5ZFS73c=";
  };
}

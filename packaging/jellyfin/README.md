Run from the repo root on a native ARM64 Docker host with Buildx.

```sh
packaging/jellyfin/build.sh check
packaging/jellyfin/build.sh all
```

The first command checks that the patches apply inside Docker. The second
exports both packages to `dist/jellyfin/`. Use `server` or `ffmpeg` to build
one package. Pass an output directory as the second argument if needed.

- `jellyfin-server_12.4+ubu2604+dmd1_arm64.deb` uses Jellyfin's 12.4 release
  packaging and the pinned `v12.4` server commit. It applies the rotation metadata
  and HLS timestamp patches in `patches/`.
- `jellyfin-ffmpeg8_8.1.3-2-resolute+dmd2_arm64.deb` uses Jellyfin FFmpeg
  8.1.3-2 with the rotation patch and the VAAPI nominal frame rate correction.
- Keep the matching stock `jellyfin-web` package installed. The patches only
  affect the server and FFmpeg.

The Ubuntu image and main source revisions are pinned. Builds still fetch
dependencies from Ubuntu, NuGet, and the URLs in Jellyfin's FFmpeg recipe.
Those dependencies need to be available and can change between builds.
Jellyfin also records build-time modification dates in its generated web asset
manifest, so repeated builds can produce different package checksums.
The timestamp patch preserves HLS timestamps. The FFmpeg patch uses the
measured average for VAAPI encoding when the guessed frame rate is the
input's clock frequency and exceeds 1,000 fps.

The build command exports packages for review. Install them together with
APT when ready:

```sh
sudo apt install ./dist/jellyfin/jellyfin-server_12.4+ubu2604+dmd1_arm64.deb \
  ./dist/jellyfin/jellyfin-ffmpeg8_8.1.3-2-resolute+dmd2_arm64.deb
```

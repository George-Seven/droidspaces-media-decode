#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
case "${1:-all}" in
    all) target=packages ;;
    server|ffmpeg) target=$1 ;;
    check) target=patch-check ;;
    *) printf 'Usage: %s [all|server|ffmpeg|check] [output-directory]\n' "$0" >&2; exit 2 ;;
esac
case "$(docker info --format '{{.Architecture}}')" in
    aarch64|arm64) ;;
    *) printf 'This build requires a native arm64 Docker daemon.\n' >&2; exit 1 ;;
esac
args=(--platform linux/arm64 --target "$target" --file "$root/packaging/jellyfin/Dockerfile")
if [[ $target == patch-check ]]; then
    args+=(--output type=cacheonly)
else
    output=${2:-"$root/dist/jellyfin"}
    mkdir -p -- "$output"
    args+=(--output "type=local,dest=$output")
fi
docker buildx build "${args[@]}" "$root"

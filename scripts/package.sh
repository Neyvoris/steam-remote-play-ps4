#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${OO_PS4_TOOLCHAIN:?Set OO_PS4_TOOLCHAIN to OpenOrbis 0.5.4}"
tools="$sdk/bin/linux"
build="$(cd "${1:-$root/build}" && pwd)"
out="${2:-$root/dist}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

content_id=IV0000-SRPL00001_00-STEAMREMOTEPLAY4
mkdir -p "$stage/sce_sys/about" "$stage/sce_module"
install -m 644 "$root/assets/icon0.png" "$stage/sce_sys/icon0.png"
install -m 644 "$sdk/src/modules/right.sprx" "$stage/sce_sys/about/right.sprx"
install -m 644 "$sdk/src/modules/libc.prx" "$sdk/src/modules/libSceFios2.prx" "$stage/sce_module/"
install -m 644 "$root/LICENSE" "$stage/LICENSE.txt"

"$tools/create-fself" -in="$build/steam-remote-ps4.elf" -out="$build/steam-remote-ps4.oelf" \
    --eboot "$stage/eboot.bin" --paid 0x3800000000000011
sfo="$stage/sce_sys/param.sfo"
"$tools/PkgTool.Core" sfo_new "$sfo"
entry() { "$tools/PkgTool.Core" sfo_setentry "$sfo" "$1" --type "$2" --maxsize "$3" --value "$4"; }
entry APP_TYPE Integer 4 1
entry APP_VER Utf8 8 02.07
entry ATTRIBUTE Integer 4 0
entry CATEGORY Utf8 4 gd
entry CONTENT_ID Utf8 48 "$content_id"
entry DOWNLOAD_DATA_SIZE Integer 4 0
entry SYSTEM_VER Integer 4 0
entry TITLE Utf8 128 "Steam Remote Play PS4"
entry TITLE_ID Utf8 12 SRPL00001
entry VERSION Utf8 8 02.07
(
    cd "$stage"
    "$tools/create-gp4" -out release.gp4 --content-id="$content_id" \
        --files 'eboot.bin sce_module/libc.prx sce_module/libSceFios2.prx sce_sys/about/right.sprx sce_sys/icon0.png sce_sys/param.sfo LICENSE.txt'
    "$tools/PkgTool.Core" pkg_build release.gp4 "$out"
)
mv "$out/$content_id.pkg" "$out/SteamRemotePlay-PS4-0.2.7.pkg"
"$tools/PkgTool.Core" pkg_validate "$out/SteamRemotePlay-PS4-0.2.7.pkg"

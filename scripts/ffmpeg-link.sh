#!/usr/bin/env bash
set -euo pipefail
sdk="${OO_PS4_TOOLCHAIN:?Set OO_PS4_TOOLCHAIN to OpenOrbis 0.5.4}"
args=()
for arg in "$@"; do
    case "$arg" in
        -fuse-ld=*|--target=*|-pie) ;;
        -Wl,*)
            IFS=, read -r -a flags <<< "${arg#-Wl,}"
            args+=("${flags[@]}")
            ;;
        *) args+=("$arg") ;;
    esac
done
exec ld.lld -m elf_x86_64 --eh-frame-hdr --script "$sdk/link.x" "$sdk/lib/crt1.o" \
    -L"$sdk/lib" "${args[@]}" -lc -lkernel

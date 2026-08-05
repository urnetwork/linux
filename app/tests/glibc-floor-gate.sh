#!/usr/bin/env bash
# APPIMAGE.md R9/§10c CI gate: assert the built urnetworkd (and the vendored
# SDK .so) reference no glibc symbol newer than the declared floor.
#
# WHY THIS IS A TEST AND NOT A NOTE. The .deb declares its C runtime floor by
# hand (nfpm does not run dpkg-shlibdeps), so nothing but this check couples
# the declaration to reality. When they disagree the package installs happily
# on an older release and then fails to exec — a runtime failure produced by a
# build-time mistake, which is exactly what a gate is for.
#
#   usage: glibc-floor-gate.sh <floor, e.g. 2.35> <binary> [more binaries...]
#
# Exits 77 (meson "skipped") when it cannot run: not Linux, no readelf, or the
# binary was not built — a macOS dev tree must not fail on this.
set -uo pipefail

floor="${1:?usage: glibc-floor-gate.sh <floor> <binary>...}"
shift

command -v readelf >/dev/null 2>&1 || { echo "skip: no readelf"; exit 77; }

# version compare: is $1 > $2 (dotted)?
newer_than() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

status=0
checked=0
for bin in "$@"; do
    if [ ! -f "${bin}" ]; then
        echo "skip: ${bin} not built"
        continue
    fi
    # ELF magic; a macOS Mach-O dev build is a skip, not a failure
    if [ "$(od -An -t x1 -j 0 -N 4 "${bin}" 2>/dev/null | tr -d ' \n')" != "7f454c46" ]; then
        echo "skip: ${bin} is not an ELF binary"
        continue
    fi
    checked=$((checked + 1))

    max="$(readelf --dyn-syms -W "${bin}" 2>/dev/null \
           | grep -o 'GLIBC_2\.[0-9]\+' | sed 's/GLIBC_//' | sort -u -V | tail -1)"
    maxcxx="$(readelf --dyn-syms -W "${bin}" 2>/dev/null \
              | grep -o 'GLIBCXX_3\.4\(\.[0-9]\+\)\?' | sort -u -V | tail -1)"
    echo "${bin}: max GLIBC_${max:-none}${maxcxx:+, ${maxcxx}}"

    if [ -n "${max}" ] && newer_than "${max}" "${floor}"; then
        status=1
        echo "FAIL: ${bin} references GLIBC_${max}, above the declared floor ${floor}."
        echo "      The package's 'Depends: libc6 (>= ${floor})' would be a LIE:"
        echo "      it installs on older releases and then fails to exec."
        echo "      Offending symbols:"
        readelf --dyn-syms -W "${bin}" \
            | grep -oE '[A-Za-z_][A-Za-z0-9_]*@GLIBC_2\.[0-9]+' | sort -u \
            | while IFS= read -r sym; do
                  symver="${sym#*@GLIBC_}"
                  if newer_than "${symver}" "${floor}"; then echo "        ${sym}"; fi
              done
        echo "      Fix: build on an image whose glibc IS the floor, or raise the"
        echo "      floor in BOTH this gate and the packaging's declared dependency."
    fi
    # libstdc++ should have been linked statically (§10c): flag it, don't fail,
    # since a distro build may legitimately prefer the shared one.
    if [ -n "${maxcxx}" ]; then
        echo "      note: ${bin} still references ${maxcxx} (shared libstdc++);"
        echo "            -static-libstdc++ -static-libgcc removes this ABI axis."
    fi
done

[ "${checked}" -gt 0 ] || { echo "skip: nothing to check"; exit 77; }
exit "${status}"

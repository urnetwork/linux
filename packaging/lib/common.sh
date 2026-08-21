#!/bin/bash
# Shared helpers for the URnetwork Linux packaging build scripts
# (MIGRATION.md workstream B). Sourced, never executed.
#
# The one thing this file really is: the MIGRATION.md installed-path table,
# encoded once. build-deb.sh and build-tarball.sh both assemble their package
# root through assemble_daemon_root() so the .deb and the install.sh tarball
# can never drift apart.
#
# Input contract: the app build is consumed ONLY through the staging tree
# produced by `meson install --destdir <staging>` (workstream A's output).
# Built artifacts (daemon binary, SDK library, app data, gettext catalogs)
# must come from there; static integration files (launcher, unit, desktop
# entry, icons, autostart template, NM/udev marking) come from the canonical
# sources in linux/app/packaging/, which this workstream owns.

# shellcheck disable=SC2034  # consumers use these after sourcing
COMMON_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGING_DIR="$(cd "${COMMON_LIB_DIR}/.." && pwd)"
LINUX_DIR="$(cd "${PACKAGING_DIR}/.." && pwd)"
# The pipeline exports APP_DIR=linux/app (MIGRATION.md invocation table);
# fall back to the in-repo location for manual runs.
APP_PACKAGING_DIR="${APP_DIR:-${LINUX_DIR}/app}/packaging"

log()  { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# elf_arch <file> -> amd64 | arm64 | unknown
# Reads the ELF e_machine field (offset 0x12, little-endian) so build hosts
# without binutils (this macOS build server) can still assert that a payload
# matches the arch being packaged.
elf_arch() {
    local f="$1" magic machine
    magic="$(od -An -t x1 -j 0 -N 4 "$f" 2>/dev/null | tr -d ' \n')"
    if [ "$magic" != "7f454c46" ]; then
        printf 'unknown'
        return 0
    fi
    machine="$(od -An -t x1 -j 18 -N 2 "$f" 2>/dev/null | tr -d ' \n')"
    case "$machine" in
        3e00) printf 'amd64' ;;
        b700) printf 'arm64' ;;
        *)    printf 'unknown' ;;
    esac
}

# _file_mode_octal <file> -> e.g. 644
# GNU stat and BSD stat (this macOS build server) spell it differently, and the
# build must not depend on which one is present.
_file_mode_octal() {
    stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1" 2>/dev/null || printf 'unknown'
}

# require_staged <staging> <relative-path>...
# Fails with one clear, actionable message listing everything missing --
# workstream A may not have wired a given install target yet, and "file not
# found" mid-copy is not an acceptable way to discover that.
require_staged() {
    local staging="$1"; shift
    local missing=() p
    for p in "$@"; do
        [ -e "${staging}/${p}" ] || missing+=("${p}")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        {
            printf 'error: staging tree %s is missing expected files:\n' "${staging}"
            printf '  %s\n' "${missing[@]}"
            printf 'These come from "meson install --destdir <staging>" of linux/app\n'
            printf '(workstream A). See the installed-path table in linux/MIGRATION.md.\n'
        } >&2
        exit 1
    fi
}

# glob_nonempty <description> <glob>...
# Asserts at least one glob expansion exists.
glob_nonempty() {
    local desc="$1"; shift
    local p
    for p in "$@"; do
        [ -e "$p" ] && return 0
    done
    die "staging tree has no ${desc} (looked for: $*) -- see linux/MIGRATION.md"
}

# assemble_daemon_root <staging> <root>
# Builds the daemon package root (shared by .deb and install tarball) per the
# MIGRATION.md installed-path table. <root> must exist and be empty.
assemble_daemon_root() {
    local staging="$1" root="$2"
    [ -d "${staging}" ] || die "staging directory not found: ${staging}"
    [ -d "${root}" ] || die "package root directory not found: ${root}"

    # --- built artifacts: only ever from the meson staging tree -------------
    require_staged "${staging}" \
        usr/lib/urnetwork/urnetworkd \
        usr/lib/urnetwork/libURnetworkSdk.so \
        usr/share/urnetwork/world-110m.json
    glob_nonempty "tray icons (usr/share/urnetwork/icons/*.png)" \
        "${staging}"/usr/share/urnetwork/icons/*.png
    glob_nonempty "gettext catalogs (usr/share/locale/*/LC_MESSAGES/urnetwork.mo)" \
        "${staging}"/usr/share/locale/*/LC_MESSAGES/urnetwork.mo

    install -d "${root}/usr/lib/urnetwork"
    cp "${staging}/usr/lib/urnetwork/urnetworkd" "${root}/usr/lib/urnetwork/urnetworkd"
    cp "${staging}/usr/lib/urnetwork/libURnetworkSdk.so" "${root}/usr/lib/urnetwork/libURnetworkSdk.so"
    chmod 0755 "${root}/usr/lib/urnetwork/urnetworkd"
    chmod 0644 "${root}/usr/lib/urnetwork/libURnetworkSdk.so"

    # Globe land outlines + tray art: the whole app share dir is daemon-pkg
    # owned per the table (the AppImage is never installed system-wide).
    install -d "${root}/usr/share/urnetwork"
    cp -R "${staging}/usr/share/urnetwork/." "${root}/usr/share/urnetwork/"

    # Gettext catalogs: copy exactly the urnetwork domain, nothing else.
    local mo rel
    while IFS= read -r mo; do
        rel="${mo#"${staging}"/}"
        install -d "${root}/$(dirname "${rel}")"
        cp "${mo}" "${root}/${rel}"
    done < <(find "${staging}/usr/share/locale" -type f -name 'urnetwork.mo' 2>/dev/null)

    # --- static integration files: canonical sources in app/packaging -------
    local src="${APP_PACKAGING_DIR}"
    local f
    for f in urnetwork-launcher urnetworkd.service com.bringyour.network.desktop \
             autostart/com.bringyour.network.desktop 95-urnetwork.conf \
             85-urnetwork-unmanaged.rules \
             icons/hicolor/48x48/apps/com.bringyour.network.png \
             icons/hicolor/64x64/apps/com.bringyour.network.png \
             icons/hicolor/128x128/apps/com.bringyour.network.png \
             icons/hicolor/256x256/apps/com.bringyour.network.png \
             icons/hicolor/512x512/apps/com.bringyour.network.png; do
        [ -f "${src}/${f}" ] || die "packaging source missing: ${src}/${f}"
    done

    install -d "${root}/usr/bin"
    cp "${src}/urnetwork-launcher" "${root}/usr/bin/urnetwork"
    chmod 0755 "${root}/usr/bin/urnetwork"

    install -d "${root}/lib/systemd/system"
    cp "${src}/urnetworkd.service" "${root}/lib/systemd/system/urnetworkd.service"

    install -d "${root}/usr/share/applications"
    cp "${src}/com.bringyour.network.desktop" "${root}/usr/share/applications/"

    # All five installed sizes, not just 48 and 256. Shipping only those two is
    # why the shell had to upscale 48 -> 64 and 256 -> 512 for the app grid and
    # the window titlebar, which is exactly the softness that showed up on a
    # HiDPI desktop. app/meson.build installs the same five for the Flatpak;
    # keep the two lists in step.
    local icon_size
    for icon_size in 48 64 128 256 512; do
        install -d "${root}/usr/share/icons/hicolor/${icon_size}x${icon_size}/apps"
        cp "${src}/icons/hicolor/${icon_size}x${icon_size}/apps/com.bringyour.network.png" \
            "${root}/usr/share/icons/hicolor/${icon_size}x${icon_size}/apps/com.bringyour.network.png"
    done

    install -d "${root}/etc/urnetwork/autostart"
    cp "${src}/autostart/com.bringyour.network.desktop" "${root}/etc/urnetwork/autostart/"

    install -d "${root}/etc/NetworkManager/conf.d"
    cp "${src}/95-urnetwork.conf" "${root}/etc/NetworkManager/conf.d/"

    install -d "${root}/etc/udev/rules.d"
    cp "${src}/85-urnetwork-unmanaged.rules" "${root}/etc/udev/rules.d/"

    # polkit action file -- the authority urnetworkd gates its privileged verbs
    # with, and the reason no user has to join a group or log out again.
    #
    # The ONE integration file sourced from linux/packaging/ rather than
    # linux/app/packaging/: its presence on disk IS the daemon's fallback
    # discriminator (ControlProtocol.hpp kPolkitPolicyPath -- present means
    # polkit is the sole authority, absent means the legacy `urnetwork` group
    # check), so it belongs next to install.sh, which is the thing that decides
    # whether a given machine gets it. app/meson.build installs the same file
    # for `meson install --destdir` consumers; this copy is what the .deb and
    # the tarball payload actually ship.
    #
    # 0644 root:root is not decoration: polkit IGNORES a group- or
    # world-writable .policy file, so a wrong mode here does not fail loudly,
    # it silently drops every action back to its built-in default. The mode
    # normalization below sets it; the assertion after it proves it.
    [ -f "${PACKAGING_DIR}/polkit/com.bringyour.network.policy" ] || \
        die "packaging source missing: ${PACKAGING_DIR}/polkit/com.bringyour.network.policy"
    install -d "${root}/usr/share/polkit-1/actions"
    cp "${PACKAGING_DIR}/polkit/com.bringyour.network.policy" \
        "${root}/usr/share/polkit-1/actions/com.bringyour.network.policy"

    # Normalize modes: directories 0755; everything except the two
    # executables 0644 (shared libraries ship 0644 on Debian).
    find "${root}" -type d -exec chmod 0755 {} +
    find "${root}" -type f ! -path '*/usr/bin/*' ! -name 'urnetworkd' -exec chmod 0644 {} +
    chmod 0755 "${root}/usr/bin/urnetwork" "${root}/usr/lib/urnetwork/urnetworkd"

    # Prove the polkit action file is exactly 0644 before it goes into a
    # package. polkit's rejection of a writable action file is SILENT (it logs
    # and skips the file), and the visible symptom would be "every Connect asks
    # for an admin password" long after the build.
    local policy_mode
    policy_mode="$(_file_mode_octal "${root}/usr/share/polkit-1/actions/com.bringyour.network.policy")"
    [ "${policy_mode}" = '644' ] || \
        die "polkit action file is mode ${policy_mode}, must be 644 (polkit ignores a group- or world-writable .policy)"
}

# check_payload_arch <root> <arch>
# Asserts the two ELF payloads match the arch being packaged.
check_payload_arch() {
    local root="$1" want="$2" f got
    for f in usr/lib/urnetwork/urnetworkd usr/lib/urnetwork/libURnetworkSdk.so; do
        got="$(elf_arch "${root}/${f}")"
        if [ "${got}" != "${want}" ]; then
            die "${f} is '${got}' but this build is --arch ${want} -- staging tree and target arch disagree"
        fi
    done
}

# sha256_file <file> -> writes <file>.sha256 next to it (portable macOS/Linux)
sha256_file() {
    local f="$1" dir base
    dir="$(cd "$(dirname "$1")" && pwd)"
    base="$(basename "$1")"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "${dir}" && sha256sum "${base}" > "${base}.sha256")
    else
        (cd "${dir}" && shasum -a 256 "${base}" > "${base}.sha256")
    fi
}

# maybe_sign <file>
# Detached, armored signature when a release key is configured -- the
# fingerprint is published out of band (APPIMAGE.md 11f/11g: a checksum inside
# the artifact proves integrity, never authorship). No key, no signature, and
# say so; never fake one.
maybe_sign() {
    local f="$1"
    if [ -n "${UR_SIGN_KEY:-}" ] && command -v gpg >/dev/null 2>&1; then
        gpg --batch --yes --local-user "${UR_SIGN_KEY}" --armor --detach-sign \
            --output "${f}.asc" "${f}"
        log "signed: ${f}.asc"
    else
        log "not signed (set UR_SIGN_KEY and have gpg on PATH to produce ${f##*/}.asc)"
    fi
}

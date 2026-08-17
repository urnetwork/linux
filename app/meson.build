# URnetwork Linux — TWO binaries since the daemon split (linux/MIGRATION.md):
#
#   urnetwork   the unprivileged GUI (gtkmm4 + libadwaita, urnet::DeviceRemote,
#               control client). Shipped as the user's AppImage, NEVER
#               installed by a package — /usr/bin/urnetwork is the daemon
#               package's launcher script (workstream B), so this target is
#               install:false and the AppImage build takes it from the build
#               dir.
#   urnetworkd  the root daemon (urnet::DeviceLocal + tun + IoLoop + control
#               socket + privileged GeoClue write). NO GUI dependency of any
#               kind — gio/glib, nlohmann_json, threads and the SDK only; the
#               daemon package must install without pulling GTK.
#
# This file is the single source of truth for the installed layout: workstream
# B packages `meson install --destdir` output verbatim, so every path below
# matches MIGRATION.md's table exactly.
# See README.md. SPDX-License-Identifier: MPL-2.0
project('urnetwork', 'cpp',
  version : '0.0.1',
  default_options : ['cpp_std=c++17', 'warning_level=2'])

# GUI toolkit deps are a FEATURE, not a hard requirement: the daemon must be
# buildable on an image that has no GTK4 at all. That is not hypothetical --
# Ubuntu 22.04 does not package libgtkmm-4.0, and 22.04 is exactly where the
# daemon wants to be built (measured: a 22.04 daemon references GLIBC_2.34, a
# 24.04 one GLIBC_2.38, and the .deb declares >= 2.35). With `gui` at its
# default 'auto' a GTK-less image quietly builds the daemon half only;
# -Dgui=enabled makes a missing toolkit a hard error for the AppImage build.
gtkmm   = dependency('gtkmm-4.0', required : get_option('gui'))     # GTK4 C++ bindings (GUI only)
adwaita = dependency('libadwaita-1', required : get_option('gui'))  # libadwaita (C API; GUI only)
build_gui = gtkmm.found() and adwaita.found()

gio     = dependency('gio-2.0')         # raw GDBus C API for the SNI tray (also brings glib/gobject)
json    = dependency('nlohmann_json')
threads = dependency('threads')
# Runtime registration of the four licensed brand faces (UrTheme.cpp):
# FcConfigAppFontAddFile against the default config, before the Pango font
# map first snapshots it. GUI only — the daemon renders no text.
fontconfig = dependency('fontconfig', required : get_option('gui'))

# The release version -> UR_APP_VERSION in both binaries: urnetworkd reports
# it as daemon_version in the control hello (the value the GUI's "daemon out
# of date" state names), the GUI registers devices under it. The pipeline
# passes -Dapp_version=$VERSION; the default is a dev sentinel.
version_args = ['-DUR_APP_VERSION="' + get_option('app_version') + '"']

# Localization (GNOME convention: gettext via glib's i18n). po/ is generated
# from the canonical localization store (localizations/keys/*.yaml, `npm run
# gen`) — msgid is the english source, msgctxt the store key id — and is never
# hand-edited. po/meson.build compiles po/<locale>.po into
# <localedir>/<locale>/LC_MESSAGES/urnetwork.mo and installs it; src/I18n.hpp
# does the lookups; main.cpp binds the domain to the two defines below.
i18n = import('i18n')
gettext_package = 'urnetwork'
# localedir is a builtin option ('share/locale' by default), and the `/` join
# keeps an absolute override intact.
localedir = get_option('prefix') / get_option('localedir')
i18n_args = [
  '-DGETTEXT_PACKAGE="' + gettext_package + '"',
  '-DUR_LOCALEDIR="' + localedir + '"',
]

# Vendored cgo SDK, per arch (fetch-deps.sh unzips URnetworkSdkLinux.zip here).
sdk_arch = get_option('sdk_arch')
sdk_root = meson.project_source_root() / 'third_party' / 'urnetwork-sdk' / sdk_arch
# is_system: the generated urnetwork_sdk.hpp is third-party (a Go->C++ binding), and
# with warning_level=2 GCC 13+ flags its members that share a name with their type
# (e.g. std::optional<BlockOverride> BlockOverride) as -Wchanges-meaning, an error by
# default. A system include suppresses its warnings; also disable the diagnostic
# explicitly where the compiler supports it (MSVC on Windows has no such warning).
sdk_inc  = include_directories('third_party' / 'urnetwork-sdk' / sdk_arch, is_system : true)
cpp = meson.get_compiler('cpp')
# Existence check at configure time (a missing vendored SDK must fail here,
# not with a link error later).
sdk_found = cpp.find_library('URnetworkSdk', dirs : sdk_root)
# ...but LINK it via -L/-l, not by the absolute path find_library hands back.
# The vendored libURnetworkSdk.so carries NO SONAME, so whatever the link line
# names is what lands in DT_NEEDED verbatim: with an absolute path, the
# installed binary asks the loader for the BUILD-machine path, which does not
# exist on the user's machine, and the loader never consults rpath at all --
# the daemon fails to exec with "error while loading shared libraries".
# Measured on Ubuntu 22.04: absolute path -> NEEDED
# "/build/third_party/.../libURnetworkSdk.so"; -L + -l -> NEEDED
# "libURnetworkSdk.so", which then resolves through install_rpath ($ORIGIN for
# the daemon, $ORIGIN/../lib for the GUI in the AppDir).
sdk_lib = declare_dependency(dependencies : sdk_found,
                             link_args : ['-L' + sdk_root, '-lURnetworkSdk'])
sdk_warn_args = cpp.get_supported_arguments('-Wno-changes-meaning', '-Wno-error=changes-meaning')
# GCC 11/12 (Ubuntu 22.04) predate -Wchanges-meaning: there the same construct
# is a HARD ERROR that -isystem does not suppress, so the options probed above
# do not exist and the SDK header will not compile at all. -fpermissive
# downgrades it. Scoped to exactly that case -- never applied to GCC 13+ or
# clang, where the targeted -Wno- option does the job. This is what keeps a
# 22.04 daemon build possible, which is what keeps the glibc floor low
# (measured: 22.04 -> GLIBC_2.34, 24.04 -> GLIBC_2.38; see glibc_floor).
if sdk_warn_args.length() == 0 and cpp.get_id() == 'gcc'
  sdk_warn_args = cpp.get_supported_arguments('-fpermissive')
endif

# Runtime data (currently the provider globe's world-110m.json land outlines).
# src/ProviderGlobe.cpp resolves the asset as $APPDIR + UR_PKGDATADIR first (the
# AppImage runtime sets APPDIR), then UR_PKGDATADIR itself for a normal install,
# then ./assets for running straight out of a build tree.
pkgdatadir = get_option('prefix') / get_option('datadir') / 'urnetwork'
pkgdata_args = ['-DUR_PKGDATADIR="' + pkgdatadir + '"']

# Embedded Stripe checkout (src/UpgradeSheet.cpp): WebKitGTK for GTK4
# (webkitgtk-6.0 — NOT webkit2gtk-4.x, which is the GTK3 series). Deliberately
# a soft dependency: when the dev package is missing the app still builds,
# without the webview, and the upgrade sheet falls back to the hosted checkout
# in the system browser — a payment path must never hard-fail on a library.
# Ubuntu 24.04 ships it as libwebkitgtk-6.0-dev.
webkit = dependency('webkitgtk-6.0', required : false)
webkit_args = []
gui_deps = [gtkmm, adwaita, gio, json, threads, sdk_lib]
if webkit.found() and build_gui
  gui_deps += webkit
  webkit_args = ['-DUR_HAVE_WEBKIT']
endif

# Optional WalletConnect Cloud project id -> src/Config.hpp (kWalletConnectProjectId).
# Empty is valid and is the default: the wallet-connect bridge then drives injected
# (browser extension) wallets only. Keeps the id out of the source tree, the way
# android (local.properties) and apple (Info.plist) do it.
wc_project_id = get_option('walletconnect_project_id')
wc_args = []
if wc_project_id != ''
  wc_args = ['-DUR_WALLETCONNECT_PROJECT_ID="' + wc_project_id + '"']
endif

if build_gui
  gui_deps += fontconfig
endif

# ---- urnetwork: the unprivileged GUI ---------------------------------------
# No Tunnel.cpp any more — the tun/IoLoop moved to urnetworkd; the GUI holds a
# DeviceRemote plus the control client.
#
# INSTALLED AS `urnetwork-gui`, NOT `urnetwork`. Three constraints meet here:
#   * MIGRATION.md reserves /usr/bin/urnetwork for the daemon package's
#     launcher SCRIPT (workstream B owns it), so the binary must not take it;
#   * `urnetwork-gui` on $PATH is the launcher's own last-resort fallback, so
#     the name is already part of the contract;
#   * workstream B's make-appimage.sh stages the GUI from this tree at
#     usr/bin/urnetwork-gui (overridable via UR_GUI_BIN).
# Installing it here is safe for the daemon package: B's assemble_daemon_root
# copies an explicit file list out of the staging tree (never all of
# usr/bin), so the GUI binary cannot leak into the .deb/tarball — "the
# AppImage is never packaged" still holds.
if build_gui
executable('urnetwork-gui',
  sources : [
    'src/main.cpp',
    'src/ControlClient.cpp',
    'src/SdkHost.cpp',
    'src/WalletConnect.cpp',
    'src/MainWindow.cpp',
    'src/UrTheme.cpp',
    'src/PaneKit.cpp',
    'src/HomeShell.cpp',
    'src/UrMotion.cpp',
    'src/LoginCarousel.cpp',
    'src/BrandIcons.cpp',
    'src/SeedphraseSheet.cpp',
    'src/NetworkServerSheet.cpp',
    'src/ConnectCanvas.cpp',
    'src/ConnectPage.cpp',
    'src/SupportPage.cpp',
    'src/NetworkPage.cpp',
    'src/SettingsPage.cpp',
    'src/DeveloperPage.cpp',
    'src/EarningsPage.cpp',
    'src/AccountPage.cpp',
    'src/LogTailClient.cpp',
    'src/Tray.cpp',
    'src/Ui.cpp',
    'src/Formatters.cpp',
    'src/TransferChart.cpp',
    'src/ConnectDrawer.cpp',
    'src/PostQuantumIdentity.cpp',
    'src/ContractsSheet.cpp',
    'src/LocationsSheet.cpp',
    'src/SplitRulesSheet.cpp',
    'src/DnsSheet.cpp',
    'src/SubscriptionBalance.cpp',
    'src/UsageBar.cpp',
    'src/RedeemCodeSheet.cpp',
    'src/UpgradeSheet.cpp',
    'src/AuthViews.cpp',
    'src/GlobeGeometry.cpp',
    'src/WorldTopology.cpp',
    'src/ProviderLocationRow.cpp',
    'src/ProviderGlobe.cpp',
    'src/ProviderLocationsSheet.cpp',
    'src/LocationOverrideState.cpp',
    'src/LocationOverride.cpp',
    'src/LocationOverrideGuide.cpp',
  ],
  include_directories : [sdk_inc],
  dependencies : gui_deps,
  cpp_args : wc_args + i18n_args + webkit_args + sdk_warn_args + pkgdata_args + version_args,
  install : true,
  # AppDir layout: usr/bin/urnetwork-gui next to usr/lib/libURnetworkSdk.so
  install_rpath : '$ORIGIN/../lib')
else
  message('gtkmm-4.0/libadwaita-1 not found: building the daemon half only ' +
          '(pass -Dgui=enabled to require the GUI)')
endif

# ---- urnetworkd: the root daemon -------------------------------------------
# gio/glib + json + threads + the SDK. NO gtkmm, NO libadwaita, NO webkit:
# keep it that way — the daemon package installs on servers/headless systems
# without pulling GTK. LocationOverride.cpp rides along for the privileged
# DirectGeoClueWriter (glib-only); ControlClient.cpp only because that file
# also defines the GUI-side DaemonGeoClueWriter's transport.
#
# Static libstdc++/libgcc (APPIMAGE.md §10c): libstdc++ is the sharper ABI
# edge than glibc — a daemon built on 24.04 otherwise references
# GLIBCXX_3.4.29 and drags a libstdc++6 version floor along with it. Linking
# them statically drops libstdc++.so.6 and libgcc_s.so.1 from NEEDED
# entirely (measured), so only the glibc floor remains to be declared. Probed,
# because a macOS dev build has no such flags.
daemon_link_args = cpp.get_supported_link_arguments('-static-libstdc++', '-static-libgcc')

host_integration = get_option('host_integration')

if host_integration
executable('urnetworkd',
  sources : [
    'src/daemon/main.cpp',
    'src/daemon/ControlServer.cpp',
    'src/daemon/TunnelHost.cpp',
    'src/daemon/DaemonLog.cpp',
    'src/Tunnel.cpp',
    'src/ControlClient.cpp',
    'src/LocationOverride.cpp',
    'src/LocationOverrideState.cpp',
  ],
  include_directories : [include_directories('src'), sdk_inc],
  dependencies : [gio, json, threads, sdk_lib],
  cpp_args : sdk_warn_args + version_args,
  link_args : daemon_link_args,
  install : true,
  install_dir : 'lib' / 'urnetwork',
  install_rpath : '$ORIGIN')

# The vendored SDK, next to the daemon (rpath $ORIGIN above). Meson owns this
# placement — the pipeline's fallback copy should never be the real mechanism.
install_data('third_party' / 'urnetwork-sdk' / sdk_arch / 'libURnetworkSdk.so',
             install_dir : 'lib' / 'urnetwork')
else
# GUI-only (Flatpak): no daemon to sit beside, so the SDK goes to the libdir
# root — which is exactly what the GUI's install_rpath '$ORIGIN/../lib'
# resolves to from bindir. Same file, one directory up.
install_data('third_party' / 'urnetwork-sdk' / sdk_arch / 'libURnetworkSdk.so',
             install_dir : get_option('libdir'))
endif

# ---- installed data (paths are normative — MIGRATION.md's table) -----------
#
# SOURCED FROM packaging/, NOT from a copy under src/. These files are shared
# with the packaging assembly (linux/packaging/lib/common.sh copies static
# integration files straight out of app/packaging/), so a second copy under
# src/dist/ meant meson installed one version while the .deb and the tarball
# shipped another. They had already diverged — the two urnetworkd.service files
# disagreed on Type=notify vs Type=exec and on the network-pre.target ordering
# — so src/dist/ was deleted on 2026-08-05 and packaging/ is the single source.
# Keep it that way: if you add an integration file, add it here only.

# systemd unit: /lib (not /usr/lib/systemd) — in every release's unit load path
if host_integration
install_data('packaging/urnetworkd.service',
             install_dir : '/lib/systemd/system')
endif

# polkit action file: the AUTHORITY the daemon gates its privileged verbs with,
# and the reason there is no longer a group to join or a session to log out of.
#
# SOURCED FROM ../packaging/polkit/, the only integration file that does NOT
# live under app/packaging/. It is owned by the packaging workstream because
# its *presence on disk* is a packaging-level decision, not a build one:
# ControlProtocol.hpp's kPolkitPolicyPath makes the installed file the fallback
# discriminator (present -> polkit is the sole authority; absent -> the legacy
# `urnetwork` group check), so packaging/tarball/install.sh must be free to
# withhold it on a machine that has no polkit at all. Keeping the single source
# next to install.sh and packaging/lib/common.sh is what keeps that decision in
# one place. Referencing it from here still installs it for `meson install
# --destdir` consumers, so the meson tree and the packaged tree agree.
#
# datadir-relative, NOT the absolute '/usr/share/...' the unit above uses:
# polkit 124+ scans /usr/share, /usr/local/share, /etc and /run action
# directories (measured: `strings` on polkit 127's polkitd), so a --prefix
# /usr/local install landing in /usr/local/share/polkit-1/actions is read
# correctly rather than being wrong. 0644 root:root is enforced by packaging;
# polkit ignores a group- or world-writable action file.
#
# host_integration=false (Flatpak) drops it with the rest: the GUI never calls
# polkit — the daemon does, against a subject the GUI cannot influence.
if host_integration
install_data('..' / 'packaging' / 'polkit' / 'network.ur.urnetwork.policy',
             install_dir : get_option('datadir') / 'polkit-1' / 'actions')
endif

# desktop file: the filename MUST match main.cpp's app id network.ur.urnetwork
# (D-Bus activation requires it; APPIMAGE.md §11d)
install_data('packaging/network.ur.urnetwork.desktop',
             install_dir : get_option('datadir') / 'applications')

# AppStream metadata — required by Flathub, read by GNOME Software / Discover
# for the .deb and .rpm too. The component id matches the .desktop basename.
install_data('packaging/network.ur.urnetwork.metainfo.xml',
             install_dir : get_option('datadir') / 'metainfo')

# hicolor icons (Icon=urnetwork in the desktop file)
install_data('packaging/icons/hicolor/48x48/apps/urnetwork.png',
             install_dir : get_option('datadir') / 'icons' / 'hicolor' / '48x48' / 'apps')
install_data('packaging/icons/hicolor/256x256/apps/urnetwork.png',
             install_dir : get_option('datadir') / 'icons' / 'hicolor' / '256x256' / 'apps')

# autostart INERT TEMPLATE: root-owned under /etc/urnetwork/autostart/, where
# no desktop reads it; the GUI symlinks it into ~/.config/autostart atomically
if host_integration
install_data('packaging/autostart/network.ur.urnetwork.desktop',
             install_dir : '/etc/urnetwork/autostart')
endif

# NetworkManager marking + udev belt: NM must never manage urnet0 (§10c — the
# external-down protection ends the moment the daemon brings the link up)
if host_integration
install_data('packaging/95-urnetwork.conf',
             install_dir : '/etc/NetworkManager/conf.d')
install_data('packaging/85-urnetwork-unmanaged.rules',
             install_dir : '/etc/udev/rules.d')
endif

# tray art (the SNI tray resolves these from the pkgdatadir)
install_data('assets/urnetwork-tray-disconnected.png',
             'assets/urnetwork-tray-connected.png',
             'assets/urnetwork-tray-provide.png',
             install_dir : get_option('datadir') / 'urnetwork/icons')

# Natural Earth 110m land outlines (quantized TopoJSON, public domain), decoded
# by src/WorldTopology.cpp for the provider globe. A missing file only costs the
# land layer -- the sphere, graticule and provider dots still draw.
install_data('assets/world-110m.json',
             install_dir : get_option('datadir') / 'urnetwork')

# The crowned-frog referral mascot (Account's royalty badge). Without this
# install the asset resolves ONLY out of a build tree — every packaged build
# and every AppImage would silently render the badge as a bare sentence.
install_data('assets/ReferralFrog.png',
             install_dir : get_option('datadir') / 'urnetwork')

# The four licensed brand faces (SEPARATELY LICENSED commercial fonts — ABC
# Dinamo + Pangram Pangram, NOT MPL; ship deliberately, per the owner's
# licenses). UrTheme.cpp registers them with fontconfig at runtime; the
# internal family names are normative ("PP NeueBit" has no space in NeueBit).
install_data('assets/fonts/abcgravity_extended.otf',
             'assets/fonts/abcgravity_extra_condensed.otf',
             'assets/fonts/pp_neue_bit_bold.ttf',
             'assets/fonts/pp_neue_montreal_regular.ttf',
             install_dir : get_option('datadir') / 'urnetwork/fonts')

# The login carousel's three brand images (LoginCarousel.cpp clips them to the
# ur globe silhouette; a missing file costs the art, the headline still runs).
install_data('assets/carousel/LoginCarousel1.jpg',
             'assets/carousel/LoginCarousel2.jpg',
             'assets/carousel/LoginCarousel3.jpg',
             install_dir : get_option('datadir') / 'urnetwork/carousel')

# ---- unit tests -------------------------------------------------------------
# The pure-logic modules (orthographic projection math, the TopoJSON decoder,
# the provider row/label helpers, the location-override state machine and the
# daemon control protocol: framing, version negotiation in both directions,
# SDK-build agreement, peer-credential authorization) carry no GTK, libadwaita
# or SDK dependency on purpose, so this binary builds and runs anywhere a
# C++17 compiler does -- including a developer machine that cannot build the
# app itself.  meson test -C build
urnetwork_tests = executable('urnetwork-tests',
  sources : [
    'tests/main.cpp',
    'tests/GlobeGeometryTest.cpp',
    'tests/WorldTopologyTest.cpp',
    'tests/ProviderLocationRowTest.cpp',
    'tests/LocationOverrideStateTest.cpp',
    'tests/ControlProtocolTest.cpp',
    'tests/NetworkServerUtilsTest.cpp',
    'tests/HealthTest.cpp',
    'src/GlobeGeometry.cpp',
    'src/WorldTopology.cpp',
    'src/ProviderLocationRow.cpp',
    'src/LocationOverrideState.cpp',
  ],
  include_directories : include_directories('src', 'tests'),
  dependencies : [json],
  cpp_args : ['-DUR_ASSETS_DIR="' + meson.project_source_root() / 'assets' + '"'],
  install : false)
test('unit', urnetwork_tests, protocol : 'exitcode')

# ABI floor gate (APPIMAGE.md R9/§10c): the daemon and the vendored SDK must
# reference no glibc symbol newer than the floor the packaging declares. The
# declaration is hand-written (nfpm does not run dpkg-shlibdeps), so this test
# is the only thing coupling it to the actual binary. Skips (exit 77) on a
# non-Linux dev tree.
test('glibc-floor',
     find_program('tests/glibc-floor-gate.sh'),
     args : [get_option('glibc_floor'),
             meson.current_build_dir() / 'urnetworkd',
             meson.project_source_root() / 'third_party' / 'urnetwork-sdk' / sdk_arch /
               'libURnetworkSdk.so'],
     protocol : 'exitcode')

# builds + installs the message catalogs (and the maintainer pot/update-po
# targets). Must live in po/: i18n.gettext() reads POTFILES.in and LINGUAS from
# the calling subdir.
subdir('po')

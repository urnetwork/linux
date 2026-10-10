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
# Secret Service backing for the GUI-owned RPC credentials (upstream's
# RpcSessionStore/SecretServiceRpcSessionStore). GUI-only, like gtkmm.
libsecret = dependency('libsecret-1', required : get_option('gui'))
# The in-app updater's HTTP client (src/UpdateChecker.cpp): the release list
# and the AppImage download, over GIO's TLS (glib-networking, which the
# AppImage already bundles with the GLib family). GUI-only, like gtkmm --
# the daemon never fetches anything. Ubuntu 24.04 ships it as
# libsoup-3.0-dev; the GNOME 49 Flatpak runtime carries it.
libsoup = dependency('libsoup-3.0', required : get_option('gui'))
build_gui = gtkmm.found() and adwaita.found()

gio     = dependency('gio-2.0')         # raw GDBus C API for the SNI tray (also brings glib/gobject)
# GDesktopAppInfo, behind the app list of Settings > Exclude apps from the VPN
# (src/ExcludeAppsSheet.cpp). GUI only, like gtkmm.
gio_unix = dependency('gio-unix-2.0', required : get_option('gui'))
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
exhaustive_switch_args = cpp.get_supported_arguments('-Werror=switch')
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
gui_deps = [gtkmm, adwaita, libsecret, libsoup, gio, gio_unix, json, threads, sdk_lib]
if webkit.found() and build_gui
  gui_deps += webkit
  webkit_args = ['-DUR_HAVE_WEBKIT']
endif

# QR DECODING for the extender import screen (connect/EXTENDER.md K8: "Windows
# and linux: the code renders through a vendored single-file encoder, import
# reads an image file through zxing-cpp plus pasted text, no camera").
#
# GUI-ONLY, and required exactly when the GUI is: with -Dgui=enabled (the
# AppImage and Flatpak builds) a missing zxing-cpp is a hard configure error,
# so the import screen can never ship silently half-working. A GTK-less or
# `auto` build without it still compiles -- the "choose image file" button is
# then absent and the pasted-text path, which needs no library at all, carries
# the whole feature.
#
# >= 2.1 for ReaderOptions (2.0 called it DecodeHints). Ubuntu 24.04 and
# Fedora 40 ship 2.2.1; the Flatpak builds it from source. The pkg-config
# module is `zxing`, which is what zxing-cpp installs.
# ENCODING is NOT this dependency: it is the vendored MIT single-file
# app/third_party/qrcodegen (see its README for why it is vendored).
zxing = dependency('zxing', version : '>= 2.1', required : get_option('gui'))
zxing_args = []
if zxing.found() and build_gui
  gui_deps += zxing
  zxing_args = ['-DUR_HAVE_ZXING']
endif

# The vendored QR encoder's header (app/third_party/qrcodegen). is_system for
# the same reason the SDK header is: it is upstream code kept verbatim, and a
# local warning policy must never become a reason to patch it.
qrcodegen_inc = include_directories('third_party' / 'qrcodegen', is_system : true)

# Optional WalletConnect Cloud project id -> src/Config.hpp (kWalletConnectProjectId).
# Used by the Bittensor WalletConnect wallet on the ur.io bittensor-connect page.
# Empty is valid and is the default: the page then uses its own id. Keeps the id
# out of the source tree, the way android (local.properties) and apple
# (Info.plist) do it.
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
    'src/StartupFailure.cpp',
    'src/SingleInstance.cpp',
    'src/ControlClient.cpp',
    'src/RpcSessionStore.cpp',
    'src/SecretServiceRpcSessionStore.cpp',
    'src/SdkHost.cpp',
    'src/WalletConnect.cpp',
    'src/MainWindow.cpp',
    'src/UrTheme.cpp',
    'src/PaneKit.cpp',
    'src/ReferralRoyalty.cpp',
    'src/ReferralPanel.cpp',
    'src/ReferralsPage.cpp',
    'src/SessionsPage.cpp',
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
    'src/VlessSheet.cpp',
    'src/UpdateChecker.cpp',
    'src/LicensesSheet.cpp',
    'src/DeveloperPage.cpp',
    'src/EarningsPage.cpp',
    'src/SolanaWalletSheet.cpp',
    'src/BittensorManualSheet.cpp',
    'src/EmojiTagSheet.cpp',
    'src/AccountPage.cpp',
    'src/GuestConversionSheet.cpp',
    'src/LogTailClient.cpp',
    'src/Tray.cpp',
    'src/Ui.cpp',
    'src/Formatters.cpp',
    'src/TransferChart.cpp',
    'src/DemandChart.cpp',
    'src/TransportPresentation.cpp',
    'src/TransportBar.cpp',
    'src/TransportSheet.cpp',
    'src/IpFamilyStatusRow.cpp',
    'src/ExtenderPanel.cpp',
    'src/ExtenderSection.cpp',
    'src/ControlDohSection.cpp',
    'src/ExtenderShareSheet.cpp',
    'src/ExtenderImportSheet.cpp',
    # the vendored MIT QR encoder behind the extender share code
    'third_party/qrcodegen/qrcodegen.cpp',
    'src/PostQuantumIdentity.cpp',
    'src/ContractsSheet.cpp',
    'src/LocationsSheet.cpp',
    'src/SplitRulesSheet.cpp',
    'src/ExcludeAppsSheet.cpp',
    'src/ExcludeLauncher.cpp',
    'src/DnsSheet.cpp',
    'src/SubscriptionBalance.cpp',
    'src/ClientEvents.cpp',
    'src/OfferCard.cpp',
    'src/UsageBar.cpp',
    'src/RedeemCodeSheet.cpp',
    'src/DataInfoSheet.cpp',
    'src/UpgradeSheet.cpp',
    'src/Onboarding.cpp',
    'src/PlanPicker.cpp',
    'src/ProCelebration.cpp',
    'src/AuthViews.cpp',
    'src/ReferralCodeBox.cpp',
    'src/GlobeGeometry.cpp',
    'src/WorldTopology.cpp',
    'src/ProviderLocationRow.cpp',
    'src/ProviderGlobe.cpp',
    'src/ProviderLocationsSheet.cpp',
    'src/LocationOverrideState.cpp',
    'src/LocationOverride.cpp',
    'src/LocationOverrideGuide.cpp',
  ],
  include_directories : [sdk_inc, qrcodegen_inc],
  dependencies : gui_deps,
  cpp_args : wc_args + i18n_args + webkit_args + zxing_args + sdk_warn_args + pkgdata_args +
             version_args + exhaustive_switch_args,
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
    'src/daemon/ExitSampler.cpp',
    'src/daemon/HostMemory.cpp',
    'src/daemon/NetworkCountryWatcher.cpp',
    'src/daemon/SupportDiagnostics.cpp',
    'src/daemon/GlogFlusher.cpp',
    'src/Tunnel.cpp',
    'src/ControlClient.cpp',
    'src/LocationOverride.cpp',
    'src/LocationOverrideState.cpp',
  ],
  include_directories : [include_directories('src'), sdk_inc],
  dependencies : [gio, json, threads, sdk_lib],
  cpp_args : sdk_warn_args + version_args + exhaustive_switch_args,
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
install_data('..' / 'packaging' / 'polkit' / 'com.bringyour.network.policy',
             install_dir : get_option('datadir') / 'polkit-1' / 'actions')
endif

# desktop file: the filename MUST match main.cpp's app id com.bringyour.network
# (D-Bus activation requires it; APPIMAGE.md §11d)
install_data('packaging/com.bringyour.network.desktop',
             install_dir : get_option('datadir') / 'applications')

# AppStream metadata — required by Flathub, read by GNOME Software / Discover.
# The component id matches the .desktop basename.
#
# THE RELEASE STAMP IS DERIVED, NOT HARDCODED. The release version is
# <YYYY>.<M>.<D>-<run>[-<suffix>]. Nothing used to write it into this file, so
# it sat at whatever was typed last while the pipeline shipped something else —
# and this element is exactly what GNOME Software, KDE Discover and the Flathub
# page display.
#
# The FULL version goes in, suffix and all: appstreamcli 1.0.2 (the version on
# the ubuntu-24.04 image CI validates on) accepts a version like
# "2026.8.16-1020679030-beta" with exit 0, so there is no reason to truncate.
# Truncating would advertise a version that matches no artifact — every other
# consumer of $VERSION (UR_APP_VERSION in both binaries, every package
# filename, the release-asset gates) uses the whole string — and would collapse
# every build of the same UTC day onto one indistinguishable release element.
#
# Only the DATE is derived, from the leading <YYYY>.<M>.<D>.
#
# NB: the .deb, .rpm and tarball do NOT ship this file. packaging/lib/common.sh's
# assemble_daemon_root() copies an explicit whitelist (desktop entry, polkit
# policy, icons) that does not include usr/share/metainfo, and make-appimage.sh
# does not package it either. The Flatpak is the only artifact that installs it,
# which is why -Dapp_version has to reach the Flatpak build.
_appver = get_option('app_version')
_av = _appver.split('-')[0]
_vp = _av.split('.')
if _vp.length() == 3 and _vp[0].to_int() > 2000
  _mm = _vp[1].to_int() < 10 ? '0' + _vp[1] : _vp[1]
  _dd = _vp[2].to_int() < 10 ? '0' + _vp[2] : _vp[2]
  appstream_date = _vp[0] + '-' + _mm + '-' + _dd
else
  # -Dapp_version was not passed (the 0.0.0 dev sentinel). LOUD on purpose: a
  # document saying version="0.0.0" is perfectly valid AppStream, so neither the
  # meson test below nor CI's validate step can catch it — the build log is the
  # only place it can surface. A release build must never take this branch.
  warning('app_version was not set, so the AppStream release stamp will say "' +
          _appver + '". Pass -Dapp_version=<release version> for anything you ' +
          'intend to ship; the store front reads this element.')
  appstream_date = '2026-08-20'
endif

metainfo_xml = configure_file(
  input : 'packaging/com.bringyour.network.metainfo.xml.in',
  output : 'com.bringyour.network.metainfo.xml',
  configuration : {
    'APPSTREAM_VERSION' : _appver,
    'APPSTREAM_DATE' : appstream_date,
  },
)
install_data(metainfo_xml,
             install_dir : get_option('datadir') / 'metainfo')

# Nothing validated this file before, which is how a stale version survived in
# it. appstreamcli is the validator Flathub gates submissions on, so a bad edit
# fails here instead of in review. Optional: not every build host ships it.
appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test('appstream-metainfo', appstreamcli,
       args : ['validate', '--no-net', '--pedantic', metainfo_xml])
endif

# hicolor icons (Icon=com.bringyour.network in the desktop file)
# Every size is a downscale of ONE 1024x1024 master, so they cannot drift the
# way separately-drawn assets do. Shipping only 48 and 256 meant the shell had
# to upscale for the 64px app grid and the 128/256 HiDPI lookups, which is the
# softness that showed up on a HiDPI desktop.
#
# 512 IS THE CEILING ON PURPOSE. flatpak refuses to export an icon larger than
# 512x512 -- "Image too large (1024x1024). Max. size 512x512" -- and fails the
# whole build at export time. The master is kept at
# packaging/icons/com.bringyour.network-master-1024.png for regenerating these
# and is deliberately not installed; nothing downstream asks for more than 512
# (Flathub renders the store page at up to 512).
foreach size : ['48', '64', '128', '256', '512']
  install_data('packaging/icons/hicolor/' + size + 'x' + size + '/apps/com.bringyour.network.png',
               install_dir : get_option('datadir') / 'icons' / 'hicolor' /
                             (size + 'x' + size) / 'apps')
endforeach

# autostart INERT TEMPLATE: root-owned under /etc/urnetwork/autostart/, where
# no desktop reads it; the GUI symlinks it into ~/.config/autostart atomically
if host_integration
install_data('packaging/autostart/com.bringyour.network.desktop',
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
install_data('assets/UrPerson1.png', 'assets/UrPerson2.png', 'assets/UrPerson3.png',
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
# the provider row/label helpers, the location-override state machine, the
# transport bar's boundary-tween geometry, the ip family status row's
# counting and ranking, the extender ring geometry, the extender panel's reading of
# ExtenderStatus, the provider extender row's reading of ExtenderProvideStatus
# and the statistics sections' visibility, the compact count and count-rate
# labels, the extender share/import decisions, the reset's owed re-send and its
# call sites, the Solana payout wallet's card
# and connect-sheet rules, the wallet bridge's return routing, the dual-stack
# tunnel policy and v6 capture set, the updater's release selection (the one
# official repo, the tag grammar, the own-arch asset and its digest, the
# install-kind probe, the launch throttle), when it asks GitHub again after
# a refusal, what Settings says the last check came to, and its wiring, the
# split-rules
# activity row's reading of the block action reason (the safety-rule chip and
# the route-locally offer) and its search and chips, the VLESS editor's form,
# visibility rules, settings mapping and messages and the stored network space
# values every space write starts from, the per-app exclusion's slice, rules
# and call sites, what a network peer row connects to, the provider card's idle
# reason, status line, demand histogram and ranking rows, the connect page's
# provide line, fold table and second doors, the activity rows' keyed
# reconcile and filter, the connect inspector's quick actions, the exclusion
# sheet's .desktop parsing, Exec wrapping and launcher copies, the network
# country's operator-code table and default-route rule, the support log's
# lines and their filters, which device carries a feedback's log upload, for
# how long, and the upload in flight off the daemon's main loop, the GUI's own
# log files that ride in it by descriptor over a socketpair, how a launch
# reaches the running instance and how it answers, the user's autostart entry
# behind the startup toggle, the Settings panes' fold and pane A's second
# doors, and the daemon
# control protocol: framing,
# version
# negotiation in both directions, SDK-build agreement, peer-credential
# authorization) carry no GTK, libadwaita or SDK dependency on purpose, so this
# binary builds and runs anywhere a C++17 compiler does -- including a
# developer machine that cannot build the app itself.  meson test -C build
urnetwork_tests = executable('urnetwork-tests',
  sources : [
    'tests/main.cpp',
    'tests/GlobeGeometryTest.cpp',
    'tests/WorldTopologyTest.cpp',
    'tests/ProviderLocationRowTest.cpp',
    'tests/LocationOverrideStateTest.cpp',
    'tests/ControlProtocolTest.cpp',
    'tests/RpcSessionStoreTest.cpp',
    'tests/TunnelPolicyTest.cpp',
    'tests/KillSwitchExceptionCopyTest.cpp',
    'tests/FixedIpSubtitleCopyTest.cpp',
    'tests/IoLoopFdTest.cpp',
    'tests/NetworkServerUtilsTest.cpp',
    'tests/NetworkSpaceBootstrapTest.cpp',
    'tests/NetworkQualityTest.cpp',
    'tests/HealthTest.cpp',
    'tests/InsufficientBalanceNoticeTest.cpp',
    'tests/DataInfoTest.cpp',
    'tests/ConnectBlockedOutOfBalanceTest.cpp',
    'tests/TapSequenceGateTest.cpp',
    'tests/TransportBarGeometryTest.cpp',
    'tests/IpFamilyStatusTest.cpp',
    'tests/ExtenderRingGeometryTest.cpp',
    'tests/ExtenderStatusPresentationTest.cpp',
    'tests/ExtenderProvidePresentationTest.cpp',
    'tests/CountFormatTest.cpp',
    'tests/ExtenderSharePresentationTest.cpp',
    'tests/ExtenderResetTest.cpp',
    'tests/ExtenderResetWiringTest.cpp',
    'tests/ControlDohWiringTest.cpp',
    'tests/PotfilesTest.cpp',
    'tests/TransportStatusPresentationTest.cpp',
    'tests/DaemonUnreachableCopyTest.cpp',
    'tests/CheckoutEnvelopeTest.cpp',
    'tests/SsoBridgeTest.cpp',
    'tests/PricePresentationTest.cpp',
    'tests/BalanceCodeRedeemTest.cpp',
    'tests/OnboardingRoutingTest.cpp',
    'tests/LeaderboardIndicatorTest.cpp',
    'tests/CheckoutCrashRouteTest.cpp',
    'tests/CheckoutSessionModeTest.cpp',
    'tests/CheckoutBridgeErrorTest.cpp',
    'tests/ServerRefusalTextTest.cpp',
    'tests/SolanaWalletPresentationTest.cpp',
    'tests/SolanaBridgeErrorTest.cpp',
    'tests/PayoutNoExtensionTest.cpp',
    'tests/PayoutPromotionWiringTest.cpp',
    'tests/SnPayoutPresentationTest.cpp',
    'tests/WalletBridgeRouteTest.cpp',
    'tests/BittensorWalletFlowTest.cpp',
    'tests/ManageSubscriptionTest.cpp',
    'tests/LicensesPresentationTest.cpp',
    'tests/EarningsPageTeardownTest.cpp',
    'tests/GuestAccountTest.cpp',
    'tests/GuestConversionTest.cpp',
    'tests/GuestPurchaseRefusalWiringTest.cpp',
    'tests/ReleaseSelectionTest.cpp',
    'tests/UpdateScheduleTest.cpp',
    'tests/UpdateStatePresentationTest.cpp',
    'tests/UpdateWiringTest.cpp',
    'tests/CatalogLookupTest.cpp',
    'tests/ReferralCodeStateTest.cpp',
    'tests/ReferralShareTest.cpp',
    'tests/ReferralTotalsStateTest.cpp',
    'tests/UsageBarReferralRowTest.cpp',
    'tests/UsageBarDataTest.cpp',
    'tests/VerifySendNoticeTest.cpp',
    'tests/DeleteAccountOutcomeTest.cpp',
    'tests/FeedbackSendStateTest.cpp',
    'tests/FastDnsCopyTest.cpp',
    'tests/FastDnsWiringTest.cpp',
    'tests/CountedCopyPluralTest.cpp',
    'tests/SafetyRulePresentationTest.cpp',
    'tests/NetworkNameCheckTest.cpp',
    'tests/ReferralCodeFieldTest.cpp',
    'tests/ProUpgradeProvideModeTest.cpp',
    'tests/ProUpgradeReactionTest.cpp',
    'tests/SeekerCopyTest.cpp',
    'tests/SupportContactTest.cpp',
    'tests/CloudProxyLinkTest.cpp',
    'tests/ConfirmationPollGateTest.cpp',
    'tests/AddSignInFlowTest.cpp',
    'tests/AddSignInWiringTest.cpp',
    'tests/VlessPresentationTest.cpp',
    'tests/ErrorIdInternalTest.cpp',
    'tests/PeerLocationTest.cpp',
    'tests/ProviderIdleReasonTest.cpp',
    'tests/ProviderStatusPresentationTest.cpp',
    'tests/ProviderStatusWiringTest.cpp',
    'tests/ProvideLifecycleTest.cpp',
    'tests/ProvideLineTest.cpp',
    'tests/LegacyHomeRemovedTest.cpp',
    'tests/ConnectActivityRowsWiringTest.cpp',
    'tests/ConnectFoldTest.cpp',
    'tests/ConnectFoldDoorsWiringTest.cpp',
    'tests/KeyedReconcileTest.cpp',
    'tests/ConnectionFilterTest.cpp',
    'tests/QuickActionTest.cpp',
    'tests/QuickActionWiringTest.cpp',
    'tests/SplitRulesSheetWiringTest.cpp',
    'tests/SignOutTest.cpp',
    'tests/SignOutWiringTest.cpp',
    'tests/ProvideWhileDisconnectedWiringTest.cpp',
    'tests/ProviderOnlyStatusWiringTest.cpp',
    'tests/ExcludeLauncherTest.cpp',
    'tests/NetworkCountryTest.cpp',
    'tests/NetworkCountryWiringTest.cpp',
    'tests/DiagnosticLinesTest.cpp',
    'tests/DiagnosticLogWiringTest.cpp',
    'tests/LogUploadTest.cpp',
    'tests/LogUploadWiringTest.cpp',
    'tests/PassedLogFilesTest.cpp',
    'tests/DeveloperExitPresentationTest.cpp',
    'tests/InstanceHandoverTest.cpp',
    'tests/InstanceHandoverWiringTest.cpp',
    'tests/LaunchAtStartupTest.cpp',
    'tests/LaunchAtStartupWiringTest.cpp',
    'tests/Utf8TruncateTest.cpp',
    'tests/ControlReplyWiringTest.cpp',
    'tests/DaemonInstanceGuardTest.cpp',
    'tests/DaemonCrashOutputTest.cpp',
    'tests/TunnelHostLockWiringTest.cpp',
    'tests/GlogFlushWiringTest.cpp',
    'tests/TunnelWatchdogTest.cpp',
    'tests/DeadTunnelWiringTest.cpp',
    'tests/FailsafeNoticeTest.cpp',
    'tests/FailsafeNoticeWiringTest.cpp',
    'tests/TeardownOrderWiringTest.cpp',
    'tests/SelfRestartTest.cpp',
    'tests/SelfRestartWiringTest.cpp',
    'tests/SettingsFoldTest.cpp',
    'tests/SettingsFoldDoorsWiringTest.cpp',
    'tests/StatusStripWiringTest.cpp',
    'tests/LocationSelectionTest.cpp',
    'tests/ConnectFunnelWiringTest.cpp',
    'tests/RowConnectCoalescerTest.cpp',
    'tests/ShellLayoutTest.cpp',
    'tests/StatusStripPresentationTest.cpp',
    'tests/ConnectionHealthWiringTest.cpp',
    'tests/DaemonStatusPollWiringTest.cpp',
    'tests/BrowserSignInGateTest.cpp',
    'tests/TrayPolicyTest.cpp',
    'tests/AccountPlanRowsTest.cpp',
    'tests/AppPrefsTest.cpp',
    'tests/WindowGeometryTest.cpp',
    'tests/SeedphraseInputWiringTest.cpp',
    'tests/StartupFailureTest.cpp',
    'tests/LocationRowNameTest.cpp',
    'tests/LocationsSheetWiringTest.cpp',
    'tests/CopyTextButtonWiringTest.cpp',
    'tests/DisplayTextTest.cpp',
    'tests/NetworkQuickPickTest.cpp',
    'tests/ProvideModeGlyphTest.cpp',
    'tests/EarningsRankingWiringTest.cpp',
    'tests/DeveloperActionsWiringTest.cpp',
    'tests/RevealBreadcrumbsWiringTest.cpp',
    'tests/HomeEntranceWiringTest.cpp',
    'tests/ConnectCanvasRimWiringTest.cpp',
    'tests/PlanPickerWiringTest.cpp',
    'tests/ClientInfoWiringTest.cpp',
    'tests/ApiSignOutWiringTest.cpp',
    'tests/AuthLogoutTest.cpp',
    'tests/GlyphPathTest.cpp',
    'tests/AccountRowIconsTest.cpp',
    'tests/RelativeTimeSpanTest.cpp',
    'tests/SessionsPresentationTest.cpp',
    'tests/SessionsBindingTest.cpp',
    'tests/SessionsWiringTest.cpp',
    'src/GlobeGeometry.cpp',
    'src/WorldTopology.cpp',
    'src/ProviderLocationRow.cpp',
    'src/LocationOverrideState.cpp',
    'src/RpcSessionStore.cpp',
    'src/ExcludeLauncher.cpp',
  ],
  include_directories : include_directories('src', 'tests'),
  dependencies : [json],
  cpp_args : ['-DUR_ASSETS_DIR="' + meson.project_source_root() / 'assets' + '"',
              # CatalogLookupTest reads every GUI source, po/urnetwork.pot and
              # po/linux-keys.txt: each string looked up must be an entry of the
              # generated catalog, and its key tagged linux in the store.
              # TunnelPolicyTest reads Tunnel.cpp to prove the dual-stack guard
              # still has a caller. A predicate with no call site is this
              # project's most-repeated defect, and it is invisible to a test
              # that only exercises the predicate. It reads the per-app
              # exclusion's call sites, its launcher (../packaging) and
              # ../../packaging/lib/common.sh the same way.
              # EarningsPageTeardownTest reads EarningsPage.cpp the same way:
              # its destructor must touch no widget (issue #13).
              # PotfilesTest reads every source and po/POTFILES.in the same way.
              # FastDnsWiringTest reads the DNS surfaces and po/en.po the same way.
              # CountedCopyPluralTest reads the counted-copy surfaces and po/ the same way.
              # ErrorIdInternalTest reads every source the same way: the C ABI's
              # internal error id has one source, the SDK header.
              # ExtenderResetWiringTest reads the reset's daemon, client, host, section and
              # page call sites the same way.
              # ProviderStatusWiringTest reads SdkHost.cpp, EarningsPage.cpp and
              # po/en.po the same way.
              # ProvideWhileDisconnectedWiringTest reads the provider-only device's
              # daemon and GUI call sites the same way, and
              # ProviderOnlyStatusWiringTest its statistics and settings call sites.
              # ProvideLineTest reads the connect page's provide line the same way.
              # LegacyHomeRemovedTest reads the window, the shell and the build lists
              # the same way: the legacy home stays gone and every page is reachable.
              # ConnectActivityRowsWiringTest reads the connect page's ip family row
              # and extender panel, and their host feed, the same way.
              # QuickActionWiringTest reads the quick actions' host writers, press,
              # row menu and toast the same way.
              # SplitRulesSheetWiringTest reads the split rules sheet's search, chips and
              # row buttons the same way.
              # ConnectFoldDoorsWiringTest reads the connect page's second doors the same
              # way.
              # SignOutWiringTest reads SdkHost, the health poll and the pages' sign-out
              # resets the same way.
              # ExcludeLauncherTest reads the exclusion sheet and its Settings row the same way.
              # NetworkCountryWiringTest reads the network country's watcher and its daemon and
              # GUI call sites the same way, and DiagnosticLogWiringTest the support log's.
              # LogUploadWiringTest reads the log upload's daemon and GUI call sites the same way.
              # DeveloperExitPresentationTest reads DeveloperPage.cpp and po/ the same way.
              # InstanceHandoverWiringTest reads the launch's call sites in main.cpp and
              # SingleInstance.cpp, the autostart entry and the launcher the same way.
              # LaunchAtStartupWiringTest reads the startup toggle's Settings and launch call
              # sites, meson.build and ../../packaging/lib/common.sh the same way.
              # ControlReplyWiringTest reads the daemon's reply sends and log ring the same way.
              # DaemonInstanceGuardTest reads the daemon's start and revert paths the same way.
              # DaemonCrashOutputTest reads ../packaging/urnetworkd.service the same way.
              # TunnelHostLockWiringTest reads TunnelHost's opMutex_ locks the same way.
              # GlogFlushWiringTest reads the daemon's glog flushes the same way.
              # DeadTunnelWiringTest reads the dead-tunnel failsafe's daemon call sites the
              # same way, and FailsafeNoticeWiringTest its health poll and connect page.
              # TeardownOrderWiringTest reads TunnelHost's teardowns the same way.
              # SelfRestartWiringTest reads the daemon's self-restart and the unit the same way.
              # StatusStripWiringTest reads the status strip's writers in MainWindow.cpp
              # and ConnectPage.cpp the same way.
              # LocationSelectionTest reads the location rows' pages the same way.
              # ConnectFunnelWiringTest reads the connect gestures' window and host call
              # sites the same way.
              # RowConnectCoalescerTest reads the row clicks' host and window call sites
              # the same way.
              # ShellLayoutTest reads the signed-in shell's layout the same way.
              # StatusStripPresentationTest reads the status strip's fields and their
              # writer the same way.
              # ConnectionHealthWiringTest reads the degrade hold's host and page call
              # sites the same way.
              # DaemonStatusPollWiringTest reads the health poll's window and host halves
              # the same way.
              # BrowserSignInGateTest reads the browser sign-ins' window call sites the
              # same way.
              # TrayPolicyTest reads the tray's watcher and the window's close and
              # reachability the same way.
              # AccountPlanRowsTest reads the Account page's Plan pane the same way.
              # AppPrefsTest reads AppPrefs.hpp's calls into PrefsFile.hpp the same way.
              # WindowGeometryTest reads the window's restore and save and their
              # callers the same way.
              # SeedphraseInputWiringTest reads the seedphrase step the same way.
              # StartupFailureTest reads the failed start's main.cpp branch and its
              # dialog the same way.
              # LocationsSheetWiringTest reads the location chooser's rows, the Network
              # page's row name and their css the same way.
              # CopyTextButtonWiringTest reads the click-to-copy texts' sheets and their
              # button the same way.
              # DisplayTextTest reads the external names' display sites the same way.
              # NetworkQuickPickTest reads the Network page's best available detail the
              # same way.
              # ProvideModeGlyphTest reads the provide indicators' two pages the same way.
              # EarningsRankingWiringTest reads the own ranking block the same way.
              # DeveloperActionsWiringTest reads the Developer page's action buttons the
              # same way.
              # RevealBreadcrumbsWiringTest reads the window reveal's log lines the same way.
              # HomeEntranceWiringTest reads the root stack's sign-in swap the same way.
              # ConnectCanvasRimWiringTest reads the hero canvas's rim and orbits the same way.
              # PlanPickerWiringTest reads the plan cards and their css the same way.
              # ClientInfoWiringTest reads the GUI's and the daemon's Api adoption
              # the same way, and ApiSignOutWiringTest the Api's and the device's
              # sign-out reports and the sign-in page's notice.
              # AccountRowIconsTest reads the Account pane's rows, the kit's
              # leading icon and its css the same way.
              # RelativeTimeSpanTest reads RelativeTime's words the same way, and
              # SessionsWiringTest the Sessions page, its Account row, the window's
              # navigation and the rail; SessionsPresentationTest reads po/en.po.
              '-DUR_SRC_DIR="' + meson.project_source_root() / 'src' + '"'] +
             exhaustive_switch_args,
  install : false)
test('unit', urnetwork_tests, protocol : 'exitcode')

test('appimage-gsettings-schema-closure',
     find_program('../packaging/gsettings.test.sh'),
     protocol : 'exitcode')

# install.sh --update: latest stable urnetwork/linux release via the GitHub
# API, sha256 digest verified before anything runs. Fake curl/uname, no network.
test('tarball-update-release-source',
     find_program('../packaging/tarball-update.test.sh'),
     protocol : 'exitcode')

# urnetwork-exclude: the command runs in the user's urnetwork-exclude.slice,
# created and let out by urnetworkd before the command starts, and a host
# without the cgroup v2 unified hierarchy is refused. Fake systemd-run/sleep,
# nothing on the host touched.
test('exclude-launcher',
     find_program('../packaging/urnetwork-exclude.test.sh'),
     protocol : 'exitcode')

# /usr/bin/urnetwork: the autostart entry's --autostart reaches the GUI as
# URNETWORK_AUTOSTART=1, never as an option a GUI from before it refuses. Fake
# GUI and flatpak, emptied environment, nothing on the host touched.
test('launcher-autostart',
     find_program('../packaging/urnetwork-launcher.test.sh'),
     protocol : 'exitcode')

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

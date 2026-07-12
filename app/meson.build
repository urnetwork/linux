# URnetwork Linux app — C++ / GTK4 (gtkmm4 + libadwaita C API), single process,
# consuming the vendored cgo SDK (libURnetworkSdk.so + urnetwork_sdk.hpp).
# See README.md. SPDX-License-Identifier: MPL-2.0
project('urnetwork', 'cpp',
  version : '0.0.1',
  default_options : ['cpp_std=c++17', 'warning_level=2'])

gtkmm   = dependency('gtkmm-4.0')       # GTK4 C++ bindings
adwaita = dependency('libadwaita-1')    # libadwaita (C API, called from C++)
gio     = dependency('gio-2.0')         # raw GDBus C API for the SNI tray (also brings glib/gobject)
json    = dependency('nlohmann_json')
threads = dependency('threads')

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
sdk_inc  = include_directories('third_party' / 'urnetwork-sdk' / sdk_arch)
cpp = meson.get_compiler('cpp')
sdk_lib = cpp.find_library('URnetworkSdk', dirs : sdk_root)

deps = [gtkmm, adwaita, gio, json, threads, sdk_lib]

# Embedded Stripe checkout (src/UpgradeSheet.cpp): WebKitGTK for GTK4
# (webkitgtk-6.0 — NOT webkit2gtk-4.x, which is the GTK3 series). Deliberately
# a soft dependency: when the dev package is missing the app still builds,
# without the webview, and the upgrade sheet falls back to the hosted checkout
# in the system browser — a payment path must never hard-fail on a library.
# Ubuntu 24.04 ships it as libwebkitgtk-6.0-dev (snap/snapcraft.yaml
# build-packages and the build/all/linux Dockerfile both install it).
webkit = dependency('webkitgtk-6.0', required : false)
webkit_args = []
if webkit.found()
  deps += webkit
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

executable('urnetwork',
  sources : [
    'src/main.cpp',
    'src/Tunnel.cpp',
    'src/SdkHost.cpp',
    'src/WalletConnect.cpp',
    'src/MainWindow.cpp',
    'src/Tray.cpp',
    'src/Ui.cpp',
    'src/Formatters.cpp',
    'src/TransferChart.cpp',
    'src/ConnectDrawer.cpp',
    'src/ContractsSheet.cpp',
    'src/SplitRulesSheet.cpp',
    'src/DnsSheet.cpp',
    'src/SubscriptionBalance.cpp',
    'src/UsageBar.cpp',
    'src/RedeemCodeSheet.cpp',
    'src/UpgradeSheet.cpp',
    'src/AuthViews.cpp',
  ],
  include_directories : [sdk_inc],
  dependencies : deps,
  cpp_args : wc_args + i18n_args + webkit_args,
  install : true,
  install_rpath : '$ORIGIN/../lib')

install_data('assets/urnetwork-tray-disconnected.png',
             'assets/urnetwork-tray-connected.png',
             'assets/urnetwork-tray-provide.png',
             install_dir : get_option('datadir') / 'urnetwork/icons')

# builds + installs the message catalogs (and the maintainer pot/update-po
# targets). Must live in po/: i18n.gettext() reads POTFILES.in and LINGUAS from
# the calling subdir.
subdir('po')

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

# Vendored cgo SDK, per arch (fetch-deps.sh unzips URnetworkSdkLinux.zip here).
sdk_arch = get_option('sdk_arch')
sdk_root = meson.project_source_root() / 'third_party' / 'urnetwork-sdk' / sdk_arch
sdk_inc  = include_directories('third_party' / 'urnetwork-sdk' / sdk_arch)
cpp = meson.get_compiler('cpp')
sdk_lib = cpp.find_library('URnetworkSdk', dirs : sdk_root)

deps = [gtkmm, adwaita, gio, json, threads, sdk_lib]

executable('urnetwork',
  sources : [
    'src/main.cpp',
    'src/Tunnel.cpp',
    'src/SdkHost.cpp',
    'src/WalletConnect.cpp',
    'src/MainWindow.cpp',
    'src/Tray.cpp',
  ],
  include_directories : [sdk_inc],
  dependencies : deps,
  install : true,
  install_rpath : '$ORIGIN/../lib')

install_data('assets/urnetwork-tray-disconnected.png',
             'assets/urnetwork-tray-connected.png',
             'assets/urnetwork-tray-provide.png',
             install_dir : get_option('datadir') / 'urnetwork/icons')

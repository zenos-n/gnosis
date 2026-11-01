fs = import('fs')
cef_root = get_option('cef_root')
ublock_dir = get_option('ublock_dir')
if cef_root == '' or ublock_dir == ''
  error('Set cef_root to the pinned CEF SDK and ublock_dir to official full uBlock Origin; nix develop supplies both.')
endif
if get_option('cef_runtime') == 'linked' and not fs.is_absolute(cef_root)
  error('Linked CEF runtime requires an absolute, immutable SDK location.')
endif
# ICU is located relative to the loaded libcef.so, including development builds.
# Immutable SDKs can be referenced directly instead of recopying/stripping CEF
# in every application output. Portable installers keep the default copy mode.
cef_runtime = {
  'Release': ['libcef.so', 'libEGL.so', 'libGLESv2.so', 'libvulkan.so.1', 'libvk_swiftshader.so', 'vk_swiftshader_icd.json', 'snapshot_blob.bin', 'v8_context_snapshot.bin', 'chrome-sandbox'],
  'Resources': ['icudtl.dat', 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'resources.pak', 'locales'],
}
foreach directory, names : cef_runtime
  foreach name : names
    source = cef_root / directory / name
    if fs.exists(source)
      run_command('ln', '-sfn', source, meson.current_build_dir() / name, check: true)
      if get_option('cef_runtime') == 'linked'
        install_symlink(name, pointing_to: source, install_dir: pkglibdir / 'cef')
      endif
    endif
  endforeach
endforeach
cpp = meson.get_compiler('cpp')
cef_dep = cpp.find_library('cef', dirs: meson.current_build_dir())
if get_option('cef_wrapper') != ''
  wrapper_dep = declare_dependency(link_args: get_option('cef_wrapper'))
else
  cmake = import('cmake')
  wrapper_options = cmake.subproject_options()
  wrapper_options.add_cmake_defines({'CEF_ROOT': cef_root})
  wrapper_project = cmake.subproject('cef-wrapper', options: wrapper_options)
  wrapper_dep = wrapper_project.dependency('libcef_dll_wrapper')
endif
engine_resource = gnome.compile_resources('gnosis-engine-resources',
  'data/engine.gresource.xml', c_name: 'gnosis_engine', source_dir: 'data')
engine_includes = include_directories('.', 'select', cef_root)
bridge_dir = developer_mode ? meson.current_source_dir() / 'data/tab-bridge' : pkgdatadir / 'tab-bridge'
manager_dir = developer_mode ? meson.current_source_dir() / 'data/extension-manager' : pkgdatadir / 'extension-manager'
integration_dir = developer_mode ? meson.current_source_dir() / 'data/native-integration' : pkgdatadir / 'native-integration'
scanner = find_program('wayland-scanner')
protocol_dir = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
compositor_protocols = []
foreach item : [
  ['staging/cursor-shape/cursor-shape-v1.xml', 'cursor-shape-v1-protocol.h', 'server-header'],
  ['stable/xdg-shell/xdg-shell.xml', 'xdg-shell-protocol.h', 'server-header'],
  ['unstable/xdg-decoration/xdg-decoration-unstable-v1.xml', 'xdg-decoration-unstable-v1-protocol.h', 'server-header'],
  ['unstable/pointer-constraints/pointer-constraints-unstable-v1.xml', 'pointer-constraints-unstable-v1-protocol.h', 'server-header'],
  ['unstable/pointer-constraints/pointer-constraints-unstable-v1.xml', 'pointer-constraints-client.h', 'client-header'],
  ['unstable/pointer-constraints/pointer-constraints-unstable-v1.xml', 'pointer-constraints-code.c', 'private-code'],
  ['unstable/relative-pointer/relative-pointer-unstable-v1.xml', 'relative-pointer-client.h', 'client-header'],
  ['unstable/relative-pointer/relative-pointer-unstable-v1.xml', 'relative-pointer-code.c', 'private-code'],
]
  compositor_protocols += custom_target(item[1], input: protocol_dir / item[0], output: item[1], command: [scanner, item[2], '@INPUT@', '@OUTPUT@'])
endforeach
compositor_deps = [gtk_dep, json_glib_dep, libxml_dep, dependency('gio-unix-2.0'), dependency('wlroots-0.19'), dependency('wayland-server'), dependency('wayland-client'), dependency('xkbcommon'), dependency('libdrm'), dependency('glesv2'), dependency('pixman-1')]
compositor_lib = static_library('gnosis-compositor', ['brave/compositor.c', 'brave/wayland-source.c', 'brave/outer-pointer.c', 'brave/clipboard-bridge.c', 'brave/profile.c', compositor_protocols],
  dependencies: compositor_deps, c_args: ['-Wno-declaration-after-statement'], pic: true)
compositor_dep = declare_dependency(link_with: compositor_lib, dependencies: compositor_deps)
if get_option('unit_tests').enabled()
  wayland_source_test = executable('wayland-source-test',
    ['brave/wayland-source.c', 'tests/wayland-source-test.c'],
    include_directories: include_directories('brave'),
    dependencies: [dependency('glib-2.0'), dependency('wayland-server')])
  test('Nested Wayland idle dispatch', wayland_source_test)
  foreach name : ['web-app-config', 'loopback']
    check = executable(name + '-test', 'tests/' + name + '-test.c',
      include_directories: include_directories('.'), dependencies: dependency('gio-2.0'))
    test(name, check)
  endforeach
endif
engine = static_library('gnosis-engine',
  ['brave/browser.cpp', 'brave/debug-adapters.cpp', 'ephy-engine-download.cpp', 'ephy-engine-authentication.cpp', 'ephy-engine-client-certificate.cpp', 'ephy-engine-extensions.cpp', 'ephy-engine-permission.cpp', 'ephy-engine-session.cpp', 'ephy-engine-response.cpp', 'ephy-engine-settings.cpp', 'ephy-engine-origin.cpp', 'ephy-engine-certificate.cpp', 'ephy-engine-favicon.c', 'ephy-engine-view.cpp', 'ephy-engine-scheme.cpp', 'ephy-engine-print.cpp', 'select/application.cpp', 'select/browser.cpp', 'select/file-dialog.cpp', 'select/page-messages.cpp', 'select/webstore-guard.cpp', 'select/clipboard.cpp',
   'select/cursor.cpp', 'select/policy.cpp', 'select/extensions.cpp', 'select/extension-host.cpp', engine_resource],
  include_directories: engine_includes,
  dependencies: [gtk_dep, libadwaita_dep, json_glib_dep, libsoup_dep, webkitgtk_dep, dependency('epoxy'), dependency('x11'), dependency('libqrencode'), compositor_dep, cef_dep, wrapper_dep],
  cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"',
    '-DGNOSIS_UBLOCK_DIR="' + ublock_dir + '"',
    '-DGNOSIS_BRIDGE_DIR="' + bridge_dir + '"',
    '-DGNOSIS_MANAGER_DIR="' + manager_dir + '"',
    '-DGNOSIS_KEEPASSXC_DIR="' + get_option('keepassxc_dir') + '"',
    '-DGNOSIS_NATIVE_INTEGRATION_DIR="' + integration_dir + '"'],
  pic: true)
ephyengine_dep = declare_dependency(link_with: engine, include_directories: include_directories('.'), dependencies: [cef_dep, wrapper_dep, json_glib_dep])
if get_option('cef_runtime') == 'copy'
  install_subdir(cef_root / 'Resources', install_dir: pkglibdir / 'cef', strip_directory: true)
  install_subdir(cef_root / 'Release', install_dir: pkglibdir / 'cef', strip_directory: true)
endif
install_subdir('data/tab-bridge', install_dir: pkgdatadir)
install_subdir('data/extension-manager', install_dir: pkgdatadir)
install_subdir('data/native-integration', install_dir: pkgdatadir)
install_data(cef_root / 'LICENSE.txt', cef_root / 'CREDITS.html', install_dir: datadir / 'licenses/gnosis-cef')
if get_option('unit_tests').enabled()
  test('Native extension popup owner', find_program('node'),
    args: [files('tests/popup-owner-test.js'), files('data/native-integration/popup-owner.js')])
  test('Brave navigation target adapter', find_program('node'),
    args: [files('tests/native-navigation-target-test.js'), files('data/native-integration/navigation-target.js')])
  test('KeePassXC shared associations', find_program('node'),
    args: [files('tests/keepassxc-pairing-test.js'), files('data/keepassxc/pairing.js')])
  pairing_store_test = executable('brave-keepassxc-pairing-store-test',
    ['tests/keepassxc-pairing-store-test.c', 'brave/profile.c'],
    include_directories: include_directories('.'), dependencies: [dependency('gio-2.0'), json_glib_dep], c_args: ['-UNDEBUG'])
  test('KeePassXC association store', pairing_store_test)
  debug_adapters_test = executable('brave-debug-adapters-test',
    ['tests/brave-debug-adapters-test.cpp', 'brave/debug-adapters.cpp'],
    dependencies: [gtk_dep, json_glib_dep], include_directories: include_directories('.'))
  foreach mode : ['defaults', 'overrides', 'invalid']
    test('Brave debug adapters ' + mode, debug_adapters_test, args: [mode])
  endforeach
  private_profile_test = executable('brave-private-profile-test', 'tests/private-profile-test.cpp',
    dependencies: [glib_dep], include_directories: include_directories('.'), cpp_args: ['-UNDEBUG'])
  test('Brave private profile ownership', private_profile_test)
  bundled_extension_test = executable('brave-bundled-extension-test', 'tests/bundled-extension-test.cpp',
    dependencies: [glib_dep, json_glib_dep], include_directories: include_directories('.'), cpp_args: ['-UNDEBUG'])
  test('Brave bundled extension updates', bundled_extension_test)
  extension_probe = executable('extension-probe', 'tests/extension-probe.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  popup_test = executable('engine-popup-test', 'tests/popup-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium popup windows', find_program('python3'),
    args: [files('tools/check-native.py'), '--timeout', '90', popup_test], timeout: 100, is_parallel: false)
  gtk_selection_test = executable('gtk-x11-selection-clear-test', 'tests/gtk-x11-selection-clear-test.cpp',
    dependencies: [gtk_dep, dependency('x11'), meson.get_compiler('cpp').find_library('dl')])
  test('GTK X11 stale selection ownership', find_program('python3'),
    args: [files('tools/check-native.py'), gtk_selection_test], timeout: 40, is_parallel: false)
  tls_test = executable('engine-tls-test', 'tests/tls-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium TLS', find_program('python3'),
    args: [files('tools/check-native.py'), '--timeout', '120', find_program('python3'), files('tools/check-tls.py'), tls_test],
    timeout: 150, is_parallel: false)
  clipboard_test = executable('engine-clipboard-test', 'tests/capabilities-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium clipboard', find_program('python3'),
    args: [files('tools/check-native.py'), clipboard_test, '--clipboard-only'], timeout: 90, is_parallel: false)
  page_messages_test = executable('engine-page-messages-test', 'tests/page-messages-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine internal page messages', find_program('python3'),
    args: [files('tools/check-native.py'), page_messages_test], timeout: 90, is_parallel: false)
  # Parallel CEF/GTK processes currently expose unresolved runtime timeouts.
  native_authentication_test = executable('engine-authentication-test', 'tests/authentication-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep, libsoup_dep],
    include_directories: engine_includes)
  test('Engine Chromium HTTP authentication', find_program('python3'),
    args: [files('tools/check-native.py'), '--timeout', '120', native_authentication_test], timeout: 130, is_parallel: false)
  native_permission_test = executable('engine-permission-test', 'tests/permission-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  portal_test = executable('engine-portal-test', 'tests/portal-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium permissions', find_program('python3'),
    args: [files('tools/check-native.py'), native_permission_test, '--use-fake-device-for-media-stream'], timeout: 60, is_parallel: false)
  script_test = executable('engine-script-test', 'tests/script-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium scripts', find_program('python3'),
    args: [files('tools/check-native.py'), script_test], timeout: 60, is_parallel: false)
  session_test = executable('engine-session-test', 'tests/session-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep, libsoup_dep],
    include_directories: engine_includes)
  test('Engine Chromium sessions', find_program('python3'),
    args: [files('tools/check-native.py'), session_test], timeout: 60, is_parallel: false)
  settings_test = executable('engine-settings-test', 'tests/settings-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium settings', find_program('python3'),
    args: [files('tools/check-native.py'), settings_test], timeout: 60, is_parallel: false)
  test('Engine Chromium feature overrides', find_program('python3'),
    args: [files('tools/check-native.py'), settings_test, '--flags'], timeout: 60, is_parallel: false)
  response_test = executable('engine-response-test', 'tests/response-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium responses', find_program('python3'),
    args: [files('tools/check-native.py'), response_test], timeout: 60, is_parallel: false)
  download_test = executable('engine-download-test', 'tests/download-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium downloads', find_program('python3'),
    args: [files('tools/check-native.py'), download_test], timeout: 60, is_parallel: false)
  diagnostic_test = executable('diagnostic-process-test', 'tests/diagnostic-process-test.cpp')
  test('Diagnostic process failures', diagnostic_test)
  permission_test = executable('extension-permissions-test', 'tests/extension-permissions-test.cpp',
    dependencies: glib_dep, include_directories: include_directories('select'))
  test('Extension host permissions', permission_test)
  favicon_test = executable('engine-favicon-test', ['tests/favicon-test.c', 'ephy-engine-favicon.c'],
    dependencies: gtk_dep, include_directories: include_directories('.'))
  test('Engine favicon cache', favicon_test)
  origin_test = executable('engine-origin-test', ['tests/origin-test.cpp', 'ephy-engine-origin.cpp'],
    dependencies: [glib_dep, cef_dep, wrapper_dep], include_directories: engine_includes,
    cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"'])
  test('Engine Chromium origins', find_program('python3'),
    args: [files('tools/check-native.py'), origin_test], timeout: 60, is_parallel: false)
endif

project(
	'havoc',
	'c',
	version : '0.6.0',
	meson_version : '>= 1.8.0',
	license : 'CC0-1.0 OR MIT',
	license_files : 'LICENSE',
)

compiler_flags = [
	'-Wno-unused-parameter',
	'-Wno-parentheses',
	'-Wno-format-overflow',
]

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments(compiler_flags),
	language: 'c'
)

add_project_arguments('-DVERSION="@0@"'.format(meson.project_version()), language : 'c')

xkbcommon = dependency('xkbcommon')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
m_dep = cc.find_library('m', required : false)

subdir('tsm')

wl = import('wayland')

executable(
	'havoc',
	sources: [
		'glyph.c',
		'main.c',
		wl.scan_xml(wl.find_protocol('xdg-shell')),
		wl.scan_xml(wl.find_protocol('xdg-decoration', state: 'unstable', version: 1)),
		wl.scan_xml(wl.find_protocol('primary-selection', state: 'unstable', version: 1)),
	],
	dependencies: [
		wayland_client,
		wayland_cursor,
		xkbcommon,
		m_dep,
	],
	link_with: [
		htsm
	],
	install : true,
)

install_data('havoc.cfg', rename: 'example.cfg')
install_data(meson.project_license_files(), install_dir: get_option('datadir') / 'licenses' / meson.project_name())

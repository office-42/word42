# word42 - a classic word processor
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later

project('word42', 'c',
  version: '0.9.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=gnu11,c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
gnome = import('gnome')

add_project_arguments(
  '-DW42_VERSION="' + meson.project_version() + '"',
  '-DG_LOG_DOMAIN="word42"',
  language: 'c',
)

# Warnings worth having on top of -Wall -Wextra.  Anything the compiler at
# hand does not know about is dropped rather than failing the configure.
extra_warnings = [
  '-Wmissing-prototypes',
  '-Wstrict-prototypes',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wshadow',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wno-unused-parameter',
]
add_project_arguments(cc.get_supported_arguments(extra_warnings), language: 'c')

glib_dep       = dependency('glib-2.0',    version: '>= 2.76')
gobject_dep    = dependency('gobject-2.0', version: '>= 2.76')
gio_dep        = dependency('gio-2.0',     version: '>= 2.76')
cairo_dep      = dependency('cairo',       version: '>= 1.16')
pango_dep      = dependency('pango',       version: '>= 1.50')
pangocairo_dep = dependency('pangocairo',  version: '>= 1.50')
gtk_dep        = dependency('gtk4',        version: '>= 4.10')
pixbuf_dep     = dependency('gdk-pixbuf-2.0', version: '>= 2.42')
m_dep          = cc.find_library('m', required: false)

# Reading PDF needs poppler.  Writing it does not -- cairo does that -- so a
# build without poppler still exports PDF and simply cannot open one.
poppler_dep = dependency('poppler-glib', version: '>= 20.0', required: false)
if poppler_dep.found()
  add_project_arguments('-DHAVE_POPPLER=1', language: 'c')
endif

# Spelling needs Enchant, which finds Hunspell, Aspell or the platform's
# own checker.  Without it there is no Spelling box and no red underlines.
enchant_dep = dependency('enchant-2', required: false)
if enchant_dep.found()
  add_project_arguments('-DHAVE_ENCHANT=1', language: 'c')
endif

# Hyphenation needs libhyphen and its hyph_*.dic pattern files, the ones
# LibreOffice uses.  Without them Tools > Hyphenation says so.
hyphen_dep = cc.find_library('hyphen', required: false)
if hyphen_dep.found() and not cc.has_header('hyphen.h')
  hyphen_dep = disabler()
endif
if hyphen_dep.found()
  add_project_arguments('-DHAVE_HYPHEN=1', language: 'c')
  add_project_arguments('-DW42_HYPHEN_DIR="@0@"'.format(get_option('prefix') / get_option('datadir') / 'hyphen'),
                        language: 'c')
endif

core_deps = [glib_dep, gobject_dep, gio_dep, cairo_dep, pango_dep,
             pangocairo_dep, pixbuf_dep, m_dep]
if hyphen_dep.found()
  core_deps += [hyphen_dep]
endif
if poppler_dep.found()
  core_deps += [poppler_dep]
endif
if enchant_dep.found()
  core_deps += [enchant_dep]
endif
app_deps  = core_deps + [gtk_dep]

subdir('data')
subdir('src')

summary({
    'prefix'  : get_option('prefix'),
    'GTK'     : gtk_dep.version(),
    'Pango'   : pango_dep.version(),
    'Cairo'   : cairo_dep.version(),
    'PDF import (poppler)' : poppler_dep.found(),
    'Spelling (enchant)'   : enchant_dep.found(),
    'Hyphenation (hyphen)' : hyphen_dep.found(),
  }, section: 'word42 ' + meson.project_version())

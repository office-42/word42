# SPDX-License-Identifier: GPL-3.0-or-later

w42_inc = include_directories('util', 'model', 'layout', 'io', 'ui')

# Everything below the user interface: the document model, the layout engine
# and the importers.  Kept as its own library so the tests can link it
# without pulling in GTK.
w42_core_sources = files(
  'util/w42-image.c',
  'util/w42-lang.c',
  'util/w42-shape.c',
  'model/w42-object.c',
  'model/w42-style.c',
  'model/w42-attrs.c',
  'model/w42-piecetable.c',
  'model/w42-document.c',
  'model/w42-search.c',
  'model/w42-spell.c',
  'layout/w42-layout.c',
  'io/w42-io.c',
  'io/w42-rtf.c',
  'io/w42-pdf.c',
  'io/w42-doc.c',
  'io/w42-html.c',
  'io/w42-htmlin.c',
  'io/w42-merge.c',
  'io/w42-zip.c',
  'io/w42-build.c',
  'io/w42-docx.c',
  'io/w42-abw.c',
  'io/w42-odt.c',
  'io/w42-pptx.c',
  'model/w42-hyphenate.c',
  'model/w42-autocorrect.c',
  'model/w42-tableformat.c',
)

w42_core = static_library('w42core',
  w42_core_sources,
  dependencies: core_deps,
  include_directories: w42_inc,
  install: false,
)

w42_core_dep = declare_dependency(
  link_with: w42_core,
  include_directories: w42_inc,
  dependencies: core_deps,
)

w42_ui_sources = files(
  'ui/w42-settings.c',
  'ui/w42-autotext.c',
  'ui/w42-view.c',
  'ui/w42-ruler.c',
  'ui/w42-find-dialog.c',
  'ui/w42-spell-dialog.c',
  'ui/w42-dialogs.c',
  'ui/w42-print.c',
  'ui/w42-preview.c',
  'ui/w42-slideshow.c',
  'ui/w42-window.c',
  'ui/w42-application.c',
  'main.c',
)

executable('word42',
  w42_ui_sources,
  w42_resources,
  w42_icon_resources,
  dependencies: [w42_core_dep, gtk_dep],
  include_directories: w42_inc,
  install: true,
  win_subsystem: 'windows',
)

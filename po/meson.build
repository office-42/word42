# SPDX-License-Identifier: GPL-3.0-or-later

# The translations: po/LINGUAS names them, POTFILES says where the strings
# are.  word42-pot makes the template, word42-update-po merges it into
# every language, and each language's .mo is built and installed.
i18n.gettext(meson.project_name(),
  args: [
    '--from-code=UTF-8',
    '--add-comments=Translators:',
    '--keyword=_',
    '--keyword=N_',
    '--keyword=C_:1c,2',
    '--keyword=NC_:1c,2',
    '--keyword=ngettext:1,2',
    '--flag=N_:1:pass-c-format',
    '--flag=C_:2:pass-c-format',
    '--flag=NC_:2:pass-c-format',
    '--flag=g_strdup_printf:1:c-format',
    '--flag=g_string_printf:2:c-format',
    '--flag=g_string_append_printf:2:c-format',
    '--flag=g_set_error:4:c-format',
    '--flag=g_error_new:3:c-format',
    '--msgid-bugs-address=https://github.com/office-42/word42/issues',
  ],
)

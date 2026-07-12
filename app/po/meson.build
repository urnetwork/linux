# Message catalogs. Everything else in this directory (LINGUAS, *.po,
# urnetwork.pot) is generated from the canonical localization store
# (localizations/keys/*.yaml -> `npm run gen`) and must never be hand-edited.
#
# i18n.gettext() reads LINGUAS + POTFILES.in from this directory, compiles each
# <locale>.po with msgfmt and installs it as
# <localedir>/<locale>/LC_MESSAGES/urnetwork.mo, which is what main.cpp's
# bindtextdomain() points at.
#
# The 'glib' preset supplies the standard xgettext keywords (_, N_, C_, ...);
# the extra keywords below teach the maintainer-only `urnetwork-pot` target
# about the app's own pgettext/npgettext wrappers (src/I18n.hpp), so an
# extraction lines up with the generated pot instead of dropping every string.
# Regenerating the pot from source is a check, not the source of truth: the pot
# in this directory comes from the store.
i18n.gettext(gettext_package,
  preset : 'glib',
  args : [
    '--keyword=T_:1c,2',
    '--keyword=TN_:1c,2,3',
    '--from-code=UTF-8',
  ])

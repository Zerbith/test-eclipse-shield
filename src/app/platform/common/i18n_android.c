/* Android has no libintl, so there is no gettext catalogue: UI strings stay in English.
 * STATUS: first draft, not compiled on a device yet. */
#include "util/i18n.h"

#include <locale.h>

const char *locstr(const char *msgid) {
    return msgid;
}

bool i18n_is_loaded(void) {
    return true;
}

const char *i18n_locale() {
    return "en";
}

const char *app_get_locale_lang() {
    return "en";
}

void i18n_setlocale(const char *locale) {
    (void) locale;
}

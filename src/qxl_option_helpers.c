#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <strings.h>

#include <xf86.h>
#include <xf86Opt.h>

#include "qxl_option_helpers.h"

int get_int_option(OptionInfoPtr options, int option_index,
                   const char *env_name)
{
    if (env_name && getenv(env_name)) {
        return atoi(getenv(env_name));
    }
    return options[option_index].value.num;
}

const char *get_str_option(OptionInfoPtr options, int option_index,
                           const char *env_name)
{
    if (getenv(env_name)) {
        return getenv(env_name);
    }
    return options[option_index].value.str;
}

int get_bool_option(OptionInfoPtr options, int option_index,
                     const char *env_name)
{
    const char* value = getenv(env_name);

    if (!value) {
        return options[option_index].value.bool;
    }
    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "off") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0) {
        return FALSE;
    }
    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0) {
        return TRUE;
    }

    fprintf(stderr, "spice: treating invalid boolean %s as true: %s\n", env_name, value);
    return TRUE;
}

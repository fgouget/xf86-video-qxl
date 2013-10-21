#include "config.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "qxl_option_helpers.h"
#include "spiceqxl_util.h"

void spiceqxl_chown_agent_file(qxl_screen_t *qxl, const char *filename)
{
    int uid, gid;

    uid = get_int_option(qxl->options, OPTION_SPICE_VDAGENT_UID, "XSPICE_VDAGENT_UID");
    gid = get_int_option(qxl->options, OPTION_SPICE_VDAGENT_GID, "XSPICE_VDAGENT_GID");
    if (uid && gid) {
        if (chown(filename, uid, gid) != 0) {
            fprintf(stderr, "spice: failed to chain ownership of '%s' to %d/%d: %s\n",
                    filename, uid, gid, strerror(errno));
        }
    }
}

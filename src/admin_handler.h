/******************************************************************************
 * admin_handler.h — Admin API on a separate port for runtime control.
 *
 * Endpoints:
 *   GET  /admin/config                  — dump running config
 *   PUT  /admin/backends/<name>/drain   — set backend to DRAINING
 *   PUT  /admin/backends/<name>/enable  — set backend to UP
 *   PUT  /admin/reload                  — re-read config file
 ******************************************************************************/

#ifndef ADMIN_HANDLER_H
#define ADMIN_HANDLER_H

#include "config.h"

/* Start the admin listener on cfg->admin_port.  Returns 0 on success. */
int  admin_start(proxy_config_t *cfg, const char *config_path);

/* Stop the admin listener (called during shutdown). */
void admin_stop(void);

#endif /* ADMIN_HANDLER_H */

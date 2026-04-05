/******************************************************************************
 * proxy_handler.h — Core reverse-proxy request handler.
 ******************************************************************************/

#ifndef PROXY_HANDLER_H
#define PROXY_HANDLER_H

#include "config.h"

/* Called by a worker thread for each accepted connection in proxy mode. */
int proxy_handle_client(void *arg);

#endif /* PROXY_HANDLER_H */

/******************************************************************************
 * static_handler.h — Legacy static-file serving mode.
 ******************************************************************************/

#ifndef STATIC_HANDLER_H
#define STATIC_HANDLER_H

/* Called by a worker thread for each accepted connection in static mode. */
int static_handle_client(void *arg);

#endif /* STATIC_HANDLER_H */

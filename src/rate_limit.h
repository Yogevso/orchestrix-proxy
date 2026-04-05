/******************************************************************************
 * rate_limit.h — Per-client-IP fixed-window rate limiter.
 ******************************************************************************/

#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

/* Initialise the rate limiter.  limit = max requests per IP per minute. */
void ratelimit_init(int limit);

/* Check whether a request from client_ip is allowed.
 * Returns 1 if allowed, 0 if rate-limited (should respond 429). */
int  ratelimit_allow(const char *client_ip);

/* Free all rate-limit state (called during shutdown). */
void ratelimit_destroy(void);

#endif /* RATE_LIMIT_H */

/******************************************************************************
 * http_parse.h — HTTP/1.x request and response-head parsing.
 ******************************************************************************/

#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#define MAX_HEADERS     64
#define MAX_METHOD_LEN  16
#define MAX_PATH_LEN    2048
#define MAX_PROTO_LEN   16
#define MAX_HDR_KEY     256
#define MAX_HDR_VAL     2048
#define HTTP_BUF_SIZE   8192

typedef struct {
    char key[MAX_HDR_KEY];
    char value[MAX_HDR_VAL];
} http_header_t;

/* Parsed HTTP request. */
typedef struct {
    char           method[MAX_METHOD_LEN];
    char           path[MAX_PATH_LEN];
    char           proto[MAX_PROTO_LEN];
    http_header_t  headers[MAX_HEADERS];
    int            header_count;
    int            content_length;       /* -1 = not present */
    /* Raw buffer: everything after headers (partial body) lives at raw+body_offset */
    char           raw[HTTP_BUF_SIZE];
    int            raw_len;              /* bytes in raw[] */
    int            head_len;             /* bytes consumed by request-line + headers + blank line */
} http_request_t;

/* Parsed HTTP response status line + headers (no body). */
typedef struct {
    int            status_code;
    char           status_text[64];
    char           proto[MAX_PROTO_LEN];
    http_header_t  headers[MAX_HEADERS];
    int            header_count;
    int            content_length;       /* -1 = not present */
} http_response_head_t;

/*
 * Parse the request from raw bytes already read into req->raw (req->raw_len bytes).
 * Returns  0 on success,
 *         -1 on malformed input,
 *         -2 if more data needed (incomplete headers).
 */
int  http_parse_request(http_request_t *req);

/*
 * Parse response head from a buffer.
 * buf/buf_len: raw bytes.  *head_len set to end-of-headers offset.
 * Returns 0 on success, -1 malformed, -2 incomplete.
 */
int  http_parse_response_head(const char *buf, int buf_len,
                              http_response_head_t *resp, int *head_len);

/* Lookup a header value (case-insensitive).  Returns NULL if not found. */
const char *http_header_get(const http_header_t *headers, int count, const char *key);

/* Check if a header name is hop-by-hop (must not be forwarded). */
int  http_is_hop_by_hop(const char *name);

#endif /* HTTP_PARSE_H */

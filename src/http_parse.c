/******************************************************************************
 * http_parse.c — HTTP/1.x request and response-head parsing.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "http_parse.h"

/* Hop-by-hop headers that must not be forwarded through a proxy. */
static const char *hop_by_hop[] = {
    "connection", "keep-alive", "proxy-authenticate",
    "proxy-authorization", "te", "trailers",
    "transfer-encoding", "upgrade", NULL
};

int http_is_hop_by_hop(const char *name) {
    for (const char **h = hop_by_hop; *h; h++)
        if (strcasecmp(name, *h) == 0) return 1;
    return 0;
}

const char *http_header_get(const http_header_t *headers, int count, const char *key) {
    for (int i = 0; i < count; i++)
        if (strcasecmp(headers[i].key, key) == 0)
            return headers[i].value;
    return NULL;
}

/*
 * Parse a request from the raw buffer already in req->raw (req->raw_len bytes).
 *
 * On success:
 *   - method, path, proto, headers[], header_count, content_length populated
 *   - req->head_len set to the byte offset just past the blank line
 *
 * Returns 0 on success, -1 malformed, -2 incomplete.
 */
int http_parse_request(http_request_t *req) {
    char *buf = req->raw;

    /* Find end of headers (\r\n\r\n or \n\n). */
    char *end = NULL;
    int   end_len = 0;
    end = strstr(buf, "\r\n\r\n");
    if (end) { end_len = 4; }
    else {
        end = strstr(buf, "\n\n");
        if (end) { end_len = 2; }
    }
    if (!end) return -2;   /* incomplete */

    req->head_len = (int)(end - buf) + end_len;

    /* Parse request line. */
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) line_end = strchr(buf, '\n');
    if (!line_end) return -1;

    int line_len = (int)(line_end - buf);
    char line[MAX_PATH_LEN + 64];
    if (line_len >= (int)sizeof(line)) return -1;
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    if (sscanf(line, "%15s %2047s %15s", req->method, req->path, req->proto) != 3)
        return -1;

    /* Parse headers. */
    req->header_count  = 0;
    req->content_length = -1;

    char *pos = line_end;
    /* Skip past \r\n or \n */
    if (*pos == '\r') pos++;
    if (*pos == '\n') pos++;

    while (pos < end && req->header_count < MAX_HEADERS) {
        char *hdr_end = strstr(pos, "\r\n");
        if (!hdr_end) hdr_end = strchr(pos, '\n');
        if (!hdr_end || hdr_end == pos) break;

        int hdr_len = (int)(hdr_end - pos);
        char *colon = memchr(pos, ':', hdr_len);
        if (!colon) { pos = hdr_end + 1; if (*(hdr_end-1) != '\r' && *hdr_end == '\r') pos++; continue; }

        http_header_t *h = &req->headers[req->header_count];

        int key_len = (int)(colon - pos);
        if (key_len >= MAX_HDR_KEY) key_len = MAX_HDR_KEY - 1;
        memcpy(h->key, pos, key_len);
        h->key[key_len] = '\0';

        /* Skip ": " */
        char *vstart = colon + 1;
        while (vstart < hdr_end && *vstart == ' ') vstart++;

        int val_len = (int)(hdr_end - vstart);
        if (val_len >= MAX_HDR_VAL) val_len = MAX_HDR_VAL - 1;
        if (val_len < 0) val_len = 0;
        memcpy(h->value, vstart, val_len);
        h->value[val_len] = '\0';

        if (strcasecmp(h->key, "Content-Length") == 0)
            req->content_length = atoi(h->value);

        req->header_count++;

        pos = hdr_end;
        if (*pos == '\r') pos++;
        if (*pos == '\n') pos++;
    }

    return 0;
}

/*
 * Parse the status line and headers of an HTTP response.
 */
int http_parse_response_head(const char *buf, int buf_len,
                             http_response_head_t *resp, int *head_len) {
    /* Find end of headers. */
    const char *end = NULL;
    int end_sz = 0;
    end = strstr(buf, "\r\n\r\n");
    if (end) { end_sz = 4; }
    else {
        end = strstr(buf, "\n\n");
        if (end) { end_sz = 2; }
    }
    if (!end) return -2;
    *head_len = (int)(end - buf) + end_sz;

    (void)buf_len;

    /* Parse status line: "HTTP/1.x 200 OK" */
    const char *line_end = strstr(buf, "\r\n");
    if (!line_end) line_end = strchr(buf, '\n');
    if (!line_end) return -1;

    int line_len = (int)(line_end - buf);
    char line[512];
    if (line_len >= (int)sizeof(line)) return -1;
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    char status_rest[MAX_HDR_VAL] = "";
    if (sscanf(line, "%15s %d %255[^\r\n]", resp->proto, &resp->status_code, status_rest) < 2)
        return -1;
    snprintf(resp->status_text, sizeof(resp->status_text), "%.63s", status_rest);

    /* Parse headers. */
    resp->header_count  = 0;
    resp->content_length = -1;

    const char *pos = line_end;
    if (*pos == '\r') pos++;
    if (*pos == '\n') pos++;

    while (pos < end && resp->header_count < MAX_HEADERS) {
        const char *hdr_end = strstr(pos, "\r\n");
        if (!hdr_end) hdr_end = strchr(pos, '\n');
        if (!hdr_end || hdr_end == pos) break;

        int hdr_len = (int)(hdr_end - pos);
        const char *colon = memchr(pos, ':', hdr_len);
        if (!colon) { pos = hdr_end + 1; continue; }

        http_header_t *h = &resp->headers[resp->header_count];

        int key_len = (int)(colon - pos);
        if (key_len >= MAX_HDR_KEY) key_len = MAX_HDR_KEY - 1;
        memcpy(h->key, pos, key_len);
        h->key[key_len] = '\0';

        const char *vstart = colon + 1;
        while (vstart < hdr_end && *vstart == ' ') vstart++;
        int val_len = (int)(hdr_end - vstart);
        if (val_len >= MAX_HDR_VAL) val_len = MAX_HDR_VAL - 1;
        if (val_len < 0) val_len = 0;
        memcpy(h->value, vstart, val_len);
        h->value[val_len] = '\0';

        if (strcasecmp(h->key, "Content-Length") == 0)
            resp->content_length = atoi(h->value);

        resp->header_count++;

        pos = hdr_end;
        if (*pos == '\r') pos++;
        if (*pos == '\n') pos++;
    }

    return 0;
}

/*
 * apc.h -- the CTerm APC commands for pictures, sound and the file cache, gathered into one output buffer. See apc.c.
 */

#ifndef APC_H
#define APC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every command starts with this. It is the protocol's fixed tag (CTerm names it after the terminal it began in), so
 * any terminal that speaks these commands listens for exactly these bytes. */
#define APC_PREFIX "\033_SyncTERM:"
#define APC_END    "\033\\"

typedef struct
{
    char  *data;
    size_t len, cap;
} outbuf_t;

void out_bytes(outbuf_t *o, const void *data, size_t len);
void out_str(outbuf_t *o, const char *s);
void out_fmt(outbuf_t *o, const char *format, ...) __attribute__((format(printf, 2, 3)));
void out_base64(outbuf_t *o, const void *data, size_t len);

/* One command with no payload: APC_PREFIX then the text */
void apc_cmd(outbuf_t *o, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* One command carrying a file: APC_PREFIX, head (ending in the ';' before the payload), then the data in base64 */
void apc_blob(outbuf_t *o, const char *head, const void *data, size_t len);

/* 32 lower-case hex digits: what the terminal's cache listing (C;L) reports for each file */
void md5_hex(const void *data, size_t len, char hex[33]);

#endif

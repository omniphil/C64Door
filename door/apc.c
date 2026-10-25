/*
 * apc.c -- the CTerm APC commands, gathered into one output buffer.
 *
 * Every command is APC_PREFIX <verb>;<options>[;payload] APC_END. Only printable ASCII may appear inside, so files and
 * pictures travel as base64 (a third bigger than they are). The protocol is documented with CTerm (cterm.txt).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apc.h"

static bool reserve(outbuf_t *o, size_t more)
{
    size_t cap = o->cap ? o->cap : 65536;
    char *grown;

    if (o->len + more <= o->cap)
        return true;
    while (cap < o->len + more)
        cap *= 2;
    grown = realloc(o->data, cap);
    if (grown == NULL)
        return false;
    o->data = grown;
    o->cap = cap;
    return true;
}

void out_bytes(outbuf_t *o, const void *data, size_t len)
{
    if (!reserve(o, len))
        return;
    memcpy(o->data + o->len, data, len);
    o->len += len;
}

void out_str(outbuf_t *o, const char *s)
{
    out_bytes(o, s, strlen(s));
}

static void out_vfmt(outbuf_t *o, const char *format, va_list ap)
{
    char text[512];
    int n = vsnprintf(text, sizeof(text), format, ap);
    if (n > 0)
        out_bytes(o, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}

void out_fmt(outbuf_t *o, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    out_vfmt(o, format, ap);
    va_end(ap);
}

void out_base64(outbuf_t *o, const void *data, size_t len)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = data;
    char *w;

    if (!reserve(o, (len + 2) / 3 * 4))
        return;
    w = o->data + o->len;
    for (; len >= 3; p += 3, len -= 3)
    {
        *w++ = A[p[0] >> 2];
        *w++ = A[((p[0] & 3) << 4) | (p[1] >> 4)];
        *w++ = A[((p[1] & 15) << 2) | (p[2] >> 6)];
        *w++ = A[p[2] & 63];
    }
    if (len > 0)
    {
        *w++ = A[p[0] >> 2];
        *w++ = A[((p[0] & 3) << 4) | (len > 1 ? p[1] >> 4 : 0)];
        *w++ = len > 1 ? A[(p[1] & 15) << 2] : '=';
        *w++ = '=';
    }
    o->len = (size_t)(w - o->data);
}

void apc_cmd(outbuf_t *o, const char *format, ...)
{
    va_list ap;
    out_str(o, APC_PREFIX);
    va_start(ap, format);
    out_vfmt(o, format, ap);
    va_end(ap);
    out_str(o, APC_END);
}

void apc_blob(outbuf_t *o, const char *head, const void *data, size_t len)
{
    out_str(o, APC_PREFIX);
    out_str(o, head);
    out_base64(o, data, len);
    out_str(o, APC_END);
}

/* ---- MD5 (RFC 1321), only to compare against what the terminal's cache already holds ---- */

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const int R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t h[4], const uint8_t *b)
{
    uint32_t w[16], a = h[0], bb = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] | (uint32_t)b[i * 4 + 1] << 8 | (uint32_t)b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++)
    {
        uint32_t f, t;
        int g;
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);        g = (7 * i) & 15; }
        t = d;
        d = c;
        c = bb;
        f += a + K[i] + w[g];
        bb += (f << R[i]) | (f >> (32 - R[i]));
        a = t;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
}

void md5_hex(const void *data, size_t len, char hex[33])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t *p = data;
    uint8_t tail[128] = { 0 };
    size_t full = len / 64 * 64, rest = len - full, tail_len;
    uint64_t bits = (uint64_t)len * 8;

    for (size_t i = 0; i < full; i += 64)
        md5_block(h, p + i);
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    tail_len = rest < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 8 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(h, tail);
    if (tail_len == 128)
        md5_block(h, tail + 64);
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xFF);
}

/*
 * jxl_enc.h -- JPEG XL pictures for the JPEG XL graphics mode, from the libjxl the BBS box already has. See jxl_enc.c.
 */

#ifndef JXL_ENC_H
#define JXL_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Loads libjxl. False when the box has none, and the JPEG XL option is then shown as unavailable. */
bool jxl_enc_available(void);

/*
 * Encodes a w x h piece of an RGB24 picture (rows `stride` bytes apart) as a bare JPEG XL codestream. distance is
 * libjxl's quality: 0 is lossless, 1 visually lossless, higher is smaller and softer. The result lives until the next
 * call. Returns its size, or 0 on failure.
 */
size_t jxl_enc_rgb(const uint8_t *rgb, int stride, int w, int h, float distance, const uint8_t **out);

#endif

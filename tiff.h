/*
 * TIFF reader for the scope's screen picture. See tiff.c.
 */
#ifndef TIFF_H
#define TIFF_H

#include <stddef.h>
#include <stdint.h>

/* Decode an 8 bit RGB TIFF (uncompressed or LZW, strips) to top-down BGR
   pixels in a malloc'd buffer, w*h*3 bytes. 0 on success, -1 if the
   picture is damaged or of another kind. */
int tiff_decode(const uint8_t *data, size_t n, int *w, int *h, uint8_t **pix);

#endif

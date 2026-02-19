/* mkico.c — create a .ico file from multiple PNG files.
   Usage: mkico output.ico input16.png input24.png input32.png ...
   Each input PNG is embedded as-is (PNG-compressed ICO entry). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t reserved;   /* 0 */
    uint16_t type;       /* 1 = ICO */
    uint16_t count;
} ICONDIR;

typedef struct {
    uint8_t  width;      /* 0 means 256 */
    uint8_t  height;
    uint8_t  colorCount; /* 0 */
    uint8_t  reserved;   /* 0 */
    uint16_t planes;     /* 1 */
    uint16_t bitCount;   /* 32 */
    uint32_t bytesInRes;
    uint32_t imageOffset;
} ICONDIRENTRY;
#pragma pack(pop)

/* Read PNG header to extract width/height */
static int png_dimensions(const uint8_t *data, long len, int *w, int *h)
{
    if (len < 24) return -1;
    /* PNG signature check */
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G')
        return -1;
    /* IHDR chunk starts at offset 8, width at 16, height at 20 (big-endian) */
    *w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    *h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: mkico output.ico input1.png [input2.png ...]\n");
        return 1;
    }

    int n = argc - 2;
    uint8_t **bufs = calloc(n, sizeof(uint8_t *));
    long *sizes = calloc(n, sizeof(long));
    int *widths = calloc(n, sizeof(int));
    int *heights = calloc(n, sizeof(int));

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(argv[i + 2], "rb");
        if (!f) { fprintf(stderr, "Cannot open %s\n", argv[i + 2]); return 1; }
        fseek(f, 0, SEEK_END);
        sizes[i] = ftell(f);
        fseek(f, 0, SEEK_SET);
        bufs[i] = malloc(sizes[i]);
        fread(bufs[i], 1, sizes[i], f);
        fclose(f);

        if (png_dimensions(bufs[i], sizes[i], &widths[i], &heights[i]) != 0) {
            fprintf(stderr, "%s is not a valid PNG\n", argv[i + 2]);
            return 1;
        }
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "Cannot create %s\n", argv[1]); return 1; }

    ICONDIR hdr = { 0, 1, (uint16_t)n };
    fwrite(&hdr, sizeof(hdr), 1, out);

    uint32_t offset = sizeof(ICONDIR) + n * sizeof(ICONDIRENTRY);

    for (int i = 0; i < n; i++) {
        ICONDIRENTRY e = {0};
        e.width = (widths[i] >= 256) ? 0 : (uint8_t)widths[i];
        e.height = (heights[i] >= 256) ? 0 : (uint8_t)heights[i];
        e.planes = 1;
        e.bitCount = 32;
        e.bytesInRes = (uint32_t)sizes[i];
        e.imageOffset = offset;
        fwrite(&e, sizeof(e), 1, out);
        offset += (uint32_t)sizes[i];
    }

    for (int i = 0; i < n; i++) {
        fwrite(bufs[i], 1, sizes[i], out);
        free(bufs[i]);
    }

    fclose(out);
    free(bufs); free(sizes); free(widths); free(heights);

    printf("Created %s with %d images\n", argv[1], n);
    return 0;
}

/*
 * Внутренний layout файла CBMF v1 для реализации cbmf_mount / cbmf_get_glyph.
 * Не часть публичного API: включать только из cbmf.c.
 * Спецификация байтов — README.md (раздел CBMF v1 file layout).
 */

#ifndef CBMF_FILE_H
#define CBMF_FILE_H

#include <stdint.h>

#define CBMF_MAGIC 0x464D4243u /* 'CBMF', uint32 LE */

#define CBMF_FILE_HEADER_BYTES 40u
#define CBMF_FILE_GLYPH_BYTES 20u

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif

typedef struct CbmfFileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint32_t file_size;

    uint32_t glyph_count;
    uint32_t fallback_codepoint;

    uint16_t line_height;
    int16_t baseline;

    uint32_t glyph_table_offset;
    uint32_t glyph_table_size;

    uint32_t bitmap_blob_offset;
    uint32_t bitmap_blob_size;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((packed))
#endif
CbmfFileHeader;

typedef struct CbmfFileGlyph {
    uint32_t codepoint;
    uint32_t bitmap_offset;

    uint16_t width;
    uint16_t height;
    uint16_t row_bytes;
    uint16_t advance_x;

    int8_t offset_x;
    int8_t offset_y;

    uint16_t reserved;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((packed))
#endif
CbmfFileGlyph;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#endif /* CBMF_FILE_H */

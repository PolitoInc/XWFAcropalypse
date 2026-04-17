#pragma once

#define PNGMAGIC 0x89504e470d0a1a0a
#define PNG_CHECKSUM_LEN 0x4
#define PNG_CHUNK_IEND 0x49454e44

// https://github.com/corkami/formats/blob/master/image/jpeg.md
#define JPG_FOOTER 0xffd9
#define JPG_SEGMENT_MARKER	0xff

#pragma pack(push, 1)

typedef struct JPG_SEGMENT_HEADER {
	BYTE segment_hibyte;
	BYTE segment_lobyte;
	UINT16 segment_size;
} JPG_SEGMENT_HEADER, *PJPG_SEGMENT_HEADER;

typedef struct PNGCHUNKHEADER {
	UINT32 size;
	UINT32 type;
} PNGCHUNKHEADER, *PPNGCHUNKHEADER;

#pragma pack(pop)
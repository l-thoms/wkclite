#pragma once

#include <stdint.h>

// CVBS formats should be strictly aligned with each lines via vsync ISR.
// I2S pixel timing: 480 * 312.5 * 50 = 7,500,000
#define DISPLAY_WIDTH_PAL 384
#define DISPLAY_HEIGHT_PAL 288
// 476.67 * 262.5 * 59.94 = 7,500,000
#define DISPLAY_WIDTH_NISC 320
#define DISPLAY_HEIGHT_NTSC 240

// XGA Format is reserved for future use.
// For non-tiled orientations, pixels are 2x downscaled and use PAL resources.
// For vertical tiled orientation, pixels are 3x downscaled and uses NTSC resources,
// while horizontal stride in buffers should be 344 pixels for 8-pixel alignment,
// the value of padding pixels is not cared but recommended to be filled with 00.
// For non-tiled orientations, I2S pixel timing is 572 * 437.5 * 29.97 = 7,500,000.
// For vertical tiled orientation, same total timing as NTSC.
// XGA format only uses primary frame buffers while both I2S channels share the same
// one. For non-tiled orientations, draw calls on the secondary buffers should be ignored;
// for vertical tiled orientation, draw calls on the both buffers should be mapped into the
// top and bottom side of the primary buffers.
// Since I2S signals will be fully stored by external screen controller and the pixel
// clock of screen and camera are originated from I2S clock, the precision of vsync ISR
// is not strictly required.
// Format switching and connectivity detection is not used in XGA format, but sync
// lost detection should be reserved. Enabling XGA format is by probing I2C address of
// external screen controller (FPGA). If not probed, use CVBS formats.
#define DISPLAY_WIDTH_XGA 512
#define DISPLAY_WIDTH_XGA_VERTICAL_TILED 340
#define DISPLAY_WIDTH_XGA_VERTICAL_TILED_VIRTUAL 408
#define DISPLAY_HEIGHT_XGA 384
#define DISPLAY_HEIGHT_XGA_VERTICAL_TILED 256
#define DISPLAY_HEIGHT_XGA_VERTICAL_TILED_VIRTUAL 306

typedef enum
{
    DISPLAY_FORMAT_NTSC,
    DISPLAY_FORMAT_PAL,
    DISPLAY_FORMAT_XGA,
    DISPLAY_FORMAT_UNDEFINDED
} display_format_t;

typedef enum
{
    DISPLAY_ORIENTATION_HORIZONTAL,
    DISPLAY_ORIENTATION_VERTICAL,
    DISPLAY_ORIENTATION_VERTICAL_TILED,
    DISPLAY_ORIENTATION_COUNT
} display_orientation_t;

typedef enum
{
    DISPLAY_COLOR_BLANKING = 0x00,
    DISPLAY_COLOR_TRANSPARENT = 0x01,
    DISPLAY_COLOR_BLACK = 0x02,
    DISPLAY_COLOR_WHITE = 0x03,
} display_color_t;

typedef struct
{
    int x;
    int y;
    int width;
    int height;
} display_rect_t;

typedef struct
{
    int x1;
    int y1;
    int x2;
    int y2;
} display_line_t;

typedef struct
{
    int x;
    int y;
} display_vector_t;

#define FONT_FILE_HEIGHT 18

typedef struct
{
    int x;
    int y;
    uint16_t utf16_char;
} text_position_descriptor_t;

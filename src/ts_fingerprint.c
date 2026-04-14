/*
 * MPEG-TS Fingerprint Injector
 *
 * Standalone tool that reads MPEG-TS from stdin, injects DVB subtitle
 * packets containing fingerprint text, and outputs to stdout.
 * No video re-encoding - video and audio pass through untouched.
 *
 * Usage:
 *   ffmpeg -i input -c:v copy -c:a copy -f mpegts pipe:1 | \
 *     ts_fingerprint --zmq tcp://127.0.0.1:5556 | \
 *     output_destination
 *
 * ZMQ Commands:
 *   "SHOW <text>"              - Show fingerprint text
 *   "SHOW <text> <position>"   - Show at specific position (0-8)
 *   "HIDE"                     - Hide fingerprint
 *   "STATUS"                   - Get current status
 *
 * Positions: 0=top_left 1=top_center 2=top_right
 *            3=mid_left 4=center     5=mid_right
 *            6=bot_left 7=bot_center 8=bot_right
 *
 * Copyright (c) 2026 - Custom FFmpeg Plugin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <zmq.h>

/* ------------------------------------------------------------------ */
/*  MPEG-TS Constants                                                 */
/* ------------------------------------------------------------------ */

#define TS_PACKET_SIZE      188
#define TS_SYNC_BYTE        0x47
#define TS_PAT_PID          0x0000
#define TS_NULL_PID         0x1FFF
#define TS_MAX_PAYLOAD      184

/* Our injected subtitle PID */
#define SUBTITLE_PID        0x0120  /* 288 decimal */

/* DVB Subtitle stream type in PMT */
#define STREAM_TYPE_DVB_SUB 0x06

/* Subtitle injection interval in packets (~1 second at ~3Mbps) */
#define INJECT_INTERVAL_PACKETS 2000

/* PTS offset for subtitles ahead of video (40ms at 90kHz) */
#define SUBTITLE_PTS_OFFSET 3600

/* ------------------------------------------------------------------ */
/*  Built-in 8x16 bitmap font (ASCII 32-126)                         */
/*  Simplified monospace font for DVB subtitle rendering              */
/* ------------------------------------------------------------------ */

/* Each character is 8 pixels wide, 16 pixels tall = 16 bytes per char */
/* Only printable ASCII (32-126) = 95 characters */

static const uint8_t bitmap_font_8x16[95][16] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 '"' */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    /* 37 '%' */ {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    /* 38 '&' */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 39 ''' */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 41 ')' */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    /* 48 '0' */ {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 49 '1' */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 50 '2' */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 51 '3' */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 52 '4' */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 53 '5' */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 54 '6' */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 55 '7' */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 56 '8' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 57 '9' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 60 '<' */ {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 63 '?' */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 64 '@' */ {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    /* 65 'A' */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 66 'B' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* 67 'C' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 68 'D' */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* 69 'E' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 70 'F' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 71 'G' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* 72 'H' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 73 'I' */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 74 'J' */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 75 'K' */ {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 76 'L' */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 77 'M' */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 78 'N' */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 79 'O' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 80 'P' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 81 'Q' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    /* 82 'R' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 83 'S' */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 84 'T' */ {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 85 'U' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 86 'V' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 87 'W' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    /* 88 'X' */ {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 89 'Y' */ {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 90 'Z' */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 91 '[' */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    /* 92 '\' */ {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 93 ']' */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00},
    /* 96 '`' */ {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 98 'b' */ {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*100 'd' */ {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*101 'e' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*102 'f' */ {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /*103 'g' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    /*104 'h' */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*105 'i' */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*106 'j' */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    /*107 'k' */ {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*108 'l' */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*109 'm' */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    /*110 'n' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /*111 'o' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*112 'p' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /*113 'q' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    /*114 'r' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /*115 's' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*116 't' */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    /*117 'u' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*118 'v' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /*119 'w' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    /*120 'x' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /*121 'y' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    /*122 'z' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /*123 '{' */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    /*124 '|' */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /*125 '}' */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /*126 '~' */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ------------------------------------------------------------------ */
/*  Fingerprint state                                                 */
/* ------------------------------------------------------------------ */

#define MAX_TEXT_LEN 256

typedef struct {
    char text[MAX_TEXT_LEN];
    int  active;
    int  position;      /* 0-8 position index */
    pthread_mutex_t mutex;
} FingerprintState;

static FingerprintState g_state = {
    .text = "",
    .active = 0,
    .position = -1,  /* -1 = random */
};

/* Position coordinates for 720p reference (scaled proportionally) */
/* Format: {x_percent, y_percent} as percentage of video dimensions */
static const int positions[9][2] = {
    {5,  5},   /* 0: top_left */
    {40, 5},   /* 1: top_center */
    {75, 5},   /* 2: top_right */
    {5,  45},  /* 3: mid_left */
    {40, 45},  /* 4: center */
    {75, 45},  /* 5: mid_right */
    {5,  85},  /* 6: bottom_left */
    {40, 85},  /* 7: bottom_center */
    {75, 85},  /* 8: bottom_right */
};

/* ------------------------------------------------------------------ */
/*  DVB Subtitle Encoding                                             */
/* ------------------------------------------------------------------ */

/*
 * DVB Subtitle segments (simplified, EN 300 743):
 *
 * Each subtitle "display set" consists of:
 * 1. Display Definition Segment (DDS) - display dimensions
 * 2. Page Composition Segment (PCS)
 * 3. Region Composition Segment (RCS)
 * 4. CLUT Definition Segment (CDS)
 * 5. Object Data Segment (ODS) - contains the bitmap
 * 6. End of Display Set Segment (EDS)
 *
 * All wrapped in a PES packet with stream_id 0xBD (private_stream_1)
 */

/* Segment types */
#define DVB_SEG_PAGE_COMPOSITION     0x10
#define DVB_SEG_REGION_COMPOSITION   0x11
#define DVB_SEG_CLUT_DEFINITION      0x12
#define DVB_SEG_OBJECT_DATA          0x13
#define DVB_SEG_DISPLAY_DEFINITION   0x14
#define DVB_SEG_END_OF_DISPLAY       0x80

/* Write a 16-bit big-endian value */
static void put_be16(uint8_t *p, uint16_t val)
{
    p[0] = (val >> 8) & 0xFF;
    p[1] = val & 0xFF;
}

/*
 * Render text to a bitmap buffer.
 * Returns allocated bitmap (caller frees), sets *w and *h.
 *
 * Color indices (avoiding 0 because 4-bit DVB RLE uses 0 as escape):
 *   1 = semi-transparent black background
 *   2 = white text
 *   (0 is never used in pixel data - region fill handles the transparent area)
 */
static uint8_t *render_text_bitmap(const char *text, int *w, int *h)
{
    int text_len = strlen(text);
    if (text_len == 0) return NULL;

    /* Character dimensions */
    int char_w = 8;
    int char_h = 16;
    int padding = 4; /* pixels around text */

    *w = text_len * char_w + padding * 2;
    *h = char_h + padding * 2;

    /* Allocate bitmap (1 byte per pixel for simplicity) */
    uint8_t *bmp = calloc(*w * *h, 1);
    if (!bmp) return NULL;

    /* Draw background (semi-transparent black = index 1) */
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++)
            bmp[y * *w + x] = 1;

    /* Draw each character */
    for (int c = 0; c < text_len; c++) {
        unsigned char ch = (unsigned char)text[c];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *glyph = bitmap_font_8x16[ch - 32];

        for (int y = 0; y < char_h; y++) {
            uint8_t row = glyph[y];
            for (int x = 0; x < char_w; x++) {
                if (row & (0x80 >> x)) {
                    int px = padding + c * char_w + x;
                    int py = padding + y;
                    bmp[py * *w + px] = 2; /* white text */
                }
            }
        }
    }

    return bmp;
}

/*
 * Encode bitmap as DVB subtitle pixel data using 8-bit/pixel coding.
 * DVB spec EN 300 743, Table 14: 8-bit/pixel code string
 *
 * 8-bit encoding is simpler and more reliable than 4-bit:
 * - Each pixel is one byte (no nibble alignment issues)
 * - data_type = 0x12 introduces a line of 8-bit pixel data
 * - Single byte 0x00 within a line signals end_of_string
 * - Final 0x00 byte = end_of_object_line
 *
 * To encode color 0 (transparent), we use the RLE escape:
 *   0x00 followed by non-zero byte = run of N pixels of color 0
 *   0x00 followed by 0x00 = end_of_string
 *
 * Since we avoid color 0 in pixel data, every pixel is a literal non-zero byte.
 */
/*
 * Encode one field of DVB subtitle pixel data using 8-bit/pixel coding.
 * field=0: encode even lines (0, 2, 4, ...) for top field
 * field=1: encode odd lines (1, 3, 5, ...) for bottom field
 *
 * Structure per field (DVB EN 300 743):
 *   For each line in this field:
 *     0x12                  - data_type: 8-bit pixel code string
 *     <pixel bytes>         - literal non-zero bytes (or 0x00+0xNN for color 0 runs)
 *     0x00 0x00             - end_of_string_signal
 *     0x00                  - end_of_object_line_code (resets x, advances y by 2)
 */
static int encode_dvb_rle_8bit_field(const uint8_t *bitmap, int w, int h,
                                      int field, uint8_t **out_buf, int *out_size)
{
    /* Max size per line: data_type(1) + pixels(w) + end_of_string(2) + end_of_line(1) */
    int lines_in_field = (h + 1 - field) / 2; /* ceil for top, floor for bottom */
    int max_size = lines_in_field * (1 + w + 2 + 1) + 64;
    uint8_t *buf = malloc(max_size);
    if (!buf) return -1;

    int pos = 0;

    for (int y = field; y < h; y += 2) {
        /* data_type = 0x12 (8-bit pixel data) */
        buf[pos++] = 0x12;

        const uint8_t *row = bitmap + y * w;

        for (int x = 0; x < w; x++) {
            uint8_t pixel = row[x];
            if (pixel == 0) {
                /* Color 0 escape: 0x00 followed by run count */
                buf[pos++] = 0x00;
                buf[pos++] = 0x01; /* 1 pixel of color 0 */
            } else {
                /* Non-zero pixel: literal byte */
                buf[pos++] = pixel;
            }
        }

        /*
         * Line termination for FFmpeg's dvbsub decoder:
         *
         * When all pixels in a line are written (pixels_read == region_width),
         * the decoder exits its while loop and reads ONE extra byte (expects 0x00
         * to confirm no overflow). After that, the outer loop reads the next
         * data_type byte.
         *
         * We write: 0x00 (consumed by the post-loop check)
         * Then: 0xF0 (end_of_line data_type, resets x and advances y by 2)
         *
         * Note: 0xF0 is used because the outer loop checks
         * (*buf != 0xF0 && x_pos >= region_width) BEFORE reading data_type.
         * Only 0xF0 is exempt from this check (see dvbsubdec.c).
         */
        buf[pos++] = 0x00; /* consumed by post-loop overflow check */
        buf[pos++] = 0xF0; /* end_of_line: resets x, advances y by 2 */
    }

    *out_buf = buf;
    *out_size = pos;
    return 0;
}

/*
 * Build a complete DVB subtitle display set.
 * Returns PES payload data (caller frees).
 *
 * page_state: 0 = normal, 2 = mode_change (forces decoder acquisition)
 */
static int build_dvb_subtitle_pes(const char *text, int position,
                                   int page_id, int region_id,
                                   int page_state,
                                   uint8_t **out_buf, int *out_size)
{
    static uint8_t page_version = 0;

    int bmp_w, bmp_h;
    uint8_t *bitmap = render_text_bitmap(text, &bmp_w, &bmp_h);
    if (!bitmap) return -1;

    /* Calculate position (for 720p reference) */
    int vid_w = 720, vid_h = 576; /* DVB standard display size */
    int pos_x, pos_y;
    if (position >= 0 && position <= 8) {
        pos_x = (positions[position][0] * vid_w) / 100;
        pos_y = (positions[position][1] * vid_h) / 100;
    } else {
        /* Random position */
        srand(time(NULL));
        pos_x = (positions[rand() % 9][0] * vid_w) / 100;
        pos_y = (positions[rand() % 9][1] * vid_h) / 100;
    }

    /* Clamp to display bounds */
    if (pos_x + bmp_w > vid_w) pos_x = vid_w - bmp_w;
    if (pos_y + bmp_h > vid_h) pos_y = vid_h - bmp_h;
    if (pos_x < 0) pos_x = 0;
    if (pos_y < 0) pos_y = 0;

    /* Encode bitmap to RLE - separate top and bottom fields (interlaced DVB) */
    uint8_t *top_field = NULL, *bottom_field = NULL;
    int top_size = 0, bottom_size = 0;
    encode_dvb_rle_8bit_field(bitmap, bmp_w, bmp_h, 0, &top_field, &top_size);
    encode_dvb_rle_8bit_field(bitmap, bmp_w, bmp_h, 1, &bottom_field, &bottom_size);
    free(bitmap);

    if (!top_field || !bottom_field) {
        free(top_field);
        free(bottom_field);
        return -1;
    }

    /* Build the complete subtitle display set */
    int max_pes_size = 256 + top_size + bottom_size + 256;
    uint8_t *pes = malloc(max_pes_size);
    if (!pes) { free(top_field); free(bottom_field); return -1; }

    int p = 0;

    /* DVB subtitle PES data header */
    pes[p++] = 0x20; /* data_identifier = DVB subtitle */
    pes[p++] = 0x00; /* subtitle_stream_id */

    /* --- Display Definition Segment (DDS) --- */
    /* Required by DVB spec to tell decoder the display dimensions */
    pes[p++] = 0x0F; /* sync_byte */
    pes[p++] = DVB_SEG_DISPLAY_DEFINITION;
    put_be16(pes + p, page_id); p += 2; /* page_id */
    put_be16(pes + p, 5); p += 2; /* segment_length = 5 (no display window) */
    /* dds_version(4 bits)=0 + display_window_flag(1 bit)=0 + reserved(3 bits)=0 */
    pes[p++] = 0x00;
    /* display_width = 719 (720-1), standard SD DVB */
    put_be16(pes + p, 719); p += 2;
    /* display_height = 575 (576-1), standard SD DVB */
    put_be16(pes + p, 575); p += 2;

    /* --- Page Composition Segment --- */
    pes[p++] = 0x0F; /* sync_byte */
    pes[p++] = DVB_SEG_PAGE_COMPOSITION;
    put_be16(pes + p, page_id); p += 2; /* page_id */
    int pcs_len_pos = p;
    put_be16(pes + p, 0); p += 2; /* segment_length (filled later) */
    int pcs_start = p;
    pes[p++] = 30; /* page_time_out (30 seconds) */
    /*
     * page_version_number(4 bits) | page_state(2 bits) | reserved(2 bits)
     * reserved bits should be 11 per DVB spec
     */
    pes[p++] = ((page_version & 0x0F) << 4) | ((page_state & 0x03) << 2) | 0x03;
    page_version = (page_version + 1) & 0x0F;
    /* Region info in PCS */
    pes[p++] = region_id; /* region_id */
    pes[p++] = 0x00; /* reserved */
    put_be16(pes + p, pos_x); p += 2; /* region_horizontal_address */
    put_be16(pes + p, pos_y); p += 2; /* region_vertical_address */
    put_be16(pes + pcs_len_pos, p - pcs_start);

    /* --- Region Composition Segment --- */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_REGION_COMPOSITION;
    put_be16(pes + p, page_id); p += 2;
    int rcs_len_pos = p;
    put_be16(pes + p, 0); p += 2;
    int rcs_start = p;
    pes[p++] = region_id; /* region_id */
    pes[p++] = 0x00; /* region_version_number(4b)=0 + region_fill_flag(1b)=0 + reserved(3b) */
    put_be16(pes + p, bmp_w); p += 2; /* region_width */
    put_be16(pes + p, bmp_h); p += 2; /* region_height */
    /*
     * region_level_of_compatibility(3b) + region_depth(3b) + reserved(2b)
     * For 8-bit depth: compatibility=011(8bit), depth=011(8bit) = 0x6C
     */
    pes[p++] = 0x6C;
    pes[p++] = 0x00; /* CLUT_id = 0 */
    pes[p++] = 0x00; /* region_8-bit_pixel_code (background) */
    pes[p++] = 0x00; /* region_4-bit_pixel_code(4b)=0 + region_2-bit_pixel_code(2b)=0 + reserved(2b) */
    /* Object reference in region */
    put_be16(pes + p, 0x0000); p += 2; /* object_id = 0 */
    /* object_type(2b)=00(bitmap) + object_provider_flag(2b)=00 + object_horizontal_position(12b) */
    put_be16(pes + p, 0x0000); p += 2; /* type=bitmap, h_pos=0 */
    put_be16(pes + p, 0x0000); p += 2; /* reserved(4b) + object_vertical_position(12b) = 0 */
    put_be16(pes + rcs_len_pos, p - rcs_start);

    /* --- CLUT Definition Segment --- */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_CLUT_DEFINITION;
    put_be16(pes + p, page_id); p += 2;
    int cds_len_pos = p;
    put_be16(pes + p, 0); p += 2;
    int cds_start = p;
    pes[p++] = 0x00; /* CLUT_id = 0 */
    pes[p++] = 0x00; /* CLUT_version_number(4b)=0 + reserved(4b) */
    /*
     * CLUT entry flags byte:
     *   bit 7: 2-bit/entry_CLUT_flag
     *   bit 6: 4-bit/entry_CLUT_flag
     *   bit 5: 8-bit/entry_CLUT_flag
     *   bits 4-1: reserved
     *   bit 0: full_range_flag (1=Y,Cr,Cb,T each 8 bits)
     * We use 4-bit CLUT with full range = 0x41
     */
    /*
     * CLUT entries - indices match render_text_bitmap():
     *   0 = not used in pixel data (but define as transparent for safety)
     *   1 = semi-transparent black background
     *   2 = white text (fully opaque)
     */
    /* Entry 0: transparent (default/unused) */
    pes[p++] = 0x00; /* CLUT_entry_id = 0 */
    pes[p++] = 0x21; /* 8-bit CLUT flag + full_range */
    pes[p++] = 0x00; /* Y = 0 */
    pes[p++] = 0x80; /* Cr = 128 (neutral) */
    pes[p++] = 0x80; /* Cb = 128 (neutral) */
    pes[p++] = 0x00; /* T = 0 (fully transparent) */
    /* Entry 1: semi-transparent black background */
    pes[p++] = 0x01; /* CLUT_entry_id = 1 */
    pes[p++] = 0x21; /* 8-bit CLUT flag + full_range */
    pes[p++] = 0x10; /* Y = 16 (black in BT.601) */
    pes[p++] = 0x80; /* Cr = 128 (neutral) */
    pes[p++] = 0x80; /* Cb = 128 (neutral) */
    pes[p++] = 0x80; /* T = 128 (semi-transparent) */
    /* Entry 2: white text */
    pes[p++] = 0x02; /* CLUT_entry_id = 2 */
    pes[p++] = 0x21; /* 8-bit CLUT flag + full_range */
    pes[p++] = 0xEB; /* Y = 235 (white in BT.601) */
    pes[p++] = 0x80; /* Cr = 128 (neutral) */
    pes[p++] = 0x80; /* Cb = 128 (neutral) */
    pes[p++] = 0xFF; /* T = 255 (fully opaque) */
    put_be16(pes + cds_len_pos, p - cds_start);

    /* --- Object Data Segment --- */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_OBJECT_DATA;
    put_be16(pes + p, page_id); p += 2;
    int ods_len_pos = p;
    put_be16(pes + p, 0); p += 2;
    int ods_start = p;
    put_be16(pes + p, 0x0000); p += 2; /* object_id = 0 */
    /*
     * object_version_number(4b) + object_coding_method(2b) + non_modifying_colour_flag(1b) + reserved(1b)
     * version=0, coding=00(bitmap), non_mod=0
     */
    pes[p++] = 0x00;
    /* Top and bottom field data block lengths */
    put_be16(pes + p, top_size); p += 2; /* top_field_data_block_length */
    put_be16(pes + p, bottom_size); p += 2; /* bottom_field_data_block_length */
    /* Top field pixel data (even lines: 0, 2, 4, ...) */
    memcpy(pes + p, top_field, top_size); p += top_size;
    /* Bottom field pixel data (odd lines: 1, 3, 5, ...) */
    memcpy(pes + p, bottom_field, bottom_size); p += bottom_size;
    /* Align to 16-bit boundary if needed */
    if (p & 1) pes[p++] = 0x00;
    put_be16(pes + ods_len_pos, p - ods_start);

    free(top_field);
    free(bottom_field);

    /* --- End of Display Set Segment --- */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_END_OF_DISPLAY;
    put_be16(pes + p, page_id); p += 2;
    put_be16(pes + p, 0); p += 2; /* segment_length = 0 */

    /* End of PES data */
    pes[p++] = 0xFF; /* end marker */

    *out_buf = pes;
    *out_size = p;
    return 0;
}

/*
 * Build a "clear" DVB subtitle display set (empty page).
 */
static int build_dvb_subtitle_clear(int page_id, uint8_t **out_buf, int *out_size)
{
    uint8_t *pes = malloc(64);
    if (!pes) return -1;

    int p = 0;

    pes[p++] = 0x20; /* data_identifier */
    pes[p++] = 0x00; /* subtitle_stream_id */

    /* Empty Page Composition Segment (mode_change to clear) */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_PAGE_COMPOSITION;
    put_be16(pes + p, page_id); p += 2;
    put_be16(pes + p, 2); p += 2; /* segment_length */
    pes[p++] = 1;    /* page_time_out */
    pes[p++] = 0x80; /* page_state = mode_change (clear page) */

    /* End of Display Set */
    pes[p++] = 0x0F;
    pes[p++] = DVB_SEG_END_OF_DISPLAY;
    put_be16(pes + p, page_id); p += 2;
    put_be16(pes + p, 0); p += 2;

    pes[p++] = 0xFF;

    *out_buf = pes;
    *out_size = p;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  MPEG-TS Packet Construction                                       */
/* ------------------------------------------------------------------ */

static uint8_t subtitle_cc = 0; /* continuity counter for subtitle PID */
static uint8_t pat_cc = 0;
static uint8_t pmt_cc = 0;

/* CRC32 for MPEG-TS PSI tables */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void init_crc32_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc = crc << 1;
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

static uint32_t calc_crc32(const uint8_t *data, int len)
{
    if (!crc32_initialized) init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++)
        crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

/*
 * Build TS packets from PES data.
 * Returns array of 188-byte TS packets (caller frees).
 */
static int build_ts_packets(uint16_t pid, const uint8_t *pes_payload,
                             int pes_size, int64_t pts,
                             uint8_t **out_packets, int *out_count)
{
    /* Build PES packet */
    int pes_header_len = 14; /* PES header with PTS */
    int total_pes_len = pes_header_len + pes_size;

    uint8_t *pes = malloc(total_pes_len);
    if (!pes) return -1;

    int p = 0;
    /* PES start code */
    pes[p++] = 0x00;
    pes[p++] = 0x00;
    pes[p++] = 0x01;
    pes[p++] = 0xBD; /* private_stream_1 for DVB subtitles */

    /* PES packet length */
    int pes_pkt_len = total_pes_len - 6;
    if (pes_pkt_len > 0xFFFF) pes_pkt_len = 0; /* 0 = unbounded */
    put_be16(pes + p, pes_pkt_len); p += 2;

    /* PES header flags */
    pes[p++] = 0x80; /* '10' + scrambling(0) + priority(0) + ... */
    pes[p++] = 0x80; /* PTS_DTS_flags = '10' (PTS only) */
    pes[p++] = 5;    /* PES_header_data_length */

    /* PTS */
    pes[p++] = 0x21 | (((pts >> 30) & 0x07) << 1);
    pes[p++] = (pts >> 22) & 0xFF;
    pes[p++] = 0x01 | (((pts >> 15) & 0x7F) << 1);
    pes[p++] = (pts >> 7) & 0xFF;
    pes[p++] = 0x01 | (((pts) & 0x7F) << 1);

    /* PES payload (DVB subtitle data) */
    memcpy(pes + p, pes_payload, pes_size);
    p += pes_size;

    /* Now split into TS packets */
    int num_packets = (total_pes_len + TS_MAX_PAYLOAD - 1) / TS_MAX_PAYLOAD;
    if (num_packets < 1) num_packets = 1;

    uint8_t *packets = calloc(num_packets, TS_PACKET_SIZE);
    if (!packets) { free(pes); return -1; }

    int pes_offset = 0;
    for (int i = 0; i < num_packets; i++) {
        uint8_t *ts = packets + i * TS_PACKET_SIZE;
        int remaining = total_pes_len - pes_offset;
        int payload_size = remaining > TS_MAX_PAYLOAD ? TS_MAX_PAYLOAD : remaining;
        int stuffing = TS_MAX_PAYLOAD - payload_size;

        ts[0] = TS_SYNC_BYTE;
        ts[1] = (pid >> 8) & 0x1F;
        if (i == 0) ts[1] |= 0x40; /* payload_unit_start_indicator */
        ts[2] = pid & 0xFF;

        if (stuffing > 0) {
            /* Need adaptation field for stuffing */
            ts[3] = 0x30 | (subtitle_cc & 0x0F); /* adaptation + payload */
            subtitle_cc = (subtitle_cc + 1) & 0x0F;
            ts[4] = stuffing - 1; /* adaptation_field_length */
            if (stuffing > 1) {
                ts[5] = 0x00; /* flags */
                memset(ts + 6, 0xFF, stuffing - 2); /* stuffing bytes */
            }
            memcpy(ts + 4 + stuffing, pes + pes_offset, payload_size);
        } else {
            ts[3] = 0x10 | (subtitle_cc & 0x0F); /* payload only */
            subtitle_cc = (subtitle_cc + 1) & 0x0F;
            memcpy(ts + 4, pes + pes_offset, payload_size);
        }

        pes_offset += payload_size;
    }

    free(pes);

    *out_packets = packets;
    *out_count = num_packets;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  MPEG-TS PAT/PMT modification                                      */
/* ------------------------------------------------------------------ */

/* State for tracking original stream PIDs */
static uint16_t video_pid = 0;
static uint16_t audio_pid = 0;
static uint16_t pmt_pid = 0;
static int pmt_found = 0;
static uint8_t original_pmt[TS_PACKET_SIZE];
static int original_pmt_len = 0;
static uint8_t original_pmt_version = 0; /* track original PMT version */

/*
 * Parse PAT to find PMT PID
 */
static void parse_pat(const uint8_t *ts_packet)
{
    if (ts_packet[0] != TS_SYNC_BYTE) return;

    int payload_start = (ts_packet[1] & 0x40) != 0;
    if (!payload_start) return;

    int has_adaptation = (ts_packet[3] >> 5) & 0x01;
    int offset = 4;

    if (has_adaptation) {
        int adapt_len = ts_packet[4];
        offset = 5 + adapt_len;
    }

    if (offset >= TS_PACKET_SIZE) return;

    /* Skip pointer field */
    offset += ts_packet[offset] + 1;
    if (offset + 8 >= TS_PACKET_SIZE) return;

    /* PAT table */
    /* table_id should be 0x00 */
    if (ts_packet[offset] != 0x00) return;

    int section_length = ((ts_packet[offset + 1] & 0x0F) << 8) | ts_packet[offset + 2];
    int entries_start = offset + 8; /* skip table header */
    int entries_end = offset + 3 + section_length - 4; /* minus CRC */

    for (int i = entries_start; i + 3 < entries_end && i + 3 < TS_PACKET_SIZE; i += 4) {
        uint16_t program_num = (ts_packet[i] << 8) | ts_packet[i + 1];
        uint16_t pid = ((ts_packet[i + 2] & 0x1F) << 8) | ts_packet[i + 3];

        if (program_num != 0) { /* not NIT */
            pmt_pid = pid;
            break;
        }
    }
}

/*
 * Parse PMT to find video/audio PIDs and store original PMT
 */
static void parse_pmt(const uint8_t *ts_packet)
{
    if (ts_packet[0] != TS_SYNC_BYTE) return;

    int payload_start = (ts_packet[1] & 0x40) != 0;
    if (!payload_start) return;

    int has_adaptation = (ts_packet[3] >> 5) & 0x01;
    int offset = 4;

    if (has_adaptation) {
        int adapt_len = ts_packet[4];
        offset = 5 + adapt_len;
    }

    if (offset >= TS_PACKET_SIZE) return;

    /* Skip pointer field */
    offset += ts_packet[offset] + 1;
    if (offset + 12 >= TS_PACKET_SIZE) return;

    /* PMT table */
    if (ts_packet[offset] != 0x02) return;

    int section_length = ((ts_packet[offset + 1] & 0x0F) << 8) | ts_packet[offset + 2];
    int program_info_length = ((ts_packet[offset + 10] & 0x0F) << 8) | ts_packet[offset + 11];

    /* Extract original version_number from byte offset+5:
     * byte = reserved(2) | version_number(5) | current_next_indicator(1)
     */
    original_pmt_version = (ts_packet[offset + 5] >> 1) & 0x1F;

    int stream_start = offset + 12 + program_info_length;
    int stream_end = offset + 3 + section_length - 4; /* minus CRC */

    /* Store original PMT data for modification */
    memcpy(original_pmt, ts_packet, TS_PACKET_SIZE);
    original_pmt_len = section_length + 3;

    for (int i = stream_start; i + 4 < stream_end && i + 4 < TS_PACKET_SIZE; i += 5) {
        uint8_t stream_type = ts_packet[i];
        uint16_t elem_pid = ((ts_packet[i + 1] & 0x1F) << 8) | ts_packet[i + 2];
        int es_info_length = ((ts_packet[i + 3] & 0x0F) << 8) | ts_packet[i + 4];

        /* Video stream types: H.264=0x1B, H.265=0x24 */
        if (stream_type == 0x1B || stream_type == 0x24 || stream_type == 0x02) {
            video_pid = elem_pid;
        }
        /* Audio stream types */
        if (stream_type == 0x03 || stream_type == 0x04 || stream_type == 0x0F ||
            stream_type == 0x11 || stream_type == 0x81) {
            if (audio_pid == 0) audio_pid = elem_pid;
        }

        i += es_info_length; /* skip ES descriptors */
    }

    pmt_found = 1;
}

/*
 * Build a modified PMT that includes our subtitle PID.
 * Returns a single TS packet.
 * Increments the PMT version_number to signal the change to decoders.
 */
static int build_modified_pmt(uint8_t *out_ts)
{
    if (!pmt_found) return -1;

    /* Start with original PMT */
    memcpy(out_ts, original_pmt, TS_PACKET_SIZE);

    /* Find the payload offset */
    int has_adaptation = (out_ts[3] >> 5) & 0x01;
    int offset = 4;
    if (has_adaptation) offset = 5 + out_ts[4];
    offset += out_ts[offset] + 1; /* pointer field */

    /* Bounds check */
    if (offset + 3 >= TS_PACKET_SIZE) return -1;

    /* Increment version_number in the PMT header.
     * Byte at offset+5: reserved(2) | version_number(5) | current_next_indicator(1)
     */
    uint8_t new_version = (original_pmt_version + 1) & 0x1F;
    out_ts[offset + 5] = (out_ts[offset + 5] & 0xC1) | (new_version << 1);

    /* Find section_length */
    int section_length = ((out_ts[offset + 1] & 0x0F) << 8) | out_ts[offset + 2];
    int section_end = offset + 3 + section_length - 4; /* before CRC */

    /*
     * DVB subtitle stream entry we need to add:
     *   stream_type:     1 byte  (0x06)
     *   elementary_PID:  2 bytes (with reserved bits)
     *   ES_info_length:  2 bytes (with reserved bits)
     *   descriptor 0x59: 2 + 8 = 10 bytes
     *     tag(1) + length(1) + lang(3) + type(1) + comp_page(2) + anc_page(2)
     * Total: 5 + 10 = 15 bytes
     * Plus 4 bytes for CRC after.
     */
    int entry_size = 15;

    /* Check if we have room in the TS packet */
    if (section_end + entry_size + 4 > TS_PACKET_SIZE) {
        /* Not enough room - skip PMT modification */
        return -1;
    }

    int p = section_end;

    /* stream_type = 0x06 (private data / DVB subtitle) */
    out_ts[p++] = STREAM_TYPE_DVB_SUB;

    /* elementary_PID with reserved bits (3 bits reserved = 111) */
    out_ts[p++] = 0xE0 | ((SUBTITLE_PID >> 8) & 0x1F);
    out_ts[p++] = SUBTITLE_PID & 0xFF;

    /* ES_info_length = 10 (descriptor tag + len + 8 bytes payload) */
    int es_info_len = 10;
    out_ts[p++] = 0xF0 | ((es_info_len >> 8) & 0x0F);
    out_ts[p++] = es_info_len & 0xFF;

    /* DVB Subtitle descriptor (tag=0x59) */
    out_ts[p++] = 0x59;  /* descriptor_tag */
    out_ts[p++] = 8;     /* descriptor_length (8 bytes of payload) */
    out_ts[p++] = 'e';   /* ISO 639 language code: "eng" */
    out_ts[p++] = 'n';
    out_ts[p++] = 'g';
    out_ts[p++] = 0x10;  /* subtitling_type: normal DVB */
    out_ts[p++] = 0x00;  /* composition_page_id high */
    out_ts[p++] = 0x01;  /* composition_page_id low */
    out_ts[p++] = 0x00;  /* ancillary_page_id high */
    out_ts[p++] = 0x01;  /* ancillary_page_id low */

    /* Update section_length */
    int new_section_length = section_length + entry_size;
    out_ts[offset + 1] = 0xB0 | ((new_section_length >> 8) & 0x0F);
    out_ts[offset + 2] = new_section_length & 0xFF;

    /* Recalculate CRC32 */
    int crc_pos = p;
    int crc_data_len = crc_pos - offset;
    uint32_t crc = calc_crc32(out_ts + offset, crc_data_len);
    out_ts[crc_pos++] = (crc >> 24) & 0xFF;
    out_ts[crc_pos++] = (crc >> 16) & 0xFF;
    out_ts[crc_pos++] = (crc >> 8) & 0xFF;
    out_ts[crc_pos++] = crc & 0xFF;

    /* Fill remainder with 0xFF (stuffing) */
    for (int i = crc_pos; i < TS_PACKET_SIZE; i++)
        out_ts[i] = 0xFF;

    /* Update continuity counter */
    out_ts[3] = (out_ts[3] & 0xF0) | (pmt_cc & 0x0F);
    pmt_cc = (pmt_cc + 1) & 0x0F;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Video PTS extraction                                              */
/* ------------------------------------------------------------------ */

/*
 * Extract PTS from a PES header.
 * Returns 1 if PTS found and stored in *pts_out, 0 otherwise.
 * The data pointer should point to the start of PES data (after TS header).
 */
static int extract_pes_pts(const uint8_t *data, int len, int64_t *pts_out)
{
    /* Need at least PES start code (3) + stream_id (1) + length (2) + flags (3) = 9 bytes */
    if (len < 9) return 0;

    /* Check PES start code: 00 00 01 */
    if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) return 0;

    /* PTS_DTS_flags are in byte 7, bits 7-6 */
    uint8_t pts_dts_flags = (data[7] >> 6) & 0x03;

    /* Need PTS (flags = 10 or 11) */
    if (pts_dts_flags < 2) return 0;

    /* PES header data length is in byte 8 */
    /* PTS starts at byte 9 and is 5 bytes */
    if (len < 14) return 0;

    int64_t pts = 0;
    pts  = ((int64_t)(data[9] >> 1) & 0x07) << 30;
    pts |= ((int64_t)data[10]) << 22;
    pts |= ((int64_t)(data[11] >> 1)) << 15;
    pts |= ((int64_t)data[12]) << 7;
    pts |= ((int64_t)(data[13] >> 1));

    *pts_out = pts;
    return 1;
}

/*
 * Check if a TS packet has the random_access_indicator set in its
 * adaptation field (signals a keyframe / RAP).
 */
static int ts_has_random_access(const uint8_t *ts_packet)
{
    /* Check adaptation_field_control: bits 5-4 of byte 3 */
    int afc = (ts_packet[3] >> 4) & 0x03;

    /* adaptation field present if afc == 2 or 3 */
    if (afc < 2) return 0;

    /* adaptation_field_length at byte 4 */
    int adapt_len = ts_packet[4];
    if (adapt_len < 1) return 0;

    /* Flags byte at byte 5:
     * bit 7: discontinuity_indicator
     * bit 6: random_access_indicator
     * bit 5: elementary_stream_priority_indicator
     * ...
     */
    return (ts_packet[5] & 0x40) != 0;
}

/* ------------------------------------------------------------------ */
/*  ZMQ listener thread                                               */
/* ------------------------------------------------------------------ */

static void *zmq_thread_func(void *arg)
{
    void *zmq_socket = arg;
    char buf[MAX_TEXT_LEN + 64];

    while (1) {
        int len = zmq_recv(zmq_socket, buf, sizeof(buf) - 1, 0);
        if (len < 0) {
            if (zmq_errno() == ETERM) break;
            continue;
        }
        buf[len] = '\0';

        char reply[256] = "OK";

        pthread_mutex_lock(&g_state.mutex);

        if (strncmp(buf, "SHOW ", 5) == 0) {
            /* Parse: "SHOW text" or "SHOW text position" */
            char *text_start = buf + 5;
            char *space = strrchr(text_start, ' ');
            int pos = -1;

            if (space && space != text_start) {
                /* Check if last token is a number (position) */
                char *endptr;
                long val = strtol(space + 1, &endptr, 10);
                if (*endptr == '\0' && val >= 0 && val <= 8) {
                    pos = (int)val;
                    *space = '\0'; /* truncate text */
                }
            }

            strncpy(g_state.text, text_start, MAX_TEXT_LEN - 1);
            g_state.text[MAX_TEXT_LEN - 1] = '\0';
            g_state.position = pos;
            g_state.active = 1;

            fprintf(stderr, "[ts_fingerprint] SHOW '%s' pos=%d\n",
                    g_state.text, g_state.position);

        } else if (strcmp(buf, "HIDE") == 0) {
            g_state.active = 0;
            g_state.text[0] = '\0';
            fprintf(stderr, "[ts_fingerprint] HIDE\n");

        } else if (strcmp(buf, "STATUS") == 0) {
            snprintf(reply, sizeof(reply), "active=%d text=%s pos=%d",
                     g_state.active, g_state.text, g_state.position);

        } else {
            snprintf(reply, sizeof(reply), "ERR unknown command");
        }

        pthread_mutex_unlock(&g_state.mutex);

        zmq_send(zmq_socket, reply, strlen(reply), 0);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Subtitle injection helper                                         */
/* ------------------------------------------------------------------ */

/*
 * Inject subtitle TS packets into the output stream.
 * Handles both SHOW (with text) and HIDE (clear) cases.
 * Uses video PTS for synchronization.
 *
 * Returns 1 if subtitle was injected, 0 otherwise.
 */
static int inject_subtitle(int64_t video_pts, int *last_active,
                           int *first_display_done)
{
    pthread_mutex_lock(&g_state.mutex);
    int active = g_state.active;
    char text[MAX_TEXT_LEN];
    int position = g_state.position;
    if (active)
        strncpy(text, g_state.text, MAX_TEXT_LEN);
    pthread_mutex_unlock(&g_state.mutex);

    if (active && text[0]) {
        /* Determine page_state: mode_change(2) for first display, normal(0) after */
        int page_state = 0;
        if (!*first_display_done) {
            page_state = 2; /* mode_change to force decoder acquisition */
            *first_display_done = 1;
        }

        /* Inject subtitle TS packets */
        uint8_t *sub_pes = NULL;
        int sub_pes_size = 0;

        build_dvb_subtitle_pes(text, position, 1, 1, page_state,
                               &sub_pes, &sub_pes_size);

        if (sub_pes) {
            uint8_t *ts_packets = NULL;
            int ts_count = 0;
            /* Subtitle PTS = video PTS + small offset (40ms ahead) */
            int64_t sub_pts = video_pts + SUBTITLE_PTS_OFFSET;

            build_ts_packets(SUBTITLE_PID, sub_pes, sub_pes_size,
                             sub_pts, &ts_packets, &ts_count);

            if (ts_packets) {
                fwrite(ts_packets, TS_PACKET_SIZE, ts_count, stdout);
                free(ts_packets);
            }
            free(sub_pes);
        }
        *last_active = 1;
        return 1;

    } else if (*last_active && !active) {
        /* Send clear command */
        uint8_t *clear_pes = NULL;
        int clear_size = 0;
        build_dvb_subtitle_clear(1, &clear_pes, &clear_size);

        if (clear_pes) {
            uint8_t *ts_packets = NULL;
            int ts_count = 0;
            int64_t sub_pts = video_pts + SUBTITLE_PTS_OFFSET;

            build_ts_packets(SUBTITLE_PID, clear_pes, clear_size,
                             sub_pts, &ts_packets, &ts_count);

            if (ts_packets) {
                fwrite(ts_packets, TS_PACKET_SIZE, ts_count, stdout);
                free(ts_packets);
            }
            free(clear_pes);
        }
        *last_active = 0;
        *first_display_done = 0; /* reset for next SHOW */
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main processing loop                                              */
/* ------------------------------------------------------------------ */

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Reads MPEG-TS from stdin, injects DVB subtitle fingerprint,\n"
        "writes MPEG-TS to stdout.\n"
        "\n"
        "Options:\n"
        "  --zmq ADDR     ZeroMQ bind address (default: tcp://127.0.0.1:5556)\n"
        "  --text TEXT     Initial fingerprint text\n"
        "  --position N   Position 0-8 (-1=random, default=-1)\n"
        "  --help         Show this help\n"
        "\n"
        "ZMQ Commands:\n"
        "  SHOW <text>          Show fingerprint\n"
        "  SHOW <text> <pos>    Show at position (0-8)\n"
        "  HIDE                 Hide fingerprint\n"
        "  STATUS               Get current state\n"
        "\n"
        "Example:\n"
        "  ffmpeg -i input -c:v copy -c:a copy -f mpegts pipe:1 | \\\n"
        "    %s --zmq tcp://127.0.0.1:5556 | \\\n"
        "    ffmpeg -i pipe:0 -c copy -f flv rtmp://server/live/key\n"
        "\n", progname, progname);
}

int main(int argc, char *argv[])
{
    const char *zmq_addr = "tcp://127.0.0.1:5556";
    const char *initial_text = NULL;
    int initial_pos = -1;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--zmq") == 0 && i + 1 < argc) {
            zmq_addr = argv[++i];
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            initial_text = argv[++i];
        } else if (strcmp(argv[i], "--position") == 0 && i + 1 < argc) {
            initial_pos = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Set initial state */
    pthread_mutex_init(&g_state.mutex, NULL);
    if (initial_text) {
        strncpy(g_state.text, initial_text, MAX_TEXT_LEN - 1);
        g_state.active = 1;
        g_state.position = initial_pos;
    }

    /* Initialize ZMQ */
    void *zmq_ctx = zmq_ctx_new();
    void *zmq_sock = zmq_socket(zmq_ctx, ZMQ_REP);

    if (zmq_bind(zmq_sock, zmq_addr) != 0) {
        fprintf(stderr, "Failed to bind ZMQ to %s: %s\n",
                zmq_addr, zmq_strerror(zmq_errno()));
        return 1;
    }
    fprintf(stderr, "[ts_fingerprint] ZMQ listening on %s\n", zmq_addr);

    /* Start ZMQ listener thread */
    pthread_t zmq_thread;
    pthread_create(&zmq_thread, NULL, zmq_thread_func, zmq_sock);

    /* Initialize CRC table */
    init_crc32_table();

    /* Set stdin/stdout to binary mode */
    #ifdef _WIN32
    _setmode(fileno(stdin), _O_BINARY);
    _setmode(fileno(stdout), _O_BINARY);
    #endif

    /* Main TS processing loop */
    uint8_t ts_packet[TS_PACKET_SIZE];
    int packet_count = 0;
    int last_active = 0;
    int first_display_done = 0;  /* tracks whether first mode_change display set has been sent */
    int64_t last_video_pts = 0;  /* last extracted video PTS */
    int have_video_pts = 0;      /* whether we have extracted at least one video PTS */
    int packets_since_inject = 0; /* packet counter for injection interval */
    (void)pat_cc; /* reserved for future PAT rewriting */

    fprintf(stderr, "[ts_fingerprint] Processing MPEG-TS stream...\n");

    while (1) {
        /* Read one TS packet */
        int bytes_read = 0;
        while (bytes_read < TS_PACKET_SIZE) {
            int r = fread(ts_packet + bytes_read, 1,
                          TS_PACKET_SIZE - bytes_read, stdin);
            if (r <= 0) goto done;
            bytes_read += r;
        }

        /* Sync check */
        if (ts_packet[0] != TS_SYNC_BYTE) {
            /* Try to resync */
            int synced = 0;
            for (int i = 1; i < TS_PACKET_SIZE; i++) {
                if (ts_packet[i] == TS_SYNC_BYTE) {
                    memmove(ts_packet, ts_packet + i, TS_PACKET_SIZE - i);
                    int remaining = i;
                    while (remaining > 0) {
                        int r = fread(ts_packet + TS_PACKET_SIZE - remaining,
                                      1, remaining, stdin);
                        if (r <= 0) goto done;
                        remaining -= r;
                    }
                    synced = 1;
                    break;
                }
            }
            if (!synced) continue;
        }

        /* Get PID */
        uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];

        /* Parse PAT */
        if (pid == TS_PAT_PID) {
            parse_pat(ts_packet);
        }

        /* Parse PMT */
        if (pmt_pid != 0 && pid == pmt_pid) {
            if (!pmt_found) {
                parse_pmt(ts_packet);
                if (pmt_found) {
                    fprintf(stderr, "[ts_fingerprint] Found video PID=0x%04X audio PID=0x%04X\n",
                            video_pid, audio_pid);
                }
            }

            /* Replace PMT with modified version (includes subtitle PID) */
            if (pmt_found) {
                uint8_t modified_pmt[TS_PACKET_SIZE];
                if (build_modified_pmt(modified_pmt) == 0) {
                    fwrite(modified_pmt, 1, TS_PACKET_SIZE, stdout);
                    packet_count++;
                    packets_since_inject++;
                    continue; /* already wrote modified PMT */
                }
            }
        }

        /* Extract PTS from video PES packets */
        if (video_pid != 0 && pid == video_pid) {
            int payload_start = (ts_packet[1] & 0x40) != 0;
            int keyframe_detected = 0;

            /* Check for random access indicator (keyframe) */
            if (ts_has_random_access(ts_packet)) {
                keyframe_detected = 1;
            }

            if (payload_start) {
                /* This packet starts a new PES - extract PTS */
                int afc = (ts_packet[3] >> 4) & 0x03;
                int payload_offset = 4;

                if (afc == 2 || afc == 3) {
                    /* Has adaptation field */
                    int adapt_len = ts_packet[4];
                    payload_offset = 5 + adapt_len;
                }

                if (payload_offset < TS_PACKET_SIZE) {
                    int pes_len = TS_PACKET_SIZE - payload_offset;
                    int64_t pts;
                    if (extract_pes_pts(ts_packet + payload_offset, pes_len, &pts)) {
                        last_video_pts = pts;
                        have_video_pts = 1;
                    }
                }
            }

            /* Inject on keyframe if we have a valid PTS */
            if (keyframe_detected && have_video_pts && pmt_found) {
                /* Write the video packet first */
                fwrite(ts_packet, 1, TS_PACKET_SIZE, stdout);
                packet_count++;
                packets_since_inject = 0;

                inject_subtitle(last_video_pts, &last_active, &first_display_done);
                continue; /* already wrote the video packet */
            }
        }

        /* Pass through the current packet */
        fwrite(ts_packet, 1, TS_PACKET_SIZE, stdout);
        packet_count++;
        packets_since_inject++;

        /* Periodic injection based on packet count (~every 1 second) */
        if (packets_since_inject >= INJECT_INTERVAL_PACKETS && have_video_pts && pmt_found) {
            packets_since_inject = 0;
            inject_subtitle(last_video_pts, &last_active, &first_display_done);
        }
    }

done:
    fprintf(stderr, "[ts_fingerprint] Processed %d TS packets\n", packet_count);

    /* Cleanup */
    zmq_close(zmq_sock);
    zmq_ctx_destroy(zmq_ctx);
    pthread_mutex_destroy(&g_state.mutex);

    return 0;
}

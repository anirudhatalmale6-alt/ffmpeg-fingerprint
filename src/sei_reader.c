/*
 * SEI Fingerprint Reader / Extractor
 *
 * Reads H.264/H.265 stream and extracts fingerprint data from
 * SEI user_data_unregistered NAL units injected by fingerprint_inject BSF.
 *
 * Usage:
 *   ffmpeg -i stream_url -c:v copy -bsf:v fingerprint_read -f null -
 *
 * Or as standalone:
 *   ffprobe -show_frames -select_streams v:0 input | grep fingerprint
 *
 * This tool can also work as a pipe reader:
 *   ffmpeg -i input -c:v copy -f h264 pipe:1 | sei_reader
 *
 * Copyright (c) 2026 - Custom FFmpeg Plugin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Our UUID for fingerprint SEI */
static const uint8_t fingerprint_uuid[16] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
    0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90
};

#define BUFFER_SIZE (1024 * 1024) /* 1MB read buffer */

static int find_start_code(const uint8_t *data, int size, int *sc_len)
{
    for (int i = 0; i < size - 2; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *sc_len = 3;
                return i;
            }
            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *sc_len = 4;
                return i;
            }
        }
    }
    return -1;
}

/* Remove emulation prevention bytes (0x00 0x00 0x03 -> 0x00 0x00) */
static int remove_epb(const uint8_t *src, int src_len,
                       uint8_t *dst, int dst_max)
{
    int di = 0;
    int zero_count = 0;

    for (int si = 0; si < src_len && di < dst_max; si++) {
        if (zero_count >= 2 && src[si] == 0x03) {
            /* Skip emulation prevention byte */
            zero_count = 0;
            continue;
        }
        dst[di++] = src[si];
        if (src[si] == 0x00)
            zero_count++;
        else
            zero_count = 0;
    }

    return di;
}

/*
 * Parse SEI NAL unit and extract fingerprint data.
 * Returns 1 if fingerprint found, 0 otherwise.
 */
static int parse_sei_h264(const uint8_t *nal_data, int nal_size,
                           char *text_out, int text_max)
{
    /* Remove emulation prevention bytes */
    uint8_t *rbsp = malloc(nal_size);
    if (!rbsp) return 0;
    int rbsp_size = remove_epb(nal_data, nal_size, rbsp, nal_size);

    /* Skip NAL header (1 byte for H.264) */
    int pos = 1;
    int found = 0;

    while (pos < rbsp_size - 1) {
        /* Read payload_type */
        int payload_type = 0;
        while (pos < rbsp_size && rbsp[pos] == 0xFF) {
            payload_type += 255;
            pos++;
        }
        if (pos >= rbsp_size) break;
        payload_type += rbsp[pos++];

        /* Read payload_size */
        int payload_size = 0;
        while (pos < rbsp_size && rbsp[pos] == 0xFF) {
            payload_size += 255;
            pos++;
        }
        if (pos >= rbsp_size) break;
        payload_size += rbsp[pos++];

        /* Check for user_data_unregistered (type 5) */
        if (payload_type == 5 && payload_size >= 16) {
            /* Check UUID */
            if (pos + 16 <= rbsp_size &&
                memcmp(rbsp + pos, fingerprint_uuid, 16) == 0) {
                /* Extract text */
                int text_len = payload_size - 16;
                if (text_len > text_max - 1)
                    text_len = text_max - 1;
                if (pos + 16 + text_len <= rbsp_size) {
                    memcpy(text_out, rbsp + pos + 16, text_len);
                    text_out[text_len] = '\0';
                    found = 1;
                }
            }
        }

        pos += payload_size;
    }

    free(rbsp);
    return found;
}

static int parse_sei_hevc(const uint8_t *nal_data, int nal_size,
                           char *text_out, int text_max)
{
    uint8_t *rbsp = malloc(nal_size);
    if (!rbsp) return 0;
    int rbsp_size = remove_epb(nal_data, nal_size, rbsp, nal_size);

    /* Skip NAL header (2 bytes for HEVC) */
    int pos = 2;
    int found = 0;

    while (pos < rbsp_size - 1) {
        int payload_type = 0;
        while (pos < rbsp_size && rbsp[pos] == 0xFF) {
            payload_type += 255;
            pos++;
        }
        if (pos >= rbsp_size) break;
        payload_type += rbsp[pos++];

        int payload_size = 0;
        while (pos < rbsp_size && rbsp[pos] == 0xFF) {
            payload_size += 255;
            pos++;
        }
        if (pos >= rbsp_size) break;
        payload_size += rbsp[pos++];

        if (payload_type == 5 && payload_size >= 16) {
            if (pos + 16 <= rbsp_size &&
                memcmp(rbsp + pos, fingerprint_uuid, 16) == 0) {
                int text_len = payload_size - 16;
                if (text_len > text_max - 1)
                    text_len = text_max - 1;
                if (pos + 16 + text_len <= rbsp_size) {
                    memcpy(text_out, rbsp + pos + 16, text_len);
                    text_out[text_len] = '\0';
                    found = 1;
                }
            }
        }

        pos += payload_size;
    }

    free(rbsp);
    return found;
}

int main(int argc, char *argv[])
{
    int is_hevc = 0;
    const char *input_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hevc") == 0 || strcmp(argv[i], "-265") == 0) {
            is_hevc = 1;
        } else if (strcmp(argv[i], "--h264") == 0 || strcmp(argv[i], "-264") == 0) {
            is_hevc = 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "SEI Fingerprint Reader\n"
                "Usage: %s [--h264|--hevc] [input_file]\n"
                "  Reads raw H.264/H.265 bitstream from file or stdin\n"
                "  and extracts fingerprint SEI data.\n\n"
                "  --h264    Treat as H.264 (default)\n"
                "  --hevc    Treat as H.265/HEVC\n\n"
                "Example:\n"
                "  ffmpeg -i stream.ts -c:v copy -f h264 pipe:1 | %s\n"
                "  ffmpeg -i stream.ts -c:v copy -f hevc pipe:1 | %s --hevc\n",
                argv[0], argv[0], argv[0]);
            return 0;
        } else {
            input_file = argv[i];
        }
    }

    FILE *fp = stdin;
    if (input_file) {
        fp = fopen(input_file, "rb");
        if (!fp) {
            fprintf(stderr, "Cannot open %s\n", input_file);
            return 1;
        }
    }

    uint8_t *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    int buf_len = 0;
    int total_fingerprints = 0;
    char text[512];

    fprintf(stderr, "Reading %s bitstream...\n", is_hevc ? "HEVC" : "H.264");

    while (1) {
        /* Read more data */
        int space = BUFFER_SIZE - buf_len;
        if (space > 0) {
            int r = fread(buffer + buf_len, 1, space, fp);
            if (r <= 0 && buf_len == 0) break;
            buf_len += r;
        }

        /* Find NAL units */
        int offset = 0;
        while (offset < buf_len - 4) {
            int sc_len;
            int pos = find_start_code(buffer + offset, buf_len - offset, &sc_len);
            if (pos < 0) break;

            int nal_start = offset + pos + sc_len;

            /* Find the end of this NAL (next start code) */
            int next_sc_len;
            int next_pos = find_start_code(buffer + nal_start,
                                            buf_len - nal_start, &next_sc_len);
            if (next_pos < 0) {
                /* NAL extends beyond buffer - need more data */
                break;
            }

            int nal_size = next_pos;
            uint8_t *nal_data = buffer + nal_start;

            /* Check NAL type */
            int is_sei = 0;
            if (is_hevc) {
                int nal_type = (nal_data[0] >> 1) & 0x3F;
                is_sei = (nal_type == 39 || nal_type == 40); /* PREFIX/SUFFIX SEI */
            } else {
                int nal_type = nal_data[0] & 0x1F;
                is_sei = (nal_type == 6); /* SEI */
            }

            if (is_sei) {
                int found;
                if (is_hevc)
                    found = parse_sei_hevc(nal_data, nal_size, text, sizeof(text));
                else
                    found = parse_sei_h264(nal_data, nal_size, text, sizeof(text));

                if (found) {
                    total_fingerprints++;
                    printf("[FINGERPRINT #%d] %s\n", total_fingerprints, text);
                    fflush(stdout);
                }
            }

            offset = nal_start + nal_size;
        }

        /* Compact buffer - keep unprocessed data */
        if (offset > 0 && offset < buf_len) {
            memmove(buffer, buffer + offset, buf_len - offset);
            buf_len -= offset;
        } else if (offset >= buf_len) {
            buf_len = 0;
        }

        /* If buffer is full but we can't find a complete NAL, skip some data */
        if (buf_len >= BUFFER_SIZE - 4096) {
            memmove(buffer, buffer + buf_len / 2, buf_len / 2);
            buf_len = buf_len / 2;
        }
    }

    fprintf(stderr, "Found %d fingerprints total\n", total_fingerprints);

    free(buffer);
    if (fp != stdin) fclose(fp);

    return 0;
}

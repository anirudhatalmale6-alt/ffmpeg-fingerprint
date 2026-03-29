/*
 * FFmpeg Bitstream Filter: fingerprint_inject
 *
 * Injects fingerprint data into H.264/H.265 streams as SEI NAL units
 * without re-encoding. Works with -c:v copy.
 *
 * For H.264: Injects SEI type 5 (user_data_unregistered) before keyframes
 * For H.265: Injects PREFIX_SEI_NUT (type 39) before keyframes
 *
 * Automatic mode (single command, no Python trigger needed):
 *   ffmpeg -i input -c:v copy -c:a copy \
 *     -bsf:v fingerprint_inject=text=USERNAME:show_duration=300:hide_duration=600 \
 *     -f mpegts output
 *
 * Always-on mode (fingerprint always visible):
 *   ffmpeg -i input -c:v copy -c:a copy \
 *     -bsf:v fingerprint_inject=text=USERNAME \
 *     -f mpegts output
 *
 * ZMQ dynamic mode (controlled by external script):
 *   ffmpeg -i input -c:v copy -c:a copy \
 *     -bsf:v fingerprint_inject=zmq_addr=tcp\\://127.0.0.1\\:5555 \
 *     -f mpegts output
 *
 * Options:
 *   text=STRING          Username/fingerprint text (auto-fetched from your DB)
 *   show_duration=N      Show for N seconds, then hide (0=always on)
 *   hide_duration=N      Hide for N seconds between shows (0=never hide)
 *   zmq_addr=ADDR        Optional ZMQ for external control
 *   inject_interval=N    Inject every N keyframes (0=every keyframe)
 *
 * Copyright (c) 2026 - Custom FFmpeg Plugin
 */

#include "libavcodec/bsf.h"
#include "libavcodec/bsf_internal.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avutil.h"

#include <string.h>
#include <pthread.h>
#include <zmq.h>

/* ------------------------------------------------------------------ */
/*  NAL unit type constants                                           */
/* ------------------------------------------------------------------ */

/* H.264 NAL types */
#define H264_NAL_SEI            6
#define H264_NAL_IDR            5
#define H264_NAL_SLICE          1
#define H264_NAL_SPS            7
#define H264_NAL_PPS            8

/* H.265 NAL types */
#define HEVC_NAL_PREFIX_SEI     39
#define HEVC_NAL_SUFFIX_SEI     40
#define HEVC_NAL_IDR_W_RADL    19
#define HEVC_NAL_IDR_N_LP      20
#define HEVC_NAL_CRA           21
#define HEVC_NAL_BLA_W_LP      16
#define HEVC_NAL_BLA_W_RADL    17
#define HEVC_NAL_BLA_N_LP      18
#define HEVC_NAL_VPS           32
#define HEVC_NAL_SPS           33
#define HEVC_NAL_PPS           34

/* SEI payload type for user_data_unregistered */
#define SEI_TYPE_USER_DATA_UNREGISTERED 5

/* Maximum fingerprint text length */
#define MAX_FINGERPRINT_LEN 512

/* Start code */
static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

/* UUID for our fingerprint SEI (randomly generated, fixed) */
static const uint8_t fingerprint_uuid[16] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
    0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90
};

/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */

typedef struct FingerprintBSFContext {
    const AVClass *class;

    /* Options */
    char *zmq_addr;
    char *initial_text;
    int   inject_interval;  /* inject every N keyframes (0 = every keyframe) */
    int   show_duration;    /* how long to show fingerprint (seconds), 0=always */
    int   hide_duration;    /* how long to hide between shows (seconds), 0=never hide */
    int   auto_cycle;       /* 1=auto show/hide cycle, 0=manual/always-on */

    /* Runtime state */
    char fingerprint_text[MAX_FINGERPRINT_LEN];
    int  text_active;       /* whether fingerprint is currently active */
    int  keyframe_count;
    int64_t first_pts;      /* PTS of first packet (for timing) */
    int64_t cycle_start_pts;/* PTS when current show/hide cycle started */
    int  cycle_showing;     /* 1=currently in "show" phase, 0=in "hide" phase */
    int  position_index;    /* current position (0-8), -1=random each cycle */
    int  pts_initialized;

    /* ZMQ */
    void *zmq_ctx;
    void *zmq_socket;
    pthread_t zmq_thread;
    pthread_mutex_t text_mutex;
    volatile int zmq_running;

    /* Codec detection */
    int is_hevc;
    int annexb;  /* 1 = annex-b (start codes), 0 = length-prefixed (mp4) */
} FingerprintBSFContext;

/* ------------------------------------------------------------------ */
/*  ZMQ listener thread                                               */
/* ------------------------------------------------------------------ */

/*
 * ZMQ message protocol:
 *   "SHOW <text>"    - activate fingerprint with given text
 *   "HIDE"           - deactivate fingerprint
 *   "TEXT <text>"    - update text without changing active state
 *   "STATUS"         - reply with current status
 */
static void *zmq_listener_thread(void *arg)
{
    FingerprintBSFContext *ctx = (FingerprintBSFContext *)arg;
    char buf[MAX_FINGERPRINT_LEN + 64];

    while (ctx->zmq_running) {
        /* Poll with timeout so we can check zmq_running */
        zmq_pollitem_t items[] = {{ ctx->zmq_socket, 0, ZMQ_POLLIN, 0 }};
        int rc = zmq_poll(items, 1, 100); /* 100ms timeout */

        if (rc <= 0)
            continue;

        if (!(items[0].revents & ZMQ_POLLIN))
            continue;

        int len = zmq_recv(ctx->zmq_socket, buf, sizeof(buf) - 1, 0);
        if (len < 0)
            continue;
        buf[len] = '\0';

        char reply[256] = "OK";

        pthread_mutex_lock(&ctx->text_mutex);

        if (strncmp(buf, "SHOW ", 5) == 0) {
            strncpy(ctx->fingerprint_text, buf + 5, MAX_FINGERPRINT_LEN - 1);
            ctx->fingerprint_text[MAX_FINGERPRINT_LEN - 1] = '\0';
            ctx->text_active = 1;
            av_log(NULL, AV_LOG_INFO,
                   "fingerprint_inject: SHOW '%s'\n", ctx->fingerprint_text);

        } else if (strcmp(buf, "HIDE") == 0) {
            ctx->text_active = 0;
            ctx->fingerprint_text[0] = '\0';
            av_log(NULL, AV_LOG_INFO, "fingerprint_inject: HIDE\n");

        } else if (strncmp(buf, "TEXT ", 4) == 0) {
            strncpy(ctx->fingerprint_text, buf + 4, MAX_FINGERPRINT_LEN - 1);
            ctx->fingerprint_text[MAX_FINGERPRINT_LEN - 1] = '\0';
            av_log(NULL, AV_LOG_INFO,
                   "fingerprint_inject: TEXT '%s'\n", ctx->fingerprint_text);

        } else if (strcmp(buf, "STATUS") == 0) {
            snprintf(reply, sizeof(reply), "active=%d text=%s",
                     ctx->text_active, ctx->fingerprint_text);

        } else {
            snprintf(reply, sizeof(reply), "ERR unknown command");
        }

        pthread_mutex_unlock(&ctx->text_mutex);

        zmq_send(ctx->zmq_socket, reply, strlen(reply), 0);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  SEI NAL unit construction                                         */
/* ------------------------------------------------------------------ */

/*
 * Build a user_data_unregistered SEI payload:
 *   payload_type = 5
 *   payload_size = 16 (UUID) + text_len
 *   UUID (16 bytes)
 *   user_data (text bytes)
 *
 * Returns the complete SEI NAL unit (with start code) in *out_buf.
 * Caller must av_free() the buffer.
 */
static int build_sei_nal_h264(const char *text, uint8_t **out_buf, int *out_size)
{
    int text_len = strlen(text);
    int payload_size = 16 + text_len; /* UUID + text */

    /* Calculate SEI RBSP size:
     * payload_type bytes + payload_size bytes + payload + rbsp_trailing_bits
     */
    int sei_size_estimate = 1 + 4 + 4 + payload_size + 2;
    uint8_t *sei_rbsp = av_malloc(sei_size_estimate);
    if (!sei_rbsp)
        return AVERROR(ENOMEM);

    int pos = 0;

    /* payload_type = 5 (user_data_unregistered) */
    sei_rbsp[pos++] = SEI_TYPE_USER_DATA_UNREGISTERED;

    /* payload_size (variable length: 0xFF bytes then remainder) */
    int remaining = payload_size;
    while (remaining >= 255) {
        sei_rbsp[pos++] = 0xFF;
        remaining -= 255;
    }
    sei_rbsp[pos++] = (uint8_t)remaining;

    /* UUID */
    memcpy(sei_rbsp + pos, fingerprint_uuid, 16);
    pos += 16;

    /* User data (fingerprint text) */
    memcpy(sei_rbsp + pos, text, text_len);
    pos += text_len;

    /* RBSP trailing bits: bit 1 followed by alignment zeros */
    sei_rbsp[pos++] = 0x80;

    int rbsp_size = pos;

    /* Now build the full NAL unit with emulation prevention */
    /* Worst case: every 2 bytes need a 0x03 escape */
    int max_nal_size = 4 + 1 + rbsp_size * 3 / 2 + 4;
    uint8_t *nal = av_malloc(max_nal_size);
    if (!nal) {
        av_free(sei_rbsp);
        return AVERROR(ENOMEM);
    }

    int nal_pos = 0;

    /* Start code */
    memcpy(nal + nal_pos, start_code, 4);
    nal_pos += 4;

    /* NAL header: forbidden_zero_bit(0) + nal_ref_idc(0) + nal_unit_type(6=SEI) */
    nal[nal_pos++] = H264_NAL_SEI; /* 0x06 */

    /* Copy RBSP with emulation prevention (escape 00 00 00/01/02/03) */
    int zero_count = 0;
    for (int i = 0; i < rbsp_size; i++) {
        if (zero_count >= 2 && sei_rbsp[i] <= 0x03) {
            nal[nal_pos++] = 0x03; /* emulation prevention byte */
            zero_count = 0;
        }
        nal[nal_pos++] = sei_rbsp[i];
        if (sei_rbsp[i] == 0x00)
            zero_count++;
        else
            zero_count = 0;
    }

    av_free(sei_rbsp);

    *out_buf = nal;
    *out_size = nal_pos;
    return 0;
}

static int build_sei_nal_hevc(const char *text, uint8_t **out_buf, int *out_size)
{
    int text_len = strlen(text);
    int payload_size = 16 + text_len;

    int sei_size_estimate = 1 + 4 + 4 + payload_size + 2;
    uint8_t *sei_rbsp = av_malloc(sei_size_estimate);
    if (!sei_rbsp)
        return AVERROR(ENOMEM);

    int pos = 0;

    /* payload_type = 5 */
    sei_rbsp[pos++] = SEI_TYPE_USER_DATA_UNREGISTERED;

    int remaining = payload_size;
    while (remaining >= 255) {
        sei_rbsp[pos++] = 0xFF;
        remaining -= 255;
    }
    sei_rbsp[pos++] = (uint8_t)remaining;

    /* UUID */
    memcpy(sei_rbsp + pos, fingerprint_uuid, 16);
    pos += 16;

    /* User data */
    memcpy(sei_rbsp + pos, text, text_len);
    pos += text_len;

    /* RBSP trailing bits */
    sei_rbsp[pos++] = 0x80;

    int rbsp_size = pos;

    int max_nal_size = 4 + 2 + rbsp_size * 3 / 2 + 4;
    uint8_t *nal = av_malloc(max_nal_size);
    if (!nal) {
        av_free(sei_rbsp);
        return AVERROR(ENOMEM);
    }

    int nal_pos = 0;

    /* Start code */
    memcpy(nal + nal_pos, start_code, 4);
    nal_pos += 4;

    /* HEVC NAL header (2 bytes):
     * forbidden_zero_bit(1) + nal_unit_type(6) + nuh_layer_id(6) + nuh_temporal_id_plus1(3)
     * PREFIX_SEI_NUT = 39, layer_id=0, temporal_id=1
     */
    nal[nal_pos++] = (HEVC_NAL_PREFIX_SEI << 1); /* 0x4E */
    nal[nal_pos++] = 0x01; /* nuh_temporal_id_plus1 = 1 */

    /* Copy RBSP with emulation prevention */
    int zero_count = 0;
    for (int i = 0; i < rbsp_size; i++) {
        if (zero_count >= 2 && sei_rbsp[i] <= 0x03) {
            nal[nal_pos++] = 0x03;
            zero_count = 0;
        }
        nal[nal_pos++] = sei_rbsp[i];
        if (sei_rbsp[i] == 0x00)
            zero_count++;
        else
            zero_count = 0;
    }

    av_free(sei_rbsp);

    *out_buf = nal;
    *out_size = nal_pos;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Annex-B NAL unit scanning                                         */
/* ------------------------------------------------------------------ */

/* Find the next start code (00 00 01 or 00 00 00 01) in data */
static int find_start_code(const uint8_t *data, int size, int *sc_len)
{
    for (int i = 0; i < size - 2; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (i + 2 < size && data[i + 2] == 0x01) {
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

/* Check if H.264 packet contains a keyframe */
static int h264_is_keyframe(const uint8_t *data, int size)
{
    int offset = 0;
    while (offset < size - 4) {
        int sc_len;
        int pos = find_start_code(data + offset, size - offset, &sc_len);
        if (pos < 0)
            break;
        int nal_start = offset + pos + sc_len;
        if (nal_start >= size)
            break;
        int nal_type = data[nal_start] & 0x1F;
        if (nal_type == H264_NAL_IDR)
            return 1;
        offset = nal_start + 1;
    }
    return 0;
}

/* Check if H.265 packet contains a keyframe */
static int hevc_is_keyframe(const uint8_t *data, int size)
{
    int offset = 0;
    while (offset < size - 5) {
        int sc_len;
        int pos = find_start_code(data + offset, size - offset, &sc_len);
        if (pos < 0)
            break;
        int nal_start = offset + pos + sc_len;
        if (nal_start >= size)
            break;
        int nal_type = (data[nal_start] >> 1) & 0x3F;
        if (nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA)
            return 1;
        offset = nal_start + 1;
    }
    return 0;
}

/*
 * Find the insertion point for SEI in annex-b bitstream.
 * For H.264: after SPS/PPS, before the first slice NAL.
 * For H.265: after VPS/SPS/PPS, before the first slice NAL.
 */
static int find_sei_insert_point_h264(const uint8_t *data, int size)
{
    int offset = 0;
    int last_param_end = 0;

    while (offset < size - 4) {
        int sc_len;
        int pos = find_start_code(data + offset, size - offset, &sc_len);
        if (pos < 0)
            break;
        int nal_header_pos = offset + pos + sc_len;
        if (nal_header_pos >= size)
            break;
        int nal_type = data[nal_header_pos] & 0x1F;

        if (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS ||
            nal_type == H264_NAL_SEI) {
            /* Track end of parameter set NALs */
            last_param_end = nal_header_pos;
            /* Find the NEXT start code to get the end of this NAL */
            int next_offset = nal_header_pos + 1;
            int next_sc_len;
            int next_pos = find_start_code(data + next_offset,
                                            size - next_offset, &next_sc_len);
            if (next_pos >= 0)
                last_param_end = next_offset + next_pos;
            else
                last_param_end = size;
        }

        if (nal_type == H264_NAL_IDR || nal_type == H264_NAL_SLICE) {
            /* Insert before this slice NAL */
            return offset + pos;
        }

        offset = nal_header_pos + 1;
    }

    /* Fallback: insert at beginning */
    return 0;
}

static int find_sei_insert_point_hevc(const uint8_t *data, int size)
{
    int offset = 0;

    while (offset < size - 5) {
        int sc_len;
        int pos = find_start_code(data + offset, size - offset, &sc_len);
        if (pos < 0)
            break;
        int nal_header_pos = offset + pos + sc_len;
        if (nal_header_pos + 1 >= size)
            break;
        int nal_type = (data[nal_header_pos] >> 1) & 0x3F;

        /* Insert before any slice NAL (types 0-21) */
        if (nal_type <= HEVC_NAL_CRA) {
            return offset + pos;
        }

        offset = nal_header_pos + 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Length-prefixed (MP4/AVCC) mode helpers                            */
/* ------------------------------------------------------------------ */

static int h264_avcc_is_keyframe(const uint8_t *data, int size, int length_size)
{
    int offset = 0;
    while (offset + length_size < size) {
        int nal_len = 0;
        for (int i = 0; i < length_size; i++)
            nal_len = (nal_len << 8) | data[offset + i];
        offset += length_size;
        if (offset + nal_len > size)
            break;
        int nal_type = data[offset] & 0x1F;
        if (nal_type == H264_NAL_IDR)
            return 1;
        offset += nal_len;
    }
    return 0;
}

static int hevc_avcc_is_keyframe(const uint8_t *data, int size, int length_size)
{
    int offset = 0;
    while (offset + length_size < size) {
        int nal_len = 0;
        for (int i = 0; i < length_size; i++)
            nal_len = (nal_len << 8) | data[offset + i];
        offset += length_size;
        if (offset + nal_len > size)
            break;
        int nal_type = (data[offset] >> 1) & 0x3F;
        if (nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA)
            return 1;
        offset += nal_len;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  BSF init / filter / close                                         */
/* ------------------------------------------------------------------ */

static int fingerprint_bsf_init(AVBSFContext *bsf)
{
    FingerprintBSFContext *ctx = bsf->priv_data;
    int ret;

    /* Detect codec */
    ctx->is_hevc = (bsf->par_in->codec_id == AV_CODEC_ID_HEVC);

    /* Detect annex-b vs length-prefixed */
    if (bsf->par_in->extradata_size > 0 && bsf->par_in->extradata) {
        /* If extradata starts with 0x01 (version), it's AVCC/HVCC format */
        ctx->annexb = (bsf->par_in->extradata[0] != 0x01);
    } else {
        ctx->annexb = 1; /* default to annex-b for MPEG-TS */
    }

    av_log(bsf, AV_LOG_INFO,
           "fingerprint_inject: codec=%s format=%s\n",
           ctx->is_hevc ? "HEVC" : "H.264",
           ctx->annexb ? "annex-b" : "length-prefixed");

    /* Initialize text */
    pthread_mutex_init(&ctx->text_mutex, NULL);
    ctx->text_active = 0;
    ctx->fingerprint_text[0] = '\0';
    ctx->keyframe_count = 0;
    ctx->first_pts = AV_NOPTS_VALUE;
    ctx->cycle_start_pts = 0;
    ctx->cycle_showing = 0;
    ctx->pts_initialized = 0;
    ctx->position_index = -1;

    if (ctx->initial_text && ctx->initial_text[0]) {
        strncpy(ctx->fingerprint_text, ctx->initial_text, MAX_FINGERPRINT_LEN - 1);
        ctx->fingerprint_text[MAX_FINGERPRINT_LEN - 1] = '\0';
        ctx->text_active = 1;

        /* Auto-cycle mode: if show_duration or hide_duration set */
        if (ctx->show_duration > 0 || ctx->hide_duration > 0) {
            ctx->auto_cycle = 1;
            ctx->cycle_showing = 1; /* start in "show" phase */
            av_log(bsf, AV_LOG_INFO,
                   "fingerprint_inject: auto-cycle mode: show=%ds hide=%ds\n",
                   ctx->show_duration, ctx->hide_duration);
        }

        av_log(bsf, AV_LOG_INFO,
               "fingerprint_inject: text='%s' active=%d\n",
               ctx->fingerprint_text, ctx->text_active);
    }

    /* Initialize ZMQ if address provided */
    ctx->zmq_running = 0;
    if (ctx->zmq_addr && ctx->zmq_addr[0]) {
        ctx->zmq_ctx = zmq_ctx_new();
        if (!ctx->zmq_ctx) {
            av_log(bsf, AV_LOG_ERROR,
                   "fingerprint_inject: failed to create ZMQ context\n");
            return AVERROR_EXTERNAL;
        }

        ctx->zmq_socket = zmq_socket(ctx->zmq_ctx, ZMQ_REP);
        if (!ctx->zmq_socket) {
            av_log(bsf, AV_LOG_ERROR,
                   "fingerprint_inject: failed to create ZMQ socket\n");
            zmq_ctx_destroy(ctx->zmq_ctx);
            return AVERROR_EXTERNAL;
        }

        ret = zmq_bind(ctx->zmq_socket, ctx->zmq_addr);
        if (ret != 0) {
            av_log(bsf, AV_LOG_ERROR,
                   "fingerprint_inject: failed to bind ZMQ to %s: %s\n",
                   ctx->zmq_addr, zmq_strerror(zmq_errno()));
            zmq_close(ctx->zmq_socket);
            zmq_ctx_destroy(ctx->zmq_ctx);
            return AVERROR_EXTERNAL;
        }

        av_log(bsf, AV_LOG_INFO,
               "fingerprint_inject: ZMQ listening on %s\n", ctx->zmq_addr);

        ctx->zmq_running = 1;
        ret = pthread_create(&ctx->zmq_thread, NULL,
                             zmq_listener_thread, ctx);
        if (ret != 0) {
            av_log(bsf, AV_LOG_ERROR,
                   "fingerprint_inject: failed to create ZMQ thread\n");
            zmq_close(ctx->zmq_socket);
            zmq_ctx_destroy(ctx->zmq_ctx);
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

static int fingerprint_bsf_filter(AVBSFContext *bsf, AVPacket *pkt)
{
    FingerprintBSFContext *ctx = bsf->priv_data;
    int ret;

    ret = ff_bsf_get_packet_ref(bsf, pkt);
    if (ret < 0)
        return ret;

    /* Check if this is a keyframe */
    int is_keyframe = 0;
    if (pkt->flags & AV_PKT_FLAG_KEY) {
        is_keyframe = 1;
    } else if (ctx->annexb) {
        is_keyframe = ctx->is_hevc ?
            hevc_is_keyframe(pkt->data, pkt->size) :
            h264_is_keyframe(pkt->data, pkt->size);
    } else {
        int length_size = 4; /* default AVCC/HVCC length size */
        if (bsf->par_in->extradata_size > 0) {
            if (ctx->is_hevc) {
                if (bsf->par_in->extradata_size >= 22)
                    length_size = (bsf->par_in->extradata[21] & 0x03) + 1;
            } else {
                if (bsf->par_in->extradata_size >= 5)
                    length_size = (bsf->par_in->extradata[4] & 0x03) + 1;
            }
        }
        is_keyframe = ctx->is_hevc ?
            hevc_avcc_is_keyframe(pkt->data, pkt->size, length_size) :
            h264_avcc_is_keyframe(pkt->data, pkt->size, length_size);
    }

    if (!is_keyframe)
        return 0; /* pass through non-keyframe packets unchanged */

    ctx->keyframe_count++;

    /* Initialize PTS tracking */
    if (!ctx->pts_initialized && pkt->pts != AV_NOPTS_VALUE) {
        ctx->first_pts = pkt->pts;
        ctx->cycle_start_pts = pkt->pts;
        ctx->pts_initialized = 1;
    }

    /* Auto-cycle: compute show/hide state based on PTS timing */
    if (ctx->auto_cycle && ctx->pts_initialized && pkt->pts != AV_NOPTS_VALUE) {
        /* Assume 90kHz PTS timebase (standard for MPEG-TS) */
        int64_t elapsed_ticks = pkt->pts - ctx->cycle_start_pts;
        double elapsed_sec = (double)elapsed_ticks / 90000.0;

        if (ctx->cycle_showing) {
            /* Currently showing - check if show_duration has passed */
            if (ctx->show_duration > 0 && elapsed_sec >= ctx->show_duration) {
                if (ctx->hide_duration > 0) {
                    /* Transition to hide phase */
                    ctx->cycle_showing = 0;
                    ctx->cycle_start_pts = pkt->pts;
                    av_log(bsf, AV_LOG_INFO,
                           "fingerprint_inject: auto-hide (showed for %ds)\n",
                           ctx->show_duration);
                } else {
                    /* No hide duration - pick new random position, restart show */
                    ctx->cycle_start_pts = pkt->pts;
                    ctx->position_index = -1; /* will re-randomize */
                    av_log(bsf, AV_LOG_INFO,
                           "fingerprint_inject: repositioning\n");
                }
            }
        } else {
            /* Currently hidden - check if hide_duration has passed */
            if (ctx->hide_duration > 0 && elapsed_sec >= ctx->hide_duration) {
                /* Transition to show phase with new random position */
                ctx->cycle_showing = 1;
                ctx->cycle_start_pts = pkt->pts;
                ctx->position_index = -1; /* new random position */
                av_log(bsf, AV_LOG_INFO,
                       "fingerprint_inject: auto-show (hidden for %ds)\n",
                       ctx->hide_duration);
            }
        }
    }

    /* Check interval */
    if (ctx->inject_interval > 0 &&
        (ctx->keyframe_count % ctx->inject_interval) != 1)
        return 0;

    /* Get current fingerprint text */
    pthread_mutex_lock(&ctx->text_mutex);
    int active = ctx->text_active;
    char text[MAX_FINGERPRINT_LEN];
    if (active)
        strncpy(text, ctx->fingerprint_text, MAX_FINGERPRINT_LEN);
    pthread_mutex_unlock(&ctx->text_mutex);

    /* In auto-cycle mode, respect the show/hide state */
    if (ctx->auto_cycle && !ctx->cycle_showing)
        active = 0;

    if (!active || text[0] == '\0')
        return 0; /* no fingerprint to inject */

    /* Build SEI NAL unit */
    uint8_t *sei_nal = NULL;
    int sei_nal_size = 0;

    if (ctx->is_hevc)
        ret = build_sei_nal_hevc(text, &sei_nal, &sei_nal_size);
    else
        ret = build_sei_nal_h264(text, &sei_nal, &sei_nal_size);

    if (ret < 0)
        return ret;

    if (ctx->annexb) {
        /* Annex-B: insert SEI NAL before the first slice NAL */
        int insert_pos;
        if (ctx->is_hevc)
            insert_pos = find_sei_insert_point_hevc(pkt->data, pkt->size);
        else
            insert_pos = find_sei_insert_point_h264(pkt->data, pkt->size);

        /* Create new packet with SEI inserted */
        int new_size = pkt->size + sei_nal_size;
        uint8_t *new_data = av_malloc(new_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!new_data) {
            av_free(sei_nal);
            return AVERROR(ENOMEM);
        }

        memcpy(new_data, pkt->data, insert_pos);
        memcpy(new_data + insert_pos, sei_nal, sei_nal_size);
        memcpy(new_data + insert_pos + sei_nal_size,
               pkt->data + insert_pos, pkt->size - insert_pos);
        memset(new_data + new_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        /* Replace packet data */
        av_buffer_unref(&pkt->buf);
        pkt->buf = av_buffer_create(new_data, new_size + AV_INPUT_BUFFER_PADDING_SIZE,
                                     av_buffer_default_free, NULL, 0);
        if (!pkt->buf) {
            av_free(new_data);
            av_free(sei_nal);
            return AVERROR(ENOMEM);
        }
        pkt->data = new_data;
        pkt->size = new_size;
    } else {
        /* Length-prefixed: prepend SEI as a new NAL unit with length prefix */
        int length_size = 4;
        if (bsf->par_in->extradata_size > 0) {
            if (ctx->is_hevc) {
                if (bsf->par_in->extradata_size >= 22)
                    length_size = (bsf->par_in->extradata[21] & 0x03) + 1;
            } else {
                if (bsf->par_in->extradata_size >= 5)
                    length_size = (bsf->par_in->extradata[4] & 0x03) + 1;
            }
        }

        /* SEI NAL content (skip the annex-b start code) */
        int sei_content_size = sei_nal_size - 4;
        int new_nal_size = length_size + sei_content_size;
        int new_size = new_nal_size + pkt->size;

        uint8_t *new_data = av_malloc(new_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!new_data) {
            av_free(sei_nal);
            return AVERROR(ENOMEM);
        }

        /* Write length prefix for SEI NAL */
        for (int i = length_size - 1; i >= 0; i--) {
            new_data[length_size - 1 - i] = (sei_content_size >> (i * 8)) & 0xFF;
        }

        /* Write SEI NAL content (skip start code) */
        memcpy(new_data + length_size, sei_nal + 4, sei_content_size);

        /* Append original packet data */
        memcpy(new_data + new_nal_size, pkt->data, pkt->size);
        memset(new_data + new_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        av_buffer_unref(&pkt->buf);
        pkt->buf = av_buffer_create(new_data, new_size + AV_INPUT_BUFFER_PADDING_SIZE,
                                     av_buffer_default_free, NULL, 0);
        if (!pkt->buf) {
            av_free(new_data);
            av_free(sei_nal);
            return AVERROR(ENOMEM);
        }
        pkt->data = new_data;
        pkt->size = new_size;
    }

    av_free(sei_nal);

    av_log(bsf, AV_LOG_DEBUG,
           "fingerprint_inject: injected SEI [%s] into keyframe #%d\n",
           text, ctx->keyframe_count);

    return 0;
}

static void fingerprint_bsf_close(AVBSFContext *bsf)
{
    FingerprintBSFContext *ctx = bsf->priv_data;

    /* Stop ZMQ thread */
    if (ctx->zmq_running) {
        ctx->zmq_running = 0;
        pthread_join(ctx->zmq_thread, NULL);
        zmq_close(ctx->zmq_socket);
        zmq_ctx_destroy(ctx->zmq_ctx);
    }

    pthread_mutex_destroy(&ctx->text_mutex);
}

/* ------------------------------------------------------------------ */
/*  Options                                                           */
/* ------------------------------------------------------------------ */

#define OFFSET(x) offsetof(FingerprintBSFContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_BSF_PARAM)

static const AVOption fingerprint_options[] = {
    { "zmq_addr", "ZeroMQ bind address for dynamic control",
      OFFSET(zmq_addr), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "text", "Fingerprint text (username from DB)",
      OFFSET(initial_text), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "show_duration", "Show fingerprint for N seconds (0=always visible)",
      OFFSET(show_duration), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 86400, FLAGS },
    { "hide_duration", "Hide fingerprint for N seconds between shows (0=never hide)",
      OFFSET(hide_duration), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 86400, FLAGS },
    { "inject_interval", "Inject every N keyframes (0=every keyframe)",
      OFFSET(inject_interval), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1000, FLAGS },
    { NULL }
};

static const AVClass fingerprint_class = {
    .class_name = "fingerprint_inject",
    .item_name  = av_default_item_name,
    .option     = fingerprint_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static enum AVCodecID fingerprint_codec_ids[] = {
    AV_CODEC_ID_H264,
    AV_CODEC_ID_HEVC,
    AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_fingerprint_inject_bsf = {
    .p.name         = "fingerprint_inject",
    .p.codec_ids    = fingerprint_codec_ids,
    .p.priv_class   = &fingerprint_class,
    .priv_data_size = sizeof(FingerprintBSFContext),
    .init           = fingerprint_bsf_init,
    .filter         = fingerprint_bsf_filter,
    .close          = fingerprint_bsf_close,
};

/*
 * demuxer.c — đọc device_meta + từng video_packet từ video socket (xem docs/PROTOCOL.md §2,§3).
 * Phase 0: khung + đọc meta; đọc packet hoàn thiện ở Phase 2.
 */
#include "rc_internal.h"

#include <stdlib.h>
#include <string.h>

/* Ghép big-endian từ buffer. */
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

rc_status rc_demux_read_meta(int fd, rc_device_meta *out) {
    if (!out) return RC_ERR_INVALID_ARG;
    uint8_t hdr[4 + 2 + 4 + 2 + 2 + RC_DEVICE_NAME_LEN];
    rc_status r = rc_net_read_full(fd, hdr, sizeof hdr);
    if (r != RC_OK) return r;

    out->magic = be32(hdr + 0);
    out->version = be16(hdr + 4);
    out->codec_id = be32(hdr + 6);
    out->width = be16(hdr + 10);
    out->height = be16(hdr + 12);
    memcpy(out->device_name, hdr + 14, RC_DEVICE_NAME_LEN);
    out->device_name[RC_DEVICE_NAME_LEN - 1] = '\0';

    if (out->magic != RC_META_MAGIC) return RC_ERR_PROTOCOL;
    return RC_OK;
}

rc_status rc_demux_read_audio_meta(int fd, rc_audio_meta *out) {
    if (!out) return RC_ERR_INVALID_ARG;
    uint8_t hdr[4 + 4 + 4 + 1];
    rc_status r = rc_net_read_full(fd, hdr, sizeof hdr);
    if (r != RC_OK) return r;

    out->magic = be32(hdr + 0);
    out->codec_id = be32(hdr + 4);
    out->sample_rate = be32(hdr + 8);
    out->channels = hdr[12];

    if (out->magic != RC_AUDIO_MAGIC) return RC_ERR_PROTOCOL;
    return RC_OK;
}

rc_status rc_demux_read_packet(int fd, uint8_t **buf, size_t *cap, size_t *out_len, int *is_config,
                               int *is_key, int64_t *pts_us) {
    uint8_t hdr[8 + 4];
    rc_status r = rc_net_read_full(fd, hdr, sizeof hdr);
    if (r != RC_OK) return r;

    uint64_t flags = be64(hdr);
    uint32_t len = be32(hdr + 8);

    if (is_config) *is_config = (flags & RC_PKT_FLAG_CONFIG) ? 1 : 0;
    if (is_key) *is_key = (flags & RC_PKT_FLAG_KEYFRAME) ? 1 : 0;
    if (pts_us) *pts_us = (int64_t)(flags & RC_PKT_PTS_MASK);

    if (len == 0 || len > (32u << 20)) return RC_ERR_PROTOCOL; /* chặn packet vô lý */
    if (*cap < len) {
        uint8_t *nb = realloc(*buf, len);
        if (!nb) return RC_ERR_NOMEM;
        *buf = nb;
        *cap = len;
    }

    r = rc_net_read_full(fd, *buf, len);
    if (r != RC_OK) return r;

    *out_len = len;
    return RC_OK;
}

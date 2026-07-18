/*
 * miniaudio_impl.c — translation unit chứa bản cài đặt của miniaudio (single-header).
 *
 * Tách riêng để phần cài đặt ~95k dòng chỉ biên dịch một lần, không dựng lại mỗi khi sửa
 * audio.c. Ta chỉ dùng device playback + ring buffer (ma_pcm_rb) nên tắt các phân hệ không cần
 * (decoding/encoding/generation/resource manager/node graph/engine) để giảm thời gian build và
 * bề mặt phụ thuộc.
 */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#include "miniaudio.h"

/* control_msg.c — gửi buffer control đã serialize (bởi client.c) ra control socket. */
#include "rc_internal.h"

rc_status rc_control_send(rc_client *c, const uint8_t *buf, size_t len) {
    if (!c) return RC_ERR_INVALID_ARG;
    if (!c->cfg.control) return RC_ERR_INVALID_ARG; /* kênh điều khiển bị tắt */
    if (c->control_fd < 0) return RC_ERR_CONNECT;   /* chưa kết nối */
    return rc_net_write_full(c->control_fd, buf, len);
}

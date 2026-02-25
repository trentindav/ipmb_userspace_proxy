#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <linux/fuse.h>
#include <linux/ioctl.h>
#include <linux/ipmi.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef IPMI_CHANNEL_INFO_DEFINED
struct ipmi_channel_info {
	uint8_t channel;
	uint8_t medium_type;
	uint8_t protocol_type;
	uint8_t session_supported;
	uint8_t active_sessions;
	uint8_t protocol;
	uint8_t reserved[2];
};
#endif

#define IPMI_MAX_DATA_LEN 1024

struct ipmi_devintf_state {
	unsigned char my_addr;
	unsigned char my_lun;
	bool gets_events;
};

struct ipmi_pending_resp {
	bool valid;
	long msgid;
	unsigned char netfn;
	unsigned char cmd;
	unsigned int addr_len;
	unsigned char addr[IPMI_MAX_ADDR_SIZE];
	unsigned short data_len;
	unsigned char data[IPMI_MAX_DATA_LEN];
	int recv_type;
};

struct ipmi_msg32 {
	uint8_t netfn;
	uint8_t cmd;
	uint16_t data_len;
	uint32_t data;
};

struct ipmi_recv32 {
	int32_t recv_type;
	uint32_t addr;
	uint32_t addr_len;
	int32_t msgid;
	struct ipmi_msg32 msg;
};

static struct ipmi_devintf_state ipmi_state = {
	.my_addr = 0x30,
	.my_lun = 0x0,
	.gets_events = false,
};

static struct ipmi_pending_resp ipmi_pending = {
	.valid = false,
};

static void ipmi_log_ioctl(int cmd, size_t in_bufsz, size_t out_bufsz)
{
	fprintf(stderr, "ipmi0 ioctl: cmd=0x%x in_bufsz=%zu out_bufsz=%zu\n",
		cmd, in_bufsz, out_bufsz);
	fflush(stderr);
}

static bool ipmi_ioctl_retry_if_needed(fuse_req_t req,
				       const void *in_buf, size_t in_bufsz,
				       size_t out_bufsz)
{
	if (!in_buf && in_bufsz > 0) {
		struct iovec in_iov = { (void *)0, in_bufsz };
		fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
		return true;
	}

	if (out_bufsz > 0) {
		struct iovec out_iov = { (void *)0, out_bufsz };
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return true;
	}

	return false;
}

static void ipmi_reply_u8(fuse_req_t req, unsigned char value)
{
	fuse_reply_ioctl(req, 0, &value, sizeof(value));
}

static void ipmi_reply_int(fuse_req_t req, int value)
{
    fuse_reply_ioctl(req, 0, &value, sizeof(value));
}

static void ipmi_reply_channel_info(fuse_req_t req,
                    const void *in_buf, size_t in_bufsz)
{
    struct ipmi_channel_info info;

    memset(&info, 0, sizeof(info));
    if (in_buf && in_bufsz >= sizeof(info)) {
        memcpy(&info, in_buf, sizeof(info));
    } else if (in_buf && in_bufsz >= sizeof(unsigned int)) {
        info.channel = *(const unsigned int *)in_buf;
    }

    fuse_reply_ioctl(req, 0, &info, sizeof(info));
}

static void ipmi_hexdump(const char *label, const uint8_t *buf, size_t len)
{
	size_t i;

	if (!buf || len == 0) {
		fprintf(stderr, "%s: <empty>\n", label);
		return;
	}

	fprintf(stderr, "%s (%zu):", label, len);
	for (i = 0; i < len; i++)
		fprintf(stderr, " %02x", buf[i]);
	fprintf(stderr, "\n");
}

static uint8_t ipmb_checksum1(uint8_t rs_sa, uint8_t netfn_rs_lun)
{
	uint8_t csum = rs_sa;

	csum += netfn_rs_lun;
	return (uint8_t)(-csum);
}

static uint8_t ipmb_host_checksum(const uint8_t *data, size_t size, uint8_t start)
{
	uint8_t csum = start;

	for (; size > 0; size--, data++)
		csum = (uint8_t)(csum + *data);

	return (uint8_t)(-csum);
}

static bool ipmi_ioctl_get_req_buffers(fuse_req_t req, void *arg,
				       const void *in_buf, size_t in_bufsz,
				       size_t *req_size)
{
	struct iovec iov;

	*req_size = sizeof(struct ipmi_req);

	if (!in_buf || in_bufsz < *req_size) {
		iov.iov_base = arg;
		iov.iov_len = *req_size;
		fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		return true;
	}

	return false;
}

static bool ipmi_ioctl_get_req_payloads(fuse_req_t req, void *arg,
					const struct ipmi_req *req_in,
					const void *in_buf, size_t in_bufsz)
{
	size_t total = sizeof(struct ipmi_req);
	struct iovec iov[3];
	int iovcnt = 0;

	if (!req_in)
		return false;

	// if (req_in->addr_len < 0 || req_in->msg.data_len < 0)
	// 	return false;

	if (req_in->addr_len > IPMI_MAX_DATA_LEN ||
		req_in->msg.data_len > IPMI_MAX_DATA_LEN)
		return false;

	total += req_in->addr_len + req_in->msg.data_len;

	if (in_buf && in_bufsz >= total)
		return false;

	iov[iovcnt].iov_base = arg;
	iov[iovcnt].iov_len = sizeof(struct ipmi_req);
	iovcnt++;

	if (req_in->addr_len > 0) {
		iov[iovcnt].iov_base = req_in->addr;
		iov[iovcnt].iov_len = (size_t)req_in->addr_len;
		iovcnt++;
	}

	if (req_in->msg.data_len > 0) {
		iov[iovcnt].iov_base = req_in->msg.data;
		iov[iovcnt].iov_len = (size_t)req_in->msg.data_len;
		iovcnt++;
	}

	fuse_reply_ioctl_retry(req, iov, iovcnt, NULL, 0);
	return true;
}

static bool ipmi_ioctl_get_recv_buffers(fuse_req_t req, void *arg,
				       const void *in_buf, size_t in_bufsz,
				       size_t *recv_size)
{
	struct iovec iov;

	*recv_size = sizeof(struct ipmi_recv);

	if (!in_buf || in_bufsz < *recv_size) {
		iov.iov_base = arg;
		iov.iov_len = *recv_size;
		fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		return true;
	}

	return false;
}

static bool ipmi_ioctl_get_recv_buffers_size(fuse_req_t req, void *arg,
				       const void *in_buf, size_t in_bufsz,
				       size_t recv_size)
{
	struct iovec iov;

	if (!in_buf || in_bufsz < recv_size) {
		iov.iov_base = arg;
		iov.iov_len = recv_size;
		fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		return true;
	}

	return false;
}

static bool ipmi_ioctl_get_recv_payloads(fuse_req_t req, void *arg,
					const struct ipmi_recv *recv_in,
					void *addr_ptr,
					void *data_ptr,
					size_t out_bufsz,
					size_t *addr_len,
					size_t *data_len)
{
	struct iovec in_iov;
	struct iovec out_iov[3];
	int iovcnt = 0;
	size_t total = sizeof(struct ipmi_recv);

	if (!recv_in || !addr_len || !data_len)
		return false;

	*addr_len = recv_in->addr_len;
	*data_len = recv_in->msg.data_len;

	if (*addr_len > IPMI_MAX_ADDR_SIZE)
		*addr_len = IPMI_MAX_ADDR_SIZE;
	if (*data_len > IPMI_MAX_DATA_LEN)
		*data_len = IPMI_MAX_DATA_LEN;

	total += *addr_len + *data_len;
	if (out_bufsz >= total)
		return false;

	in_iov.iov_base = arg;
	in_iov.iov_len = sizeof(struct ipmi_recv);

	out_iov[iovcnt].iov_base = arg;
	out_iov[iovcnt].iov_len = sizeof(struct ipmi_recv);
	iovcnt++;

	if (*addr_len > 0) {
		out_iov[iovcnt].iov_base = addr_ptr;
		out_iov[iovcnt].iov_len = *addr_len;
		iovcnt++;
	}

	if (*data_len > 0) {
		out_iov[iovcnt].iov_base = data_ptr;
		out_iov[iovcnt].iov_len = *data_len;
		iovcnt++;
	}

	fprintf(stderr,
		"ipmi0 ioctl: retrying for recv payloads (addr_len=%zu, data_len=%zu, out_bufsz=%zu)\n",
		*addr_len, *data_len, out_bufsz);
	fuse_reply_ioctl_retry(req, &in_iov, 1, out_iov, iovcnt);
	return true;
}

static void ipmi_log_req(const struct ipmi_req *req,
			 const uint8_t *addr,
			 const uint8_t *data)
{
	if (!req)
		return;

	fprintf(stderr,
		"ipmi_req: addr_len=%d msgid=%ld netfn=0x%02x cmd=0x%02x data_len=%u\n",
		req->addr_len, req->msgid, req->msg.netfn, req->msg.cmd,
		req->msg.data_len);

	if (req->addr_len > 0)
		ipmi_hexdump("addr", addr, (size_t)req->addr_len);
	if (req->msg.data_len > 0)
		ipmi_hexdump("data", data, (size_t)req->msg.data_len);
}

void ipmi_devintf_ioctl(fuse_req_t req, unsigned long cmd, void *arg,
			struct fuse_file_info *fi, unsigned flags,
			const void *in_buf, size_t in_bufsz,
			size_t out_bufsz)
{
	(void)arg;
	(void)fi;

	ipmi_log_ioctl(cmd, in_bufsz, out_bufsz);

	// if (ipmi_ioctl_retry_if_needed(req, in_buf, in_bufsz, out_bufsz)) {
	// 	fprintf(stderr, "ipmi0 ioctl: retrying for buffers\n");
	// 	return;
	// }

	// fprintf(stderr, "IPMICTL_GET_MY_ADDRESS_CMD=0x%x\n", IPMICTL_GET_MY_ADDRESS_CMD);
	// fprintf(stderr, "IPMICTL_GET_MY_LUN_CMD=0x%x\n", IPMICTL_GET_MY_LUN_CMD);
	// fprintf(stderr, "IPMICTL_SET_MY_ADDRESS_CMD=0x%x\n", IPMICTL_SET_MY_ADDRESS_CMD);
	// fprintf(stderr, "IPMICTL_SET_MY_LUN_CMD=0x%x\n", IPMICTL_SET_MY_LUN_CMD);
	// fprintf(stderr, "IPMICTL_SET_GETS_EVENTS_CMD=0x%x\n", IPMICTL_SET_GETS_EVENTS_CMD);
	// fprintf(stderr, "IPMICTL_REGISTER_FOR_CMD=0x%x\n", IPMICTL_REGISTER_FOR_CMD);
	// fprintf(stderr, "IPMICTL_RECEIVE_MSG=0x%x\n", IPMICTL_RECEIVE_MSG);
	// fprintf(stderr, "typeof IPMICTL_SET_GETS_EVENTS_CMD=%x, typeof cmd=%x\n", IPMICTL_SET_GETS_EVENTS_CMD, cmd);

	if (cmd == IPMICTL_GET_MY_ADDRESS_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_GET_MY_ADDRESS_CMD\n");
        ipmi_reply_u8(req, ipmi_state.my_addr);
        return;
    } else if (cmd == IPMICTL_GET_MY_LUN_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_GET_MY_LUN_CMD\n");
        ipmi_reply_u8(req, ipmi_state.my_lun);
        return;
    } else if (cmd == IPMICTL_SET_MY_ADDRESS_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_SET_MY_ADDRESS_CMD\n");
        if (!in_buf) {
            // fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        	fuse_reply_ioctl(req, 0, NULL, 0);
            return;
        }
        if (in_bufsz >= sizeof(unsigned char)) {
            ipmi_state.my_addr = *(const unsigned char *)in_buf;
        }
        fuse_reply_ioctl(req, 0, NULL, 0);
        return;
    } else if (cmd == IPMICTL_SET_MY_LUN_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_SET_MY_LUN_CMD\n");
        if (!in_buf) {
            struct iovec iov = { (void *)0, sizeof(unsigned char) };
            // fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        	fuse_reply_ioctl(req, 0, NULL, 0);
            return;
        }
        if (in_bufsz >= sizeof(unsigned char)) {
            ipmi_state.my_lun = *(const unsigned char *)in_buf;
        }
        fuse_reply_ioctl(req, 0, NULL, 0);
        return;
    } else if (cmd == IPMICTL_SET_GETS_EVENTS_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_SET_GETS_EVENTS_CMD\n");
        if (!in_buf) {
            struct iovec iov = { (void *)0, sizeof(int) };
            // fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        	fuse_reply_ioctl(req, 0, NULL, 0);
            return;
        }
        if (in_bufsz >= sizeof(int)) {
            ipmi_state.gets_events = (*(const int *)in_buf != 0);
            fprintf(stderr, "ipmi0: gets_events set to %d\n", ipmi_state.gets_events);
        }
        // Return success via ioctl (0 return value, not error)
        fuse_reply_ioctl(req, 0, NULL, 0);
        return;
#ifdef IPMICTL_GET_DEV_NAME_CMD
    } else if (cmd == IPMICTL_GET_DEV_NAME_CMD) {
        fuse_reply_ioctl(req, 0, "ipmi-cuse", strlen("ipmi-cuse") + 1);
        return;
#endif
#ifdef IPMICTL_GET_DEV_TYPE_CMD
    } else if (cmd == IPMICTL_GET_DEV_TYPE_CMD) {
        ipmi_reply_int(req, 0);
        return;
#endif
#ifdef IPMICTL_GET_CHANNEL_INFO_CMD
    } else if (cmd == IPMICTL_GET_CHANNEL_INFO_CMD) {
        ipmi_reply_channel_info(req, in_buf, in_bufsz);
        return;
#endif
    } else if (cmd == IPMICTL_REGISTER_FOR_CMD ||
           cmd == IPMICTL_UNREGISTER_FOR_CMD ||
           cmd == IPMICTL_SET_MY_CHANNEL_ADDRESS_CMD) {
		fprintf(stderr, "ipmi0 ioctl: ELSE\n");
        fuse_reply_err(req, 0);
        return;
    }


	/*
	 * The following ioctls use nested pointers in their arguments
	 * (struct ipmi_req / struct ipmi_recv). Proper support requires
	 * marshalling extra buffers from userspace and is not implemented here.
	 */
	if (cmd == IPMICTL_SEND_COMMAND) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_SEND_COMMAND\n");

		const struct ipmi_req *req_in;
		const uint8_t *p;
		size_t req_size;
		const uint8_t *response;
		const uint8_t *ipmi_rsp;
		size_t ipmi_rsp_len;

		if (ipmi_ioctl_get_req_buffers(req, arg, in_buf, in_bufsz, &req_size)) {
			fprintf(stderr, "ipmi0 ioctl: retrying for req buffers (need %zu bytes)\n", req_size);
            return;
		}
		req_in = (const struct ipmi_req *)in_buf;

		if (ipmi_ioctl_get_req_payloads(req, arg, req_in, in_buf, in_bufsz)) {
			fprintf(stderr, "ipmi0 ioctl: retrying for req payloads (addr_len=%d, data_len=%u)\n",
				req_in->addr_len, req_in->msg.data_len);
			return;
		}

		fprintf(stderr, "DAVIDE: received IPMI netFn=%u; cmd=%u\n", req_in->msg.netfn, req_in->msg.cmd);

		p = (const uint8_t *)in_buf + sizeof(*req_in);

		{
			const struct ipmi_ipmb_addr *ipmb_addr = NULL;
			uint8_t rs_sa_7bit = 0x10;  // TODO configurable
			uint8_t rs_sa = rs_sa_7bit << 1;  // TODO configurable
			uint8_t rq_sa = (uint8_t)(ipmi_state.my_addr << 1);
			uint8_t rs_lun = 0; // TODI configuragle
			uint8_t rq_lun = (uint8_t)(ipmi_state.my_lun & 0x3);
			// uint8_t seq = (uint8_t)(req_in->msgid & 0x3f);
			uint8_t seq = (uint8_t)(0 & 0x3f);
			uint8_t netfn_rs_lun;
			uint8_t rq_seq_rq_lun;
			uint8_t checksum1;
			uint8_t checksum2;
			const uint8_t *msg_data = p + req_in->addr_len;
			size_t msg_data_len = req_in->msg.data_len;
			uint8_t ipmb_msg[IPMI_MAX_DATA_LEN + 6];
			size_t ipmb_len;
			uint8_t i2c_raw_frame[IPMI_MAX_DATA_LEN + 7];
			size_t i2c_raw_len;
			uint8_t tx_frame[IPMI_MAX_DATA_LEN + 8];
			size_t tx_frame_len;

			// print req_in msg
			fprintf(stderr, "ipmi0 ioctl: IPMI request netFn=0x%02x cmd=0x%02x data_len=%u\n",
				req_in->msg.netfn, req_in->msg.cmd, req_in->msg.data_len);

			netfn_rs_lun = (uint8_t)((req_in->msg.netfn << 2) | rs_lun);
			rq_seq_rq_lun = (uint8_t)((seq << 2) | rq_lun);
			checksum1 = ipmb_checksum1(rs_sa, netfn_rs_lun);

			ipmb_msg[0] = netfn_rs_lun;
			ipmb_msg[1] = checksum1;
			ipmb_msg[2] = rq_sa;
			ipmb_msg[3] = rq_seq_rq_lun;
			ipmb_msg[4] = req_in->msg.cmd;
			if (msg_data_len > 0)
				memcpy(&ipmb_msg[5], msg_data, msg_data_len);
			checksum2 = ipmb_host_checksum(&ipmb_msg[2], 3 + msg_data_len, 0);
			ipmb_msg[5 + msg_data_len] = checksum2;
			ipmb_len = 6 + msg_data_len;

			fprintf(stderr, "ipmi0 ioctl: built IPMB message (%zu bytes)\n", ipmb_len);
			ipmi_hexdump("  IPMB", ipmb_msg, ipmb_len);

			i2c_raw_frame[0] = rs_sa;
			memcpy(&i2c_raw_frame[1], ipmb_msg, ipmb_len);
			i2c_raw_len = ipmb_len + 1;

			tx_frame[0] = (uint8_t)i2c_raw_len;
			memcpy(&tx_frame[1], i2c_raw_frame, i2c_raw_len);
			tx_frame_len = i2c_raw_len + 1;

			ipmi_hexdump("  TX Frame", tx_frame, tx_frame_len);
			{
				int fd;
				int fd_flags;
				ssize_t written;
				uint8_t resp_buf[IPMI_MAX_DATA_LEN + 7];
				ssize_t read_len;

				fd = open("/dev/ipmb-1", O_RDWR);
				if (fd < 0) {
					fprintf(stderr, "ipmi0 ioctl: open(/dev/ipmb-1) failed: %s\n",
						strerror(errno));
				} else {
					/* Drain any stale data and realign before sending. */
					fd_flags = fcntl(fd, F_GETFL, 0);
					if (fd_flags >= 0) {
						fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK);
						for (;;) {
							read_len = read(fd, resp_buf, sizeof(resp_buf));
							if (read_len <= 0) {
								if (read_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
									break;
								break;
							}
						}
						fcntl(fd, F_SETFL, fd_flags);
					}

					written = write(fd, tx_frame, tx_frame_len);
					if (written < 0) {
						fprintf(stderr, "ipmi0 ioctl: write(/dev/ipmb-1) failed: %s\n",
							strerror(errno));
					} else {
						fprintf(stderr, "ipmi0 ioctl: wrote %zd bytes to /dev/ipmb-1\n", written);
						read_len = read(fd, resp_buf, sizeof(resp_buf));
						if (read_len < 0) {
							fprintf(stderr, "ipmi0 ioctl: read(/dev/ipmb-1) failed: %s\n",
								strerror(errno));
						} else {
							fprintf(stderr, "ipmi0 ioctl: read %zd bytes from /dev/ipmb-1\n", read_len);
							ipmi_hexdump("  IPMB Rsp", resp_buf, (size_t)read_len);
							{
								size_t resp_off = 0;
								size_t resp_len = (size_t)read_len;

								if (resp_len > 0 && resp_buf[0] == resp_len - 1) {
									resp_off = 1;
									resp_len -= 1;
								}

								if (resp_len >= 7) {
									// 6 bytes: target_addr, netfn_rs_lun, checksum1, rq_sa, rq_seq, cmd
									ipmi_rsp = resp_buf + resp_off + 6;
									response = ipmi_rsp;
									ipmi_rsp_len = resp_len - 6;

									fprintf(stderr, "ipmi0 ioctl: IPMI response bytes (%zu)\n",
										ipmi_rsp_len);
									ipmi_hexdump("  IPMI Rsp", ipmi_rsp, ipmi_rsp_len);
								}
							}
						}
					}
					close(fd);
				}
			}

			if (!ipmb_addr)
				fprintf(stderr, "ipmi0 ioctl: warning: missing IPMB addr, rs_sa=0x%02x rs_lun=%u\n",
					rs_sa, rs_lun);
		}

		{
			// static const uint8_t response[] = {
			// 	0x00, 0x01, 0x82, 0x17, 0x10, 0x02, 0x9f, 0x19, 0x81,
			// 	0x00, 0x04, 0x00, 0x10, 0x05, 0x00, 0x00, 0xe2
			// };
			size_t addr_len = req_in->addr_len;
			size_t data_len = ipmi_rsp_len-1;

			if (addr_len > IPMI_MAX_ADDR_SIZE)
				addr_len = IPMI_MAX_ADDR_SIZE;
			if (data_len > IPMI_MAX_DATA_LEN)
				data_len = IPMI_MAX_DATA_LEN;

			memcpy(ipmi_pending.addr, p, addr_len);
			memcpy(ipmi_pending.data, ipmi_rsp, data_len);
			ipmi_pending.addr_len = (unsigned int)addr_len;
			ipmi_pending.data_len = (unsigned short)data_len;
			ipmi_pending.msgid = req_in->msgid;
			ipmi_pending.netfn = (unsigned char)(req_in->msg.netfn | 0x01);
			ipmi_pending.cmd = req_in->msg.cmd;
			ipmi_pending.recv_type = IPMI_RESPONSE_RECV_TYPE;
			ipmi_pending.valid = true;

			fprintf(stderr, "Queued fixed response with len=%u:\n", ipmi_pending.data_len);
			ipmi_hexdump("  Response Data", ipmi_pending.data, ipmi_pending.data_len);
		}

		fuse_reply_ioctl(req, 0, NULL, 0);
		return;
	}

	if (cmd == IPMICTL_RECEIVE_MSG || cmd == IPMICTL_RECEIVE_MSG_TRUNC) {
		const struct ipmi_recv *recv_in;
		struct ipmi_recv recv_out;
		struct ipmi_recv recv_tmp;
		struct ipmi_recv32 recv_out32;
		struct iovec iov[3];
		int iovcnt = 0;
		size_t recv_size;
		size_t addr_copy;
		size_t data_copy;
		size_t req_addr_len = 0;
		size_t req_data_len = 0;
		uintptr_t addr_ptr;
		uintptr_t data_ptr;
		bool compat32 = false;

		fprintf(stderr, "ipmi0 ioctl: IPMICTL_RECEIVE_MSG\n");

		if (!ipmi_pending.valid) {
			fprintf(stderr, "No pending message to receive\n");
			fuse_reply_err(req, EAGAIN);
			return;
		}

		if (ipmi_ioctl_get_recv_buffers(req, arg, in_buf, in_bufsz, &recv_size)) {
			fprintf(stderr, "ipmi0 ioctl: retrying for recv buffers (need %zu bytes)\n", recv_size);
			return;
		}

		recv_in = (const struct ipmi_recv *)in_buf;
		compat32 = (sizeof(void *) == 8) &&
			((((uintptr_t)recv_in->addr) & 0xffffffff00000000ULL) == 0xffffffff00000000ULL);

		if (compat32) {
			const struct ipmi_recv32 *recv_in32 = (const struct ipmi_recv32 *)in_buf;
			struct iovec in_iov;
			struct iovec out_iov[3];
			int out_iovcnt = 0;
			size_t total;

			fprintf(stderr,
				"ipmi0 ioctl: compat32 recv_in addr_len=%u data_len=%u addr=0x%08x data=0x%08x in_bufsz=%zu out_bufsz=%zu\n",
				recv_in32->addr_len, recv_in32->msg.data_len,
				recv_in32->addr, recv_in32->msg.data,
				in_bufsz, out_bufsz);

			if (ipmi_ioctl_get_recv_buffers_size(req, arg, in_buf, in_bufsz,
					sizeof(struct ipmi_recv32))) {
				fprintf(stderr, "ipmi0 ioctl: retrying for recv32 buffers (need %zu bytes)\n",
					sizeof(struct ipmi_recv32));
				return;
			}

			addr_ptr = recv_in32->addr;
			data_ptr = recv_in32->msg.data;
			req_addr_len = recv_in32->addr_len;
			req_data_len = recv_in32->msg.data_len;

			if (req_addr_len > IPMI_MAX_ADDR_SIZE)
				req_addr_len = IPMI_MAX_ADDR_SIZE;
			if (req_data_len > IPMI_MAX_DATA_LEN)
				req_data_len = IPMI_MAX_DATA_LEN;

			total = sizeof(struct ipmi_recv32) + req_addr_len + req_data_len;
			if (out_bufsz < total) {
				in_iov.iov_base = arg;
				in_iov.iov_len = sizeof(struct ipmi_recv32);

				out_iov[out_iovcnt].iov_base = arg;
				out_iov[out_iovcnt].iov_len = sizeof(struct ipmi_recv32);
				out_iovcnt++;

				if (req_addr_len > 0) {
					out_iov[out_iovcnt].iov_base = (void *)addr_ptr;
					out_iov[out_iovcnt].iov_len = req_addr_len;
					out_iovcnt++;
				}

				if (req_data_len > 0) {
					out_iov[out_iovcnt].iov_base = (void *)data_ptr;
					out_iov[out_iovcnt].iov_len = req_data_len;
					out_iovcnt++;
				}

				fprintf(stderr,
					"ipmi0 ioctl: retrying for recv32 payloads (addr_len=%zu, data_len=%zu, out_bufsz=%zu)\n",
					req_addr_len, req_data_len, out_bufsz);
				fuse_reply_ioctl_retry(req, &in_iov, 1, out_iov, out_iovcnt);
				return;
			}

			addr_copy = ipmi_pending.addr_len;
			data_copy = ipmi_pending.data_len;
			if (req_addr_len < addr_copy)
				addr_copy = req_addr_len;
			if (req_data_len < data_copy)
				data_copy = req_data_len;

			recv_out32 = *recv_in32;
			recv_out32.recv_type = ipmi_pending.recv_type;
			recv_out32.msgid = (int32_t)ipmi_pending.msgid;
			recv_out32.addr_len = (uint32_t)addr_copy;
			recv_out32.msg.netfn = ipmi_pending.netfn;
			recv_out32.msg.cmd = ipmi_pending.cmd;
			recv_out32.msg.data_len = (uint16_t)data_copy;
			recv_out32.addr = (uint32_t)addr_ptr;
			recv_out32.msg.data = (uint32_t)data_ptr;

			iov[iovcnt].iov_base = &recv_out32;
			iov[iovcnt].iov_len = sizeof(recv_out32);
			iovcnt++;

			if (addr_copy > 0) {
				iov[iovcnt].iov_base = ipmi_pending.addr;
				iov[iovcnt].iov_len = addr_copy;
				iovcnt++;
			}

			if (data_copy > 0) {
				iov[iovcnt].iov_base = ipmi_pending.data;
				iov[iovcnt].iov_len = data_copy;
				iovcnt++;
			}

			ipmi_pending.valid = false;
			fuse_reply_ioctl_iov(req, 0, iov, iovcnt);
			return;
		}

		addr_ptr = (uintptr_t)recv_in->addr;
		data_ptr = (uintptr_t)recv_in->msg.data;

		if ((recv_in->addr_len > 0 && addr_ptr == 0) ||
			(recv_in->msg.data_len > 0 && data_ptr == 0)) {
			fuse_reply_err(req, EFAULT);
			return;
		}

		recv_tmp = *recv_in;
		recv_tmp.addr = (unsigned char *)addr_ptr;
		recv_tmp.msg.data = (unsigned char *)data_ptr;

		if (ipmi_ioctl_get_recv_payloads(req, arg, &recv_tmp,
			recv_tmp.addr, recv_tmp.msg.data, out_bufsz,
			&req_addr_len, &req_data_len))
			return;
		fprintf(stderr, "ipmi0 ioctl: recv requested addr_len=%zu data_len=%zu\n",
			req_addr_len, req_data_len);
		addr_copy = ipmi_pending.addr_len;
		data_copy = ipmi_pending.data_len;

		if (recv_in->addr_len < addr_copy ||
			recv_in->msg.data_len < data_copy) {
			if (cmd == IPMICTL_RECEIVE_MSG) {
				fuse_reply_err(req, EMSGSIZE);
				return;
			}
			if (recv_in->addr_len < addr_copy)
				addr_copy = recv_in->addr_len;
			if (recv_in->msg.data_len < data_copy)
				data_copy = recv_in->msg.data_len;
		}

		recv_out = *recv_in;
		recv_out.recv_type = ipmi_pending.recv_type;
		recv_out.msgid = ipmi_pending.msgid;
		recv_out.addr_len = (unsigned int)addr_copy;
		recv_out.msg.netfn = ipmi_pending.netfn;
		recv_out.msg.cmd = ipmi_pending.cmd;
		recv_out.msg.data_len = (unsigned short)data_copy;

		{
			uint8_t addr_buf[IPMI_MAX_ADDR_SIZE];
			uint8_t data_buf[IPMI_MAX_DATA_LEN];

			memset(addr_buf, 0, req_addr_len);
			memset(data_buf, 0, req_data_len);
			if (addr_copy > 0)
				memcpy(addr_buf, ipmi_pending.addr, addr_copy);
			if (data_copy > 0)
				memcpy(data_buf, ipmi_pending.data, data_copy);

			iov[iovcnt].iov_base = &recv_out;
			iov[iovcnt].iov_len = sizeof(recv_out);
			iovcnt++;

			if (req_addr_len > 0) {
				iov[iovcnt].iov_base = addr_buf;
				iov[iovcnt].iov_len = req_addr_len;
				iovcnt++;
			}

			if (req_data_len > 0) {
				iov[iovcnt].iov_base = data_buf;
				iov[iovcnt].iov_len = req_data_len;
				iovcnt++;
			}

			fprintf(stderr, "Replying to receive with %d iovecs (addr_len=%zu data_len=%zu)\n",
				iovcnt, addr_copy, data_copy);
			if (data_copy > 0) {
				fprintf(stderr, "ipmi0 ioctl: response bytes:\n");
				ipmi_hexdump("  Response Data", ipmi_pending.data, data_copy);
			}
			ipmi_pending.valid = false;
			fuse_reply_ioctl_iov(req, 0, iov, iovcnt);
		}
		return;
	}

    if (cmd == IPMICTL_GET_TIMING_PARMS_CMD ||
        cmd == IPMICTL_SET_TIMING_PARMS_CMD) {
		fprintf(stderr, "ipmi0 ioctl: IPMICTL_GET_TIMING_PARMS_CMD\n");
        fuse_reply_err(req, ENOSYS);
        return;
    }

	fuse_reply_err(req, ENOTTY);
}


#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

void ipmi_devintf_ioctl(fuse_req_t req, unsigned long cmd, void *arg,
			struct fuse_file_info *fi, unsigned flags,
			const void *in_buf, size_t in_bufsz,
			size_t out_bufsz);

static void ipmi0_open(fuse_req_t req, struct fuse_file_info *fi)
{
	(void)fi;
	fuse_reply_open(req, fi);
}

static void ipmi0_release(fuse_req_t req, struct fuse_file_info *fi)
{
	(void)fi;
	fuse_reply_err(req, 0);
}

static void ipmi0_read(fuse_req_t req, size_t size, off_t off,
					  struct fuse_file_info *fi)
{
	(void)off;
	(void)fi;
	char buf[1] = {0};
	if (size == 0) {
		fprintf(stdout, "ipmi0 read: size 0 requested\n");
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	fprintf(stdout, "ipmi0 read: other size requested\n");
	fuse_reply_buf(req, buf, 1);
}

static void ipmi0_write(fuse_req_t req, const char *buf, size_t size,
					   off_t off, struct fuse_file_info *fi)
{
	(void)off;
	(void)fi;
	fprintf(stdout, "ipmi0 write: %zu bytes\n", size);
	if (size > 0) {
		fwrite(buf, 1, size, stderr);
		fputc('\n', stderr);
	}
	fflush(stderr);
	fuse_reply_write(req, size);
}

static void ipmi0_ioctl(fuse_req_t req, unsigned long cmd, void *arg,
			    struct fuse_file_info *fi, unsigned flags,
			    const void *in_buf, size_t in_bufsz,
			    size_t out_bufsz)
{
	ipmi_devintf_ioctl(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
}

static const struct cuse_lowlevel_ops ipmi0_clop = {
	.open = ipmi0_open,
	.read = ipmi0_read,
	.write = ipmi0_write,
	.ioctl = ipmi0_ioctl,
	.release = ipmi0_release,
};

int main(int argc, char **argv)
{
	struct cuse_info ci;
	char *devname = (char *)"DEVNAME=ipmi0";
	const char *cuse_argv[] = { argv[0], "-f" };

	memset(&ci, 0, sizeof(ci));
	ci.flags = CUSE_UNRESTRICTED_IOCTL;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = &devname;

	fprintf(stderr, "Starting CUSE device /dev/ipmi0\n");
	return cuse_lowlevel_main((int)(sizeof(cuse_argv) / sizeof(cuse_argv[0])),
							  (char **)cuse_argv, &ci, &ipmi0_clop, NULL);
}

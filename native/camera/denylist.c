// Fire OS's libstagefright asks the media_codec_denylist service (hosted by system_server) about
// every codec it creates. Without system_server that lookup waits 5 s, three times per
// createEncoderByType, which makes opening the camera take 15 s or more per encoder. Answering
// every call on an empty service with UNKNOWN_TRANSACTION is what libstagefright already handles
// as "no denylist", without the wait. This talks to servicemanager over raw binder, so it needs
// no libbinder headers, and it only registers when the name is still free.
#include "platform.h"
#include "denylist.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG COUNTERTOP_PREFIX "-camera"
#define logi(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define logw(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define SERVICE "media_codec_denylist"
#define CHECK_SERVICE 2
#define ADD_SERVICE 3
#define PING ((uint32_t)'_' << 24 | (uint32_t)'P' << 16 | (uint32_t)'N' << 8 | (uint32_t)'G')
#define UNKNOWN_TRANSACTION (-EBADMSG)
#define DUMP_PRIORITY_DEFAULT 8
#define MAP_SIZE (128 * 1024)

static int node; // its address identifies our binder object

typedef struct {
	uint8_t buf[256];
	size_t n;
	binder_size_t offsets[1];
	size_t noffsets;
} parcel;

static void put32(parcel *p, uint32_t v) {
	memcpy(p->buf + p->n, &v, 4);
	p->n += 4;
}

static void put16s(parcel *p, const char *s) {
	size_t len = strlen(s);
	put32(p, (uint32_t)len);
	for (size_t i = 0; i <= len; i++) {
		uint16_t c = (uint8_t)s[i];
		memcpy(p->buf + p->n, &c, 2);
		p->n += 2;
	}
	p->n = (p->n + 3) & ~(size_t)3;
}

static void header(parcel *p, const char *name) {
	put32(p, 0); // strict mode policy
	put16s(p, "android.os.IServiceManager");
	put16s(p, name);
}

static int write_cmds(int fd, const void *cmds, size_t n) {
	struct binder_write_read bwr = {.write_size = n, .write_buffer = (binder_uintptr_t)(uintptr_t)cmds};
	while (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0)
		if (errno != EINTR) return -1;
	return 0;
}

static void free_buffer(int fd, binder_uintptr_t buf) {
	struct __attribute__((packed)) {
		uint32_t cmd;
		binder_uintptr_t buf;
	} c = {BC_FREE_BUFFER, buf};
	write_cmds(fd, &c, sizeof c);
}

static void answer(int fd, const struct binder_transaction_data *in) {
	free_buffer(fd, in->data.ptr.buffer);
	if (in->flags & TF_ONE_WAY) return;
	static const int32_t unknown = UNKNOWN_TRANSACTION;
	struct __attribute__((packed)) {
		uint32_t cmd;
		struct binder_transaction_data tr;
	} c = {.cmd = BC_REPLY};
	if (in->code != PING) {
		c.tr.flags = TF_STATUS_CODE;
		c.tr.data_size = sizeof unknown;
		c.tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)&unknown;
	}
	write_cmds(fd, &c, sizeof c);
}

// Processes what the driver hands us. With reply set, stops at the reply to our transaction
// and returns its first word, or -1 when servicemanager answered with a status or failed.
static int pump(int fd, int *found) {
	uint8_t in[512];
	for (;;) {
		struct binder_write_read bwr = {.read_size = sizeof in, .read_buffer = (binder_uintptr_t)(uintptr_t)in};
		if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		for (size_t at = 0; at + 4 <= bwr.read_consumed;) {
			uint32_t cmd;
			memcpy(&cmd, in + at, 4);
			at += 4;
			switch (cmd) {
			case BR_NOOP:
			case BR_TRANSACTION_COMPLETE:
			case BR_SPAWN_LOOPER:
				break;
			case BR_INCREFS:
			case BR_ACQUIRE: {
				struct __attribute__((packed)) {
					uint32_t cmd;
					struct binder_ptr_cookie pc;
				} c = {cmd == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE};
				memcpy(&c.pc, in + at, sizeof c.pc);
				at += sizeof c.pc;
				write_cmds(fd, &c, sizeof c);
				break;
			}
			case BR_RELEASE:
			case BR_DECREFS:
				at += sizeof(struct binder_ptr_cookie);
				break;
			case BR_TRANSACTION: {
				struct binder_transaction_data tr;
				memcpy(&tr, in + at, sizeof tr);
				at += sizeof tr;
				answer(fd, &tr);
				break;
			}
			case BR_REPLY: {
				struct binder_transaction_data tr;
				memcpy(&tr, in + at, sizeof tr);
				int32_t word = -1;
				if (!(tr.flags & TF_STATUS_CODE) && tr.data_size >= 4)
					memcpy(&word, (const void *)(uintptr_t)tr.data.ptr.buffer, 4);
				if (found) *found = tr.offsets_size > 0;
				free_buffer(fd, tr.data.ptr.buffer);
				return (tr.flags & TF_STATUS_CODE) ? -1 : word;
			}
			case BR_DEAD_REPLY:
			case BR_FAILED_REPLY:
				return -1;
			default:
				logw(SERVICE ": unexpected binder command 0x%x", cmd);
				return -1;
			}
		}
	}
}

static int call(int fd, uint32_t code, parcel *p, int *found) {
	struct __attribute__((packed)) {
		uint32_t cmd;
		struct binder_transaction_data tr;
	} c = {.cmd = BC_TRANSACTION};
	c.tr.target.handle = 0;
	c.tr.code = code;
	c.tr.flags = TF_ACCEPT_FDS;
	c.tr.data_size = p->n;
	c.tr.offsets_size = p->noffsets * sizeof(binder_size_t);
	c.tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)p->buf;
	c.tr.data.ptr.offsets = (binder_uintptr_t)(uintptr_t)p->offsets;
	if (write_cmds(fd, &c, sizeof c) < 0) return -1;
	return pump(fd, found);
}

// Runs on one thread: the driver only hands incoming calls to a thread that entered the looper.
static void *serve(void *arg) {
	(void)arg;
	int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	if (fd < 0) return NULL;
	struct binder_version v = {0};
	if (ioctl(fd, BINDER_VERSION, &v) < 0 || v.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION ||
	    mmap(NULL, MAP_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0) == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	uint32_t zero = 0;
	ioctl(fd, BINDER_SET_MAX_THREADS, &zero);
	uint32_t enter = BC_ENTER_LOOPER;
	write_cmds(fd, &enter, sizeof enter);

	parcel p = {0};
	header(&p, SERVICE);
	int found = 0;
	if (call(fd, CHECK_SERVICE, &p, &found) < 0 || found) {
		close(fd);
		return NULL;
	}

	parcel add = {0};
	header(&add, SERVICE);
	struct flat_binder_object obj = {
		.hdr.type = BINDER_TYPE_BINDER,
		.flags = 0x7f | FLAT_BINDER_FLAG_ACCEPTS_FDS,
		.binder = (binder_uintptr_t)(uintptr_t)&node,
	};
	add.offsets[add.noffsets++] = add.n;
	memcpy(add.buf + add.n, &obj, sizeof obj);
	add.n += sizeof obj;
	put32(&add, 0); // allow isolated
	put32(&add, DUMP_PRIORITY_DEFAULT);
	if (call(fd, ADD_SERVICE, &add, NULL) != 0) {
		logw(SERVICE ": servicemanager refused it");
		close(fd);
		return NULL;
	}
	logi(SERVICE ": serving an empty one");
	for (;;)
		if (pump(fd, NULL) < 0) usleep(100000);
	return NULL;
}

void denylist_start(void) {
	pthread_t t;
	if (pthread_create(&t, NULL, serve, NULL) == 0) pthread_detach(t);
}

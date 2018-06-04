#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/backend/session/interface.h>
#include <wlr/util/log.h>
#include "util/signal.h"

enum { DRM_MAJOR = 226 };

const struct session_impl session_fake;

struct fake_session {
	struct wlr_session base;
	int tty_fd;
	int old_kbmode;
	int sock;
	pid_t child;

	struct wl_event_source *vt_source;
};

static int fake_session_open(struct wlr_session *base, const char *path) {
	errno = 0;

	// These are the same flags that logind opens files with
	int fd = open(path, O_RDWR|O_CLOEXEC|O_NOCTTY|O_NONBLOCK);
	int ret = errno;
	if (fd == -1) {
		goto error;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		ret = errno;
		goto error;
	}

	uint32_t maj = major(st.st_rdev);
	if (maj != INPUT_MAJOR && maj != DRM_MAJOR) {
		ret = ENOTSUP;
		goto error;
	}
	return fd;
error:
	close(fd);
	return ret;
}

static void fake_session_close(struct wlr_session *base, int fd) {
	struct fake_session *session = wl_container_of(base, session, base);
	struct stat st;
	if (fstat(fd, &st) < 0) {
		wlr_log_errno(L_ERROR, "Stat failed");
		close(fd);
		return;
	}

	if (major(st.st_rdev) == INPUT_MAJOR) {
		ioctl(fd, EVIOCREVOKE, 0);
	}

	close(fd);
}

static bool fake_change_vt(struct wlr_session *base, unsigned vt) {
	return true;
}

static void fake_session_destroy(struct wlr_session *base) {
	struct fake_session *session = wl_container_of(base, session, base);
	free(session);
}

static struct wlr_session *fake_session_create(struct wl_display *disp) {
	struct fake_session *session = calloc(1, sizeof(*session));
	if (!session) {
		wlr_log_errno(L_ERROR, "Allocation failed");
		return NULL;
	}

	// XXX: Is it okay to trust the environment like this?
	const char *seat = getenv("XDG_SEAT");
	if (!seat) {
		seat = "seat0";
	}

	wlr_log(L_INFO, "Successfully loaded fake session");

	snprintf(session->base.seat, sizeof(session->base.seat), "%s", seat);
	session->base.impl = &session_fake;
	return &session->base;
}

const struct session_impl session_fake = {
	.create = fake_session_create,
	.destroy = fake_session_destroy,
	.open = fake_session_open,
	.close = fake_session_close,
	.change_vt = fake_change_vt,
};

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include <sync/sync.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>
#include "backend/hwcomposer.h"
#include "util/signal.h"

void present(void *user_data, struct ANativeWindow *window,
                                struct ANativeWindowBuffer *buffer)
{
    struct wlr_hwcomposer_backend *hwc = (struct wlr_hwcomposer_backend *)user_data;

    hwc_display_contents_1_t **contents = hwc->hwcContents;
    hwc_layer_1_t *fblayer = hwc->fblayer;
    hwc_composer_device_1_t *hwcdevice = hwc->hwcDevicePtr;

    int oldretire = contents[0]->retireFenceFd;
    contents[0]->retireFenceFd = -1;

    fblayer->handle = buffer->handle;
    fblayer->acquireFenceFd = HWCNativeBufferGetFence(buffer);
    fblayer->releaseFenceFd = -1;
    int err = hwcdevice->prepare(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
    assert(err == 0);

    err = hwcdevice->set(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
    // in android surfaceflinger ignores the return value as not all display types may be supported
    HWCNativeBufferSetFence(buffer, fblayer->releaseFenceFd);

    if (oldretire != -1)
    {
        sync_wait(oldretire, -1);
        close(oldretire);
    }
}

static EGLSurface egl_create_surface(struct wlr_hwcomposer_backend *backend, unsigned int width,
		unsigned int height) {
	struct wlr_egl *egl = &backend->egl;
	struct ANativeWindow *win = HWCNativeWindowCreate(width, height, HAL_PIXEL_FORMAT_RGBA_8888, present, backend);

	EGLSurface surf = eglCreateWindowSurface(egl->display, egl->config, (EGLNativeWindowType)win, NULL);
	if (surf == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "Failed to create EGL surface");
		return EGL_NO_SURFACE;
	}
	return surf;
}

static bool output_set_custom_mode(struct wlr_output *wlr_output, int32_t width,
		int32_t height, int32_t refresh) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *backend = output->backend;

	if (refresh <= 0) {
		refresh = HWCOMPOSER_DEFAULT_REFRESH;
	}

	wlr_egl_destroy_surface(&backend->egl, output->egl_surface);

	output->egl_display = eglGetDisplay(NULL);
	backend->egl.display = output->egl_display;

	output->egl_surface = egl_create_surface(&backend->egl, width, height);
	if (output->egl_surface == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "Failed to recreate EGL surface");
		wlr_output_destroy(wlr_output);
		return false;
	}

	output->frame_delay = 1000000 / refresh;

	wlr_output_update_custom_mode(&output->wlr_output, width, height, refresh);
	return true;
}

static bool output_attach_render(struct wlr_output *wlr_output,
		int *buffer_age) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	return wlr_egl_make_current(&output->backend->egl, output->egl_surface,
		buffer_age);
}

static bool output_test(struct wlr_output *wlr_output) {
	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_ENABLED) {
		wlr_log(WLR_DEBUG, "Cannot disable a hwcomposer output");
		return false;
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_MODE) {
		assert(wlr_output->pending.mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
	}

	return true;
}

static bool output_commit(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	if (!output_test(wlr_output)) {
		return false;
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_MODE) {
		if (!output_set_custom_mode(wlr_output,
				wlr_output->pending.custom_mode.width,
				wlr_output->pending.custom_mode.height,
				wlr_output->pending.custom_mode.refresh)) {
			return false;
		}
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) {
		// Nothing needs to be done for pbuffers
		wlr_output_send_present(wlr_output, NULL);
	}

	wlr_egl_make_current(&output->backend->egl, EGL_NO_SURFACE, NULL);

	return true;
}

static void output_rollback(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	wlr_egl_make_current(&output->backend->egl, EGL_NO_SURFACE, NULL);
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	wl_list_remove(&output->link);

	wl_event_source_remove(output->frame_timer);

	wlr_egl_destroy_surface(&output->backend->egl, output->egl_surface);
	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.attach_render = output_attach_render,
	.commit = output_commit,
	.rollback = output_rollback,
};

bool wlr_output_is_hwcomposer(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(void *data) {
	struct wlr_hwcomposer_output *output = data;
	wlr_output_send_frame(&output->wlr_output);
	wl_event_source_timer_update(output->frame_timer, output->frame_delay);
	return 0;
}

struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *wlr_backend,
		unsigned int width, unsigned int height) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;

	struct wlr_hwcomposer_output *output =
		calloc(1, sizeof(struct wlr_hwcomposer_output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_hwcomposer_output");
		return NULL;
	}
	output->backend = backend;
	wlr_output_init(&output->wlr_output, &backend->backend, &output_impl,
		backend->display);
	struct wlr_output *wlr_output = &output->wlr_output;

	output->egl_surface = egl_create_surface(&backend->egl, width, height);
	if (output->egl_surface == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "Failed to create EGL surface");
		goto error;
	}

	output_set_custom_mode(wlr_output, width, height, 0);
	strncpy(wlr_output->make, "hwcomposer", sizeof(wlr_output->make));
	strncpy(wlr_output->model, "hwcomposer", sizeof(wlr_output->model));
	snprintf(wlr_output->name, sizeof(wlr_output->name), "HWCOMPOSER-%zd",
		++backend->last_output_num);

	char description[128];
	snprintf(description, sizeof(description),
		"Headless output %zd", backend->last_output_num);
	wlr_output_set_description(wlr_output, description);

	if (!wlr_egl_make_current(&output->backend->egl, output->egl_surface,
			NULL)) {
		goto error;
	}

	wlr_renderer_begin(backend->renderer, wlr_output->width, wlr_output->height);
	wlr_renderer_clear(backend->renderer, (float[]){ 1.0, 1.0, 1.0, 1.0 });
	wlr_renderer_end(backend->renderer);

	struct wl_event_loop *ev = wl_display_get_event_loop(backend->display);
	output->frame_timer = wl_event_loop_add_timer(ev, signal_frame, output);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_event_source_timer_update(output->frame_timer, output->frame_delay);
		wlr_output_update_enabled(wlr_output, true);
		wlr_signal_emit_safe(&backend->backend.events.new_output, wlr_output);
	}

	return wlr_output;

error:
	wlr_output_destroy(&output->wlr_output);
	return NULL;
}

/*
 * Copyright © 2013 Jonas Ådahl
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <assert.h>

#include "libinput.h"
#include "libinput-private.h"
#include "evdev.h"
#include "udev-seat.h"

struct libinput_source {
	libinput_source_dispatch_t dispatch;
	void *user_data;
	int fd;
	struct list link;
};

static void
libinput_post_event(struct libinput *libinput,
		    struct libinput_event *event);

struct libinput_source *
libinput_add_fd(struct libinput *libinput,
		int fd,
		libinput_source_dispatch_t dispatch,
		void *user_data)
{
	struct libinput_source *source;
	struct epoll_event ep;

	source = malloc(sizeof *source);
	if (!source)
		return NULL;

	source->dispatch = dispatch;
	source->user_data = user_data;
	source->fd = fd;

	memset(&ep, 0, sizeof ep);
	ep.events = EPOLLIN;
	ep.data.ptr = source;

	if (epoll_ctl(libinput->epoll_fd, EPOLL_CTL_ADD, fd, &ep) < 0) {
		close(source->fd);
		free(source);
		return NULL;
	}

	return source;
}

void
libinput_remove_source(struct libinput *libinput,
		       struct libinput_source *source)
{
	epoll_ctl(libinput->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);
	close(source->fd);
	source->fd = -1;
	list_insert(&libinput->source_destroy_list, &source->link);
}

int
libinput_init(struct libinput *libinput,
	      const struct libinput_interface *interface,
	      void *user_data)
{
	libinput->epoll_fd = epoll_create1(EPOLL_CLOEXEC);;
	if (libinput->epoll_fd < 0)
		return -1;

	libinput->interface = interface;
	libinput->user_data = user_data;
	list_init(&libinput->source_destroy_list);
	list_init(&libinput->seat_list);

	return 0;
}

LIBINPUT_EXPORT void
libinput_destroy(struct libinput *libinput)
{
	struct libinput_event *event;

	while ((event = libinput_get_event(libinput)))
	       free(event);
	free(libinput->events);

	close(libinput->epoll_fd);
	free(libinput);
}

int
open_restricted(struct libinput *libinput,
		const char *path, int flags)
{
	return libinput->interface->open_restricted(path,
						    flags,
						    libinput->user_data);
}

void
close_restricted(struct libinput *libinput, int fd)
{
	return libinput->interface->close_restricted(fd, libinput->user_data);
}

void
libinput_seat_init(struct libinput_seat *seat,
		   struct libinput *libinput,
		   const char *name)
{
	seat->refcount = 1;
	seat->libinput = libinput;
	seat->name = strdup(name);
	list_init(&seat->devices_list);
}

LIBINPUT_EXPORT void
libinput_seat_ref(struct libinput_seat *seat)
{
	seat->refcount++;
}

LIBINPUT_EXPORT void
libinput_seat_unref(struct libinput_seat *seat)
{
	seat->refcount--;
	if (seat->refcount == 0) {
		free(seat->name);
		udev_seat_destroy((struct udev_seat *) seat);
	}
}

LIBINPUT_EXPORT void
libinput_seat_set_user_data(struct libinput_seat *seat, void *user_data)
{
	seat->user_data = user_data;
}

LIBINPUT_EXPORT void *
libinput_seat_get_user_data(struct libinput_seat *seat)
{
	return seat->user_data;
}

LIBINPUT_EXPORT const char *
libinput_seat_get_name(struct libinput_seat *seat)
{
	return seat->name;
}

void
libinput_device_init(struct libinput_device *device,
		     struct libinput_seat *seat)
{
	device->seat = seat;
	device->refcount = 1;
}

LIBINPUT_EXPORT void
libinput_device_ref(struct libinput_device *device)
{
	device->refcount++;
}

LIBINPUT_EXPORT void
libinput_device_unref(struct libinput_device *device)
{
	device->refcount--;
	if (device->refcount == 0)
		evdev_device_destroy((struct evdev_device *) device);
}

LIBINPUT_EXPORT int
libinput_get_fd(struct libinput *libinput)
{
	return libinput->epoll_fd;
}

LIBINPUT_EXPORT int
libinput_dispatch(struct libinput *libinput)
{
	struct libinput_source *source, *next;
	struct epoll_event ep[32];
	int i, count;

	count = epoll_wait(libinput->epoll_fd, ep, ARRAY_LENGTH(ep), 0);
	if (count < 0)
		return -errno;

	for (i = 0; i < count; ++i) {
		source = ep[i].data.ptr;
		if (source->fd == -1)
			continue;

		source->dispatch(source->user_data);
	}

	list_for_each_safe(source, next, &libinput->source_destroy_list, link)
		free(source);
	list_init(&libinput->source_destroy_list);

	return libinput->events_count > 0 ? 0 : -EAGAIN;
}

static void
init_event_base(struct libinput_event *event,
		enum libinput_event_type type,
		union libinput_event_target target)
{
	event->type = type;
	event->target = target;
}

static void
post_base_event(struct libinput *libinput,
		enum libinput_event_type type,
		struct libinput_event *event)
{
	init_event_base(event, type,
			(union libinput_event_target) { .libinput = libinput });
	libinput_post_event(libinput, event);
}

#if 0
static void
post_seat_event(struct libinput_seat *seat,
		enum libinput_event_type type,
		struct libinput_event *event)
{
	init_event_base(event, type,
			(union libinput_event_target) { .seat = seat });
	libinput_post_event(seat->libinput, event);
}
#endif

static void
post_device_event(struct libinput_device *device,
		  enum libinput_event_type type,
		  struct libinput_event *event)
{
	init_event_base(event, type,
			(union libinput_event_target) { .device = device });
	libinput_post_event(device->seat->libinput, event);
}

void
notify_added_seat(struct libinput_seat *seat)
{
	struct libinput_event_added_seat *added_seat_event;

	added_seat_event = malloc(sizeof *added_seat_event);
	if (!added_seat_event)
		return;

	*added_seat_event = (struct libinput_event_added_seat) {
		.seat = seat,
	};

	post_base_event(seat->libinput,
			LIBINPUT_EVENT_ADDED_SEAT,
			&added_seat_event->base);
}

void
notify_removed_seat(struct libinput_seat *seat)
{
	struct libinput_event_removed_seat *removed_seat_event;

	removed_seat_event = malloc(sizeof *removed_seat_event);
	if (!removed_seat_event)
		return;

	*removed_seat_event = (struct libinput_event_removed_seat) {
		.seat = seat,
	};

	post_base_event(seat->libinput,
			LIBINPUT_EVENT_REMOVED_SEAT,
			&removed_seat_event->base);
}

void
notify_added_device(struct libinput_device *device)
{
	struct libinput_event_added_device *added_device_event;

	added_device_event = malloc(sizeof *added_device_event);
	if (!added_device_event)
		return;

	*added_device_event = (struct libinput_event_added_device) {
		.device = device,
	};

	post_base_event(device->seat->libinput,
			LIBINPUT_EVENT_ADDED_DEVICE,
			&added_device_event->base);
}

void
notify_removed_device(struct libinput_device *device)
{
	struct libinput_event_removed_device *removed_device_event;

	removed_device_event = malloc(sizeof *removed_device_event);
	if (!removed_device_event)
		return;

	*removed_device_event = (struct libinput_event_removed_device) {
		.device = device,
	};

	post_base_event(device->seat->libinput,
			LIBINPUT_EVENT_REMOVED_DEVICE,
			&removed_device_event->base);
}

void
device_register_capability(struct libinput_device *device,
			   enum libinput_device_capability capability)
{
	struct libinput_event_device_register_capability *capability_event;

	capability_event = malloc(sizeof *capability_event);

	*capability_event = (struct libinput_event_device_register_capability) {
		.capability = capability,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_DEVICE_REGISTER_CAPABILITY,
			  &capability_event->base);
}

void
device_unregister_capability(struct libinput_device *device,
			     enum libinput_device_capability capability)
{
	struct libinput_event_device_unregister_capability *capability_event;

	capability_event = malloc(sizeof *capability_event);

	*capability_event = (struct libinput_event_device_unregister_capability) {
		.capability = capability,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_DEVICE_UNREGISTER_CAPABILITY,
			  &capability_event->base);
}

void
keyboard_notify_key(struct libinput_device *device,
		    uint32_t time,
		    uint32_t key,
		    enum libinput_keyboard_key_state state)
{
	struct libinput_event_keyboard_key *key_event;

	key_event = malloc(sizeof *key_event);
	if (!key_event)
		return;

	*key_event = (struct libinput_event_keyboard_key) {
		.time = time,
		.key = key,
		.state = state,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_KEYBOARD_KEY,
			  &key_event->base);
}

void
pointer_notify_motion(struct libinput_device *device,
		      uint32_t time,
		      li_fixed_t dx,
		      li_fixed_t dy)
{
	struct libinput_event_pointer_motion *motion_event;

	motion_event = malloc(sizeof *motion_event);
	if (!motion_event)
		return;

	*motion_event = (struct libinput_event_pointer_motion) {
		.time = time,
		.dx = dx,
		.dy = dy,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_POINTER_MOTION,
			  &motion_event->base);
}

void
pointer_notify_motion_absolute(struct libinput_device *device,
			       uint32_t time,
			       li_fixed_t x,
			       li_fixed_t y)
{
	struct libinput_event_pointer_motion_absolute *motion_absolute_event;

	motion_absolute_event = malloc(sizeof *motion_absolute_event);
	if (!motion_absolute_event)
		return;

	*motion_absolute_event = (struct libinput_event_pointer_motion_absolute) {
		.time = time,
		.x = x,
		.y = y,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE,
			  &motion_absolute_event->base);
}

void
pointer_notify_button(struct libinput_device *device,
		      uint32_t time,
		      int32_t button,
		      enum libinput_pointer_button_state state)
{
	struct libinput_event_pointer_button *button_event;

	button_event = malloc(sizeof *button_event);
	if (!button_event)
		return;

	*button_event = (struct libinput_event_pointer_button) {
		.time = time,
		.button = button,
		.state = state,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_POINTER_BUTTON,
			  &button_event->base);
}

void
pointer_notify_axis(struct libinput_device *device,
		    uint32_t time,
		    enum libinput_pointer_axis axis,
		    li_fixed_t value)
{
	struct libinput_event_pointer_axis *axis_event;

	axis_event = malloc(sizeof *axis_event);
	if (!axis_event)
		return;

	*axis_event = (struct libinput_event_pointer_axis) {
		.time = time,
		.axis = axis,
		.value = value,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_POINTER_AXIS,
			  &axis_event->base);
}

void
touch_notify_touch(struct libinput_device *device,
		   uint32_t time,
		   int32_t slot,
		   li_fixed_t x,
		   li_fixed_t y,
		   enum libinput_touch_type touch_type)
{
	struct libinput_event_touch_touch *touch_event;

	touch_event = malloc(sizeof *touch_event);
	if (!touch_event)
		return;

	*touch_event = (struct libinput_event_touch_touch) {
		.time = time,
		.slot = slot,
		.x = x,
		.y = y,
		.touch_type = touch_type,
	};

	post_device_event(device,
			  LIBINPUT_EVENT_TOUCH_TOUCH,
			  &touch_event->base);
}

static void
libinput_post_event(struct libinput *libinput,
		    struct libinput_event *event)
{
	struct libinput_event **events = libinput->events;
	size_t events_len = libinput->events_len;
	size_t events_count = libinput->events_count;
	size_t move_len;
	size_t new_out;

	events_count++;
	if (events_count > events_len) {
		if (events_len == 0)
			events_len = 4;
		else
			events_len *= 2;
		events = realloc(events, events_len * sizeof *events);
		if (!events) {
			fprintf(stderr, "Failed to reallocate event ring "
				"buffer");
			return;
		}

		if (libinput->events_count > 0 && libinput->events_in == 0) {
			libinput->events_in = libinput->events_len;
		} else if (libinput->events_count > 0 &&
			   libinput->events_out >= libinput->events_in) {
			move_len = libinput->events_len - libinput->events_out;
			new_out = events_len - move_len;
			memmove(events + new_out,
				libinput->events + libinput->events_out,
				move_len * sizeof *events);
			libinput->events_out = new_out;
		}

		libinput->events = events;
		libinput->events_len = events_len;
	}

	libinput->events_count = events_count;
	events[libinput->events_in] = event;
	libinput->events_in = (libinput->events_in + 1) % libinput->events_len;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_get_event(struct libinput *libinput)
{
	struct libinput_event *event;

	if (libinput->events_count == 0)
		return NULL;

	event = libinput->events[libinput->events_out];
	libinput->events_out =
		(libinput->events_out + 1) % libinput->events_len;
	libinput->events_count--;

	return event;
}

LIBINPUT_EXPORT void *
libinput_get_user_data(struct libinput *libinput)
{
	return libinput->user_data;
}

LIBINPUT_EXPORT int
libinput_resume(struct libinput *libinput)
{
	return udev_input_enable((struct udev_input *) libinput);
}

LIBINPUT_EXPORT void
libinput_suspend(struct libinput *libinput)
{
	udev_input_disable((struct udev_input *) libinput);
}

LIBINPUT_EXPORT void
libinput_device_set_user_data(struct libinput_device *device, void *user_data)
{
	device->user_data = user_data;
}

LIBINPUT_EXPORT void *
libinput_device_get_user_data(struct libinput_device *device)
{
	return device->user_data;
}

LIBINPUT_EXPORT const char *
libinput_device_get_output_name(struct libinput_device *device)
{
	return evdev_device_get_output((struct evdev_device *) device);
}

LIBINPUT_EXPORT struct libinput_seat *
libinput_device_get_seat(struct libinput_device *device)
{
	return device->seat;
}

LIBINPUT_EXPORT void
libinput_device_led_update(struct libinput_device *device,
			   enum libinput_led leds)
{
	evdev_device_led_update((struct evdev_device *) device, leds);
}

LIBINPUT_EXPORT int
libinput_device_get_keys(struct libinput_device *device,
			 char *keys, size_t size)
{
	return evdev_device_get_keys((struct evdev_device *) device,
				     keys,
				     size);
}

LIBINPUT_EXPORT void
libinput_device_calibrate(struct libinput_device *device,
			  float calibration[6])
{
	evdev_device_calibrate((struct evdev_device *) device, calibration);
}
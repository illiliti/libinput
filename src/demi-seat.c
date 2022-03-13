/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
 * Copyright © 2017 Michael Forney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <demi.h>

#include "evdev.h"
#include "demi-seat.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

static struct demi_seat *
demi_seat_create(struct demi_input *input,
		    const char *device_seat,
		    const char *seat_name);
static struct demi_seat *
demi_seat_get_named(struct demi_input *input, const char *seat_name);

static inline bool
filter_duplicates(struct demi_seat *seat,
		  const char *devnode)
{
	struct evdev_device *device;
	bool ignore_device = false;

	if (!seat)
		return false;

	list_for_each(device, &seat->base.devices_list, base.link) {
		if (streq(device->devnode, devnode))
			ignore_device = true;

		if (ignore_device)
			break;
	}

	return ignore_device;
}

static int
device_added(struct demi_input *input,
	     const char *devnode)
{
	struct evdev_device *device;
	const char *device_seat, *seat_name, *sysname;
	struct demi_seat *seat;

	device_seat = default_seat;
	seat_name = default_seat_name;
	seat = demi_seat_get_named(input, seat_name);

	/* There is a race at startup: a device added between setting
	 * up the udev monitor and enumerating all current devices may show
	 * up in both lists. Filter those out.
	 */
	if (filter_duplicates(seat, devnode))
		return 0;

	if (seat)
		libinput_seat_ref(&seat->base);
	else {
		seat = demi_seat_create(input, device_seat, seat_name);
		if (!seat)
			return -1;
	}

	sysname = strrchr(devnode, '/');
	if (sysname)
		++sysname;
	else
		sysname = "";

	device = evdev_device_create(&seat->base, NULL, devnode, sysname);
	libinput_seat_unref(&seat->base);

	if (device == EVDEV_UNHANDLED_DEVICE) {
		log_info(&input->base,
			 "%-7s - not using input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	}

	if (device == NULL) {
		log_info(&input->base,
			 "%-7s - failed to create input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	}

	evdev_read_calibration_prop(device);

	return 0;
}

static void
device_removed(struct demi_input *input, const char *devnode)
{
	struct evdev_device *device;
	struct demi_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		list_for_each_safe(device,
				   &seat->base.devices_list, base.link) {
			if (streq(devnode, device->devnode)) {
				evdev_device_remove(device);
				break;
			}
		}
	}
}

static int
select_device(const struct dirent *entry)
{
	const char *p;

	if (!strneq(entry->d_name, "event", 5))
		return 0;
	for (p = entry->d_name + 5; '0' <= *p && *p <= '9'; ++p)
		;
	return *p == '\0';
}

static int
demi_input_add_devices(struct demi_input *input)
{
	struct dirent **devices;
	char path[PATH_MAX];
	int i, n, len;

	n = scandir("/dev/input", &devices, &select_device, &alphasort);
	if (n == -1)
		return -1;
	for (i = 0; i < n; ++i) {
		len = snprintf(path, sizeof(path), "/dev/input/%s", devices[i]->d_name);
		free(devices[i]);
		if (len < 0 || (size_t)len >= sizeof(path)) {
			free(devices);
			return -1;
		}
		device_added(input, path);
	}
	free(devices);

	return 0;
}

static void
demi_input_remove_devices(struct demi_input *input)
{
	struct evdev_device *device;
	struct demi_seat *seat;

	list_for_each_safe(seat, &input->base.seat_list, base.link) {
		libinput_seat_ref(&seat->base);
		list_for_each_safe(device,
				   &seat->base.devices_list, base.link) {
			evdev_device_remove(device);
		}
		libinput_seat_unref(&seat->base);
	}
}

static void
demi_input_disable(struct libinput *libinput)
{
	struct demi_input *input = (struct demi_input*)libinput;

	if (input->fd == -1)
		return;

	close(input->fd);
	input->fd = -1;
	libinput_remove_source(&input->base, input->source);
	input->source = NULL;

	demi_input_remove_devices(input);
}

static void
demi_handler(void *data)
{
	struct demi_input *input = data;

	struct demi_event event;
	if (demi_read(input->fd, &event) == -1) {
		return;
	}

	if (event.de_type == DEMI_UNKNOWN) {
		return;
	}

	const char *name = strrchr(event.de_devname, '/');
	name = name ? name + 1 : event.de_devname;

	if (!strneq(name, "event", 5))
		return;

	char devnode[sizeof("/dev/") + sizeof(event.de_devname)];
	snprintf(devnode, sizeof(devnode), "/dev/%s", event.de_devname);

	if (event.de_type == DEMI_ATTACH)
		device_added(input, devnode);
	else if (event.de_type == DEMI_DETACH)
		device_removed(input, devnode);
}

static int
demi_input_enable(struct libinput *libinput)
{
	struct demi_input *input = (struct demi_input*)libinput;
	int fd;

	if (input->fd != -1)
		return 0;
	fd = demi_init(DEMI_CLOEXEC | DEMI_NONBLOCK);
	if (fd == -1)
		return -1;
	input->source = libinput_add_fd(&input->base, fd, demi_handler, input);
	if (!input->source) {
		close(fd);
		return -1;
	}
	input->fd = fd;
	if (demi_input_add_devices(input) < 0) {
		demi_input_disable(libinput);
		return -1;
	}

	return 0;
}

static void
demi_input_destroy(struct libinput *input)
{
}

static void
demi_seat_destroy(struct libinput_seat *seat)
{
	struct demi_seat *nseat = (struct demi_seat*)seat;
	free(nseat);
}

static struct demi_seat *
demi_seat_create(struct demi_input *input,
		    const char *device_seat,
		    const char *seat_name)
{
	struct demi_seat *seat;

	seat = zalloc(sizeof(*seat));

	libinput_seat_init(&seat->base, &input->base,
			   device_seat, seat_name,
			   demi_seat_destroy);

	return seat;
}

static struct demi_seat *
demi_seat_get_named(struct demi_input *input, const char *seat_name)
{
	struct demi_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		if (streq(seat->base.logical_name, seat_name))
			return seat;
	}

	return NULL;
}

static int
demi_device_change_seat(struct libinput_device *device,
			   const char *seat_name)
{
	struct libinput *libinput = device->seat->libinput;
	struct demi_input *input = (struct demi_input *)libinput;
	struct evdev_device *evdev = evdev_device(device);
	char *devnode;
	int rc;

	devnode = safe_strdup(evdev->devnode);
	device_removed(input, devnode);
	rc = device_added(input, devnode);
	free(devnode);

	return rc;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = demi_input_enable,
	.suspend = demi_input_disable,
	.destroy = demi_input_destroy,
	.device_change_seat = demi_device_change_seat,
};

LIBINPUT_EXPORT struct libinput *
libinput_create_context(const struct libinput_interface *interface,
				void *user_data)
{
	struct demi_input *input;

	if (!interface)
		return NULL;

	input = zalloc(sizeof(*input));
	input->fd= -1;

	if (libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		libinput_unref(&input->base);
		free(input);
		return NULL;
	}

	return &input->base;
}

LIBINPUT_EXPORT int
libinput_assign_seat(struct libinput *libinput,
			     const char *seat_id)
{
	struct demi_input *input = (struct demi_input*)libinput;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return -1;
	}

	/* We cannot do this during create_context because the log
	 * handler isn't set up there but we really want to log to the right
	 * place if the quirks run into parser errors. So we have to do it
	 * here since we can expect the log handler to be set up by now.
	 */
	libinput_init_quirks(libinput);

	if (demi_input_enable(&input->base) < 0)
		return -1;

	return 0;
}

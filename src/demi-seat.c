/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
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

#include "demi-seat.h"
#if !HAVE_UDEV

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "evdev.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

static struct demi_seat *
demi_seat_create(struct demi_input *input,
		 const char *device_seat,
		 const char *seat_name);
static struct demi_seat *
demi_seat_get_named(struct demi_input *input, const char *seat_name);


static inline bool
filter_duplicates(struct demi_seat *demi_seat,
		  struct demi_device *demi_device)
{
	struct evdev_device *device;
	const char *new_devnode;
	bool ignore_device = false;

	if (!demi_seat)
		return false;

	if (demi_device_get_devnode(demi_device, &new_devnode) == -1)
		return false;

	list_for_each(device, &demi_seat->base.devices_list, base.link) {
		const char *devnode;
		if (demi_device_get_devnode(&device->demi_device, &devnode) == -1)
			continue;

		if (streq(devnode, new_devnode))
			ignore_device = true;

		if (ignore_device)
			break;
	}

	return ignore_device;
}

static int
device_added(struct demi_device *demi_device,
	     struct demi_input *input,
	     const char *seat_name)
{
	struct evdev_device *device;
	const char *devnode, *sysname;
	const char *device_seat;
	struct demi_seat *seat;

	if (demi_device_get_seat(demi_device, &device_seat) == -1)
		device_seat = default_seat;

	if (!streq(device_seat, input->seat_id))
		return 0;

	if (demi_device_get_devnode(demi_device, &devnode) == -1)
		return -1;

	if (demi_device_get_devname(demi_device, &sysname) == -1)
		return -1;

	seat = demi_seat_get_named(input, default_seat_name);

	/* There is a race at startup: a device added between setting
	 * up the demi monitor and enumerating all current devices may show
	 * up in both lists. Filter those out.
	 */
	if (filter_duplicates(seat, demi_device))
		return 0;

	if (seat)
		libinput_seat_ref(&seat->base);
	else {
		seat = demi_seat_create(input, device_seat, seat_name);
		if (!seat)
			return -1;
	}

	device = evdev_device_create(&seat->base, NULL, demi_device);
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
device_removed(struct demi_device *demi_device, struct demi_input *input)
{
	struct evdev_device *device;
	struct demi_seat *seat;
	const char *devnode;

	if (demi_device_get_devnode(demi_device, &devnode) == -1)
		return;

	list_for_each(seat, &input->base.seat_list, base.link) {
		list_for_each_safe(device,
				   &seat->base.devices_list, base.link) {
			const char *old_devnode;

			if (demi_device_get_devnode(
				&device->demi_device, &old_devnode) == -1)
				continue;

			if (streq(devnode, old_devnode)) {
				evdev_device_remove(device);
				break;
			}
		}
	}
}

static int
demi_input_add_devices(struct demi_input *input, struct demi *demi)
{
	struct demi_device dd;
	struct dirent *de;
	struct stat st;
	mode_t type;
	int dfd;
	DIR *dp;

	dp = opendir("/dev/input");

	if (!dp) {
		return -1;
	}

	dfd = dirfd(dp);

	while ((de = readdir(dp))) {
		if (!strneq(de->d_name, "event", 5)) {
			continue;
		}

		if (fstatat(dfd, de->d_name, &st, 0) == -1) {
			continue;
		}

		type = st.st_mode & (S_IFCHR | S_IFBLK);

		if (!type) {
			continue;
		}

		if (demi_device_init_devnum(&dd, demi, st.st_rdev, type) == -1) {
			continue;
		}

		if (device_added(&dd, input, NULL) < 0) {
			demi_device_finish(&dd);
			return -1;
		}

		// demi_device_finish(device); // TODO
	}

	closedir(dp);
	return 0;
}

static void
evdev_demi_handler(void *data)
{
	struct demi_input *input = data;
	struct demi_device demi_device;
	enum demi_action action;
	enum demi_class class;
	const char *devname;

	if (demi_monitor_recv_device(&input->demi_monitor, &demi_device) == -1)
		return;

	if (demi_device_get_class(&demi_device, &class) == -1)
		goto out;

	if (class != DEMI_CLASS_INPUT)
		goto out;

	if (demi_device_get_devname(&demi_device, &devname) == -1)
		goto out;

	if (!strneq("input/event", devname, 11))
		goto out;

	if (demi_device_get_action(&demi_device, &action) == -1)
		goto out;

	if (action == DEMI_ACTION_ATTACH)
		device_added(&demi_device, input, NULL);
	else if (action == DEMI_ACTION_DETACH)
		device_removed(&demi_device, input);

out:
	(void)demi_device;
	// demi_device_finish(&demi_device); // TODO
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

	if (!input->demi_monitor_source)
		return;

	demi_monitor_finish(&input->demi_monitor);
	libinput_remove_source(&input->base, input->demi_monitor_source);
	input->demi_monitor_source = NULL;

	demi_input_remove_devices(input);
}

static int
demi_input_enable(struct libinput *libinput)
{
	struct demi_input *input = (struct demi_input*)libinput;
	struct demi *demi = &input->demi;
	int fd;

	if (input->demi_monitor_source || !input->seat_id)
		return 0;

	if (demi_monitor_init(&input->demi_monitor, demi) == -1) {
		log_info(libinput,
			 "demi: failed to initialize the demi monitor\n");
		return -1;
	}

	fd = demi_monitor_get_fd(&input->demi_monitor);
	input->demi_monitor_source = libinput_add_fd(&input->base,
						     fd,
						     evdev_demi_handler,
						     input);
	if (!input->demi_monitor_source) {
		demi_monitor_finish(&input->demi_monitor);
		return -1;
	}

	if (demi_input_add_devices(input, demi) < 0) {
		demi_input_disable(libinput);
		return -1;
	}

	return 0;
}

static void
demi_input_destroy(struct libinput *input)
{
	struct demi_input *demi_input = (struct demi_input*)input;

	if (input == NULL)
		return;

	free(demi_input->seat_id);
}

static void
demi_seat_destroy(struct libinput_seat *seat)
{
	struct demi_seat *useat = (struct demi_seat*)seat;
	free(useat);
}

static struct demi_seat *
demi_seat_create(struct demi_input *input,
		 const char *device_seat,
		 const char *seat_name)
{
	struct demi_seat *seat;

	seat = zalloc(sizeof *seat);

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
	struct demi_device *demi_device = &evdev->demi_device;
	int rc;

	// demi_device_ref(demi_device); // TODO demi
	device_removed(demi_device, input);
	rc = device_added(demi_device, input, seat_name);
	// demi_device_unref(demi_device); // TODO demi

	return rc;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = demi_input_enable,
	.suspend = demi_input_disable,
	.destroy = demi_input_destroy,
	.device_change_seat = demi_device_change_seat,
};

LIBINPUT_EXPORT struct libinput *
libinput_demi_create_context(const struct libinput_interface *interface,
			     void *user_data,
			     struct demi *demi)
{
	struct demi_input *input;

	if (!interface || !demi)
		return NULL;

	input = zalloc(sizeof *input);

	if (libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		libinput_unref(&input->base);
		free(input);
		return NULL;
	}

	input->demi = *demi;
	return &input->base;
}

LIBINPUT_EXPORT int
libinput_demi_assign_seat(struct libinput *libinput,
			  const char *seat_id)
{
	struct demi_input *input = (struct demi_input*)libinput;

	if (!seat_id)
		return -1;

	if (strlen(seat_id) > 256) {
		log_bug_client(libinput,
			       "Unexpected seat id, limited to 256 characters.\n");
		return -1;
	}

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return -1;
	}

	if (input->seat_id != NULL)
		return -1;

	/* We cannot do this during demi_create_context because the log
	 * handler isn't set up there but we really want to log to the right
	 * place if the quirks run into parser errors. So we have to do it
	 * here since we can expect the log handler to be set up by now.
	 */
	libinput_init_quirks(libinput);

	input->seat_id = safe_strdup(seat_id);

	if (demi_input_enable(&input->base) < 0)
		return -1;

	return 0;
}

#else

LIBINPUT_EXPORT struct libinput *
libinput_demi_create_context(const struct libinput_interface *interface,
			     void *user_data,
			     struct demi *demi)
{
	return NULL;
}

LIBINPUT_EXPORT int
libinput_demi_assign_seat(struct libinput *libinput,
			  const char *seat_id)
{
	return -1;
}

#endif

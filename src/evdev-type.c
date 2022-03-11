#include <stdint.h>
#include <libevdev/libevdev.h>

#include "evdev-type.h"
#include "linux/input.h"

uint32_t get_input_type(struct libevdev *evdev)
{
	uint32_t type = 0;
	unsigned long bit;

	if (libevdev_has_property(evdev, INPUT_PROP_POINTING_STICK)) {
		type |= EVDEV_TYPE_POINTING_STICK;
	}

	if (libevdev_has_property(evdev, INPUT_PROP_ACCELEROMETER)) {
		type |= EVDEV_TYPE_ACCELEROMETER;
	}

	if (libevdev_has_event_type(evdev, EV_SW)) {
		type |= EVDEV_TYPE_SWITCH;
	}

	if (libevdev_has_event_type(evdev, EV_REL)) {
		if (libevdev_has_event_code(evdev, EV_REL, REL_Y) &&
		    libevdev_has_event_code(evdev, EV_REL, REL_X) &&
		    libevdev_has_event_code(evdev, EV_KEY, BTN_MOUSE)) {
			type |= EVDEV_TYPE_MOUSE;
		}
	}
	else if (libevdev_has_event_type(evdev, EV_ABS)) {
		if (libevdev_has_event_code(evdev, EV_KEY, BTN_SELECT) ||
		    libevdev_has_event_code(evdev, EV_KEY, BTN_TR) ||
		    libevdev_has_event_code(evdev, EV_KEY, BTN_START) ||
		    libevdev_has_event_code(evdev, EV_KEY, BTN_TL)) {
			if (libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH)) {
				type |= EVDEV_TYPE_TOUCHSCREEN;
			}
			else {
				type |= EVDEV_TYPE_JOYSTICK;
			}
		}
		else if (libevdev_has_event_code(evdev, EV_ABS, ABS_Y) &&
			 libevdev_has_event_code(evdev, EV_ABS, ABS_X)) {
			if (libevdev_has_event_code(evdev, EV_ABS, ABS_Z) &&
			    !libevdev_has_event_type(evdev, EV_KEY)) {
				type |= EVDEV_TYPE_ACCELEROMETER;
			}
			else if (libevdev_has_event_code(evdev, EV_KEY, BTN_STYLUS) ||
				 libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_PEN)) {
				type |= EVDEV_TYPE_TABLET;
			}
			else if (libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH)) {
				if (libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_FINGER)) {
					type |= EVDEV_TYPE_TOUCHPAD;
				}
				else {
					type |= EVDEV_TYPE_TOUCHSCREEN;
				}
			}
			else if (libevdev_has_event_code(evdev, EV_KEY, BTN_MOUSE)) {
				type |= EVDEV_TYPE_MOUSE;
			}
		}
	}

	for (bit = KEY_ESC; bit < BTN_MISC; bit++) {
		if (libevdev_has_event_code(evdev, EV_KEY, bit)) {
			type |= EVDEV_TYPE_KEY;

			if (libevdev_has_event_code(evdev, EV_KEY, KEY_ENTER)) {
				type |= EVDEV_TYPE_KEYBOARD;
			}

			break;
		}
	}

	return type ? type : EVDEV_TYPE_UNKNOWN;
}

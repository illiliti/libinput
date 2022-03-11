#include <stdint.h>
#include <libevdev/libevdev.h>

enum {
	EVDEV_TYPE_UNKNOWN = 1 << 0,
	EVDEV_TYPE_MOUSE = 1 << 1,
	EVDEV_TYPE_TABLET = 1 << 2,
	EVDEV_TYPE_TOUCHPAD = 1 << 3,
	EVDEV_TYPE_KEYBOARD = 1 << 4,
	EVDEV_TYPE_JOYSTICK = 1 << 5,
	EVDEV_TYPE_TOUCHSCREEN = 1 << 6,
	EVDEV_TYPE_SWITCH = 1 << 7,
	EVDEV_TYPE_ACCELEROMETER = 1 << 8,
	EVDEV_TYPE_POINTING_STICK = 1 << 9,
	EVDEV_TYPE_KEY = 1 << 10,
};

uint32_t get_input_type(struct libevdev *evdev);

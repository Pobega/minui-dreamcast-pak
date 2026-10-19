// Mirrors a gamepad's hat (dpad) onto arrow keys on a virtual keyboard.
//
// flycast's menu only moves between items on dpad or arrow key input: imgui
// feeds an analog stick to GamepadLStick*, which only moves windows. So once
// the dpad is remapped to the Dreamcast analog stick, a device with no analog
// stick has nothing left to navigate the menu with.
//
// This mirrors the dpad onto arrow keys on a separate uinput device, which
// flycast picks up as a keyboard and hands to imgui. The pad itself is opened
// read only and never grabbed, so flycast keeps receiving the hat as well.
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int uinput_fd = -1;
static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
	(void)sig;
	running = 0;
}

static void emit(int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;

	if (write(uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		/* best effort: a dropped key is better than dying mid-game */
	}
}

static void key(int code, int pressed)
{
	emit(EV_KEY, code, pressed);
	emit(EV_SYN, SYN_REPORT, 0);
}

static int make_keyboard(void)
{
	static const int codes[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
	// the older uinput_user_dev API rather than UI_DEV_SETUP, so this builds
	// against any kernel headers the toolchain happens to ship
	struct uinput_user_dev device;
	size_t i;

	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}

	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
		perror("UI_SET_EVBIT");
		return -1;
	}
	for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		if (ioctl(uinput_fd, UI_SET_KEYBIT, codes[i]) < 0) {
			perror("UI_SET_KEYBIT");
			return -1;
		}
	}

	memset(&device, 0, sizeof(device));
	strncpy(device.name, "dc-dpad-arrows", UINPUT_MAX_NAME_SIZE - 1);
	device.id.bustype = BUS_VIRTUAL;
	device.id.vendor = 0x1209;
	device.id.product = 0x0001;
	device.id.version = 1;

	if (write(uinput_fd, &device, sizeof(device)) != (ssize_t)sizeof(device)) {
		perror("write uinput_user_dev");
		return -1;
	}
	if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
		perror("UI_DEV_CREATE");
		return -1;
	}

	return 0;
}

// Translate one axis into the key for its negative and positive direction,
// releasing whichever key it was holding before.
static void axis(int value, int *held, int negative, int positive)
{
	int want = value < 0 ? negative : (value > 0 ? positive : 0);

	if (want == *held) {
		return;
	}
	if (*held) {
		key(*held, 0);
	}
	if (want) {
		key(want, 1);
	}
	*held = want;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/input/event1";
	struct sigaction sa;
	int pad;
	int held_x = 0;
	int held_y = 0;

	// sigaction rather than signal: glibc's signal() implies SA_RESTART, which
	// silently restarts the blocking read below instead of failing it with
	// EINTR, so the mirror would ignore SIGTERM until the next dpad event and
	// outlive the session that started it.
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = stop;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	// read only, and deliberately no EVIOCGRAB: flycast needs this too
	pad = open(path, O_RDONLY);
	if (pad < 0) {
		perror(path);
		return 1;
	}

	if (make_keyboard() < 0) {
		return 1;
	}

	fprintf(stderr, "dpad-arrows: %s hat -> arrow keys\n", path);

	while (running) {
		struct input_event ev;
		ssize_t n = read(pad, &ev, sizeof(ev));

		if (n != (ssize_t)sizeof(ev)) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
				continue;
			}
			if (n < 0) {
				break;
			}
			continue;
		}

		if (ev.type != EV_ABS) {
			continue;
		}
		if (ev.code == ABS_HAT0X) {
			axis(ev.value, &held_x, KEY_LEFT, KEY_RIGHT);
		} else if (ev.code == ABS_HAT0Y) {
			axis(ev.value, &held_y, KEY_UP, KEY_DOWN);
		}
	}

	if (held_x) {
		key(held_x, 0);
	}
	if (held_y) {
		key(held_y, 0);
	}

	ioctl(uinput_fd, UI_DEV_DESTROY);
	close(uinput_fd);
	close(pad);

	return 0;
}

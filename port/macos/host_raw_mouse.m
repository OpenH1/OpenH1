/*
HOST_RAW_MOUSE.M

The mouse's own motion on macOS, unaccelerated. SDL's relative motion on
macOS is the NSEvent's delta, which the system's pointer acceleration has
already scaled, so the game's mouse aim (input.mouse_sensitivity, on
Counter-Strike 2's scale: port/linux/include/mouse_sensitivity.h) would not
turn 0.022 degrees a count. The GameController framework's GCMouse reports
the device's counts as they are: its handler runs on a queue of its own,
off the main thread, and adds them up here, and host_sdl_poll_event
(port/android/host/host_sdl.c) gives them to the game in place of SDL's
while the mouse is captured, as each poll finds them: SDL's motion events
take what has come since the last, and a poll that finds no event left
hands over what has come since as one more, so that a count that arrived
before the game reads its input this frame turns this frame's view.

A pointer GCMouse does not report (a trackpad, which is no GCMouse) keeps
SDL's motion: while no count has come for RAW_MOUSE_IDLE_NS, SDL's
deltas pass as they are. Not on iOS, whose SDL reads GCMouse itself.
*/

#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <pthread.h>
#include <time.h>

/* how long after the last count the mouse is taken to be still, and SDL's
motion (a trackpad's) to be the one moving */
#define RAW_MOUSE_IDLE_NS 100000000ull

static pthread_mutex_t raw_lock = PTHREAD_MUTEX_INITIALIZER;
static float raw_x, raw_y;
static unsigned long long raw_moved_ns;
/* the first count's time since the last take, 0 for none */
static unsigned long long raw_first_ns;
static dispatch_queue_t raw_queue;
static id raw_observer;

static unsigned long long raw_now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void raw_mouse_watch(GCMouse *mouse)
{
	mouse.handlerQueue = raw_queue;
	mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *input, float delta_x, float delta_y) {
		(void)input;
		pthread_mutex_lock(&raw_lock);
		/* (GCMouse's y is up; SDL's, and the game's, down) */
		raw_x += delta_x;
		raw_y -= delta_y;
		raw_moved_ns = raw_now_ns();
		if (!raw_first_ns)
			raw_first_ns = raw_moved_ns;
		pthread_mutex_unlock(&raw_lock);
	};
}

void host_raw_mouse_start(void)
{
	if (raw_queue)
		return;
	/* (as the system's own input: a count waits for no other work) */
	raw_queue = dispatch_queue_create("com.openh1.raw-mouse",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
	for (GCMouse *mouse in [GCMouse mice])
		raw_mouse_watch(mouse);
	raw_observer = [[NSNotificationCenter defaultCenter] addObserverForName:GCMouseDidConnectNotification object:nil
		queue:nil usingBlock:^(NSNotification *note) {
			raw_mouse_watch(note.object);
		}];
}

/* the counts since the last call, if the mouse has moved lately (else 0:
SDL's motion stands), and how long ago the first of them came, in
nanoseconds (for the motion's event's time) */
int host_raw_mouse_take(float *x, float *y, unsigned long long *age_ns)
{
	int active;
	unsigned long long now = raw_now_ns();

	pthread_mutex_lock(&raw_lock);
	active = raw_moved_ns && now - raw_moved_ns < RAW_MOUSE_IDLE_NS;
	*x = raw_x;
	*y = raw_y;
	*age_ns = raw_first_ns && now > raw_first_ns ? now - raw_first_ns : 0;
	raw_x = 0.0f;
	raw_y = 0.0f;
	raw_first_ns = 0;
	pthread_mutex_unlock(&raw_lock);
	return active;
}

#endif

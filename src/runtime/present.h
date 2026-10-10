/* The window: shows dos32's screen and feeds it keys and the mouse (present.c). */
#pragma once

/* Open the window and run its message loop until it closes. */
void present_run(void);

/* Close the window from another thread (the game exited). */
void present_close(void);

/* An Xbox controller as the mouse and keys (pad.c). */
#pragma once

/* Poll the pad once (fw x fh: the game's screen); speed_step(1) faster. */
void pad_poll(int fw, int fh, void (*speed_step)(int up));

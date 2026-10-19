# Pacing: the frame limiter Theme Park never had

## The symptom

Lifted and running natively, Theme Park booted and drew everything correctly,
and then:

- the attract demo cut between parks every half second;
- clicking the title opened the "What is your nickname" prompt, which closed
  itself about 0.1 s later, every time;
- typed letters appeared in the prompt for a moment before it vanished.

## The cause

The game has no frame limiter. On a 486 it ran as fast as the CPU could
simulate and draw, and every timeout in it counts loop iterations, not time.
The one that mattered: the input poll at `0x0040107F`, called once per pass of
every loop the game has, counts idle passes in `0x4C6606`, and at 3000 calls
`0x0044B55A`, which drops the state word `0x4C6652` to 0 and starts the attract
demo. Natively the title loop and the prompt loop run thousands of passes a
second, so 3000 idle passes is a fraction of a second.

Found with a hardware watchpoint on the state word (pcrecomp dos32
`DOS32_BREAK`), one run:

```
dos32: write 004C6652 = 0066 in sub_00402452 (esp 004D22B4, ret 00000000)
dos32: write 004C6652 = 0050 in sub_0040B7BB (esp 004D22BC, ret 004D22EC)
dos32: write 004C6652 = 0050 in sub_0040A836 (esp 004D2258, ret 00003D80)
dos32: write 004C6652 = 0000 in sub_0044B55A (esp 004D2230, ret 004D2244)
```

and then reading the input poll: `inc word [0x4C6606]; cmp ax, 0xBB8; jne; call 0x44B55A`.

## The fix, and why it is where it is

`pace_frame` (src/runtime/game.c) takes over the input poll and holds each call
to `--fps` per second (default 25), then runs the lifted body. It waits with
`dos32_idle`, which keeps the timer, keyboard and mouse interrupts flowing.

The first attempt paced the *present* (`0x0040226C`, which copies the back
buffer to the screen). That fixed the attract demo's speed but not the prompt:
the title and prompt loops never present through it. The input poll is the one
thing every loop calls once per pass, which is also why the game's own idle
timeout is counted there.

## Choosing the rate

There is no measured target: the original's speed was whatever the machine
managed. Running it in DOSBox as a reference needs input injected into DOSBox,
which reads DirectInput and so needs window focus, so that was not done.
At 25–30 fps:

- the attract demo shows each park for 5–10 s;
- the park calendar advances about a month every 15 s of play;
- the idle timeout is 3000 / 25 = 2 minutes.

All three are plausible for the original on a fast 486 or a Pentium. `--fps`
is the knob; it becomes the speed control on the roadmap.

## Headless runs

Pacing by the wall clock would make scripted tests depend on the host's speed
(the prompt appeared at different times in different runs). Headless, dos32
runs on a virtual clock: `pace_frame` advances guest time by exactly 1/fps per
frame instead of sleeping. The same script is then the same run, and 90 guest
seconds take about 5 real ones.

## Loops that never poll

Pacing at the input poll covers every loop that calls it. Two kinds of screen
don't (issues #4 and #5):

- **The year-end report** (`0x464B84`–`0x4667A7`) presents each pass through
  `0x470210` (320x200) or `0x445507` (640x480) and closes itself after 1000
  passes with the mouse still. On a 486 each pass redrew the screen; native,
  all 1000 went by inside one frame and the report flashed and vanished.
- **The staff and goods negotiation** (`0x434EA5`, state `0x32` staff, `0x4E`
  goods) copies its frames to the screen itself and waits for one vertical
  retrace a pass (`0x470BE4`). It lasts 620 passes, the other side answering
  every 62. Several passes fit inside one 70 Hz retrace pulse natively, so it
  was over in two or three seconds.

`pace_present` and `pace_retrace` (game.c) pace those frames at the same rate
as the rest: a present, or a negotiation's retrace wait, holds for the next
frame only when the input poll has not run since the last one, so the park's
own frames (poll, then present) are still paced once. The retrace wait is
paced only when called from the negotiation; the palette fades call it many
times a frame and keep the plain 70 Hz wait.

At 25 fps the report stays up about 15 s if left alone (a click closes it
sooner) and a negotiation takes about 18 s. How they were found: `TP_TRACE`
(testing.md) over the frame the report flashed in showed a block of functions
entered exactly 1000 times.

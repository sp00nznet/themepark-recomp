# Multiplayer: the network game Bullfrog left in

Theme Park shipped with a working network game nobody could reach. It was
never in the manual or the menus: `MAIN -m N` switched it on, and it needed
NetBIOS, a 1994 LAN service. The code is complete and wired into play. The
recomp now carries it over TCP, so it runs on a LAN or the internet (issue #2).

## Playing

Up to **4 players**. One hosts and the others join; the host needs its port
(TCP 7790 by default) reachable from the others. From the settings menu (F10,
or a right-click on the bars), *Network game*: *Host a game for N players*,
*Join a game...* (it asks for the host's address and remembers it) or *Resume
the game I hosted*. Each starts the program again for the network game. Or
from the command line:

```
themepark.exe --host 2                 player 1 of 2: waits for one more player
themepark.exe --join 192.168.1.20      the others: the host's address (or ADDR:PORT)
themepark.exe --host 4 --port 8000     another port; joiners then use ADDR:8000
themepark.exe --host 2 --resume        the host, after its own crash (below)
```

The host waits at start-up until every player has joined, and then everyone
plays the game as usual: each player goes through the title, the name prompt
and *Setup new park*, and buys land. The game starts once all of you are in a
park. Everything else is the normal game, plus:

- **Ctrl+T sends thugs** into every other player's park. It costs 5000 per
  player in the game (10 000 with 2 players, 20 000 with 4).
- **The other players' parks are your rivals** in the charts and the stock
  market, alongside the computer's.
- **The park never stops.** In one-player games the simulation pauses while a
  screen such as the bank or the charts is open, and the pause and the slow
  speeds work; in a network game time keeps going, and a speed change applies
  to everyone. **Only the host sets the speed** and the game type (sandbox,
  simulation, full); the others' choices go back to the host's.
- **The year end waits for everyone**: nobody's new year starts until every
  player has reached it.
- **Players are named**: the advisor and the charts call another player by the
  name they typed at the start (the 1994 game said *HUMAN*).
- **No cheat mode** in a network game.
- **Going bust, or quitting**: that player is out, and the rest play on. A
  player who goes bust sees the film and starts a new park on their own; the
  last player left plays on alone, and is told so. (A crash is different: see
  below.)

All players move in step, so the game runs at the speed of the slowest
machine. Loading a save state is switched off in a network game: it would put
that machine somewhere the others aren't.

### Dropping out and back in

If a player's game crashes, or their connection drops, nobody else's game
ends: everyone waits, and the title bar says who for. To come back:

- **A player** starts Theme Park again and joins the same host (*Join a
  game...*, or `--join`), then clicks the title screen once. They get their
  old place, and their park comes straight back.
- **The host** starts again with *Resume the game I hosted* (`--host N
  --resume`) and clicks the title. The others reconnect on their own.

The game then carries on from the last checkpoint every player has. Each
machine saves one every 30 seconds of play (on the park's frame, in the
`states` folder beside the program, the last six kept), so at most about 30
seconds are played again, by everyone. Whatever happened after that
checkpoint, house rules included, is undone, as it is for the park itself.

A player who left while a screen of the game's own was open (the year-end
report, say) comes back when that player's machine is back on the park, so
close it. If no checkpoint is shared by everyone (a player dropped in the
first 30 seconds), the game cannot carry on and says so.

### House rules

The settings menu's *Network game* has a submenu for each other player:

- **Give** 1000, 5000, 10 000 or 50 000 of your own cash. It is booked in
  your accounts as *Misc.* and in theirs as a *Cash award*, and it needs the
  money: you can't give what you haven't got.
- **Share my research with them**: every ride, shop and feature you discover
  from then on, they get too, as if they had researched it (the research
  button flashes). Two players who both share have a research agreement;
  one can share without the other.
- **Grant** or **fine** 1000, 10 000 or 50 000, the host only: money from
  nowhere, or taken away. For house rules like Monopoly's money on Free
  Parking, or a handicap for the best player.

There is no lending as such: give the money, and give it back later. Each
park's own bank still lends as in one-player games. These are friendly
rules: they trust the players, like the rest of the network game.

## How it works

### Sessions

The network layer is about twenty functions at `0x4381C5`–`0x439B5B`. The
player count from `-m` is the byte at `0x4C7792`; above 1, the command line
parser calls `0x4393BE` to start the network, and falls back to one player if
that fails.

On DOS, each machine registered a NetBIOS name `PARK0` to `PARK7`, and the
first one it got was its player number (`0x4A91A7`, then `0x4C665C`). It then
made a session with every other player: a full mesh, every pair connected,
with a LISTEN for each player and a CALL to each one still missing, until all
were connected (`0x439414`, at new-game set-up, `0x40A0E0`). Every command was
issued no-wait through DPMI (int 31h `0300h`, real-mode int 5Ch) and polled
on the command block's completion byte. Only sessions were used, never
datagrams: ADD NAME, ADD GROUP NAME, LISTEN, CALL, SEND, RECEIVE, CANCEL and
HANG UP. If NetBIOS was not loaded, the game ran `netbios` (a TSR expected in
its folder) and tried again.

### Every frame

The command dispatcher (`0x41CCF8`, from the main loop) calls `0x4399F2`
once a frame. Each machine sends every other its 6-byte command slot
(`0x4C441E` + 6 x player) and its 4-byte tick (`0x4C3AFC`), and receives
theirs. All four slots then go through the dispatcher, so every machine runs
every player's commands in the same frame. The tick comparison at `0x439B0B`
does nothing, and there is no checksum.

Everything a player does is a command, in one-player games too: the opcode
at +1 and two word arguments at +2 and +4, dispatched through the table at
`0x41CD30`.

- **Park commands** (building, prices, staff: opcodes `0x02`–`0x24` and
  `0x2C`) run only on the machine whose player sent them. Each machine
  simulates only its own park.
- **Shared commands** run everywhere:
  - `0x32` / `0x34`: ready, counted in `0x4C7786` (the year-end barrier,
    `0x466D8C`)
  - `0x36`: thugs (below)
  - `0x38`: a cheat: money and a *HAS JUST CHEATED!* message
  - `0x3A`: new game, with a 32-bit seed (`0x4C3AF4`)
  - `0x40`: ask for a full sync
  - `0x42`: game speed (`0x4C6634`)

### Full syncs

`0x439704` makes every machine agree on the shared world. Player 1 (number 0)
is the master.

1. Each other player sends it its own records: 0x6C2 bytes at `0x4C443C`,
   0x222 bytes at `[0x4C3B00]` and 0x250 bytes at `0x4BF1E0` (each indexed by
   player).
2. The master sends everyone back the random seed (`0x4C3AF4`), every
   player's 0x6C2-byte record, and both 25-park tables: 25 x 0x222 bytes and
   25 x 0x250 bytes, about 30 KB. The 25 parks are the players and the
   computer's rivals.

It runs when the players enter their parks, three times at the year end
(`0x46401C`), once more when the year-end report closes (`0x466EF9`), and on
request (`0x40`). Between syncs each machine moves its own park and its own
random numbers on, so the tables drift apart until the next one.

### Thugs

Ctrl+T (`0x403F2A`, scancode `0x14` with Ctrl in the BIOS shift flags)
issues opcode `0x36`, network games only. The sender pays 5000 x the player
count (`0x41DD21`, finance category `0x0E`). Every other machine calls
`0x43C592`, which turns a free person record into a thug (type `0x1A`): the
same spawner the game's own trouble-makers come from.

### How many players

Four, not the sixteen once claimed. The dispatcher loops over 4 slots, there
are 5 command slots, and 5 player records fit before the next variables
(`0x4C6606`). There are 8 names and 8 sessions, but `-m 5` or more would write
past the records.

## The recomp's transport

`src/runtime/net.c` replaces only the transport; every byte the game sends is
its own. These functions are taken over (`run_lift.py` `OVERRIDES`):

| Function | On DOS | Here |
|---|---|---|
| `0x4393BE` | start the network | make the TCP links (`--host` / `--join`), then the game's own set-up |
| `0x438A01` | is NetBIOS loaded? | yes |
| `0x439414` | make the sessions | fill in what they would have: player count `0x4A91A9`, own number `0x4A91A7`, the slot table `0x4C7810` |
| `0x438F65` | send on a session | send to that player |
| `0x4390DF` | receive on a session | wait for exactly that many bytes from that player |
| `0x41CCF8` | the command dispatcher | who called it, and checkpoints and rollbacks (below) |
| `0x4399F2` | the frame's exchange | house rules, and the count of frames in step |
| `0x439B20` | hang up | close the links |

Which player a send or receive is for comes from its session block: the
game passes the block from its table at `0x4C7828` (6-byte far pointers, one
per player), and the game's own set-up still allocates those blocks.

The links are a star: each joining machine connects to the host, which is
player 1, the game's master. The host passes on what other players send each
other. Each message is a header (to, from, type, epoch, length) and the
bytes. Every pair of players is a stream in order, and that is all the game
needs: every receive asks for exactly what the other side sent. `TCP_NODELAY`
keeps the per-frame 10-byte messages from waiting. `TP_NETLOG=1` logs every
transfer bigger than a frame's command (which shows the syncs), each
checkpoint and each rollback.

### Checkpoints and rollbacks

A checkpoint has to be taken where every machine is at the same point of the
game's traffic, and where a machine that has just started can get back to.
The command dispatcher (`0x41CCF8`) is the first: its first act is the
frame's exchange, so at its start each machine has sent and received the
same frames. For the second it must be called from the main loop's frame
(`0x40226C`, which runs in every state of the game except the title, at one
depth of the stack) and not from a screen's own loop (the year-end report
`0x464B84`, the barrier `0x466DB5`, `0x46B78D`, `0x46C03E`). The lifted code
pushes a dummy return address, so the caller is told by the dispatcher's
override, which sees its caller in `g_cur_func`.

A pcrecomp save state loads only where the host's own C stack is as deep as
when it was saved (the lifted code above would otherwise return into the
wrong frames). So the checkpoint is one function, called unconditionally at
the top of the dispatcher's override, and the save or load inside it is one
indirect call: with a call site for each, gcc placed them 16 bytes apart and
no checkpoint would load on Linux.

Frames are counted from the moment the sessions exist (`0x4A91AB`), so every
machine's frame N is the same frame. Every 750 of them (`TP_NET_CP`) each
machine saves a pcrecomp save state, `states/net-GAME-pPLAYER-FRAME.tps`,
keeps the list in `net-GAME-pPLAYER.txt`, and tells the host. When a link
breaks, the host marks that player away and tells the others; a joiner whose
host has gone keeps redialling. A new program that joins a game under way
gets the place that is free (or the one it had: `net-last-join.txt`), and
sends its list of checkpoints. Once every player is back, the host picks the
newest checkpoint on every list and announces a rollback with a new epoch.

From then until it has loaded that checkpoint, a machine's sends go nowhere
and its receives come back blank, so it runs on to its next main-loop frame,
where it loads the state and carries on. A program that has just started is
on the title screen, which does not run the frame tick; the click on the
title gets it to the name prompt, which does, at the same depth as the park,
so the state loads there and the park comes back. Traffic from before the
rollback carries the old epoch and is dropped.

### Leaving for good

In the 1994 game a player who went bust (`0x4629D8`, from the year end
`0x46401C`) saw the film (`0x44C7D1`, argument 1) and got a new park
(`0x409EEE`) on their own, while the others waited at the year-end barrier
for them for ever: a black screen with the park's sound. Now that film, the
game's quit (`0x439B20`) and closing the window send a goodbye (`LEFT`, the
host passes it on). Each machine then takes that player out the way the game
itself would skip a missing one: their slot byte (`0x4C7810`) goes to 0, so
the exchange and the syncs pass them by, and the player count (`0x4C7792`),
which the barrier counts to, goes down by one. At one player, it is a
one-player game. If the host leaves, nobody can reach anyone, so every other
player plays on alone.

### Speed, game type, names, cheats

- The speed is opcode `0x42` (`0x4C6634`), run on every machine: after the
  exchange, any but the host's is dropped.
- The game type is `0x4C66F1` (0 sandbox, 1 simulation, 2 full; a new game
  sets 2 at `0x40A0C8`), each machine's own. The host sends its own; the
  others are set to it, with the menu's ticks.
- A park's owner is named from LANG group 1 (pointer table `[0x4C7B98]`) by
  the park's `+1` byte, which is its number, so player 1 was *HUMAN* and
  player 2 *BINGO HIGHWAY*. Each machine sends its player's name
  (`0x4C66B0`), kept at the end of the fifth player record (`0x4C6586`,
  unused with 4 players at most), and those entries point at it.
- Cheat mode is word `0x4C6682` (set by the name, `0x41EABB`), cleared every
  frame.

### House rules

`src/runtime/net_rules.c`. An action goes to the receiving player's machine
as a message of its own and is done there at the next main-loop frame, on
the game thread. Only the receiver's own park changes, and that is the only
park its machine simulates; the others see the result in the charts at the
next sync.

- **Money** goes through the game's ledger, `0x468943(category, amount)`,
  called on the guest stack. It changes the cash (`[0x4C3808]+0x0A`) and
  books the entry, so the finance screens and the year's accounts agree.
  A gift is category `0x18` (*Misc.*) out of the giver, after a check that
  cash less money committed (`+0x0E`) covers it, and `0x0F` (*Cash award*)
  into the receiver. The host's grants are `0x0F`, its fines `0x18`.
- **Research.** Which rides, shops and features are available is a flag
  byte per item in tables each machine keeps for itself, and no sync carries
  them: rides `0x4BCED0` (29 x 0xC8, flag at +0xC5), shops `0x4C3C62`
  (17 x 0x6B, +0x66), features `0x4BCA92` (19 x 0x38, +0x13). Bit 0 is
  available, bit 1 new. Each main-loop frame, any item that has become
  available is sent to the players this machine shares with; the receiver
  sets bits 0 and 1, as the game's research does (`0x443F13`), and flashes
  the research button (`0x4C7768`). The build menus are rebuilt from the
  flags each time they open.

The game's own opcode `0x38` is a cheat, not a gift: it adds an amount from
the receiver's own command slot, books nothing, and shows *%s HAS JUST
CHEATED!*. Nothing sends it.

## Tested

Headless, on one machine: each player scripted into a park through the usual
menus, and dumps of memory compared. `py -3 tools/mpcheck.py --players N`
does it again (2, 3 and 4 pass), and `--rejoin` the crash and rejoin.

- **2, 3 and 4 players** all connect and get numbers 1 to N, and every park
  runs.
- **Syncs:** straight after the sync at the park gates and after the year-end
  syncs, the seed, the tick, all player records and both 25-park tables are
  byte-identical on every machine (4 players, 2 checkpoints, all pairs).
- **Thugs:** Ctrl+T from player 1 (2 players) and from player 3 (3 and 4
  players, so through the host's relay) puts one thug in every other park
  and none in the sender's. The sender's money drops by 5000 x players.
- **House rules:** the host gives player 2 5000 and grants them 20 000:
  player 2 ends exactly 30 000 above the host (less the thugs, when player 2
  sent them). A ride that becomes available on the host turns up in player
  2's park.
- **A player crashing:** player 2's program ends at 65 s; the host waits; a
  new program joins with one click on the title, and both go back to the
  checkpoint at frame 1350. Player 2's park is back with the 20 000 granted
  before the crash, and thugs the host sends afterwards arrive.
- **The host crashing:** the host ends at 65 s; player 2 waits and redials;
  the host comes back with `--resume` and a click; both go back to frame
  1350, and the host's thugs reach player 2.
- **Both clocks:** the virtual clock and real time (`TP_REALTIME=1`).
- **Linux:** the same checks pass natively (gcc, WSL Debian): 2 and 4
  players, the house rules and the rejoin. Player 2's park after the rejoin
  has the same cash on Linux as on Windows.
- One-player conformance is unchanged (6/6), on both.

Two bugs only Linux showed, both fixed: a blocking `recv` that dos32's timer
signal interrupts returns `EINTR`, which is now retried rather than taken
for a lost player (and `send` uses `MSG_NOSIGNAL`); and the save and load
call sites above.

Not tested yet: the settings menu's *Network game* by hand (it is built on
Windows and Linux, not yet clicked), two players at windows, over a real LAN,
or with Linux and Windows on one game; a long game; a drop during the year end; the
stock market and the other shared opcodes (`0x3C`, `0x3E`, `0x5A`) in play;
and the multiplayer-only panel at `0x46BFFD`. A note in the code also says
buying shares skips the money check in a network game (`0x446AFA`,
`0x446C85`); that is not yet confirmed in play.

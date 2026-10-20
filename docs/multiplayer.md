# Multiplayer: the network game Bullfrog left in

Theme Park shipped with a working network game nobody could reach. It was
never in the manual or the menus: `MAIN -m N` switched it on, and it needed
NetBIOS, a 1994 LAN service. The code is complete and wired into play. The
recomp now carries it over TCP, so it runs on a LAN or the internet (issue #2).

## Playing

Up to **4 players**. One hosts and the others join; the host needs its port
(TCP 7790 by default) reachable from the others.

```
themepark.exe --host 2                 player 1 of 2: waits for one more player
themepark.exe --join 192.168.1.20      the others: the host's address (or ADDR:PORT)
themepark.exe --host 4 --port 8000     another port; joiners then use ADDR:8000
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
  to everyone.
- **The year end waits for everyone**: nobody's new year starts until every
  player has reached it.

All players move in step, so the game runs at the speed of the slowest
machine. If anyone quits or loses the connection, the game ends for everyone
(the others print *a player has left the game* and close).

Don't use save states (F10) in a network game: loading one puts that machine
somewhere the others aren't.

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
  - `0x38`: money and a message
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
its own. Six functions are taken over (`run_lift.py` `OVERRIDES`):

| Function | On DOS | Here |
|---|---|---|
| `0x4393BE` | start the network | make the TCP links (`--host` / `--join`), then the game's own set-up |
| `0x438A01` | is NetBIOS loaded? | yes |
| `0x439414` | make the sessions | fill in what they would have: player count `0x4A91A9`, own number `0x4A91A7`, the slot table `0x4C7810` |
| `0x438F65` | send on a session | send to that player |
| `0x4390DF` | receive on a session | wait for exactly that many bytes from that player |
| `0x439B20` | hang up | close the links |

Which player a send or receive is for comes from its session block: the
game passes the block from its table at `0x4C7828` (6-byte far pointers, one
per player), and the game's own set-up still allocates those blocks.

The links are a star: each joining machine connects to the host, which is
player 1, the game's master. The host passes on what other players send each
other. Each message is a small header (to, from, length) and the bytes. Every
pair of players is a stream in order, and that is all the game needs: every
receive asks for exactly what the other side sent. `TCP_NODELAY` keeps the
per-frame 10-byte messages from waiting. `TP_NETLOG=1` logs every transfer
bigger than a frame's command, which shows the syncs.

## Tested

Headless, on one machine: each player scripted into a park through the usual
menus, and dumps of memory compared. `py -3 tools/mpcheck.py --players N`
does it again: it checks the connection, the sync and the thugs, and passes
for 2, 3 and 4.

- **2, 3 and 4 players** all connect and get numbers 1 to N, and every park
  runs.
- **Syncs:** straight after the sync at the park gates and after the year-end
  syncs, the seed, the tick, all player records and both 25-park tables are
  byte-identical on every machine (4 players, 2 checkpoints, all pairs).
- **Thugs:** Ctrl+T from player 1 (2 players) and from player 3 (3 and 4
  players, so through the host's relay) puts one thug in every other park
  and none in the sender's. The sender's money drops by 5000 x players.
- **Both clocks:** the virtual clock and real time (`TP_REALTIME=1`).
- One-player conformance is unchanged (6/6).

Not tested yet: two players at windows by hand, over a real LAN, or with
Linux and Windows mixed; a long game; the stock market and the other shared
opcodes (`0x38`, `0x3C`, `0x3E`, `0x5A`) in play; and the multiplayer-only
panel at `0x46BFFD`. A note in the code also says buying shares skips the
money check in a network game (`0x446AFA`, `0x446C85`); that is not yet
confirmed in play.

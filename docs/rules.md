# Modern rules: fixes to how the 1994 game plays

Theme Park was rushed out in 1994, and some of its staff and guests never
quite worked: guards stand still while thugs go by, guests fence themselves
into corners and stay there, and the advisor falls silent an hour in. The
recomp keeps the game as Bullfrog shipped it, and offers changes to these as
**modern rules**: each one is **off** until you turn it on.

## Turning them on

F10 (or a right-click on the bars), *Modern rules*, then tick the ones you
want. They take effect at once and are kept in `themepark.ini`:

```ini
[rules]
guards=1
unstick=1
advisor=1
```

For one run, `--rules guards,unstick` turns exactly those on (and the rest
off); `--rules none` turns them all off.

**In a network game** everyone plays by the host's rules: the host's travel
to each player as they join, and the menu shows them but cannot change them
until the game is over. The rules change the simulation, and every machine
in a network game must run the same one.

## The rules

### Guards go after thugs nearby (`guards`, issue #8)

**In 1994:** a guard on patrol stands where you put it, turning now and then.
Every 16 seconds it looks once in the way it faces, one to five tiles along
the path, and only a thug standing right there sets it walking. A thug a
tile off to the side, or behind, goes unseen. It catches a thug on its own or
the next tile, and walks it out of the park.

**With the rule:** about once a second, a patrolling guard that is not busy
heads for the nearest thug within 12 tiles, by the game's own walking, and
catches it there as before. Put guards where thugs come in, and they deal
with them.

In the demo park, with one guard and 12 thugs sent in over 15 minutes, the
guard caught 5 without the rule (6 were still loose at the end) and all 12
with it, each in less than half the time.

### Stuck guests find their way out (`unstick`, issue #17)

**In 1994:** a guest who cannot find a way out (usually one fenced or hedged
into a pocket, or one the game's short search loses in a maze of paths)
stands still, asking again every two seconds, and the advisor tells you bad
planning has got someone stuck. They stay until you clear a way.

**With the rule:** after a few failed tries (up to about 8 seconds) the guest
is moved. If it is in a pocket with no way to the gate, it goes to the nearest
tile that has one; if there is a way the game's search missed, it goes 8 tiles
along the shortest one. Then it walks on as usual. Guests and paths are
otherwise unchanged, so the advisor's message still appears while someone is
stuck, but only for those few seconds.

### The advisor keeps advising (`advisor`, issue #16)

**In 1994:** each piece of advice waits longer before it can come again the
more often it has been given, and the count is never reset, not even by a
new game. An hour in, most advice has gone quiet, and what is left is the
few messages that ignore the wait (rides wearing out, guests stuck). And the
advice to raise a shop's prices is worked out from a running total that only
grows, so after a while it suggests prices twenty times what anyone pays.

**With the rule:**
- the wait stops growing after the fourth time, so useful advice comes back
  about once a game month;
- in mid November, the advisor warns that the year ends next month;
- when research finishes a ride or shop, the advisor says which, and that it
  is in the build menu (the flashing research button still works as before);
- the shop-price advice sees that running total capped, so it asks for at
  most about twice a shop's base price.

![The advisor on new research](screenshots/advisor-research.png)

These are on your screen only: what the advisor says is not part of the
simulation, so they never change a network game's other machines.

## How they work

`src/runtime/rules.c` holds each rule, with the game's own routine it
replaces and what that routine does in 1994. Each rule is a host function in
place of one of the game's (run_lift.py's `OVERRIDES`): it calls the original
and then, if its rule is on, changes what the original decided. They read and
write only the game's memory, never anything kept on the host, so every
machine of a network game makes the same decisions in the same frame, and a
save state carries everything a rule depends on.

The people (guests, staff, thugs) are 200-byte records; the comment at the
top of rules.c has the fields the rules use.

## Testing them

```
themepark.exe --headless --lowres --seconds 900 ... --rules guards --call 55:422232:c6d:8 --call 56:43c592:0
```

- `--call T:ADDR[:A1[:A2[:A3]]]` calls one of the game's functions at T
  seconds, between frames: `0x41E035 fffffffe` loads the demo park as a
  game, `0x422232 TILE JOB` hires staff (job 8: a guard), `0x43C592 0` sends a
  thug.
- `TP_PEOPLELOG=N` prints every guard and thug every N frames;
  `TP_RULESLOG=1` prints what each rule did.
- `work/ai/guards.sh` (in the development tree) is the guard test above.

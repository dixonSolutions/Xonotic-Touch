# Findings — the scoreboard, and a CSQC command that silently did not exist

## 1. There was no way to see the scoreboard

A touch-only session could not open the scoreboard at all. Both routes assumed a
keyboard:

* `bind TAB +showscores` — no TAB.
* `touch_scoreboard_gesture`, a two-finger tap. It shipped `0`, and its handler had
  already been reduced to `if (!gesture || fingers < 2) touch_two_finger_time = 0;`
  — no call to anything. So `casual`, `competitive` and `minimal`, all three of
  which set it to `1`, were enabling nothing.

All a player had was the three-line score readout in the top-right corner: no
pings, no damage, no team totals, no player list.

Fixed with a SCORE pill next to CONSOLE, and the dead gesture and cvar removed.
Verified on device: [77-scoreboard.jpg](shots/77-scoreboard.jpg) (opened, player
dead), [78-scoreboard-alive.jpg](shots/78-scoreboard-alive.jpg) (opened while alive,
controls still live over the board).

Two properties make the toggle safe rather than a trap, and both were checked:

* `Touch_Draw` runs after `HUD_Draw` in `view.qc`, so the pill draws *on top of* the
  board it opened and stays the way out of it.
* `Touch_ReleaseAll` releases `+showscores`, so it cannot survive into intermission
  or mapvote where the overlay is gone.

Both directions of the toggle were exercised: tap → board, tap → cleared, tap →
board again. Had the first tap missed, the final state would have been "cleared".

## 2. A CSQC command has to be registered or the engine never routes it

`touch_scores` was added to `Touch_ConsoleCommand` alongside `touch_chat`, compiled
clean, and did nothing. The engine printed:

```
Unknown command "touch_scores"
```

`CSQC_ConsoleCommand` is only consulted for names passed to `registercommand()`.
The dispatch branch is dead code without it, and nothing warns: the compiler cannot
see the connection, and the branch reads as if it works.

Worth remembering as a rule — **a new console command needs two edits**, the handler
branch and the `registercommand` call in `Touch_Init`.

Found by pressing the key and reading `/tmp/xonotic-dev.log`. It would not have been
found by inspection; the code looked right. The dev-loop bind for it (F4) is now in
`scripts/dev-deploy.sh` so the command path stays testable.

## 3. Two pairs of the same control, two different spacings

CONSOLE and SCORE sat 0.02 apart while MENU and CHAT sat 0.073 apart — one kind of
control with two rhythms. SCORE moved to give both pairs the same centre-to-centre
pitch (0.182).

In the same crop it showed that each pill was fitting its label independently, so
CONSOLE (7 characters) rendered about 8% smaller than SCORE (5). Chrome pills are
one level of the hierarchy, so they now all take the size the longest label in the
family needs.

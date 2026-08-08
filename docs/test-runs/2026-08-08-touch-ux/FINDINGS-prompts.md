# Findings — prompts, cost, and two false alarms

Round after the scoreboard work. Everything here came from playing a live bot match
on the Surface Pro 9 and reading frames, not from reading code.

## 1. The game kept telling the player to press keys

Dying printed **"You are dead, press SPACE to respawn"**. There is no SPACE on this
device. Tapping HOP respawned correctly ([90-respawn-hop.jpg](shots/90-respawn-hop.jpg)),
so the action was always reachable — the instruction was the broken part, and it is
the one piece of the interface a player reads when they are stuck.

Spectating was the same story: "Press SPACE to join", "Press primary fire to
spectate", and the campaign hint all name a bind.

These all come from one function, `_getcommandkey`, which is what made this a small
fix instead of a message-by-message hunt. The touch layer now answers there with the
label of the control that performs the command:

* [92-respawn-prompt.jpg](shots/92-respawn-prompt.jpg) — "You are dead, press **HOP** to respawn"
* [93-spectate-prompt.jpg](shots/93-spectate-prompt.jpg) — "Press **HOP** to join" while observing

`+show_info` still says "I", because there is no touch control for gametype info.
Naming one would just move the dead end somewhere the player can't reach either.

Worth keeping: the same word now has one home (`TOUCH_LABEL_*` in `touch_defs.qh`).
It had three — drawn label, edit-mode handle, and now these prompts — and a rename
in two of three would have the game naming a button that does not exist.

## 2. 183 draw calls, and no cost to show for it

The `touch_debug` line reports **183 fills** for a live frame, well over the 78 that
the controls alone measured and over the 34 that TOUCH_LAYOUT_SPEC treats as a
ceiling. Rather than optimise on the strength of a number, A/B'd it:

| Overlay | Reported fps |
|---|---|
| On (183 fills) | 30 |
| Off (`vid_touchscreen 0`) | 30 |

So the overlay is free at this scale on this device, and the 30 fps is the scene.
The whole probe is one cfg file and two screenshots
([87-fps.jpg](shots/87-fps.jpg), [88-nooverlay.jpg](shots/88-nooverlay.jpg)) — cheap
enough that there is no excuse for guessing.

## 3. False alarm: a blue, off-centre move knob

One frame showed the stick's knob filled with the accent colour and displaced up-left
with nobody touching the device — which reads exactly like a stuck finger holding
forward, the worst bug this layer could have. It was not: `touch_debug 1` reports per
finger state, and it showed `f0:- f1:- f2:- f3:-` with the knob back at rest
([86-debug.jpg](shots/86-debug.jpg)). A one-frame artifact of the injected respawn
keypress, not held input.

Worth noting the method: the debug line answers "is a finger live" directly, so it
should be the first thing checked before reasoning about what a frame implies.

## 4. False alarm: a stock panel reappearing while spectating

A four-row panel with icons and counts appears at the left edge while observing
([93-spectate-prompt.jpg](shots/93-spectate-prompt.jpg)), which looked like the stock
ammo panel coming back and our `hud_panel_ammo 0` being clobbered by a HUD config
reload. Probing the live cvars said otherwise:

```
XTPROBE ammo=0 ha=0 wep=0 notify=1 chat=1 mobile=1
```

It is `hud_panel_itemstime`, which shows item respawn timers *only* while
spectating — useful there, and it cannot collide with the vitals group because the
vitals have nothing to show when you are not alive.

## 5. Engine screenshots go black in some states — and the compositor is the fallback

Several captures came back as pure black (`mean=0 max=0`), with the local file
byte-identical to the one on device, so it was not a transfer race: the engine wrote
a black frame. `gdr screenshot` of the same moment was correct
([92-respawn-prompt.jpg](shots/92-respawn-prompt.jpg)).

This is the exact inverse of the earlier finding, and both are true: during a live
match the compositor copy is stale and only the engine sees the frame, but in some
states — around death and level transitions — the engine writes black and the
compositor is right. So neither is the capture method; whichever one returns a
plausible frame is, and a black JPEG should be re-checked with the other rather than
interpreted.

# Comfortable touch controls for mobile shooters and kart racers

Research brief for Xonotic Touch and SuperTuxKart Touch (September 2026).
Sources are linked inline; the checklists at the end are what the two ports
act on. The same file lives in both repositories.

## 1. Shooter touch controls: where the big titles converge

**Two-region default.** PUBG Mobile's default is "hold the left area of the
screen to move… and control the camera angle with the other thumb on the right
side"; it ships with **a fire button on both sides** so either thumb can shoot
([Google Play editorial](https://play.google.com/store/apps/editorial?id=mc_editorial_evergreen_post_install_pubg_mobile_improve_your_controls_now_fcp)).
CoD Mobile, Fortnite and Apex Mobile use the same move-left / look-right split.

**Floating vs fixed stick.** Both PUBG and CoDM default to a floating stick
that spawns under the first touch and offer a "Fixed Joystick" option
([Activision](https://blog.activision.com/call-of-duty/2019-10/Getting-a-Grip-on-the-Call-of-Duty-Mobile-Controls)).
Microsoft's Touch Adaptation Kit guide (200+ games, xCloud telemetry) says
non-relative sticks give a "higher skill ceiling" while relative sticks feel
familiar; use relative when analog magnitude matters (walk→run)
([Microsoft](https://learn.microsoft.com/en-us/gaming/gdk/_content/gc/system/overviews/game-streaming/building-touch-layouts/game-streaming-tak-designers-guide)).
Suzy Cube's post-mortem argues for a *draggable* floating stick that re-centres
on touch-down so a reversal is "only as far away as the radius of the d-pad"
([Game Developer](https://www.gamedeveloper.com/design/lessons-from-suzy-cube-mobile-controls-that-feel-great)).

**Fire modes.** Fortnite Mobile offers three: Auto Fire, Tap Anywhere,
Dedicated Button ([Fortnite wiki](https://fortnite.fandom.com/wiki/HUD_Layout_Tool),
[GameRevolution](https://www.gamerevolution.com/guides/407037-fortnite-mobile-auto-fire-how-to-use-it-is-it-cheating-turn-off)).
CoDM's Simple Mode auto-fires when the crosshair crosses an enemy; Advanced
Mode adds a hip-fire button and a "Fixed R-Fire Button" that stops the fire
button following the finger. Apex Mobile recommends tap-to-fire for beginners
with auto-fire toggleable
([GamingOnPhone](https://gamingonphone.com/guides/apex-legends-mobile-best-settings-guide/)).
Microsoft: put Aim+Fire "more toward the center of the screen… If it's placed
at the edge… they'll run out of room to swipe."

**Jump/crouch.** Microsoft's placement model is two arcs radial from each
grip: one primary action in the inner ring under the thumb, secondary at
upper-outer, "jump-like actions in the tertiary slot" directly below the
primary; upper corners for menu/system. On tablets reaching upper/lower zones
"might require a player to consciously move their thumbs."

**Sensitivity/acceleration.** CoDM exposes Fixed Speed / Distance Acceleration
/ Speed Acceleration; community consensus is Fixed Speed for steady aim
([Sportskeeda](https://sportskeeda.com/esports/codm-guide-best-aim-and-sensitivity-settings-for-call-of-duty-mobile)).

**Gyro.** Apex offers "On while ADS" vs "Always On" plus invert axes
([GamingOnPhone](https://gamingonphone.com/guides/apex-legends-mobile-guide-how-to-enable-and-customize-gyroscope/)).
Microsoft: gyro→relative mouse "is far superior for aiming… over using a
joystick"; a 90° phone turn usually maps to ~120° in-game; always-on gyro
fails in moving vehicles. Jibb Smart's guidance: natural sensitivity 1.0 = 1:1
degrees, expose 0–10 in 0.1 steps (competitive players sit at 4–5), and disable
gyro while the look-stick is deflected or via a held button
([Game Developer](https://www.gamedeveloper.com/design/the-absolute-basics-of-good-gyro-controls)).

**Weapon switch.** PUBG/Fortnite use a bottom-centre weapon strip; id-engine
ports (Quad Touch, Delta Touch) use a hold-to-open weapon wheel with its own
transparency setting and 6–18 custom buttons, plus gyro aim assist and full
gamepad support ([Quad Touch](https://play.google.com/store/apps/details?id=com.opentouchgaming.quadtouch),
[DoomWiki](https://doomwiki.org/wiki/Delta_Touch)). id's 2019 DOOM re-release
shipped controls that "completely blocked the view" and had to be redone with
slower movement momentum
([TouchArcade](https://toucharcade.com/2020/09/14/doom-and-doom-ii-mobile-fixed/)).

**Opacity/haptics.** Guides converge on 60–80% button opacity, higher outdoors
([BitTopup](https://bittopup.com/article/PUBG-Mobile-LeftFire-Controls-Master-Hold-vs-Tap-Setup));
CoDM ships vibration and per-hit "Hit Vibration" with adjustable intensity,
all toggleable ([Playbite](https://www.playbite.com/how-to-turn-off-vibration-on-cod-mobile/)).
Zach Gage's GDC rule: touch controls must work "100% of the time, not 80%"
([Game Developer](https://www.gamedeveloper.com/design/video-how-to-design-better-controls-for-touch-screen-games)).

## 2. Racing touch controls

| Game | Steering | Accelerate | Drift / boost / items |
|---|---|---|---|
| Mario Kart Tour | Swipe anywhere (default) or Gyro Handling; Smart Steering; optional Steer/Drift button | Always auto | Manual Drift makes swipe = drift; Auto-item option ([FAQ](https://faq.mariokarttour.com/hc/en-us/articles/4409330910233-Can-I-change-the-controls), [button FAQ](https://faq.mariokarttour.com/hc/en-us/articles/4409314167321-What-does-the-Steer-Drift-Button-Display-option-do)) |
| Asphalt 9 | TouchDrive (auto-steer, one-handed), Tap-to-Steer halves, Tilt + sensitivity slider | Auto | Drift/nitro stay manual in TouchDrive ([Touch Tap Play](https://www.touchtapplay.com/asphalt-9-controls-settings-guide/)) |
| Real Racing 3 | 7 methods; default "Tilt A" = tilt + auto-accel + manual brake; B manual throttle; C touch-steer; D/E on-screen wheel; brake/steer assist sliders ([RR3 wiki](https://rr3.fandom.com/f/p/2598224661577901417)) | Auto by default | — |
| Beach Buggy Racing 2 | Touch halves, arcade arrows, or tilt; brakes in bottom corners | Auto | Brake button doubles as drift ([guide](https://blog.sbenny.com/technology/games/game-guides/beach-buggy-racing-2-cheats-guide-tips-tricks/)) |
| GRID Autosport | Tilt, Wheel Touch (wheel left, accel lower-right, brake upper-right), Arrow Touch; Throttle Slider | Arrow Touch auto-accel "cannot be disabled"; auto-brake on Rookie ([Feral FAQ](https://www.feralinteractive.com/en/faqs/gridautosport/1.6/android/)) | — |
| SuperTuxKart Android | Wheel/stick left; accelerometer or gyroscope modes ([Play Store](https://play.google.com/store/apps/details?id=org.supertuxkart.stk)) | Manual by default (`multitouch_auto_acceleration=false`) | Fire/nitro/skid/look-back/rescue buttons right |

Upstream STK numbers ([user_config.hpp](https://github.com/supertuxkart/stk-code/blob/master/src/config/user_config.hpp)):
deadzone 0.1, steer sensitivity 0.2, accel-axis sensitivity 0.65, tilt factor
4.0, UI scale 1.2 (clamped 0.8–1.6), buttons 0.125×screen-height with 0.075h
margins. Every commercial racer above defaults to auto-accelerate; upstream STK
is the outlier, and the Touch port already flips that default.

## 3. Cross-cutting ergonomics

- **Grip data.** Hoober's 1,333 street observations: 49% one-handed, 36%
  cradled, 15% two-thumbs; only 10% of two-thumb use was landscape; tablets
  were excluded ([UXmatters](https://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php)).
  His tablet follow-up: large tablets are set down in ~2 of 3 sessions; when
  held, users "grab them at the sides" with thumbs settling in the
  middle-to-upper third, so the bottom edge is the worst place for controls on
  a lap-held tablet ([A List Apart](https://alistapart.com/article/how-we-hold-our-gadgets/)).
  Microsoft's arcs-radial-from-grip model fits landscape gaming better.
- **Target size.** Apple 44pt ≈ 7 mm; Microsoft 9 mm recommended / 7 mm
  minimum / 2 mm gap; Nokia ≥1 cm; MIT Touch Lab fingertip 8–10 mm, pad
  10–14 mm; thumb studies ≥9.2–9.6 mm ([LukeW](https://www.lukew.com/ff/entry.asp?1085=)).
  Material: 48×48 dp ≈ 9 mm ([Material](https://m2.material.io/design/usability/accessibility.html)).
  Size in **millimetres**, not raw dp: reported DPI is often wrong and a devlog
  documents buttons coming out tiny on tablets for exactly that reason
  ([itch.io](https://nytuo.itch.io/sansnom-re/devlog/119040/android-controls-too-small)).
- **Dead zones.** Physical sticks need 0.1–0.2 with scaled-radial rescaling
  `(mag − dz)/(1 − dz)` ([Game Developer](https://www.gamedeveloper.com/business/doing-thumbstick-dead-zones-right));
  virtual sticks have no drift, so Microsoft's templates use 0.05 radial.
- **Hide controls when hardware appears.** Apple: "hide these controls when a
  controller is connected… they might be distracting"
  ([Apple](https://developer.apple.com/library/archive/documentation/ServicesDiscovery/Conceptual/GameControllerPG/IncorporatingControllersintoYourDesign/IncorporatingControllersintoYourDesign.html)).
  Android exposes `Configuration.keyboard` (NOKEYS/QWERTY),
  `hardKeyboardHidden`, and `touchscreen` (NOTOUCH/FINGER)
  ([Android](https://developer.android.com/reference/android/content/res/Configuration))
  plus controller connect/disconnect callbacks
  ([Android](https://developer.android.com/games/sdk/game-controller/controller)).
  Steam Deck Verified requires the default gamepad config to reach all
  functionality and an automatic on-screen keyboard, and warns against locking
  settings by hardware ([Valve](https://partner.steamgames.com/doc/steamdeck/recommendations)).
  Godot's popular virtual stick has a "touchscreen only" visibility mode
  ([GitHub](https://github.com/MarcoFazioRandom/Virtual-Joystick-Godot)).
- **What players hate.** Palm/edge touches on the left edge while swiping to
  aim "5–6 times in 10 attempts"; CoDM has an ignore-edge-touch option, PUBG
  doesn't ([PUBG feedback](https://www.answeroverflow.com/m/1411679297261015050)).
  Controls occluding the view (DOOM 2019). Buttons mis-sized by DPI on 11"
  tablets. Fixed sticks that punish an off-centre first touch.

## 4. Checklists

### Xonotic Touch
- [ ] MOVE stick: floating, spawns under first touch anywhere in left ~45% ×
      lower 66%; base radius ≈ 0.15h (≈11 mm on a 6" phone), full deflection at
      0.7 radius; radial dead zone 0.05–0.08 with scaled rescale; optional
      draggable mode; forward-hold ≥0.9 = sprint.
- [ ] LOOK: entire right half is a swipe area; Fixed Speed by default,
      acceleration off; sensitivity in °/cm (expose ~6–10 presets).
- [ ] FIRE: primary button at inner-ring angle 180° (thumb neutral), plus a
      mirrored left fire and a "tap-anywhere-right-to-fire" toggle; JUMP directly
      below FIRE (tertiary slot), CROUCH at 135° outer; nothing within 5 mm of
      screen edges.
- [ ] Button diameter clamp 10–14 mm regardless of DPI (≥48 dp), 2 mm gaps, 65%
      opacity default, user scale 0.8–1.6.
- [ ] Weapon strip bottom-centre on ≥7"; hold-to-open wheel on phones; console
      pill top-left (system zone), never in either arc.
- [ ] Gyro: natural-scale 0–10 (default 2.0), "on while touching look area"
      default, disabled while look-swipe active, calibrate button.
- [ ] Haptics: short pulse on fire/hit, single toggle. Mirror-layout toggle.
- [x] Hide the overlay when a keyboard is attached and show it again when it is
      removed, live, with a toast (`vid_touchdetect.c`, `SCR_Toast`).
- [ ] Ignore touches that begin in a 4–6 mm edge gutter outside a control; never
      let a second finger steal an active control.

### SuperTuxKart Touch
- [x] Auto-accelerate ON by default for touch (matches MK Tour/Asphalt/RR3/GRID);
      brake = pull stick down; keep pedal for tilt mode.
- [ ] Keep this port's arc layout: `btn = 0.115h·scale`, R1 = 1.75·btn,
      R2 = R1 + 1.10·btn; DRIFT at 135° at 1.15×, NITRO 90°, ITEM 180°, LOOK-BACK
      outer 157.5°, RESCUE outer 112.5° — but auto-raise `scale` so btn ≥ 10 mm on
      phones and cap at ~18 mm on 11" tablets.
- [ ] Steering stick: floating in left 44%×66% region, radius 0.15h, full lock at
      0.70 radius, dead zone 0.05–0.10, keep sensitivity 0.2/0.65.
- [ ] Offer three schemes like GRID/BBR2: stick (default), tilt (tilt factor 4,
      steering-lock slider, gyro trim option à la MK Tour), left/right arrows.
- [ ] Smart-steering/steering-assist toggle for new players; opacity 60–80%;
      mirror via `m_multitouch_inverted`.
- [x] Keep the `multitouch_touch_only` auto policy; hide the HUD live when a
      keyboard connects, show it again when it goes (`input_hotplug.cpp`).
- [ ] Hide the HUD on gamepad connect too; Steam Deck runs gamepad-first with no
      overlay.

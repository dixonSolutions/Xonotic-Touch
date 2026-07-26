# Shots vanishing + map vote blocked — 2026-07-26

## 1. Projectiles “disappear into the void”

**Not a collision / fork physics regression.** Touch changes do not alter weapon/world collision.

**Cause:** thermal (default) and battery perf profiles set `cl_particles 0` / `r_particles 0`. Explosion VFX is almost entirely particles (`pointparticles(EFFECT_*_EXPLODE, …)`), so on impact the projectile entity is deleted with no flash — looks like it fell through the world. Splash damage / impact sound can still be real.

**Fix:** profiles + `touch/xonotic.cfg` / `engine/data/xonotic.cfg` now keep low-quality particles on (`cl_particles_quality 0.35` for thermal/battery). Balanced/quality also set `cl_particles 1` (was missing even when `r_particles 1`).

**Quick verify (before rebuild):** in console `cl_particles 1; r_particles 1` then fire a rocket at a wall.

## 2. Cannot select next map (controls covering vote)

**Real fork bug.** `Touch_Active()` ignored map vote / intermission, so the HUD overlay drew on top of MapVote and `Touch_InputEvent` consumed all `MOUSE1` / pointer moves before `MapVote_InputEvent` (see `main.qc` CSQC_InputEvent order). Spec R7 already required hard-release on intermission.

**Fix:** `Touch_Active()` in `qcsrc/client/touch_input.qc` returns false when `mv_active`, `intermission`, or `scoreboard_ui_enabled`.

**Rebuild:** `csprogs.dat` recompiled (CRC `0xCBE3` at build time). Deploy via Flatpak rebuild or override pk3dir (same pattern as pause fix — avoid stale `zzzz-*.pk3dir`).

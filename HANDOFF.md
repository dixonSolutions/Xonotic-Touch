# Handoff: `work/tabletfix` (tablet check of release round 2, 7a8d3ee)

Base: `origin/main` 7a8d3ee. One code commit (9fe6246). `engine/data/xonotic-data.pk3dir`
changed (one QuakeC file), so the data pin is needed at merge. `engine/darkplaces` not
changed; `component-push.sh` not run.

## Done and verified

1. **Map picker kept the previous mode's maps** (Android). Singleplayer > CTF showed the CA
   list (Afterslime, Boil...); Duel then showed the CTF list, Catharsis could be chosen and
   Play said "Catharsis cannot be played in Duel". Leaving and reopening the screen fixed the
   list. Cause: `ViewRenderer.list()` filed the list under its source once, at creation; a diff
   that changed `source` (`maps.ctf` -> `maps.duel`) left it under the old one, so the new
   rows reached no list. Fix: `moveSource()` (UiModel.kt) in the list's bind, then
   `reset(-1)` so the new source is windowed.
2. **Singleplayer Duel with bots locked the player out.** With 15 bots (the saved setting),
   both Duel places went to bots, the player was an observer ("This match is limited to 2
   players"), and Join got them kicked ("spectators aren't allowed": a local game has
   `sv_spectate 0`), which shut the local server down and returned to Home. Fix:
   `UIB_StartGame` caps Duel at one bot.

Tablet (perf package 1.2.411-r2fix built from this branch): CTF -> Duel lists follow the
mode at once; Duel on Afterslime starts with `tester` and one bot, both playing.

## Tests

- `scripts/test-mapmode.sh` with `MODES="duel dm"` (clone basedir `.suite-base`, engine
  `integ3/build/bin/xonotic`): all passed. The new check (Duel asked for 3 bots, server runs
  1) FAILED with the fix reverted (`bot_number 3`).
- `gradle :app:testReleaseUnitTest --tests '*UiBridgeTest*'` (local, offline): 11 tests,
  0 failures, including the new `listFollowsANewSource`. CI does not run Gradle unit tests.
- Release APK (arm64-v8a) builds locally with the change.

## Not done / known

- The GTK renderer was not checked for the same source-change bug.
- The Bots stepper and the Home card still say "15 bots" for Duel (only the start is capped).
- Other tablet findings are in the check report, not fixed here: Updates without an AndySU
  host shows "Version Godnotic" (no number); the pause page calls a local match "Online
  game" and its Match row does nothing when tapped; map tiles show only three mode chips
  (Afterslime under Duel shows no DUEL chip).

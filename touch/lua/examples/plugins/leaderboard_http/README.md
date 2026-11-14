# Web leaderboard

The Internet tier: `xt.http.post` to a host the manifest's `http:` line names, with the answer in a callback, and `xt.game.has("http")` to feature-test (the web build uses the browser's `fetch()`, so there the leaderboard host must also allow web pages: CORS).

## Tier

**Internet** (a switch; `sv_lua_plugin_permission leaderboard_http grant internet`): it talks to the web sites its manifest names.

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/leaderboard_http.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/leaderboard_http.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/leaderboard_http` to
  `<userdir>/plugins/leaderboard_http` and run `sv_lua_plugin_enable leaderboard_http`.
- **Web**: the web build hosts local matches too; install with *Pick file* in the Mods tab, or drop the zip on the page.

Turn it on with `sv_lua_plugin_enable leaderboard_http` (or the switch in Settings > Mods).

## What you should see

- When the Internet tier is off, `sv_lua_leaderboard_http_post` says *permission denied: the internet tier is not turned on* and nothing is sent.
- With it on, at the end of each match the scores go to the `url` setting; the console prints the answer.
- To try it locally: `python3 -m http.server 8765` does not accept POST; `scripts/test-mods.sh` runs a small receiver.

## The approval flow

Installing the mod shows that it asks for **Internet** and names the hosts. Nothing is sent until you turn the tier on (Settings > Mods > Web leaderboard, or `sv_lua_plugin_permission leaderboard_http grant internet`), and only to those hosts.

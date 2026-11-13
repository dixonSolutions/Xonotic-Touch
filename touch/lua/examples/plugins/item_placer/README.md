# Item and teleporter placer

The entity API: `xt.ents.spawn` (items and markers, as the map would place them), handles that are checked on every use (`h:valid()`), `h:remove()`, `p:teleport`, reading fields like `p.origin` and `p.angles`, `xt.vec` and `xt.forward`, and `xt.world.effect`.

## Tier

**Game rules** (a switch in Settings > Mods, or `sv_lua_plugin_permission item_placer grant gamerules`): it changes the game in matches you host.

## Install

Server mods run on a server this game hosts (a local match, or a dedicated server).
`scripts/build-example-mods.sh` writes `build/plugins/item_placer.zip`. Then:

- **Linux / Android**: Settings > Mods > Server mods > *Install...* (or the console:
  `sv_lua_plugin_install /path/to/item_placer.zip`, then `lua_install_confirm`).
- **Dedicated server**: copy `touch/lua/examples/plugins/item_placer` to
  `<userdir>/plugins/item_placer` and run `sv_lua_plugin_enable item_placer`.
- **Web**: the web build hosts local matches too; install as on Linux from the console.

Turn it on with `sv_lua_plugin_enable item_placer` (or the switch in Settings > Mods).

## What you should see

- `!drop`: a mega health appears in front of you (`!clear` removes them).
- `!mark` twice at two places, then `!portal` near one jumps you to the other.
- Turning the mod off removes everything it placed.

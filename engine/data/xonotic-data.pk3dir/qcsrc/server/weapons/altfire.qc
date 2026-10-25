#include "altfire.qh"

#include <common/gametypes/sv_rules.qh>
#include <common/items/item.qh>
#include <common/physics/player.qh>
#include <common/stats.qh>
#include <common/weapons/_all.qh>
#include <server/utils.qh>
#include <server/weapons/common.qh>
#include <server/world.qh>

// Xonotic Touch: the touch HUD shows its ALT (+fire2) button only for a weapon
// that has a secondary fire, and fades it out while that cannot fire. Only the
// server knows both: the balance cvars that switch secondaries on and off are
// not networked, and neither are ammo costs or placed mines.
//
// There is no generic answer. wr_checkammo2 says false both for "no secondary"
// (Devastator, Vortex and MachineGun when +fire2 zooms) and for "out of ammo",
// and some secondaries cost no ammo at all (detonating rockets or mines). So
// each stock weapon is listed here, mirroring the `fire & 2` branch of its
// wr_think. A weapon from a mutator that is not listed counts as having a
// secondary while wr_checkammo2 allows it, and as having none otherwise.

/** 1 if this weapon has a secondary fire under the current balance, 0 if not,
 *  -1 if it is not a stock weapon and only wr_checkammo2 can say */
int W_AltFire_Has(Weapon wep)
{
	if (wep == WEP_Null)
		return 0;
	// +fire2 switches back to the last weapon: not a fire mode.
	if (wep == WEP_BLASTER)
		return 0;
	if (wep == WEP_ARC)
		return WEP_CVAR(WEP_ARC, bolt) || WEP_CVAR(WEP_ARC, overheat_max) > 0;
	if (wep == WEP_CRYLINK)
		return boolean(WEP_CVAR(WEP_CRYLINK, secondary));
	if (wep == WEP_HAGAR)
		return boolean(WEP_CVAR(WEP_HAGAR, secondary));
	if (wep == WEP_HLAC)
		return boolean(WEP_CVAR(WEP_HLAC, secondary));
	if (wep == WEP_MACHINEGUN)
		return (WEP_CVAR(WEP_MACHINEGUN, mode) == 1)
			? boolean(WEP_CVAR(WEP_MACHINEGUN, burst))
			: boolean(WEP_CVAR(WEP_MACHINEGUN, first));
	if (wep == WEP_RIFLE)
		return boolean(WEP_CVAR(WEP_RIFLE, secondary));
	if (wep == WEP_SHOTGUN)
		return WEP_CVAR(WEP_SHOTGUN, secondary) != 0;
	if (wep == WEP_VORTEX)
		return boolean(WEP_CVAR(WEP_VORTEX, secondary));
	if (wep == WEP_DEVASTATOR || wep == WEP_ELECTRO || wep == WEP_FIREBALL
		|| wep == WEP_HOOK || wep == WEP_MINE_LAYER || wep == WEP_MORTAR
		|| wep == WEP_PORTO || wep == WEP_SEEKER || wep == WEP_TUBA
		|| wep == WEP_VAPORIZER)
		return 1;
	return -1;
}

/** Does `this` own a rocket (grenade) in flight that +fire2 has not already detonated? */
bool W_AltFire_LiveRocket(entity this)
{
	IL_EACH(g_projectiles, it.realowner == this && it.classname == "rocket" && !it.rl_detonate_later,
		{ return true; });
	return false;
}

bool W_AltFire_LiveGrenade(entity this)
{
	IL_EACH(g_projectiles, it.realowner == this && it.classname == "grenade" && !it.gl_detonate_later,
		{ return true; });
	return false;
}

int W_AltFire_State(entity this)
{
	// A spectator's +fire2 goes back to observing, which is worth a button.
	if (IS_SPEC(this))
		return (observe_blocked_if_eliminated && INGAME(this)) ? TOUCH_ALTFIRE_NONE : TOUCH_ALTFIRE_READY;
	if (!IS_PLAYER(this) || IS_DEAD(this))
		return TOUCH_ALTFIRE_NONE;
	// Every vehicle gunner seat has a secondary.
	if (this.vehicle)
		return TOUCH_ALTFIRE_READY;

	.entity weaponentity = weaponentities[0];
	entity wepent = this.(weaponentity);
	if (!wepent)
		return TOUCH_ALTFIRE_NONE;
	// The weapon being switched to: the button should change with the choice,
	// not half a second later when the raise animation ends.
	Weapon wep = wepent.m_switchweapon;

	// Detonators: ready while there is something to blow up.
	if (wep == WEP_DEVASTATOR)
		return W_AltFire_LiveRocket(this) ? TOUCH_ALTFIRE_READY : TOUCH_ALTFIRE_EMPTY;
	if (wep == WEP_MORTAR && WEP_CVAR_SEC(WEP_MORTAR, remote_detonateprimary))
		return W_AltFire_LiveGrenade(this) ? TOUCH_ALTFIRE_READY : TOUCH_ALTFIRE_EMPTY;

	bool can = wep.wr_checkammo2(wep, this, weaponentity);
	// The Mine Layer's check is "are there mines to detonate", which no amount
	// of ammo answers; for the rest, unlimited ammo is what weapon_prepareattack
	// lets through.
	if (!can && wep != WEP_MINE_LAYER && (this.items & IT_UNLIMITED_AMMO))
		can = true;

	int has = W_AltFire_Has(wep);
	if (has < 0)
		return can ? TOUCH_ALTFIRE_READY : TOUCH_ALTFIRE_NONE;
	if (!has)
		return TOUCH_ALTFIRE_NONE;
	return can ? TOUCH_ALTFIRE_READY : TOUCH_ALTFIRE_EMPTY;
}

ACCUMULATE void GlobalStats_update(entity this)
{
	STAT(TOUCH_ALTFIRE, this) = W_AltFire_State(this);
}

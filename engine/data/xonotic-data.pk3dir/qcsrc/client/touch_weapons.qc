#include "touch_weapons.qh"
#include "touch_init.qh"

const int TOUCH_WEAP_SLOTS = 16;

int touch_weap_count;
vector touch_weap_pos[TOUCH_WEAP_SLOTS];
vector touch_weap_size[TOUCH_WEAP_SLOTS];
float touch_weap_impulse[TOUCH_WEAP_SLOTS];
bool touch_weap_owned[TOUCH_WEAP_SLOTS];

void Touch_Weapons_BeginFrame(void)
{
	touch_weap_count = 0;
}

void Touch_Weapons_AddSlot(vector pos, vector size, float impulse, bool owned)
{
	if (!cvar("vid_touchscreen"))
		return;
	if (touch_weap_count >= TOUCH_WEAP_SLOTS)
		return;
	if (size.x < 2 || size.y < 2)
		return;
	int i = touch_weap_count;
	touch_weap_pos[i] = pos;
	touch_weap_size[i] = size;
	touch_weap_impulse[i] = impulse;
	touch_weap_owned[i] = owned;
	touch_weap_count += 1;
}

bool Touch_Weapons_Hit(vector p)
{
	return Touch_Weapons_HitImpulse(p) >= 0;
}

float Touch_Weapons_HitImpulse(vector p)
{
	int i;
	for (i = 0; i < touch_weap_count; ++i)
	{
		vector o = touch_weap_pos[i];
		vector s = touch_weap_size[i];
		// Slightly inflate for fat fingers
		float pad = min(s.x, s.y) * 0.12;
		if (p.x >= o.x - pad && p.x <= o.x + s.x + pad
			&& p.y >= o.y - pad && p.y <= o.y + s.y + pad)
		{
			if (!touch_weap_owned[i])
				return -1;
			return touch_weap_impulse[i];
		}
	}
	return -1;
}

void Touch_Weapons_SelectImpulse(float impulse)
{
	if (impulse < 0)
		return;
	// Same path as number keys / weapon_group_N
	localcmd("impulse ", ftos(impulse), "\n");
}

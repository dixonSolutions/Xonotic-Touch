#include "touch_weapons.qh"
#include "touch_init.qh"

// Well past any real inventory: the stock panel registers every weapon in the
// registry, owned or not, and a 16-slot table silently dropped the last rows.
const int TOUCH_WEAP_SLOTS = 64;

int touch_weap_count;
vector touch_weap_pos[TOUCH_WEAP_SLOTS];
vector touch_weap_size[TOUCH_WEAP_SLOTS];
float touch_weap_impulse[TOUCH_WEAP_SLOTS];
bool touch_weap_owned[TOUCH_WEAP_SLOTS];

// The strip's resize grip, as last drawn (Touch_Weapons_SetGrip).
bool touch_weap_grip;
vector touch_weap_grip_pos;
vector touch_weap_grip_size;
float touch_weap_grip_u;
int touch_weap_grip_rows;

// When the slots were built, and when the overlay last drew. Input runs before
// the HUD each frame, so it reads the slots the previous frame drew; if that
// frame drew no strip (dead, observing, hidden), the slots are left over from
// some earlier frame and must not take a tap on a strip that is not there.
float touch_weap_built = -1;
float touch_weap_frame = -2;

float Touch_Weapons_Now(void)
{
	return gettime(GETTIME_FRAMESTART);
}

void Touch_Weapons_MarkFrame(void)
{
	touch_weap_frame = Touch_Weapons_Now();
}

bool Touch_Weapons_Fresh(void)
{
	return touch_weap_built == touch_weap_frame;
}

void Touch_Weapons_BeginFrame(void)
{
	touch_weap_count = 0;
	touch_weap_grip = false;
	touch_weap_built = Touch_Weapons_Now();
}

// Registered whether or not the controls are up: with a keyboard attached the
// strip is still a switcher for a finger (Touch_DeskActive). Nothing reads the
// table unless a finger is being placed.
void Touch_Weapons_AddSlot(vector pos, vector size, float impulse, bool owned)
{
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

void Touch_Weapons_SetGrip(vector pos, vector size, float u, int rows)
{
	touch_weap_grip = true;
	touch_weap_grip_pos = pos;
	touch_weap_grip_size = size;
	touch_weap_grip_u = u;
	touch_weap_grip_rows = rows;
}

bool Touch_Weapons_GripHit(vector p)
{
	if (!touch_weap_grip || !Touch_Weapons_Fresh())
		return false;
	vector o = touch_weap_grip_pos;
	vector s = touch_weap_grip_size;
	return p.x >= o.x && p.x <= o.x + s.x && p.y >= o.y && p.y <= o.y + s.y;
}

float Touch_Weapons_GripU(void)
{
	return touch_weap_grip_u;
}

int Touch_Weapons_GripRows(void)
{
	return touch_weap_grip_rows;
}

bool Touch_Weapons_Hit(vector p)
{
	return Touch_Weapons_HitImpulse(p) >= 0;
}

float Touch_Weapons_HitImpulse(vector p)
{
	if (!Touch_Weapons_Fresh())
		return -1;
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

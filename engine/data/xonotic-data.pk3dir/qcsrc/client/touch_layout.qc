#include "touch_layout.qh"

// Touch UI lives in *console* draw space (vid_conwidth × vid_conheight).
// VF_SIZE can be the Wayland surface (e.g. 1440×960) while 2D is clipped to
// 960×640 — laying out in VF_SIZE pushed FIRE/JUMP off the right edge.

vector Touch_UISize(void)
{
	float cw = max(1, cvar("vid_conwidth"));
	float ch = max(1, cvar("vid_conheight"));
	// Fallback if cvars are unset during early frames.
	if (cw < 2 || ch < 2)
		return getpropertyvec(VF_SIZE);
	return eX * cw + eY * ch;
}

float Touch_ScreenMin(void)
{
	vector ui = Touch_UISize();
	return min(ui.x, ui.y);
}

float Touch_WidgetScale(void)
{
	// Controls scale with touch_scale only (HUD uses touch_hud_scale separately).
	return max(0.01, autocvar_touch_scale);
}

float Touch_MirrorX(float nx)
{
	if (!autocvar_touch_handedness)
		return nx;
	return 1.0 - nx;
}

vector Touch_WidgetCenter(float nx, float ny, float nsize)
{
	vector ui = Touch_UISize();
	float mx = Touch_MirrorX(nx);
	return mx * ui.x * eX + ny * ui.y * eY;
}

float Touch_WidgetRadius(float nsize)
{
	return Touch_ScreenMin() * nsize * 0.5 * Touch_WidgetScale();
}

vector Touch_WidgetHalfExtents(float nsize, float aspect)
{
	float short_axis = Touch_ScreenMin() * nsize * Touch_WidgetScale();
	float long_axis = short_axis * max(1.0, aspect);
	return (long_axis * 0.5) * eX + (short_axis * 0.5) * eY;
}

bool Touch_PointInCircle(vector p, vector center, float radius)
{
	vector d = p - center;
	d.z = 0;
	return vlen(d) <= radius;
}

bool Touch_HitCircle(vector p, float nx, float ny, float nsize, float grab)
{
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	return Touch_PointInCircle(p, c, Touch_WidgetRadius(nsize) * max(1.0, grab));
}

bool Touch_HitRectPx(vector p, vector centre, vector half, float grab)
{
	vector ui = Touch_UISize();
	vector h = half * max(1.0, grab);
	return p.x >= centre.x - h.x && p.x <= centre.x + h.x
		&& p.y >= centre.y - h.y && p.y <= centre.y + h.y
		&& p.x >= 0 && p.y >= 0 && p.x <= ui.x && p.y <= ui.y;
}

bool Touch_HitRect(vector p, float nx, float ny, float nsize, float aspect, float grab)
{
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	vector half = Touch_WidgetHalfExtents(nsize, aspect);
	return Touch_HitRectPx(p, c, half, grab);
}

bool Touch_PointInNormRect(vector p, float nx, float ny, float nsize)
{
	return Touch_HitRect(p, nx, ny, nsize, 1.0, 1.0);
}

bool Touch_InEdgeDeadzone(vector p)
{
	vector ui = Touch_UISize();
	float dz = autocvar_touch_edge_deadzone_px;
	return p.x < dz || p.y < dz || p.x > ui.x - dz || p.y > ui.y - dz;
}

bool Touch_InMoveZone(vector p)
{
	vector ui = Touch_UISize();
	float w = autocvar_touch_move_zone_w;
	if (w <= 0)
		w = autocvar_touch_look_zone_left;
	float left = 0;
	float right = w * ui.x;
	if (autocvar_touch_handedness)
	{
		left = (1.0 - w) * ui.x;
		right = ui.x;
	}
	return p.x >= left && p.x <= right && p.y > ui.y * 0.42;
}

bool Touch_InLookZone(vector p)
{
	vector ui = Touch_UISize();
	float left = autocvar_touch_look_zone_left * ui.x;
	float right = autocvar_touch_look_zone_right * ui.x;
	if (autocvar_touch_handedness)
	{
		float t = 1.0 - right;
		right = 1.0 - left;
		left = t;
		left *= ui.x;
		right *= ui.x;
	}
	return p.x >= left && p.x <= right;
}

bool Touch_WidgetVisible(float nx, float ny)
{
	// Legacy: nonzero position. Prefer explicit _visible cvars at call sites.
	return nx > 0.001 && ny > 0.001;
}

vector Touch_NormalizeInput(vector p)
{
	// gettouchfinger already returns console-pixel coords (× vid_conwidth/height).
	// Do not rescale to VF_SIZE — that disagrees with 2D clip and mis-hits buttons.
	p.z = 0;
	return p;
}

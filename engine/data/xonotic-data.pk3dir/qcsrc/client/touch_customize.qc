#include "touch_customize.qh"
#include "touch_config.qh"
#include "touch_glass.qh"
#include "touch_input.qh"

int touch_customize_drag_widget;
vector touch_customize_drag_offset;
bool touch_customize_pointer_down;

float Touch_Customize_Snap(float v)
{
	return 0.02 * floor(bound(0.05, v, 0.95) / 0.02 + 0.5);
}

vector Touch_Customize_GetWidgetPos(int wid)
{
	switch (wid)
	{
		case TOUCH_WIDGET_MOVE:
			return autocvar_touch_move_x * eX + autocvar_touch_move_y * eY + autocvar_touch_move_size * eZ;
		case TOUCH_WIDGET_FIRE:
			return autocvar_touch_fire_x * eX + autocvar_touch_fire_y * eY + autocvar_touch_fire_size * eZ;
		case TOUCH_WIDGET_JUMP:
			return autocvar_touch_jump_x * eX + autocvar_touch_jump_y * eY + autocvar_touch_jump_size * eZ;
		case TOUCH_WIDGET_CROUCH:
			return autocvar_touch_crouch_x * eX + autocvar_touch_crouch_y * eY + autocvar_touch_crouch_size * eZ;
		case TOUCH_WIDGET_WEAPON:
			return autocvar_touch_weapon_x * eX + autocvar_touch_weapon_y * eY + autocvar_touch_weapon_size * eZ;
		case TOUCH_WIDGET_ZOOM:
			return autocvar_touch_zoom_x * eX + autocvar_touch_zoom_y * eY + autocvar_touch_zoom_size * eZ;
		case TOUCH_WIDGET_RELOAD:
			return autocvar_touch_reload_x * eX + autocvar_touch_reload_y * eY + autocvar_touch_reload_size * eZ;
		default:
			return autocvar_touch_dodge_x * eX + autocvar_touch_dodge_y * eY + autocvar_touch_dodge_size * eZ;
	}
}

void Touch_Customize_SetWidgetPos(int wid, float nx, float ny)
{
	nx = Touch_Customize_Snap(nx);
	ny = Touch_Customize_Snap(ny);
	switch (wid)
	{
		case TOUCH_WIDGET_MOVE:
			cvar_set("touch_move_x", ftos(nx)); cvar_set("touch_move_y", ftos(ny)); break;
		case TOUCH_WIDGET_FIRE:
			cvar_set("touch_fire_x", ftos(nx)); cvar_set("touch_fire_y", ftos(ny)); break;
		case TOUCH_WIDGET_JUMP:
			cvar_set("touch_jump_x", ftos(nx)); cvar_set("touch_jump_y", ftos(ny)); break;
		case TOUCH_WIDGET_CROUCH:
			cvar_set("touch_crouch_x", ftos(nx)); cvar_set("touch_crouch_y", ftos(ny)); break;
		case TOUCH_WIDGET_WEAPON:
			cvar_set("touch_weapon_x", ftos(nx)); cvar_set("touch_weapon_y", ftos(ny)); break;
		case TOUCH_WIDGET_ZOOM:
			cvar_set("touch_zoom_x", ftos(nx)); cvar_set("touch_zoom_y", ftos(ny)); break;
		case TOUCH_WIDGET_RELOAD:
			cvar_set("touch_reload_x", ftos(nx)); cvar_set("touch_reload_y", ftos(ny)); break;
		default:
			cvar_set("touch_dodge_x", ftos(nx)); cvar_set("touch_dodge_y", ftos(ny)); break;
	}
}

string Touch_Customize_WidgetLabel(int wid)
{
	switch (wid)
	{
		case TOUCH_WIDGET_MOVE: return "MOVE";
		case TOUCH_WIDGET_FIRE: return "FIRE";
		case TOUCH_WIDGET_JUMP: return "JUMP";
		case TOUCH_WIDGET_CROUCH: return "CR";
		case TOUCH_WIDGET_WEAPON: return "WEP";
		case TOUCH_WIDGET_ZOOM: return "ZOOM";
		case TOUCH_WIDGET_RELOAD: return "R";
		default: return "DODGE";
	}
}

int Touch_Customize_HitWidget(vector p)
{
	int wid;
	for (wid = TOUCH_WIDGET_COUNT - 1; wid >= 0; wid -= 1)
	{
		if (wid == TOUCH_WIDGET_DODGE && !autocvar_touch_dodge_visible)
			continue;
		vector wp = Touch_Customize_GetWidgetPos(wid);
		float nx = wp.x, ny = wp.y, nsize = wp.z;
		if (!Touch_WidgetVisible(nx, ny))
			continue;
		vector c = Touch_WidgetCenter(nx, ny, nsize);
		float r = Touch_WidgetRadius(nsize) * 1.35;
		if (Touch_PointInCircle(p, c, r))
			return wid;
	}
	return TOUCH_WIDGET_NONE;
}

bool Touch_Customize_HitToolbar(vector p, string action)
{
	vector vf = getpropertyvec(VF_SIZE);
	float bar_h = min(vf.x, vf.y) * 0.08;
	if (p.y > bar_h)
		return false;
	float btn_w = vf.x / 3.0;
	float idx = floor(p.x / btn_w);
	if (action == "save" && idx == 0)
		return true;
	if (action == "cancel" && idx == 1)
		return true;
	if (action == "reset" && idx == 2)
		return true;
	return false;
}

vector Touch_Customize_NormFromScreen(vector p)
{
	vector vf = getpropertyvec(VF_SIZE);
	return p.x / vf.x * eX + p.y / vf.y * eY;
}

void Touch_Customize_DrawToolbar(float alpha)
{
	vector vf = getpropertyvec(VF_SIZE);
	float bar_h = min(vf.x, vf.y) * 0.08;
	drawfill_builtin('0 0 0', vf.x * eX + bar_h * eY, TOUCH_GLASS_SHADOW, alpha * 0.55, DRAWFLAG_NORMAL);
	float btn_w = vf.x / 3.0;
	vector sz = '1 1 0' * bar_h * 0.28;
	drawstring_builtin('0 0 0' + eX * (btn_w * 0.5 - bar_h * 0.5) + eY * (bar_h * 0.35), "SAVE", sz, TOUCH_GLASS_HIGHLIGHT, alpha, 0);
	drawstring_builtin('0 0 0' + eX * (btn_w * 1.5 - bar_h * 0.55) + eY * (bar_h * 0.35), "CANCEL", sz, TOUCH_GLASS_HIGHLIGHT, alpha, 0);
	drawstring_builtin('0 0 0' + eX * (btn_w * 2.5 - bar_h * 0.45) + eY * (bar_h * 0.35), "RESET", sz, TOUCH_GLASS_HIGHLIGHT, alpha, 0);
}

void Touch_Customize_DrawHandle(int wid, float alpha)
{
	vector wp = Touch_Customize_GetWidgetPos(wid);
	float nx = wp.x, ny = wp.y, nsize = wp.z;
	if (!Touch_WidgetVisible(nx, ny))
		return;
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	float r = Touch_WidgetRadius(nsize);
	bool active = touch_customize_drag_widget == wid;
	Touch_Glass_DrawCircleOutline(c, r + 4, TOUCH_GLASS_ACCENT, alpha * (active ? 0.95 : 0.65), 3);
	string label = Touch_Customize_WidgetLabel(wid);
	vector sz = '1 1 0' * r * 0.35;
	drawstring_builtin(c - eX * (strlen(label) * r * 0.08) - eY * (r * 0.12), label, sz, TOUCH_GLASS_HIGHLIGHT, alpha * 0.9, 0);
}

void Touch_Customize_Draw(void)
{
	vector vf = getpropertyvec(VF_SIZE);
	float alpha = bound(0.45, autocvar_touch_opacity + 0.15, 1.0);
	drawfill_builtin('0 0 0', vf, TOUCH_GLASS_SHADOW, 0.25, DRAWFLAG_NORMAL);

	int wid;
	for (wid = 0; wid < TOUCH_WIDGET_COUNT; wid += 1)
	{
		if (wid == TOUCH_WIDGET_DODGE && !autocvar_touch_dodge_visible)
			continue;
		Touch_Customize_DrawHandle(wid, alpha);
	}
	Touch_Customize_DrawToolbar(alpha);
	vector hint_sz = '1 1 0' * min(vf.x, vf.y) * 0.028;
	drawstring_builtin(eX * (vf.x * 0.02) + eY * (vf.y * 0.92), "Drag handles to reposition — SAVE writes touch.layout.cfg", hint_sz, TOUCH_GLASS_HIGHLIGHT, alpha * 0.75, 0);
}

void Touch_Customize_PointerDown(vector p)
{
	if (Touch_Customize_HitToolbar(p, "save"))
	{
		Touch_CmdCustomizeSave();
		return;
	}
	if (Touch_Customize_HitToolbar(p, "cancel"))
	{
		Touch_CmdCustomizeCancel();
		return;
	}
	if (Touch_Customize_HitToolbar(p, "reset"))
	{
		Touch_ResetPreset();
		return;
	}
	touch_customize_drag_widget = Touch_Customize_HitWidget(p);
	if (touch_customize_drag_widget != TOUCH_WIDGET_NONE)
	{
		vector wp = Touch_Customize_GetWidgetPos(touch_customize_drag_widget);
		vector c = Touch_WidgetCenter(wp.x, wp.y, wp.z);
		touch_customize_drag_offset = c - p;
		touch_customize_drag_offset.z = 0;
	}
}

void Touch_Customize_PointerMove(vector p)
{
	if (touch_customize_drag_widget == TOUCH_WIDGET_NONE)
		return;
	vector vf = getpropertyvec(VF_SIZE);
	vector adj = p + touch_customize_drag_offset;
	float nx = adj.x / vf.x;
	float ny = adj.y / vf.y;
	Touch_Customize_SetWidgetPos(touch_customize_drag_widget, nx, ny);
}

void Touch_Customize_PointerUp(void)
{
	touch_customize_drag_widget = TOUCH_WIDGET_NONE;
	touch_customize_pointer_down = false;
}

void Touch_Customize_Frame(void)
{
	Touch_ReleaseAll();
	if (!checkextension("DP_UT_TOUCHFINGER"))
	{
		vector p = getmousepos();
		if (Touch_MouseDown())
		{
			if (!touch_customize_pointer_down)
			{
				touch_customize_pointer_down = true;
				Touch_Customize_PointerDown(p);
			}
			else
				Touch_Customize_PointerMove(p);
		}
		else if (touch_customize_pointer_down)
			Touch_Customize_PointerUp();
		return;
	}

	int slot;
	for (slot = 0; slot < TOUCH_ENGINE_SLOTS; slot += 1)
	{
		vector f = gettouchfinger(slot);
		if (f.z <= 0)
			continue;
		vector p = f;
		p.z = 0;
		if (!touch_customize_pointer_down)
		{
			touch_customize_pointer_down = true;
			Touch_Customize_PointerDown(p);
		}
		else
			Touch_Customize_PointerMove(p);
		return;
	}
	if (touch_customize_pointer_down)
		Touch_Customize_PointerUp();
}

bool Touch_Customize_InputEvent(float bInputType, float nPrimary, float nSecondary)
{
	if (!autocvar_touch_customize || !cvar("vid_touchscreen"))
		return false;

	if (bInputType == 0 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(true);
		vector p = getmousepos();
		touch_customize_pointer_down = true;
		Touch_Customize_PointerDown(p);
		return true;
	}
	if (bInputType == 1 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(false);
		Touch_Customize_PointerUp();
		return true;
	}
	if (bInputType == 3 && Touch_MouseDown())
	{
		vector p = nPrimary * eX + nSecondary * eY;
		Touch_Customize_PointerMove(p);
		return true;
	}
	return false;
}

bool Touch_CustomizeActive(void)
{
	return cvar("vid_touchscreen") && autocvar_touch_customize;
}

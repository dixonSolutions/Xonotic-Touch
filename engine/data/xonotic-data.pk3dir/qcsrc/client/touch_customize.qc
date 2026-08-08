#include "touch_customize.qh"
#include "touch_config.qh"
#include "touch_widget.qh"
#include "touch_input.qh"
#include "touch_pointer.qh"
#include "touch_defs.qh"

int touch_customize_drag_widget;
vector touch_customize_drag_offset;
int touch_customize_hot_action;

// QC globals start at 0, which is a valid widget id and a valid toolbar action,
// so "nothing selected" has to be set explicitly before the first frame.
void Touch_Customize_Reset(void)
{
	touch_customize_drag_widget = TOUCH_WIDGET_NONE;
	touch_customize_hot_action = -1;
	Touch_Pointer_Clear();
}

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
		// Must match the in-game glyphs in touch_draw.qc, or edit mode would
		// label a control differently from the control itself.
		case TOUCH_WIDGET_MOVE: return "MOVE";
		case TOUCH_WIDGET_FIRE: return TOUCH_LABEL_FIRE;
		case TOUCH_WIDGET_JUMP: return TOUCH_LABEL_JUMP;
		case TOUCH_WIDGET_CROUCH: return TOUCH_LABEL_CROUCH;
		case TOUCH_WIDGET_WEAPON: return TOUCH_LABEL_WEAPON;
		case TOUCH_WIDGET_ZOOM: return "ZOOM";
		case TOUCH_WIDGET_RELOAD: return TOUCH_LABEL_RELOAD;
		default: return TOUCH_LABEL_DODGE;
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
		if (wid == TOUCH_WIDGET_JUMP)
		{
			// Match the drawn capsule, or the ends of the bar are not grabbable.
			if (Touch_HitRectPx(p, c, Touch_WidgetHalfExtents(nsize, autocvar_touch_jump_aspect), 1.20))
				return wid;
		}
		else if (Touch_PointInCircle(p, c, Touch_WidgetRadius(nsize) * 1.35))
			return wid;
	}
	return TOUCH_WIDGET_NONE;
}

// Toolbar geometry has one definition, used by both the hit test and the draw
// pass — they used to compute it separately and disagreed about button widths.
vector Touch_Customize_ToolbarHalf(void)
{
	float h = Touch_MMToUnits(TOUCH_MM_COMFORT) * 0.5;
	return eX * (h * 2.6) + eY * h;
}

vector Touch_Customize_ToolbarCenter(int idx)
{
	vector ui = Touch_UISize();
	vector half = Touch_Customize_ToolbarHalf();
	float gap = TOUCH_SPACE_2 * Touch_UIScale();
	float pitch = half.x * 2 + gap;
	// Inset from the top edge: GNOME reserves top-edge drags for the shell, so a
	// flush bar would fight the compositor.
	float y = half.y + TOUCH_SPACE_4 * Touch_UIScale();
	return eX * (ui.x * 0.5 + (idx - 1) * pitch) + eY * y;
}

int Touch_Customize_HitToolbarIndex(vector p)
{
	int i;
	for (i = 0; i < TOUCH_CUSTOMIZE_ACTIONS; i += 1)
	{
		if (Touch_HitRectPx(p, Touch_Customize_ToolbarCenter(i), Touch_Customize_ToolbarHalf(), 1.15))
			return i;
	}
	return -1;
}

string Touch_Customize_ActionLabel(int idx)
{
	// Serial position effect: the destructive action goes last, never first.
	if (idx == TOUCH_CUSTOMIZE_SAVE)
		return "SAVE";
	if (idx == TOUCH_CUSTOMIZE_CANCEL)
		return "CANCEL";
	return "RESET";
}

vector Touch_Customize_ActionAccent(int idx)
{
	if (idx == TOUCH_CUSTOMIZE_SAVE)
		return TOUCH_C_SUCCESS;
	if (idx == TOUCH_CUSTOMIZE_RESET)
		return TOUCH_C_DANGER;
	return TOUCH_C_ACCENT;
}

void Touch_Customize_DrawToolbar(float alpha)
{
	int i;
	for (i = 0; i < TOUCH_CUSTOMIZE_ACTIONS; i += 1)
	{
		vector center = Touch_Customize_ToolbarCenter(i);
		vector half = Touch_Customize_ToolbarHalf();
		vector accent = Touch_Customize_ActionAccent(i);
		Touch_Widget_Capsule(center, half, Touch_Customize_ActionLabel(i),
			touch_customize_hot_action == i, false, max(0.9, alpha), accent);
		// RESET discards the player's whole layout, so it carries its colour at
		// rest rather than only while pressed — by then the decision is made.
		if (i == TOUCH_CUSTOMIZE_RESET && Touch_Shape_Ready())
			Touch_Widget_CapsuleRim(center, half, accent, max(0.9, alpha) * TOUCH_A_RIM_ACTIVE);
	}
}

// In edit mode the widget is the handle: the real control is drawn beneath at
// reduced alpha, and this adds an accent ring plus its name, so the player edits
// what they will actually play with.
void Touch_Customize_DrawHandle(int wid, float alpha)
{
	vector wp = Touch_Customize_GetWidgetPos(wid);
	float nx = wp.x, ny = wp.y, nsize = wp.z;
	if (!Touch_WidgetVisible(nx, ny))
		return;
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	float r = Touch_WidgetRadius(nsize);
	bool active = touch_customize_drag_widget == wid;
	float pad = TOUCH_SPACE_1 * Touch_UIScale();
	float a_ring = alpha * (active ? TOUCH_A_RIM_ACTIVE : TOUCH_A_RIM);

	if (Touch_Shape_Ready())
	{
		if (active)
			Touch_Shape_Glow(c, r * 1.5, TOUCH_C_ACCENT, alpha * TOUCH_A_GLOW);
		if (wid == TOUCH_WIDGET_JUMP)
		{
			// The hop bar is a capsule; a circular handle around it would claim
			// the wrong bounds and misreport what is being moved.
			vector half = Touch_WidgetHalfExtents(nsize, autocvar_touch_jump_aspect);
			Touch_Widget_CapsuleRim(c, half + '1 1 0' * pad, TOUCH_C_ACCENT, a_ring);
		}
		else
		{
			Touch_Widget_Rim(c, r + pad, TOUCH_C_ACCENT, a_ring);
		}
	}
	string wlabel = Touch_Customize_WidgetLabel(wid);
	Touch_Shape_Label(c, Touch_Widget_LabelSize('1 1 0' * r, wlabel), wlabel,
		TOUCH_C_INK, alpha * TOUCH_A_INK);
}

void Touch_Customize_Draw(void)
{
	vector ui = Touch_UISize();
	float alpha = bound(0.45, autocvar_touch_opacity + 0.15, 1.0);
	// Scrim: edit mode is modal, and the dim is what says so.
	drawfill_builtin('0 0 0', ui, TOUCH_C_SURFACE, 0.45, DRAWFLAG_NORMAL);

	int wid;
	for (wid = 0; wid < TOUCH_WIDGET_COUNT; wid += 1)
	{
		if (wid == TOUCH_WIDGET_DODGE && !autocvar_touch_dodge_visible)
			continue;
		Touch_Customize_DrawHandle(wid, alpha);
	}
	Touch_Customize_DrawToolbar(alpha);

	float hint = bound(TOUCH_FONT_MIN, min(ui.x, ui.y) * 0.028, TOUCH_FONT_MAX);
	Touch_Shape_Label(eX * (ui.x * 0.5) + eY * (ui.y - hint * 2),
		hint, "Drag a control to move it — SAVE writes touch.layout.cfg",
		TOUCH_C_INK, alpha * 0.85);
}

void Touch_Customize_PointerDown(vector p)
{
	// Toolbar actions fire on release, not on touch: a mistap on RESET is
	// unrecoverable, and press-then-slide-off must be an escape hatch.
	touch_customize_hot_action = Touch_Customize_HitToolbarIndex(p);
	if (touch_customize_hot_action >= 0)
		return;

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
	if (touch_customize_hot_action >= 0)
	{
		if (Touch_Customize_HitToolbarIndex(p) != touch_customize_hot_action)
			touch_customize_hot_action = -1;
		return;
	}
	if (touch_customize_drag_widget == TOUCH_WIDGET_NONE)
		return;
	// Console space, matching Touch_WidgetCenter — VF_SIZE here made handles
	// jump away from the finger whenever the surface and console sizes differed.
	vector ui = Touch_UISize();
	vector adj = p + touch_customize_drag_offset;
	float nx = Touch_MirrorX(adj.x / ui.x);
	Touch_Customize_SetWidgetPos(touch_customize_drag_widget, nx, adj.y / ui.y);
}

void Touch_Customize_PointerUp(void)
{
	if (touch_customize_hot_action == TOUCH_CUSTOMIZE_SAVE)
		Touch_CmdCustomizeSave();
	else if (touch_customize_hot_action == TOUCH_CUSTOMIZE_CANCEL)
		Touch_CmdCustomizeCancel();
	else if (touch_customize_hot_action == TOUCH_CUSTOMIZE_RESET)
		Touch_ResetPreset();
	touch_customize_hot_action = -1;
	touch_customize_drag_widget = TOUCH_WIDGET_NONE;
}

void Touch_Customize_Frame(void)
{
	Touch_ReleaseAll();
	Touch_Pointer_Frame();
	if (Touch_Pointer_Pressed())
		Touch_Customize_PointerDown(Touch_Pointer_Pos());
	else if (Touch_Pointer_Released())
		Touch_Customize_PointerUp();
	else if (Touch_Pointer_IsDown())
		Touch_Customize_PointerMove(Touch_Pointer_Pos());
}

// Only the mouse-button state is taken from the event stream; positions and
// edges come from Touch_Pointer_Frame, so touch and mouse follow one path.
bool Touch_Customize_InputEvent(float bInputType, float nPrimary, float nSecondary)
{
	if (!autocvar_touch_customize || !cvar("vid_touchscreen"))
		return false;

	if (bInputType == 0 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(true);
		return true;
	}
	if (bInputType == 1 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(false);
		return true;
	}
	if (bInputType == 3)
		return true;
	return false;
}

bool Touch_CustomizeActive(void)
{
	return cvar("vid_touchscreen") && autocvar_touch_customize;
}

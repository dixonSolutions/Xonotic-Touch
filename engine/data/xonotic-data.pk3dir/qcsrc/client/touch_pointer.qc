#include "touch_pointer.qh"
#include "touch_input.qh"
#include "touch_defs.qh"

bool touch_pointer_down;
bool touch_pointer_pressed;
bool touch_pointer_released;
vector touch_pointer_pos;

void Touch_Pointer_Clear(void)
{
	touch_pointer_down = false;
	touch_pointer_pressed = false;
	touch_pointer_released = false;
}

void Touch_Pointer_Frame(void)
{
	bool down = false;
	vector p = touch_pointer_pos;

	// Touch_SampleEngineFinger rather than the raw builtin, so a contact means the
	// same thing here as it does to the gameplay controls.
	int slot;
	for (slot = 0; slot < TOUCH_ENGINE_SLOTS; slot += 1)
	{
		vector f = Touch_SampleEngineFinger(slot);
		if (f.z <= 0)
			continue;
		p = f;
		p.z = 0;
		down = true;
		break;
	}

	// The SDL mouse is engine slot 10, outside the real-finger range, and the
	// engine zeroes it whenever a real finger is live. Leaving it out is why a
	// mouse or stylus could not press anything on a modal sheet: the only
	// remaining path was getmousepos(), which reads 0,0 on the press frame.
	if (!down)
	{
		vector f = Touch_SampleEngineFinger(TOUCH_SRC_POINTER);
		if (f.z > 0)
		{
			p = f;
			p.z = 0;
			down = true;
		}
	}

	// Last resort. A held MOUSE1 with no finger slot at all still has to be able
	// to drag, even if its first frame reports the wrong place.
	if (!down && Touch_MouseDown())
	{
		p = getmousepos();
		p.z = 0;
		down = true;
	}

	touch_pointer_pressed = (down && !touch_pointer_down);
	touch_pointer_released = (!down && touch_pointer_down);
	touch_pointer_down = down;
	// Position is only updated while down, so a released pointer still reports
	// where it let go — which is what a commit-on-release button needs.
	if (down)
		touch_pointer_pos = p;
}

bool Touch_Pointer_IsDown(void)
{
	return touch_pointer_down;
}

bool Touch_Pointer_Pressed(void)
{
	return touch_pointer_pressed;
}

bool Touch_Pointer_Released(void)
{
	return touch_pointer_released;
}

vector Touch_Pointer_Pos(void)
{
	return touch_pointer_pos;
}

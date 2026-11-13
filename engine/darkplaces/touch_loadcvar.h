#ifndef TOUCH_LOADCVAR_H
#define TOUCH_LOADCVAR_H

// Godnotic: binary-search insertion into the sorted cvar list
// (touch_cvar_index, see touch_loadcvar.c).

// Links variable into cvars->vars where Cvar_Link's walk would; false when
// it did not (Cvar_Link then walks as before).
qbool TouchCvar_Link(cvar_t *variable, cvar_state_t *cvars);
// The list changed other than through TouchCvar_Link.
void TouchCvar_ListChanged(void);
// A static cvar took the place of an allocated one of the same name: point the
// hash entries that still name the old one at its replacement.
void TouchCvar_Replaced(cvar_state_t *cvars, cvar_t *old, cvar_t *replacement);
void TouchCvar_Init(void);

#endif

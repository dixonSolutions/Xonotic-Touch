/*
Godnotic: fewer edict array reallocations while QuakeC spawns entities.

PRVM_MEM_IncreaseEdicts grows a VM's edict storage 256 at a time. Each
growth reallocates every entity's fields (Mem_Realloc: allocate, clear, copy)
and, for the server and CSQC, unlinks every entity from the world and links
it again. Xonotic's server spawns ~4900 entities on stormkeep (its registries
and item/weapon definitions are entities), the menu ~2800, CSQC ~3300: 17
growths for the server, each dearer than the last, 0.6 s of a 2.9 s desktop
map load (the server's spawn() builtin), plus ~80 ms at start for the menu.

touch_edicts_growth 1 (default): a growth goes straight to the most this VM
needed before (this process, or the last run through the archived
_touch_edicts_peak), and otherwise grows by half the current size rather
than by 256 - a handful of growths instead of 17. The extra room is at most
half of what the VM uses (entity fields only: the edict headers are
allocated at the limit already). 0 = 256 at a time as before.
*/

#include "quakedef.h"
#include "touch_prvm.h"

static cvar_t touch_edicts_growth = {CF_CLIENT | CF_SERVER, "touch_edicts_growth", "1", "grow QuakeC edict storage to the size it reached before, or by half, instead of 256 entities at a time (Godnotic)"};
static cvar_t touch_edicts_peak = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "_touch_edicts_peak", "", "edicts each QuakeC VM needed last time (touch_edicts_growth)"};

#define TPV_VMS 4
static struct { char name[16]; int peak; } tpv_peak[TPV_VMS];
static qbool tpv_loaded;

static int *TPV_Slot(const char *name)
{
	int i;
	for (i = 0; i < TPV_VMS; i++)
		if (!strcmp(tpv_peak[i].name, name))
			return &tpv_peak[i].peak;
	for (i = 0; i < TPV_VMS; i++)
		if (!tpv_peak[i].name[0])
		{
			dp_strlcpy(tpv_peak[i].name, name, sizeof(tpv_peak[i].name));
			return &tpv_peak[i].peak;
		}
	return NULL;
}

// "server:4864 client:3328 menu:2817"
static void TPV_Load(void)
{
	const char *s = touch_edicts_peak.string;
	tpv_loaded = true;
	while (s && *s)
	{
		char name[16];
		int n, used;
		if (sscanf(s, "%15[^:]:%d%n", name, &n, &used) != 2)
			break;
		{
			int *slot = TPV_Slot(name);
			if (slot && n > 0 && n <= 65536 && n > *slot)
				*slot = n;
		}
		s += used;
		while (*s == ' ')
			s++;
	}
}

static void TPV_Save(void)
{
	char buf[256];
	size_t len = 0;
	int i;
	buf[0] = 0;
	for (i = 0; i < TPV_VMS; i++)
		if (tpv_peak[i].name[0] && tpv_peak[i].peak > 0)
			len += dpsnprintf(buf + len, sizeof(buf) - len, "%s%s:%d", len ? " " : "", tpv_peak[i].name, tpv_peak[i].peak);
	if (strcmp(buf, touch_edicts_peak.string))
		Cvar_SetQuick(&touch_edicts_peak, buf);
}

// newmax: what PRVM_MEM_IncreaseEdicts chose (the old size + 256, at most
// the limit); returns the size to allocate
int TouchPRVM_EdictGrowth(prvm_prog_t *prog, int newmax)
{
	int old = newmax - 256, target = newmax, *slot;
	// before registration (a dedicated server's first VM) the default holds
	if (touch_edicts_growth.string && atof(touch_edicts_growth.string) == 0)
		return newmax;
	if (old <= 0 || newmax >= prog->limit_edicts)
		return newmax;
	if (!tpv_loaded && touch_edicts_peak.name && Cvar_FindVar(&cvars_all, touch_edicts_peak.name, ~0))
		TPV_Load();
	slot = TPV_Slot(prog->name);
	// by half
	if (old + old / 2 > target)
		target = old + old / 2;
	// what this VM reached before: one growth gets there
	if (slot && *slot > target)
		target = *slot;
	target = (target + 255) & ~255;
	if (target > prog->limit_edicts)
		target = prog->limit_edicts;
	// remembered as what it needed (within 256): a later run that needs as
	// much makes no growth at all, so nothing compounds
	if (slot && newmax > *slot)
	{
		*slot = newmax;
		if (Cvar_FindVar(&cvars_all, touch_edicts_peak.name, ~0))
			TPV_Save();
	}
	return target;
}

void TouchPRVM_Init(void)
{
	Cvar_RegisterVariable(&touch_edicts_growth);
	Cvar_RegisterVariable(&touch_edicts_peak);
}

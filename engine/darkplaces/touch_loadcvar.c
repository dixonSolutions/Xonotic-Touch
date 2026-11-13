/*
Godnotic: O(log n) insertion into the sorted cvar list.

Cvar_Link keeps cvars->vars in name order by walking the list with strcmp
from its head for every new cvar. Xonotic's configs and progs create ~7500
cvars at start, so the walks add up to ~28 million strcmp calls: the
largest part of running the start-up configs.

touch_cvar_index (always on; the cvar is the A/B switch and costs nothing to
read) keeps an array of the list's nodes in the same order, finds the
insertion point by binary search and links the cvar exactly where the walk
would have. Any other change to the list (Cvar_RegisterVariable replacing an
allocated cvar, Cvar_Delete) drops the array; it is rebuilt from the list on
the next insert.
*/

#include "quakedef.h"
#include "touch_loadcvar.h"

static cvar_t touch_cvar_index = {CF_CLIENT | CF_SERVER, "touch_cvar_index", "1", "insert new cvars into the sorted cvar list by binary search (Godnotic); 0 = walk the list"};

static cvar_state_t *tlc_owner;   // the list the array mirrors
static cvar_t **tlc_nodes;
static int tlc_count, tlc_size;
static qbool tlc_valid;

void TouchCvar_ListChanged(void)
{
	tlc_valid = false;
}

static qbool TLC_Rebuild(cvar_state_t *cvars)
{
	cvar_t *v;
	int n = 0;
	for (v = cvars->vars; v; v = v->next)
		n++;
	if (n + 1 > tlc_size)
	{
		int size = tlc_size ? tlc_size : 4096;
		cvar_t **nodes;
		while (size < n + 1)
			size *= 2;
		nodes = (cvar_t **)realloc(tlc_nodes, size * sizeof(*nodes));
		if (!nodes)
			return false;
		tlc_nodes = nodes;
		tlc_size = size;
	}
	tlc_count = 0;
	for (v = cvars->vars; v; v = v->next)
		tlc_nodes[tlc_count++] = v;
	tlc_owner = cvars;
	tlc_valid = true;
	return true;
}

qbool TouchCvar_Link(cvar_t *variable, cvar_state_t *cvars)
{
	int lo, hi;
	cvar_t *current, *next;

	static int noindex = -1;
	// the configs run before a +set on the command line does, so the A/B
	// switch for start-up is the -nocvarindex parameter; until the cvar is
	// registered, and when switched off, the walk links and the array goes
	// stale
	if (noindex < 0)
		noindex = Sys_CheckParm("-nocvarindex") != 0;
	if (noindex || !(touch_cvar_index.flags & CF_REGISTERED) || !touch_cvar_index.integer)
	{
		tlc_valid = false;
		return false;
	}
	if ((!tlc_valid || tlc_owner != cvars) && !TLC_Rebuild(cvars))
		return false;
	if (tlc_count + 1 > tlc_size)
	{
		cvar_t **nodes = (cvar_t **)realloc(tlc_nodes, tlc_size * 2 * sizeof(*nodes));
		if (!nodes)
		{
			tlc_valid = false;
			return false;
		}
		tlc_nodes = nodes;
		tlc_size *= 2;
	}
	// first node whose name is not below the new one: where the walk stops
	lo = 0;
	hi = tlc_count;
	while (lo < hi)
	{
		int mid = (lo + hi) / 2;
		if (strcmp(tlc_nodes[mid]->name, variable->name) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	current = lo > 0 ? tlc_nodes[lo - 1] : NULL;
	next = lo < tlc_count ? tlc_nodes[lo] : NULL;
	if (current)
		current->next = variable;
	else
		cvars->vars = variable;
	variable->next = next;
	memmove(tlc_nodes + lo + 1, tlc_nodes + lo, (tlc_count - lo) * sizeof(*tlc_nodes));
	tlc_nodes[lo] = variable;
	tlc_count++;
	return true;
}

void TouchCvar_Init(void)
{
	Cvar_RegisterVariable(&touch_cvar_index);
}

/*
Cvar_RegisterVariable can find that a "set" already made an allocated cvar of
the same name (a config line, or the platform, got there before the code that
owns the cvar registered it). It relinks the list and frees the old cvar, but
upstream leaves the hash table pointing at the freed one: the next lookup of
that name by hash reads freed memory. Upstream notes the path as unused,
because it registers everything before configs run; Touch code that registers
late reached it (the console button's _touch_safe_insets, set by Android
first, crashed the menu at its first frame).
*/
void TouchCvar_Replaced(cvar_state_t *cvars, cvar_t *old, cvar_t *replacement)
{
	int i;
	cvar_hash_t *hash;
	for (i = 0; i < CVAR_HASHSIZE; i++)
		for (hash = cvars->hashtable[i]; hash; hash = hash->next)
			if (hash->cvar == old)
				hash->cvar = replacement;
}

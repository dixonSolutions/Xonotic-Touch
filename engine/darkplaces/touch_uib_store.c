/*
Godnotic UI bridge: the description store.

A flat map of keyed nodes. The menu VM (or a mod page) rebuilds a subtree by emitting
its nodes again; each node's new props and children are compared with the old ones and
only real changes bump the node's generation. Snapshot = every reachable node, parents
first; diff since rev R = reachable nodes with gen > R plus ids deleted after R.

No engine headers here: cvar access goes through UIB_CvarFind/UIB_CvarString so the
tests can build the store alone (tests/uibridge/store_test.c).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "touch_uib_int.h"


#define HASHSIZE 4096
#define TOMBS 8192

typedef struct { char *id; int64_t rev; } tomb_t;

static struct
{
	uibnode_t *hash[HASHSIZE];
	uibnode_t *all;            // list head
	int count;
	int64_t rev;               // committed
	int dirty;
	int serial;
	uibnode_t *stack[UIB_MAX_DEPTH + 2];
	int depth;
	int dummy;                 // > 0 while inside a refused node
	uibnode_t **old;
	int nold, maxold;
	tomb_t tombs[TOMBS];
	int ntomb, tombhead;
	int64_t floor;             // diffs from below this rev are impossible
	// nav
	char *stack_ids, *modal_ids, *context;
	int canback;
	char *busy;
	char *reserved;
	int navdirty;
	// toasts
	int toastseq;
	struct { uibnode_t *node; double expire; } toasts[8];
	int ntoasts;
	const char *build_origin;
	int warned;
} st;

static unsigned hashstr(const char *s)
{
	unsigned h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h & (HASHSIZE - 1);
}

uibnode_t *UIBS_Find(const char *id)
{
	for (uibnode_t *n = st.hash[hashstr(id)]; n; n = n->hnext)
		if (!strcmp(n->id, id))
			return n;
	return NULL;
}

int64_t UIBS_Rev(void) { return st.rev; }
uibnode_t *UIBS_FirstNode(void) { return st.all; }
int UIBS_NodeCount(void) { return st.count; }

static void freeprops(uibprop_t *p, int n)
{
	for (int i = 0; i < n; i++)
	{
		free(p[i].key);
		free(p[i].json);
	}
}

static void changed(uibnode_t *n)
{
	n->gen = st.rev + 1;
	free(n->json);
	n->json = NULL;
	st.dirty = 1;
}

static uibnode_t *newnode(const char *id, const char *type)
{
	if (st.count >= UIB_MAX_NODES)
		return NULL;
	uibnode_t *n = (uibnode_t *)calloc(1, sizeof(*n));
	n->id = strdup(id);
	snprintf(n->type, sizeof(n->type), "%s", type);
	unsigned h = hashstr(id);
	n->hnext = st.hash[h];
	st.hash[h] = n;
	n->lnext = st.all;
	if (st.all)
		st.all->lprev = n;
	st.all = n;
	st.count++;
	changed(n);
	return n;
}

static void tomb(const char *id)
{
	tomb_t *t = &st.tombs[st.tombhead];
	if (t->id)
	{
		if (t->rev > st.floor)
			st.floor = t->rev;
		free(t->id);
	}
	t->id = strdup(id);
	t->rev = st.rev + 1;
	st.tombhead = (st.tombhead + 1) % TOMBS;
	if (st.ntomb < TOMBS)
		st.ntomb++;
	st.dirty = 1;
}

static void freenode(uibnode_t *n)
{
	// unlink hash
	uibnode_t **pp = &st.hash[hashstr(n->id)];
	while (*pp && *pp != n)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = n->hnext;
	if (n->lprev) n->lprev->lnext = n->lnext;
	else st.all = n->lnext;
	if (n->lnext) n->lnext->lprev = n->lprev;
	st.count--;
	tomb(n->id);
	for (int i = 0; i < st.ntoasts; i++)
		if (st.toasts[i].node == n)
		{
			st.toasts[i] = st.toasts[--st.ntoasts];
			break;
		}
	freeprops(n->props, n->nprops);
	freeprops(n->pprops, n->npprops);
	free(n->props);
	free(n->pprops);
	free(n->kids);
	free(n->pkids);
	free(n->json);
	free(n->bindval);
	free(n->origin);
	free(n->id);
	free(n);
}

static void delete_subtree(uibnode_t *n)
{
	for (int i = 0; i < n->nkids; i++)
		if (n->kids[i]->parent == n)
			delete_subtree(n->kids[i]);
	freenode(n);
}

static void touch_subtree(uibnode_t *n, int depth)
{
	changed(n);
	if (depth > UIB_MAX_DEPTH)
		return;
	for (int i = 0; i < n->nkids; i++)
		touch_subtree(n->kids[i], depth + 1);
}

const char *UIBS_PropJSON(const uibnode_t *n, const char *key, size_t *len)
{
	for (int i = 0; i < n->nprops; i++)
		if (!strcmp(n->props[i].key, key))
		{
			if (len)
				*len = n->props[i].len;
			return n->props[i].json;
		}
	return NULL;
}

static void setprop(uibprop_t **props, int *np, int *maxp, const char *key, const char *json, size_t len)
{
	for (int i = 0; i < *np; i++)
		if (!strcmp((*props)[i].key, key))
		{
			free((*props)[i].json);
			(*props)[i].json = (char *)malloc(len + 1);
			memcpy((*props)[i].json, json, len);
			(*props)[i].json[len] = 0;
			(*props)[i].len = len;
			return;
		}
	if (*np >= *maxp)
	{
		*maxp = *maxp ? *maxp * 2 : 8;
		*props = (uibprop_t *)realloc(*props, sizeof(uibprop_t) * *maxp);
	}
	uibprop_t *p = &(*props)[(*np)++];
	p->key = strdup(key);
	p->json = (char *)malloc(len + 1);
	memcpy(p->json, json, len);
	p->json[len] = 0;
	p->len = len;
}

static void delprop(uibprop_t *props, int *np, const char *key)
{
	for (int i = 0; i < *np; i++)
		if (!strcmp(props[i].key, key))
		{
			free(props[i].key);
			free(props[i].json);
			memmove(&props[i], &props[i + 1], sizeof(uibprop_t) * (*np - i - 1));
			(*np)--;
			return;
		}
}

void UIBS_SetProp(uibnode_t *n, const char *key, const char *json, size_t len)
{
	size_t ol;
	const char *old = UIBS_PropJSON(n, key, &ol);
	if (json)
	{
		if (old && ol == len && !memcmp(old, json, len))
			return;
		setprop(&n->props, &n->nprops, &n->maxprops, key, json, len);
	}
	else
	{
		if (!old)
			return;
		delprop(n->props, &n->nprops, key);
	}
	changed(n);
}

// ---------------------------------------------------------------- build

static int valid_key(const char *k)
{
	size_t n = strlen(k);
	if (!n || n > 64)
		return 0;
	for (const char *s = k; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.' || *s == ':'))
			return 0;
	return 1;
}

static void collect_old(uibnode_t *n, int depth)
{
	if (st.nold >= st.maxold)
	{
		st.maxold = st.maxold ? st.maxold * 2 : 256;
		st.old = (uibnode_t **)realloc(st.old, sizeof(*st.old) * st.maxold);
	}
	st.old[st.nold++] = n;
	if (depth > UIB_MAX_DEPTH)
		return;
	for (int i = 0; i < n->nkids; i++)
		if (n->kids[i]->parent == n)
			collect_old(n->kids[i], depth + 1);
}

static void begin_pending(uibnode_t *n, const char *type)
{
	freeprops(n->pprops, n->npprops);
	n->npprops = 0;
	n->npkids = 0;
	snprintf(n->ptype, sizeof(n->ptype), "%s", type);
	n->building = 1;
	n->mark = st.serial;
}

int UIBS_BuildBegin(const char *rootid, const char *type, const char *origin)
{
	if (st.depth)
	{
		// an unfinished build: finish it so the store stays consistent
		while (st.depth > 1 || st.dummy)
			UIBS_Close();
		UIBS_BuildEnd();
	}
	if (strlen(rootid) >= UIB_MAX_ID || !valid_key(rootid) || !strcmp(rootid, "app"))
		return 0;
	st.serial++;
	st.nold = 0;
	st.build_origin = origin;
	uibnode_t *n = UIBS_Find(rootid);
	if (n)
		collect_old(n, 0);
	else if (!(n = newnode(rootid, type)))
		return 0;
	n->isroot = 1;
	begin_pending(n, type);
	if (origin && (!n->origin || strcmp(n->origin, origin)))
	{
		free(n->origin);
		n->origin = strdup(origin);
		changed(n);
	}
	else if (!origin && n->origin)
	{
		free(n->origin);
		n->origin = NULL;
		changed(n);
	}
	st.stack[0] = n;
	st.depth = 1;
	st.dummy = 0;
	return 1;
}

const char *UIBS_OpenId(void)
{
	return st.depth && !st.dummy ? st.stack[st.depth - 1]->id : "";
}

int UIBS_Open(const char *key, const char *type)
{
	if (!st.depth)
		return 0;
	uibnode_t *parent = st.stack[st.depth - 1];
	char id[UIB_MAX_ID + 64];
	if (st.dummy || st.depth >= UIB_MAX_DEPTH || !valid_key(key) || parent->npkids >= UIB_MAX_CHILDREN)
		goto refuse;
	snprintf(id, sizeof(id), "%s/%s", parent->id, key);
	if (strlen(id) >= UIB_MAX_ID)
		goto refuse;
	uibnode_t *n = UIBS_Find(id);
	if (n && n->mark == st.serial)
		goto refuse;  // the same key twice under one parent
	if (!n && !(n = newnode(id, type)))
		goto refuse;
	begin_pending(n, type);
	if (parent->npkids >= parent->maxpkids)
	{
		parent->maxpkids = parent->maxpkids ? parent->maxpkids * 2 : 8;
		parent->pkids = (uibnode_t **)realloc(parent->pkids, sizeof(uibnode_t *) * parent->maxpkids);
	}
	parent->pkids[parent->npkids++] = n;
	// origin: inherited from the build
	const char *o = st.build_origin;
	if ((o && (!n->origin || strcmp(n->origin, o))) || (!o && n->origin))
	{
		free(n->origin);
		n->origin = o ? strdup(o) : NULL;
		changed(n);
	}
	st.stack[st.depth++] = n;
	return 1;
refuse:
	st.dummy++;
	if (!st.warned++)
		fprintf(stderr, "uibridge: refused node %s under %s (duplicate key, depth or size)\n", key, parent->id);
	return 0;
}

int UIBS_Set(const char *key, const char *json, size_t len)
{
	if (!st.depth || st.dummy)
		return 0;
	uibnode_t *n = st.stack[st.depth - 1];
	setprop(&n->pprops, &n->npprops, &n->maxpprops, key, json, len);
	return 1;
}

int UIBS_Push(const char *key, const char *json, size_t len)
{
	if (!st.depth || st.dummy)
		return 0;
	uibnode_t *n = st.stack[st.depth - 1];
	for (int i = 0; i < n->npprops; i++)
		if (!strcmp(n->pprops[i].key, key))
		{
			uibprop_t *p = &n->pprops[i];
			if (p->len < 2 || p->json[0] != '[')
				return 0;
			char *j = (char *)malloc(p->len + len + 2);
			size_t o = p->len - 1;  // drop ']'
			memcpy(j, p->json, o);
			if (o > 1)
				j[o++] = ',';
			memcpy(j + o, json, len);
			o += len;
			j[o++] = ']';
			j[o] = 0;
			free(p->json);
			p->json = j;
			p->len = o;
			return 1;
		}
	uibbuf_t b = {0};
	UIBB_Putc(&b, '[');
	UIBB_Put(&b, json, len);
	UIBB_Putc(&b, ']');
	setprop(&n->pprops, &n->npprops, &n->maxpprops, key, b.p, b.n);
	UIBB_Free(&b);
	return 1;
}

static const char *pprop(uibnode_t *n, const char *key, size_t *len)
{
	for (int i = 0; i < n->npprops; i++)
		if (!strcmp(n->pprops[i].key, key))
		{
			if (len)
				*len = n->pprops[i].len;
			return n->pprops[i].json;
		}
	return NULL;
}

// The value of a cvar-bound control, as the node type wants it.
static void cvar_value_json(uibbuf_t *b, const char *type, const char *s)
{
	if (!strcmp(type, "switch"))
		UIBB_Puts(b, atof(s) != 0 ? "true" : "false");
	else if (!strcmp(type, "slider") || !strcmp(type, "stepper"))
		UIBB_Num(b, atof(s));
	else
		UIBB_Str(b, s, strlen(s), UIB_MAX_STRING, 0);
}

static void bind_fill(uibnode_t *n)
{
	size_t bl;
	const char *bj = pprop(n, "bind", &bl);
	n->bindkind = 0;
	n->bindcvar = NULL;
	if (!bj || bl < 3)
		return;
	char bind[160];
	snprintf(bind, sizeof(bind), "%.*s", (int)(bl - 2), bj + 1);  // strip quotes (bind strings need no escapes)
	if (!strncmp(bind, "cvar:", 5))
	{
		void *cv = UIB_CvarFind(bind + 5);
		n->bindkind = 'c';
		n->bindcvar = cv;
		if (cv && !pprop(n, "value", NULL) && strcmp(n->ptype, "keybind"))
		{
			const char *s = UIB_CvarString(cv);
			uibbuf_t b = {0};
			cvar_value_json(&b, n->ptype, s);
			setprop(&n->pprops, &n->npprops, &n->maxpprops, "value", b.p, b.n);
			UIBB_Free(&b);
			free(n->bindval);
			n->bindval = strdup(s);
		}
		else if (cv)
		{
			free(n->bindval);
			n->bindval = strdup(UIB_CvarString(cv));
		}
	}
	else if (!strncmp(bind, "vm:", 3))
		n->bindkind = 'v';
	else if (!strncmp(bind, "mod:", 4))
		n->bindkind = 'm';
}

void UIBS_Close(void)
{
	if (st.dummy)
	{
		st.dummy--;
		return;
	}
	if (!st.depth)
		return;
	uibnode_t *n = st.stack[--st.depth];
	bind_fill(n);
	// props the engine keeps up to date itself survive a rebuild that does not set them
	if (!strcmp(n->ptype, "list") && !pprop(n, "count", NULL))
	{
		size_t cl;
		const char *cj = UIBS_PropJSON(n, "count", &cl);
		if (cj)
			setprop(&n->pprops, &n->npprops, &n->maxpprops, "count", cj, cl);
	}
	// private values never leave the store
	size_t pl;
	const char *pv = pprop(n, "private", &pl);
	if (pv && pl == 4 && !memcmp(pv, "true", 4))
		delprop(n->pprops, &n->npprops, "value");
	int diff = strcmp(n->type, n->ptype) || n->nprops != n->npprops || n->nkids != n->npkids;
	for (int i = 0; !diff && i < n->nprops; i++)
		diff = strcmp(n->props[i].key, n->pprops[i].key) || n->props[i].len != n->pprops[i].len ||
		       memcmp(n->props[i].json, n->pprops[i].json, n->props[i].len);
	for (int i = 0; !diff && i < n->nkids; i++)
		diff = n->kids[i] != n->pkids[i];
	if (diff)
	{
		uibprop_t *tp = n->props; int tn = n->nprops, tm = n->maxprops;
		n->props = n->pprops; n->nprops = n->npprops; n->maxprops = n->maxpprops;
		n->pprops = tp; n->npprops = tn; n->maxpprops = tm;
		uibnode_t **tk = n->kids; int kn = n->nkids, km = n->maxkids;
		n->kids = n->pkids; n->nkids = n->npkids; n->maxkids = n->maxpkids;
		n->pkids = tk; n->npkids = kn; n->maxpkids = km;
		memcpy(n->type, n->ptype, sizeof(n->type));
		changed(n);
	}
	freeprops(n->pprops, n->npprops);
	n->npprops = 0;
	n->npkids = 0;
	n->building = 0;
	for (int i = 0; i < n->nkids; i++)
		if (n->kids[i]->parent != n)
		{
			n->kids[i]->parent = n;
			touch_subtree(n->kids[i], 0);
		}
}

void UIBS_BuildEnd(void)
{
	while (st.depth || st.dummy)
		UIBS_Close();
	for (int i = 0; i < st.nold; i++)
		if (st.old[i]->mark != st.serial)
			st.old[i]->mark = -1;  // condemned
	// delete condemned nodes; their ids are unique pointers, delete one by one
	for (int i = 0; i < st.nold; i++)
		if (st.old[i]->mark == -1)
			freenode(st.old[i]);
	st.nold = 0;
	st.build_origin = NULL;
}

void UIBS_Remove(const char *id)
{
	uibnode_t *n = UIBS_Find(id);
	if (!n || !strcmp(id, "app"))
		return;
	uibnode_t *p = n->parent;
	if (p)
	{
		for (int i = 0; i < p->nkids; i++)
			if (p->kids[i] == n)
			{
				memmove(&p->kids[i], &p->kids[i + 1], sizeof(uibnode_t *) * (p->nkids - i - 1));
				p->nkids--;
				changed(p);
				break;
			}
	}
	delete_subtree(n);
}

// ---------------------------------------------------------------- app and navigation

static char *dupnull(const char *s) { return s ? strdup(s) : NULL; }

void UIBS_SetNav(const char *stack, const char *modal, int canback, const char *context)
{
	if (!st.stack_ids || strcmp(st.stack_ids, stack) || !st.modal_ids || strcmp(st.modal_ids, modal) ||
	    st.canback != canback || !st.context || strcmp(st.context, context))
	{
		free(st.stack_ids); st.stack_ids = strdup(stack);
		free(st.modal_ids); st.modal_ids = strdup(modal);
		free(st.context); st.context = strdup(context);
		st.canback = canback;
		st.navdirty = 1;
	}
}

void UIBS_SetBusy(const char *json, size_t len)
{
	if (!json || !len)
	{
		if (st.busy) { free(st.busy); st.busy = NULL; st.navdirty = 1; }
		return;
	}
	if (st.busy && strlen(st.busy) == len && !memcmp(st.busy, json, len))
		return;
	free(st.busy);
	st.busy = (char *)malloc(len + 1);
	memcpy(st.busy, json, len);
	st.busy[len] = 0;
	st.navdirty = 1;
}

// app.reserved: rects (dp) the engine draws over the UI (docs/UI_BRIDGE.md 4.12)
void UIBS_SetReserved(const char *json, size_t len)
{
	if (!json || !len)
	{
		if (st.reserved) { free(st.reserved); st.reserved = NULL; st.navdirty = 1; }
		return;
	}
	if (st.reserved && strlen(st.reserved) == len && !memcmp(st.reserved, json, len))
		return;
	free(st.reserved);
	st.reserved = (char *)malloc(len + 1);
	memcpy(st.reserved, json, len);
	st.reserved[len] = 0;
	st.navdirty = 1;
}

void UIBS_Toast(const char *text, const char *kind, const char *icon, float timeout, const char *actionjson)
{
	char id[32];
	if (st.ntoasts >= 8)
		UIBS_Remove(st.toasts[0].node->id);
	snprintf(id, sizeof(id), "toast/%d", ++st.toastseq);
	uibnode_t *n = newnode(id, "toast");
	if (!n)
		return;
	uibbuf_t b = {0};
	UIBB_Str(&b, text, strlen(text), UIB_MAX_STRING, 1);
	setprop(&n->props, &n->nprops, &n->maxprops, "text", b.p, b.n);
	b.n = 0;
	UIBB_Str(&b, kind && *kind ? kind : "info", strlen(kind && *kind ? kind : "info"), 16, 0);
	setprop(&n->props, &n->nprops, &n->maxprops, "kind", b.p, b.n);
	if (icon && *icon)
	{
		b.n = 0;
		UIBB_Str(&b, icon, strlen(icon), 40, 0);
		setprop(&n->props, &n->nprops, &n->maxprops, "icon", b.p, b.n);
	}
	if (timeout <= 0)
		timeout = 3;
	b.n = 0;
	UIBB_Num(&b, (int)(timeout + 0.5f));
	setprop(&n->props, &n->nprops, &n->maxprops, "timeout", b.p, b.n);
	if (actionjson && *actionjson)
		setprop(&n->props, &n->nprops, &n->maxprops, "action", actionjson, strlen(actionjson));
	UIBB_Free(&b);
	n->isroot = 1;
	st.toasts[st.ntoasts].node = n;
	st.toasts[st.ntoasts].expire = UIB_Time() + timeout + 1.0;
	st.ntoasts++;
	st.navdirty = 1;
}

static void ids_json(uibbuf_t *b, const char *list, uibnode_t **kids, int *nk, int maxk)
{
	int first = 1;
	UIBB_Putc(b, '[');
	for (const char *s = list; s && *s;)
	{
		while (*s == ' ') s++;
		const char *e = s;
		while (*e && *e != ' ') e++;
		if (e > s)
		{
			char id[UIB_MAX_ID];
			snprintf(id, sizeof(id), "%.*s", (int)(e - s), s);
			uibnode_t *n = UIBS_Find(id);
			if (!n && !strncmp(id, "mods.page.", 10) && UIBMod_Materialize(id))
				n = UIBS_Find(id);
			if (n && n->isroot && !strcmp(n->type, "screen"))
			{
				if (!first) UIBB_Putc(b, ',');
				UIBB_Str(b, id, strlen(id), 0, 0);
				first = 0;
				if (*nk < maxk)
					kids[(*nk)++] = n;
			}
		}
		s = e;
	}
	UIBB_Putc(b, ']');
}

static uibnode_t *app_node(void)
{
	uibnode_t *a = UIBS_Find("app");
	if (!a)
	{
		a = newnode("app", "app");
		st.navdirty = 1;
	}
	return a;
}

static void reconcile_app(void)
{
	double now = UIB_Time();
	for (int i = 0; i < st.ntoasts;)
	{
		if (now > st.toasts[i].expire)
		{
			UIBS_Remove(st.toasts[i].node->id);  // freenode drops it from the list
			st.navdirty = 1;
			continue;
		}
		i++;
	}
	if (!st.navdirty)
		return;
	st.navdirty = 0;
	uibnode_t *a = app_node();
	uibnode_t *kids[64];
	int nk = 0;
	uibbuf_t s = {0}, m = {0};
	ids_json(&s, st.stack_ids, kids, &nk, 48);
	ids_json(&m, st.modal_ids, kids, &nk, 56);
	for (int i = 0; i < st.ntoasts && nk < 64; i++)
		kids[nk++] = st.toasts[i].node;
	// drop roots that left the app
	for (uibnode_t *n = st.all, *next; n; n = next)
	{
		next = n->lnext;
		if (!n->isroot || n == a)
			continue;
		int keep = 0;
		for (int i = 0; i < nk; i++)
			keep |= kids[i] == n;
		if (!keep && strcmp(n->type, "toast"))
		{
			// unlink from app's kids if it is there, then delete the subtree
			for (int i = 0; i < a->nkids; i++)
				if (a->kids[i] == n)
				{
					memmove(&a->kids[i], &a->kids[i + 1], sizeof(uibnode_t *) * (a->nkids - i - 1));
					a->nkids--;
					break;
				}
			// next may be inside the subtree: restart the scan
			delete_subtree(n);
			next = st.all;
		}
	}
	uibbuf_t ctx = {0};
	UIBB_Str(&ctx, st.context ? st.context : "menu", strlen(st.context ? st.context : "menu"), 16, 0);
	UIBS_SetProp(a, "title", "\"Godnotic\"", 10);
	UIBS_SetProp(a, "stack", s.p, s.n);
	UIBS_SetProp(a, "modal", m.p, m.n);
	UIBS_SetProp(a, "canBack", st.canback ? "true" : "false", st.canback ? 4 : 5);
	UIBS_SetProp(a, "context", ctx.p, ctx.n);
	UIBS_SetProp(a, "busy", st.busy, st.busy ? strlen(st.busy) : 0);
	UIBS_SetProp(a, "reserved", st.reserved, st.reserved ? strlen(st.reserved) : 0);
	UIBB_Free(&s);
	UIBB_Free(&m);
	UIBB_Free(&ctx);
	int same = a->nkids == nk;
	for (int i = 0; same && i < nk; i++)
		same = a->kids[i] == kids[i];
	if (!same)
	{
		if (nk > a->maxkids)
		{
			a->maxkids = 64;
			a->kids = (uibnode_t **)realloc(a->kids, sizeof(uibnode_t *) * a->maxkids);
		}
		memcpy(a->kids, kids, sizeof(uibnode_t *) * nk);
		a->nkids = nk;
		changed(a);
		for (int i = 0; i < nk; i++)
			if (kids[i]->parent != a)
			{
				kids[i]->parent = a;
				touch_subtree(kids[i], 0);
			}
	}
}

int UIBS_Commit(void)
{
	app_node();
	reconcile_app();
	if (!st.dirty)
		return 0;
	st.rev++;
	st.dirty = 0;
	return 1;
}

void UIBS_WatchTick(void)
{
	for (uibnode_t *n = st.all; n; n = n->lnext)
	{
		if (n->bindkind != 'c' || !n->bindcvar)
			continue;
		const char *s = UIB_CvarString(n->bindcvar);
		if (n->bindval && !strcmp(n->bindval, s))
			continue;
		free(n->bindval);
		n->bindval = strdup(s);
		if (strcmp(n->type, "keybind"))
		{
			uibbuf_t b = {0};
			cvar_value_json(&b, n->type, s);
			UIBS_SetProp(n, "value", b.p, b.n);
			UIBB_Free(&b);
		}
		size_t bl;
		const char *bj = UIBS_PropJSON(n, "bind", &bl);
		char bind[160];
		snprintf(bind, sizeof(bind), "%.*s", bj && bl >= 2 ? (int)(bl - 2) : 0, bj ? bj + 1 : "");
		UIBM_Event("changed", n->id, bind, "");
	}
}

void UIBS_Reset(void)
{
	while (st.depth || st.dummy)
		UIBS_Close();
	st.nold = 0;
	// delete everything but app; screens come back when the menu VM rebuilds them
	for (uibnode_t *n = st.all, *next; n; n = next)
	{
		next = n->lnext;
		if (strcmp(n->id, "app"))
			freenode(n);
	}
	st.ntoasts = 0;
	free(st.stack_ids); st.stack_ids = NULL;
	free(st.modal_ids); st.modal_ids = NULL;
	free(st.busy); st.busy = NULL;
	free(st.reserved); st.reserved = NULL;
	st.navdirty = 1;
	app_node();
	(void)dupnull;
}

// ---------------------------------------------------------------- serialisation

static void node_json(uibnode_t *n)
{
	if (n->json)
		return;
	uibbuf_t b = {0};
	UIBB_Puts(&b, "{\"id\":");
	UIBB_Str(&b, n->id, strlen(n->id), 0, 0);
	UIBB_Puts(&b, ",\"type\":\"");
	UIBB_Puts(&b, n->type);
	UIBB_Puts(&b, "\",\"props\":{");
	for (int i = 0; i < n->nprops; i++)
	{
		if (i)
			UIBB_Putc(&b, ',');
		UIBB_Putc(&b, '"');
		UIBB_Puts(&b, n->props[i].key);
		UIBB_Puts(&b, "\":");
		UIBB_Put(&b, n->props[i].json, n->props[i].len);
	}
	UIBB_Puts(&b, "},\"children\":[");
	for (int i = 0; i < n->nkids; i++)
	{
		if (i)
			UIBB_Putc(&b, ',');
		UIBB_Str(&b, n->kids[i]->id, strlen(n->kids[i]->id), 0, 0);
	}
	UIBB_Putc(&b, ']');
	if (n->origin)
	{
		UIBB_Puts(&b, ",\"origin\":");
		UIBB_Str(&b, n->origin, strlen(n->origin), 0, 0);
	}
	UIBB_Putc(&b, '}');
	n->json = UIBB_Detach(&b, &n->jsonlen);
}

static void emit(uibbuf_t *b, uibnode_t *n, int64_t since, int *first, int depth)
{
	if (n->gen > since)
	{
		node_json(n);
		if (!*first)
			UIBB_Putc(b, ',');
		*first = 0;
		UIBB_Put(b, n->json, n->jsonlen);
	}
	if (depth >= UIB_MAX_DEPTH)
		return;
	for (int i = 0; i < n->nkids; i++)
		emit(b, n->kids[i], since, first, depth + 1);
}

char *UIBS_Snapshot(size_t *len, int msgid)
{
	uibbuf_t b = {0};
	int first = 1;
	UIBB_Reserve(&b, 16384);
	UIBB_Printf(&b, "{\"uib\":1,\"t\":\"snapshot\",\"id\":%d,\"rev\":%lld,\"root\":\"app\",\"nodes\":[", msgid, (long long)st.rev);
	emit(&b, app_node(), -1, &first, 0);
	UIBB_Puts(&b, "]}");
	return UIBB_Detach(&b, len);
}

char *UIBS_Diff(int64_t since, size_t *len, int msgid)
{
	if (since < st.floor || since > st.rev)
		return NULL;
	uibbuf_t b = {0};
	int first = 1;
	UIBB_Printf(&b, "{\"uib\":1,\"t\":\"diff\",\"id\":%d,\"base\":%lld,\"rev\":%lld,\"set\":[", msgid, (long long)since, (long long)st.rev);
	emit(&b, app_node(), since, &first, 0);
	UIBB_Puts(&b, "],\"del\":[");
	first = 1;
	for (int i = 0; i < st.ntomb; i++)
	{
		int k = (st.tombhead - st.ntomb + i + TOMBS) % TOMBS;
		tomb_t *t = &st.tombs[k];
		if (t->id && t->rev > since && t->rev <= st.rev)
		{
			if (!first)
				UIBB_Putc(&b, ',');
			first = 0;
			UIBB_Str(&b, t->id, strlen(t->id), 0, 0);
		}
	}
	UIBB_Puts(&b, "]}");
	return UIBB_Detach(&b, len);
}

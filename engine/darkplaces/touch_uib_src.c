/*
Godnotic UI bridge: data sources, read by window (offset, limit, sort, filter), so a
2,000-server list never crosses the bridge at once.

  servers, servers.lan, servers.fav   the engine's server list (Internet, LAN, VPN)
  players:<address>                   a server's players from its status reply
  maps, maps:<mode>                   installed maps from the maps/ mapinfo files
  anything else                       rows the menu VM sets (modes, recent, mods ...)

Rows are kept parsed (one cell array per row) so sorting and filtering re-window without
rebuilding. Servers are re-read at most 4 times a second, and only while a list in the
tree shows them; a change bumps the source's rev and every renderer gets a `source` notice.
*/
#include "quakedef.h"
#include "netconn.h"
#include "touch_lan_logic.h"
#include "touch_uib_int.h"

typedef struct
{
	const char *k;   // points into the row's key pool
	char *s;         // string value (display), NULL for numbers/bools
	double n;
	char kind;       // 's', 'n', 'b'
} cell_t;

struct uibrow_s
{
	char *key, *image;
	cell_t *cells;
	int ncells;
};

#define MAX_SOURCES 48
#define MAX_PARAMS 12

typedef struct
{
	char name[160];
	int kind;               // 0 static, 1 servers, 2 maps, 3 players
	uibrow_t *rows;
	int nrows, maxrows;
	int64_t rev;
	unsigned sig;
	double next_poll;
	int used;               // a list node shows it
	struct { char k[32]; char v[128]; } params[MAX_PARAMS];
	int nparams;
	int64_t notified_rev;
	int lasttotal;
} source_t;

static source_t sources[MAX_SOURCES];
static int nsources;
static source_t *building;

static void freerows(source_t *s)
{
	for (int i = 0; i < s->nrows; i++)
	{
		for (int c = 0; c < s->rows[i].ncells; c++)
		{
			free((char *)s->rows[i].cells[c].k);
			free(s->rows[i].cells[c].s);
		}
		free(s->rows[i].cells);
		free(s->rows[i].key);
		free(s->rows[i].image);
	}
	s->nrows = 0;
}

static source_t *getsource(const char *name, int create)
{
	for (int i = 0; i < nsources; i++)
		if (!strcmp(sources[i].name, name))
			return &sources[i];
	if (!create || nsources >= MAX_SOURCES || strlen(name) >= sizeof(sources[0].name))
		return NULL;
	source_t *s = &sources[nsources++];
	memset(s, 0, sizeof(*s));
	dp_strlcpy(s->name, name, sizeof(s->name));
	s->rev = 1;
	if (!strncmp(name, "servers", 7))
		s->kind = 1;
	else if (!strcmp(name, "maps") || !strncmp(name, "maps:", 5))
		s->kind = 2;
	else if (!strncmp(name, "players:", 8))
		s->kind = 3;
	s->notified_rev = s->rev;
	return s;
}

static const char *param(source_t *s, const char *k, const char *def)
{
	for (int i = 0; i < s->nparams; i++)
		if (!strcmp(s->params[i].k, k))
			return s->params[i].v;
	// servers.* share the Servers screen's filters
	if (s->kind == 1 && strcmp(s->name, "servers") && strcmp(s->name, "servers.all"))
	{
		source_t *base = getsource("servers", 0);
		if (base)
			return param(base, k, def);
	}
	return def;
}

void UIBSrc_Param(const char *source, const char *key, const char *value)
{
	source_t *s = getsource(source, 1);
	if (!s)
		return;
	for (int i = 0; i < s->nparams; i++)
		if (!strcmp(s->params[i].k, key))
		{
			if (strcmp(s->params[i].v, value))
			{
				dp_strlcpy(s->params[i].v, value, sizeof(s->params[i].v));
				s->rev++;
				s->sig = 0;
				s->next_poll = 0;
			}
			return;
		}
	if (s->nparams >= MAX_PARAMS)
		return;
	dp_strlcpy(s->params[s->nparams].k, key, sizeof(s->params[0].k));
	dp_strlcpy(s->params[s->nparams].v, value, sizeof(s->params[0].v));
	s->nparams++;
	s->rev++;
	s->sig = 0;
	s->next_poll = 0;
}

// ---------------------------------------------------------------- row building

static uibrow_t *addrow(source_t *s, const char *key, const char *image)
{
	if (s->nrows >= s->maxrows)
	{
		s->maxrows = s->maxrows ? s->maxrows * 2 : 64;
		s->rows = (uibrow_t *)realloc(s->rows, sizeof(uibrow_t) * s->maxrows);
	}
	uibrow_t *r = &s->rows[s->nrows++];
	memset(r, 0, sizeof(*r));
	r->key = strdup(key);
	r->image = image && *image ? strdup(image) : NULL;
	return r;
}

static cell_t *addcell(uibrow_t *r, const char *k)
{
	r->cells = (cell_t *)realloc(r->cells, sizeof(cell_t) * (r->ncells + 1));
	cell_t *c = &r->cells[r->ncells++];
	memset(c, 0, sizeof(*c));
	c->k = strdup(k);
	return c;
}

static void cell_s(uibrow_t *r, const char *k, const char *v, size_t len)
{
	cell_t *c = addcell(r, k);
	c->kind = 's';
	c->s = (char *)malloc(len + 1);
	UIB_CleanText(v, len, c->s, len + 1);
}

static void cell_n(uibrow_t *r, const char *k, double v)
{
	cell_t *c = addcell(r, k);
	c->kind = 'n';
	c->n = v;
}

static void cell_b(uibrow_t *r, const char *k, int v)
{
	cell_t *c = addcell(r, k);
	c->kind = 'b';
	c->n = v != 0;
}

static const cell_t *getcell(const uibrow_t *r, const char *k)
{
	for (int i = 0; i < r->ncells; i++)
		if (!strcmp(r->cells[i].k, k))
			return &r->cells[i];
	return NULL;
}

// ---------------------------------------------------------------- servers

static const char *server_net(const char *cname)
{
	unsigned a, b, c, d;
	if (sscanf(cname, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
	{
		if (a == 100 && b >= 64 && b <= 127)
			return "vpn";  // CGNAT range: Tailscale, ZeroTier ...
		if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || a == 127 || (a == 169 && b == 254))
			return "lan";
		return "internet";
	}
	if (!strncasecmp(cname, "[fe80", 5) || !strncasecmp(cname, "fe80", 4))
		return "lan";
	if (!strncasecmp(cname, "[fd", 3) || !strncasecmp(cname, "fd", 2))
		return "vpn";
	if (!strncmp(cname, "local", 5))
		return "lan";
	return "internet";
}

#ifdef CONFIG_MENU
static unsigned server_sig(void)
{
	unsigned h = 2166136261u ^ serverlist_cachecount;
	for (unsigned i = 0; i < serverlist_cachecount; i++)
	{
		const serverlist_info_t *in = &serverlist_cache[i].info;
		h = (h ^ (unsigned)in->ping) * 16777619u;
		h = (h ^ (unsigned)in->numplayers) * 16777619u;
		h = (h ^ (unsigned)in->isfavorite) * 16777619u;
		h = (h ^ (unsigned)serverlist_cache[i].responded) * 16777619u;
		for (const char *p = in->map; *p; p++)
			h = (h ^ (unsigned char)*p) * 16777619u;
	}
	return h ? h : 1;
}

static void build_servers(source_t *s)
{
	freerows(s);
	int all = !strcmp(s->name, "servers.all");  // lookups by address: no filters
	int lanonly = !all && (!strcmp(s->name, "servers.lan") || atoi(param(s, "lan", "0")));
	int favonly = !strcmp(s->name, "servers.fav");
	int showempty = all || atoi(param(s, "empty", "1"));
	int showfull = all || atoi(param(s, "full", "1"));
	int showlaggy = all || atoi(param(s, "laggy", "1"));
	int laggyms = atoi(param(s, "laggyms", "150"));
	for (unsigned i = 0; i < serverlist_cachecount; i++)
	{
		const serverlist_entry_t *e = &serverlist_cache[i];
		const serverlist_info_t *in = &e->info;
		if (!e->responded || !in->cname[0])
			continue;
		const char *net = server_net(in->cname);
		if (lanonly && !strcmp(net, "internet"))
			continue;
		if (favonly && !in->isfavorite)
			continue;
		if (!showempty && in->numhumans <= 0)
			continue;
		if (!showfull && in->freeslots <= 0 && in->maxplayers > 0)
			continue;
		if (!showlaggy && in->ping >= laggyms)
			continue;
		char img[64];
		dpsnprintf(img, sizeof(img), "pk3:maps/%s", in->map);
		uibrow_t *r = addrow(s, in->cname, in->map[0] ? img : NULL);
		cell_s(r, "name", in->name, in->name_len);
		cell_s(r, "map", in->map, in->map_len);
		char mode[32] = "";
		const char *colon = strchr(in->qcstatus, ':');
		if (colon && colon - in->qcstatus < (int)sizeof(mode))
			memcpy(mode, in->qcstatus, colon - in->qcstatus), mode[colon - in->qcstatus] = 0;
		cell_s(r, "mode", mode, strlen(mode));
		cell_n(r, "players", in->numplayers);
		cell_n(r, "max", in->maxplayers);
		cell_n(r, "bots", in->numbots);
		cell_n(r, "ping", in->ping);
		cell_b(r, "lock", strstr(in->qcstatus, TOUCHLAN_QCSTATUS_LOCK) != NULL);
		cell_b(r, "fav", in->isfavorite);
		cell_s(r, "address", in->cname, in->cname_len);
		cell_s(r, "net", net, strlen(net));
		const char *ver = colon ? colon + 1 : "";
		const char *ve = strchr(ver, ':');
		size_t vl = ve ? (size_t)(ve - ver) : strlen(ver);
		cell_s(r, "version", ver, vl < 32 ? vl : 31);
	}
}

static void build_players(source_t *s)
{
	freerows(s);
	const char *addr = s->name + 8;
	for (unsigned i = 0; i < serverlist_cachecount; i++)
	{
		const serverlist_info_t *in = &serverlist_cache[i].info;
		if (strcmp(in->cname, addr))
			continue;
		// "frags ping \"name\" team" per line
		const char *p = in->players;
		int n = 0;
		while (*p && n < 256)
		{
			int frags = 0, ping = 0, team = 0;
			char name[160] = "";
			const char *eol = strchr(p, '\n');
			size_t ll = eol ? (size_t)(eol - p) : strlen(p);
			char line[256];
			if (ll >= sizeof(line)) ll = sizeof(line) - 1;
			memcpy(line, p, ll);
			line[ll] = 0;
			char *q1 = strchr(line, '"'), *q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
			if (q1 && q2)
			{
				sscanf(line, "%d %d", &frags, &ping);
				dpsnprintf(name, sizeof(name), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
				sscanf(q2 + 1, "%d", &team);
				char key[16];
				dpsnprintf(key, sizeof(key), "p%d", n++);
				uibrow_t *r = addrow(s, key, NULL);
				cell_s(r, "name", name, strlen(name));
				cell_n(r, "score", frags);
				cell_n(r, "ping", ping);
				char t[8];
				dpsnprintf(t, sizeof(t), "%d", team);
				cell_s(r, "team", team > 0 ? t : "", team > 0 ? strlen(t) : 0);
			}
			if (!eol)
				break;
			p = eol + 1;
		}
		break;
	}
}

// ---------------------------------------------------------------- maps

#else
// Dedicated server (make sv-release): there is no server browser cache.
static unsigned server_sig(void) { return 1; }
static void build_servers(source_t *s) { freerows(s); }
static void build_players(source_t *s) { freerows(s); }
#endif

static void build_maps(source_t *s)
{
	freerows(s);
	const char *mode = !strncmp(s->name, "maps:", 5) ? s->name + 5 : NULL;
	fssearch_t *search = FS_Search("maps/*.mapinfo", true, true, NULL);
	if (!search)
		return;
	for (int i = 0; i < search->numfilenames; i++)
	{
		const char *fn = search->filenames[i];
		char name[64];
		const char *base = fn + 5;
		size_t bl = strlen(base) - 8;
		if (bl >= sizeof(name) || bl == 0)
			continue;
		memcpy(name, base, bl);
		name[bl] = 0;
		if (name[0] == '_')
			continue;  // _hudsetup and other tools
		fs_offset_t len;
		char *text = (char *)FS_LoadFile(fn, tempmempool, true, &len);
		if (!text)
			continue;
		char title[128] = "", author[256] = "", desc[512] = "", modes[256] = "";
		int hasmode = !mode;
		for (char *line = text; line && *line;)
		{
			char *eol = strchr(line, '\n');
			if (eol)
				*eol = 0;
			if (!strncmp(line, "title ", 6)) dp_strlcpy(title, line + 6, sizeof(title));
			else if (!strncmp(line, "author ", 7)) dp_strlcpy(author, line + 7, sizeof(author));
			else if (!strncmp(line, "description ", 12)) dp_strlcpy(desc, line + 12, sizeof(desc));
			else if (!strncmp(line, "gametype ", 9) || !strncmp(line, "type ", 5))
			{
				char g[32];
				if (sscanf(line + (line[0] == 'g' ? 9 : 5), "%31s", g) == 1)
				{
					if (!strcmp(g, "rc")) dp_strlcpy(g, "race", sizeof(g));
					if (strlen(modes) + strlen(g) + 2 < sizeof(modes))
					{
						if (modes[0]) dp_strlcat(modes, " ", sizeof(modes));
						dp_strlcat(modes, g, sizeof(modes));
					}
					if (mode && !strcmp(g, mode))
						hasmode = 1;
				}
			}
			line = eol ? eol + 1 : NULL;
		}
		Mem_Free(text);
		if (!hasmode)
			continue;
		char img[96];
		dpsnprintf(img, sizeof(img), "pk3:maps/%s", name);
		uibrow_t *r = addrow(s, name, img);
		cell_s(r, "name", name, strlen(name));
		if (!title[0])
			dp_strlcpy(title, name, sizeof(title));
		cell_s(r, "title", title, strlen(title));
		cell_s(r, "author", author, strlen(author));
		cell_s(r, "description", desc, strlen(desc));
		cell_s(r, "modes", modes, strlen(modes));
	}
	FS_FreeSearch(search);
}

// ---------------------------------------------------------------- static rows (menu VM)

void UIBSrc_StaticBegin(const char *source)
{
	building = getsource(source, 1);
	if (building && building->kind == 0)
		freerows(building);
	else
		building = NULL;
}

void UIBSrc_StaticRow(const char *key, const char *cellsjson, const char *image)
{
	if (!building || building->nrows >= 65536)
		return;
	uibjdoc_t *d = UIBJ_Parse(cellsjson, strlen(cellsjson), NULL, 0);
	uibj_t *o = UIBJ_Root(d);
	uibrow_t *r = addrow(building, key, image);
	if (o && o->type == UIBJ_OBJ)
		for (uibj_t *m = o->child; m; m = m->next)
		{
			if (m->type == UIBJ_STR) cell_s(r, m->key, m->str, m->len);
			else if (m->type == UIBJ_NUM) cell_n(r, m->key, m->num);
			else if (m->type == UIBJ_TRUE || m->type == UIBJ_FALSE) cell_b(r, m->key, m->type == UIBJ_TRUE);
		}
	UIBJ_Free(d);
}

void UIBSrc_StaticEnd(void)
{
	if (!building)
		return;
	// a cheap signature so an unchanged set does not bump the rev
	unsigned h = 2166136261u ^ (unsigned)building->nrows;
	for (int i = 0; i < building->nrows; i++)
	{
		for (const char *p = building->rows[i].key; *p; p++)
			h = (h ^ (unsigned char)*p) * 16777619u;
		for (int c = 0; c < building->rows[i].ncells; c++)
		{
			const cell_t *cel = &building->rows[i].cells[c];
			if (cel->s) for (const char *p = cel->s; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
			else h = (h ^ (unsigned)(cel->n * 1000)) * 16777619u;
		}
	}
	if (h != building->sig)
	{
		building->sig = h;
		building->rev++;
	}
	building = NULL;
}

// ---------------------------------------------------------------- refresh, notify

static void refresh(source_t *s, int force)
{
	double now = UIB_Time();
	if (s->kind == 0)
		return;
	if (!force && now < s->next_poll)
		return;
	s->next_poll = now + (s->kind == 2 ? 3600 : 0.25);
	if (s->kind == 1 || s->kind == 3)
	{
		unsigned sig = server_sig();
		if (sig == s->sig)
			return;
		s->sig = sig;
		if (s->kind == 1) build_servers(s);
		else build_players(s);
		s->rev++;
	}
	else if (s->kind == 2)
	{
		if (s->sig && !force)
			return;
		build_maps(s);
		s->sig = 1;
		s->rev++;
	}
}

int64_t UIBSrc_Rev(const char *source)
{
	source_t *s = getsource(source, 0);
	return s ? s->rev : 0;
}

// the list nodes' count follows their source; renderers get a `source` notice
void UIBSrc_UpdateCounts(void)
{
	for (int i = 0; i < nsources; i++)
		sources[i].used = 0;
	for (uibnode_t *n = UIBS_FirstNode(); n; n = n->lnext)
	{
		if (strcmp(n->type, "list"))
			continue;
		size_t l;
		const char *sj = UIBS_PropJSON(n, "source", &l);
		if (!sj || l < 3 || l > sizeof(sources[0].name))
			continue;
		char name[160];
		dpsnprintf(name, sizeof(name), "%.*s", (int)(l - 2), sj + 1);
		source_t *s = getsource(name, 1);
		if (!s)
			continue;
		s->used = 1;
		refresh(s, 0);
		char cnt[24];
		int c = dpsnprintf(cnt, sizeof(cnt), "%d", s->nrows);
		UIBS_SetProp(n, "count", cnt, c);
	}
	for (int i = 0; i < nsources; i++)
	{
		source_t *s = &sources[i];
		if (!s->used || s->rev == s->notified_rev)
			continue;
		s->notified_rev = s->rev;
		char buf[256];
		int n = dpsnprintf(buf, sizeof(buf), "{\"uib\":1,\"t\":\"source\",\"id\":0,\"source\":\"%s\",\"rev\":%lld,\"total\":%d}",
			s->name, (long long)s->rev, s->nrows);
		UIB_Broadcast(buf, n);
	}
}

void UIBSrc_Frame(void)
{
	static double next;
	double now = UIB_Time();
	if (now < next)
		return;
	next = now + 0.25;
	UIBSrc_UpdateCounts();
}

// ---------------------------------------------------------------- windows

static const char *sortkey;
static int sortdesc;
static source_t *sortsrc;

static int cmp_rows(const void *a, const void *b)
{
	const uibrow_t *ra = &sortsrc->rows[*(const int *)a], *rb = &sortsrc->rows[*(const int *)b];
	const cell_t *ca = getcell(ra, sortkey), *cb = getcell(rb, sortkey);
	int r;
	if (!ca || !cb)
		r = (ca != NULL) - (cb != NULL);
	else if (ca->s && cb->s)
		r = strcasecmp(ca->s, cb->s);
	else if (ca->s || cb->s)
		r = ca->s ? 1 : -1;
	else
	{
		double x = ca->n, y = cb->n;
		// a ping of 0 means unknown: always last
		if (!strcmp(sortkey, "ping") && (x <= 0 || y <= 0))
			return (x <= 0) - (y <= 0);
		r = x < y ? -1 : x > y;
	}
	if (sortdesc)
		r = -r;
	if (!r)
		r = *(const int *)a - *(const int *)b;  // stable
	return r;
}

static int has_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

static int row_matches(const uibrow_t *r, const char *filter)
{
	if (!filter || !*filter)
		return 1;
	for (int i = 0; i < r->ncells; i++)
		if (r->cells[i].s && has_ci(r->cells[i].s, filter))
			return 1;
	return 0;
}

// The visible order: indices into s->rows, filtered and sorted.
static int *order(source_t *s, const char *sort, int desc, const char *filter, int *count)
{
	int *idx = (int *)malloc(sizeof(int) * (s->nrows + 1));
	int n = 0;
	for (int i = 0; i < s->nrows; i++)
		if (row_matches(&s->rows[i], filter))
			idx[n++] = i;
	if (s->kind == 1 && (!sort || !*sort))
		sort = "ping";
	if (sort && *sort)
	{
		sortkey = sort;
		sortdesc = desc;
		sortsrc = s;
		qsort(idx, n, sizeof(int), cmp_rows);
	}
	*count = n;
	return idx;
}

static void row_json(uibbuf_t *b, const uibrow_t *r)
{
	UIBB_Puts(b, "{\"key\":");
	UIBB_Str(b, r->key, strlen(r->key), 255, 0);
	if (r->image)
	{
		UIBB_Puts(b, ",\"image\":");
		UIBB_Str(b, r->image, strlen(r->image), 250, 0);
	}
	UIBB_Puts(b, ",\"cells\":{");
	for (int c = 0; c < r->ncells; c++)
	{
		const cell_t *cel = &r->cells[c];
		if (c)
			UIBB_Putc(b, ',');
		UIBB_Str(b, cel->k, strlen(cel->k), 64, 0);
		UIBB_Putc(b, ':');
		if (cel->kind == 's') UIBB_Str(b, cel->s, strlen(cel->s), UIB_MAX_STRING, 0);
		else if (cel->kind == 'b') UIBB_Puts(b, cel->n ? "true" : "false");
		else UIBB_Num(b, cel->n);
	}
	UIBB_Puts(b, "}}");
}

void UIBSrc_Window(uibbuf_t *b, int msgid, int64_t re, const char *source, int offset, int limit,
                   const char *sort, int desc, const char *filter, int hasfilter)
{
	source_t *s = getsource(source, 0);
	if (!s && (!strncmp(source, "servers", 7) || !strncmp(source, "maps", 4) || !strncmp(source, "players:", 8)))
		s = getsource(source, 1);
	if (!s)
	{
		UIBB_Printf(b, "{\"uib\":1,\"t\":\"error\",\"id\":%d,\"re\":%lld,\"code\":\"not_found\",\"msg\":\"no such source\"}", msgid, (long long)re);
		return;
	}
	refresh(s, 0);
	if (!hasfilter)
		filter = param(s, "search", "");
	if (offset < 0) offset = 0;
	if (limit < 1) limit = 1;
	if (limit > UIB_MAX_WINDOW) limit = UIB_MAX_WINDOW;
	int count;
	int *idx = order(s, sort, desc, filter, &count);
	UIBB_Printf(b, "{\"uib\":1,\"t\":\"rows\",\"id\":%d,\"re\":%lld,\"source\":", msgid, (long long)re);
	UIBB_Str(b, s->name, strlen(s->name), 0, 0);
	UIBB_Printf(b, ",\"rev\":%lld,\"total\":%d,\"offset\":%d", (long long)s->rev, count, offset);
	if (sort && *sort)
	{
		UIBB_Puts(b, ",\"sort\":");
		UIBB_Str(b, sort, strlen(sort), 64, 0);
		UIBB_Printf(b, ",\"desc\":%s", desc ? "true" : "false");
	}
	if (hasfilter)
	{
		UIBB_Puts(b, ",\"filter\":");
		UIBB_Str(b, filter, strlen(filter), 256, 0);
	}
	UIBB_Puts(b, ",\"rows\":[");
	for (int i = offset; i < count && i < offset + limit; i++)
	{
		if (i > offset)
			UIBB_Putc(b, ',');
		row_json(b, &s->rows[idx[i]]);
	}
	UIBB_Puts(b, "]}");
	free(idx);
}

// ---------------------------------------------------------------- the QuakeC renderer's window

static struct
{
	source_t *s;
	int *idx;
	int count, offset, n;
} lw;

int UIBSrc_LocalWindow(const char *source, int offset, int limit, const char *sort, int desc, const char *filter)
{
	source_t *s = getsource(source, 1);
	free(lw.idx);
	memset(&lw, 0, sizeof(lw));
	if (!s)
		return 0;
	refresh(s, 0);
	if (!filter || !*filter)
		filter = param(s, "search", "");
	lw.s = s;
	lw.idx = order(s, sort, desc, filter, &lw.count);
	if (offset < 0) offset = 0;
	lw.offset = offset;
	lw.n = lw.count - offset;
	if (lw.n > limit) lw.n = limit;
	if (lw.n < 0) lw.n = 0;
	return lw.n;
}

int UIBSrc_LocalTotal(void) { return lw.count; }

static const uibrow_t *lrow(int i)
{
	if (!lw.s || i < 0 || i >= lw.n)
		return NULL;
	return &lw.s->rows[lw.idx[lw.offset + i]];
}

const char *UIBSrc_LocalKey(int i) { const uibrow_t *r = lrow(i); return r ? r->key : ""; }
const char *UIBSrc_LocalImage(int i) { const uibrow_t *r = lrow(i); return r && r->image ? r->image : ""; }

const char *UIBSrc_LocalCell(int i, const char *key)
{
	static char num[64];
	const uibrow_t *r = lrow(i);
	const cell_t *c = r ? getcell(r, key) : NULL;
	if (!c)
		return "";
	if (c->s)
		return c->s;
	if (c->n == (double)(long long)c->n)
		dpsnprintf(num, sizeof(num), "%lld", (long long)c->n);
	else
		dpsnprintf(num, sizeof(num), "%g", c->n);
	return num;
}

void UIBSrc_Init(void) {}

void UIBSrc_Reset(void)
{
	for (int i = 0; i < nsources; i++)
		freerows(&sources[i]), free(sources[i].rows);
	nsources = 0;
	free(lw.idx);
	memset(&lw, 0, sizeof(lw));
}

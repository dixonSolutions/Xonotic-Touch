/*
Godnotic UI bridge: connections, the protocol, the frame, console commands and mod pages.
docs/UI_BRIDGE.md is the contract; touch_uib_store.c holds the tree.
*/
#include "quakedef.h"
#include "thread.h"
#include "touch_uib_int.h"
#include "touch_shellbtn.h"
#ifdef __linux__
#include <sys/eventfd.h>
#include <unistd.h>
#endif

cvar_t menu_touch_uib = {CF_CLIENT | CF_ARCHIVE, "menu_touch_uib", "0", "Godnotic: 1 draws the menu from the UI bridge descriptions (touch_uib_*.qc) with the QuakeC renderer; 0 keeps the touch menu"};
static cvar_t uib_version = {CF_CLIENT | CF_READONLY, "uib_version", TOUCHUIB_SCHEMA, "Godnotic UI bridge schema version (the menu VM checks it before using the bridge builtins)"};
static cvar_t uib_debug = {CF_CLIENT, "uib_debug", "0", "Godnotic UI bridge: 1 logs messages, 2 also logs their text"};

// ---------------------------------------------------------------- queues

typedef struct qmsg_s
{
	struct qmsg_s *next;
	size_t len;
	char data[1];
} qmsg_t;

#define CONN_IN_MAX_MSGS 256
#define CONN_IN_MAX_BYTES (8u << 20)
#define CONN_OUT_MAX_BYTES (16u << 20)

struct touchuib_conn_s
{
	int slot;
	char name[32];
	void *mutex;
	int closed;            // set by Close (any thread)
	qmsg_t *in_head, *in_tail;
	int in_count;
	size_t in_bytes;
	qmsg_t *out_head, *out_tail;
	size_t out_bytes;
	int overflow;
	int efd;
	void (*notify)(void *);
	void *notify_ctx;
	// engine thread
	int hello;
	int64_t sent_rev;
	int msgid;
	int print;             // loopback: print what the engine sends
	int local;             // the QuakeC renderer's event queue: replies are dropped
	char caps[2048];
};

static struct
{
	int up;
	void *mutex;
	touchuib_conn_t *conns[UIB_MAX_CONNS];
	double next_watch, next_tick;
	int touched;           // the store has been written to (menu VM or mods)
	touchuib_mod_event_fn modfn;
	void *modctx;
	int loopback;          // conn slot of the console loopback, -1
} uib = {0, NULL, {0}, 0, 0, 0, NULL, NULL, -1};

int uib_loopback_print;

double UIB_Time(void) { return Sys_DirtyTime(); }

void *UIB_CvarFind(const char *name)
{
	return Cvar_FindVar(&cvars_all, name, ~0u);
}

const char *UIB_CvarString(void *cv)
{
	return ((cvar_t *)cv)->string;
}

int TouchUIB_Available(void) { return uib.up; }

static void qpush(qmsg_t **head, qmsg_t **tail, qmsg_t *m)
{
	m->next = NULL;
	if (*tail) (*tail)->next = m;
	else *head = m;
	*tail = m;
}

static qmsg_t *qpop(qmsg_t **head, qmsg_t **tail)
{
	qmsg_t *m = *head;
	if (m)
	{
		*head = m->next;
		if (!*head)
			*tail = NULL;
	}
	return m;
}

static qmsg_t *qmake(const char *s, size_t len)
{
	qmsg_t *m = (qmsg_t *)malloc(sizeof(qmsg_t) + len);
	if (!m)
		return NULL;
	memcpy(m->data, s, len);
	m->data[len] = 0;
	m->len = len;
	return m;
}

static void qfree(qmsg_t **head, qmsg_t **tail)
{
	qmsg_t *m;
	while ((m = qpop(head, tail)))
		free(m);
}

touchuib_conn_t *TouchUIB_Open(const char *name, size_t name_len)
{
	if (!uib.up)
		return NULL;
	touchuib_conn_t *c = NULL;
	Thread_LockMutex(uib.mutex);
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		if (!uib.conns[i])
		{
			c = (touchuib_conn_t *)calloc(1, sizeof(*c));
			c->slot = i;
			c->mutex = Thread_CreateMutex();
			c->efd = -1;
#ifdef __linux__
			c->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
			if (name_len > sizeof(c->name) - 1)
				name_len = sizeof(c->name) - 1;
			memcpy(c->name, name ? name : "?", name ? name_len : 1);
			uib.conns[i] = c;
			break;
		}
	Thread_UnlockMutex(uib.mutex);
	return c;
}

int TouchUIB_Post(touchuib_conn_t *c, const char *json, size_t len)
{
	if (!c)
		return -1;
	if (len > UIB_MAX_MESSAGE)
		return -3;
	int r = 0;
	Thread_LockMutex(c->mutex);
	if (c->closed || !uib.up)
		r = -1;
	else if (c->in_count >= CONN_IN_MAX_MSGS || c->in_bytes + len > CONN_IN_MAX_BYTES)
		r = -2;
	else
	{
		qmsg_t *m = qmake(json, len);
		if (!m)
			r = -2;
		else
		{
			qpush(&c->in_head, &c->in_tail, m);
			c->in_count++;
			c->in_bytes += len;
		}
	}
	Thread_UnlockMutex(c->mutex);
	return r;
}

char *TouchUIB_Take(touchuib_conn_t *c, size_t *len)
{
	if (!c)
		return NULL;
	Thread_LockMutex(c->mutex);
	qmsg_t *m = qpop(&c->out_head, &c->out_tail);
	if (m)
		c->out_bytes -= m->len;
	Thread_UnlockMutex(c->mutex);
	if (!m)
		return NULL;
	// hand the payload over as its own allocation (the caller frees it with TouchUIB_Free)
	char *p = (char *)malloc(m->len + 1);
	memcpy(p, m->data, m->len + 1);
	if (len)
		*len = m->len;
	free(m);
	return p;
}

void TouchUIB_Free(char *buf) { free(buf); }

int TouchUIB_NotifyFd(touchuib_conn_t *c) { return c ? c->efd : -1; }

void TouchUIB_SetNotify(touchuib_conn_t *c, void (*fn)(void *ctx), void *ctx)
{
	if (!c)
		return;
	Thread_LockMutex(c->mutex);
	c->notify = fn;
	c->notify_ctx = ctx;
	Thread_UnlockMutex(c->mutex);
}

void TouchUIB_Close(touchuib_conn_t *c)
{
	if (!c)
		return;
	Thread_LockMutex(c->mutex);
	c->closed = 1;
	c->notify = NULL;
	Thread_UnlockMutex(c->mutex);
	// freed by the engine thread at its next frame (it may be using it right now)
}

static void conn_free(touchuib_conn_t *c)
{
	Thread_LockMutex(uib.mutex);
	uib.conns[c->slot] = NULL;
	Thread_UnlockMutex(uib.mutex);
	qfree(&c->in_head, &c->in_tail);
	qfree(&c->out_head, &c->out_tail);
#ifdef __linux__
	if (c->efd >= 0)
		close(c->efd);
#endif
	Thread_DestroyMutex(c->mutex);
	if (uib.loopback == c->slot)
		uib.loopback = -1;
	free(c);
	UIBM_Event("detach", "", "", "");
}

// ---------------------------------------------------------------- sending (engine thread)

int UIB_ConnCount(void)
{
	int n = 0;
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		n += uib.conns[i] && uib.conns[i]->hello && !uib.conns[i]->closed && !uib.conns[i]->local;
	return n;
}

int TouchUIB_NativeAttached(void)
{
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		if (uib.conns[i] && uib.conns[i]->hello && !uib.conns[i]->closed && !uib.conns[i]->print && !uib.conns[i]->local)
			return 1;
	return 0;
}

int UIB_NextMsgId(int conn)
{
	if (conn < 0 || conn >= UIB_MAX_CONNS || !uib.conns[conn])
		return 0;
	return ++uib.conns[conn]->msgid;
}

void UIB_SendTo(int conn, const char *json, size_t len)
{
	if (conn < 0 || conn >= UIB_MAX_CONNS)
		return;
	touchuib_conn_t *c = uib.conns[conn];
	if (!c || c->closed)
		return;
	if (uib_debug.integer)
		Con_Printf("uib -> %s: %.*s\n", c->name, uib_debug.integer > 1 ? (int)len : (int)(len < 120 ? len : 120), json);
	if (c->local)
		return;
	if (c->print)
	{
		Con_Printf("%.*s\n", (int)(len < 4000 ? len : 4000), json);
		return;
	}
	void (*fn)(void *) = NULL;
	void *ctx = NULL;
	Thread_LockMutex(c->mutex);
	if (c->out_bytes + len > CONN_OUT_MAX_BYTES)
	{
		// the renderer stopped taking: drop everything, resync with a snapshot
		qfree(&c->out_head, &c->out_tail);
		c->out_bytes = 0;
		c->overflow = 1;
	}
	else
	{
		qmsg_t *m = qmake(json, len);
		if (m)
		{
			qpush(&c->out_head, &c->out_tail, m);
			c->out_bytes += len;
		}
	}
	fn = c->notify;
	ctx = c->notify_ctx;
	Thread_UnlockMutex(c->mutex);
#ifdef __linux__
	if (c->efd >= 0)
	{
		uint64_t one = 1;
		ssize_t r = write(c->efd, &one, sizeof(one));
		(void)r;
	}
#endif
	if (fn)
		fn(ctx);
}

void UIB_Broadcast(const char *json, size_t len)
{
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		if (uib.conns[i] && uib.conns[i]->hello)
			UIB_SendTo(i, json, len);
}

void UIB_SendError(int conn, int64_t re, const char *code, const char *msg)
{
	uibbuf_t b = {0};
	UIBB_Printf(&b, "{\"uib\":1,\"t\":\"error\",\"id\":%d", UIB_NextMsgId(conn));
	if (re > 0)
		UIBB_Printf(&b, ",\"re\":%lld", (long long)re);
	UIBB_Printf(&b, ",\"code\":\"%s\",\"msg\":", code);
	UIBB_Str(&b, msg, strlen(msg), 512, 0);
	UIBB_Putc(&b, '}');
	UIB_SendTo(conn, b.p, b.n);
	UIBB_Free(&b);
}

static void send_simple(int conn, const char *t, int64_t re)
{
	char buf[128];
	int n = dpsnprintf(buf, sizeof(buf), "{\"uib\":1,\"t\":\"%s\",\"id\":%d,\"re\":%lld}", t, UIB_NextMsgId(conn), (long long)re);
	UIB_SendTo(conn, buf, n);
}

static void send_snapshot(int conn)
{
	size_t len;
	char *s = UIBS_Snapshot(&len, UIB_NextMsgId(conn));
	UIB_SendTo(conn, s, len);
	free(s);
	uib.conns[conn]->sent_rev = UIBS_Rev();
	uib.conns[conn]->overflow = 0;
}

static void send_hello(int conn, int64_t re)
{
	uibbuf_t b = {0};
	char session[32];
	static unsigned sess;
	if (!sess)
		sess = (unsigned)(Sys_DirtyTime() * 1000.0) ^ (unsigned)(uintptr_t)&b ^ 0x9e3779b9u;
	dpsnprintf(session, sizeof(session), "%08x", sess);
	UIBB_Printf(&b, "{\"uib\":1,\"t\":\"hello\",\"id\":%d,\"re\":%lld,\"schema\":\"" TOUCHUIB_SCHEMA "\",\"engine\":", UIB_NextMsgId(conn), (long long)re);
	UIBB_Str(&b, engineversion, strlen(engineversion), 256, 1);
	UIBB_Printf(&b, ",\"session\":\"%s\",\"limits\":{\"message\":%u,\"string\":%d,\"nodes\":%d,\"depth\":%d,\"children\":%d,\"window\":%d},"
		"\"features\":[\"rows\",\"image.path\",\"image.bytes\",\"mods\"]}",
		session, UIB_MAX_MESSAGE, UIB_MAX_STRING, UIB_MAX_NODES, UIB_MAX_DEPTH, UIB_MAX_CHILDREN, UIB_MAX_WINDOW);
	UIB_SendTo(conn, b.p, b.n);
	UIBB_Free(&b);
}

// ---------------------------------------------------------------- events

static int node_has_action(uibnode_t *n, const char *action)
{
	char pat[160];
	dpsnprintf(pat, sizeof(pat), "\"id\":\"%s\"", action);
	for (int i = 0; i < n->nprops; i++)
		if (strstr(n->props[i].json, pat))
			return 1;
	return 0;
}

// Checks a value against the node's type and range; writes the canonical cvar string.
static int check_value(uibnode_t *n, uibj_t *v, char *out, size_t outsize, const char **why)
{
	const char *t = n->type;
	size_t l;
	const char *j;
	char tmp[64];
	if (!strcmp(t, "switch"))
	{
		if (v->type != UIBJ_TRUE && v->type != UIBJ_FALSE) { *why = "switch wants a bool"; return 0; }
		dpsnprintf(out, outsize, "%d", v->type == UIBJ_TRUE);
		return 1;
	}
	if (!strcmp(t, "slider") || !strcmp(t, "stepper"))
	{
		if (v->type != UIBJ_NUM) { *why = "wants a number"; return 0; }
		double x = v->num;
		if ((j = UIBS_PropJSON(n, "min", &l)) && x < atof(j)) x = atof(j);
		if ((j = UIBS_PropJSON(n, "max", &l)) && x > atof(j)) x = atof(j);
		if (x == (double)(long long)x) dpsnprintf(out, outsize, "%lld", (long long)x);
		else dpsnprintf(out, outsize, "%.6g", x);
		return 1;
	}
	if (!strcmp(t, "keybind"))
	{
		if (v->type != UIBJ_ARR) { *why = "keybind wants an array of key names"; return 0; }
		out[0] = 0;
		for (uibj_t *k = v->child; k; k = k->next)
			if (k->type == UIBJ_STR)
				dpsnprintf(out + strlen(out), outsize - strlen(out), "%s%s", out[0] ? " " : "", k->str);
		return 1;
	}
	if (v->type != UIBJ_STR) { *why = "wants a string"; return 0; }
	if (strlen(v->str) >= outsize || v->len > UIB_MAX_STRING) { *why = "too long"; return 0; }
	if (!strcmp(t, "choice") && (j = UIBS_PropJSON(n, "options", &l)))
	{
		uibbuf_t b = {0};
		UIBB_Puts(&b, "\"value\":");
		UIBB_Str(&b, v->str, v->len, 0, 0);
		int ok = strstr(j, b.p) != NULL;
		UIBB_Free(&b);
		if (!ok) { *why = "not one of the options"; return 0; }
	}
	if (!strcmp(t, "colour"))
	{
		if (v->len != 7 || v->str[0] != '#') { *why = "colour wants #rrggbb"; return 0; }
		if ((j = UIBS_PropJSON(n, "palette", &l)))
		{
			dpsnprintf(tmp, sizeof(tmp), "\"%s\"", v->str);
			if (!strstr(j, tmp)) { *why = "not in the palette"; return 0; }
		}
	}
	if ((j = UIBS_PropJSON(n, "maxLength", &l)) && v->len > (size_t)atoi(j)) { *why = "too long"; return 0; }
	if (strchr(v->str, '\n') || strchr(v->str, '\r')) { *why = "no line breaks"; return 0; }
	dpsnprintf(out, outsize, "%s", v->str);
	return 1;
}

static void mod_of(const uibnode_t *n, char *mod, size_t size)
{
	mod[0] = 0;
	if (n && n->origin && !strncmp(n->origin, "mod:", 4))
		dpsnprintf(mod, size, "%s", n->origin + 4);
}

static void handle_event(int conn, int64_t re, uibj_t *m, const char *raw, size_t rawlen)
{
	const char *ev = UIBJ_Str(m, "ev", "");
	const char *nodeid = UIBJ_Str(m, "node", "");
	uibnode_t *n = *nodeid ? UIBS_Find(nodeid) : NULL;
	char mod[80];
	mod_of(n, mod, sizeof(mod));
	if (!strcmp(ev, "back"))
	{
		UIBM_Event("back", "", "", "");
		return;
	}
	if (!strcmp(ev, "action") || !strcmp(ev, "value") || !strcmp(ev, "submit") || !strcmp(ev, "row") ||
	    !strcmp(ev, "shown") || !strcmp(ev, "hidden") || !strcmp(ev, "dismiss") || !strcmp(ev, "visible"))
	{
		if (!n)
		{
			UIB_SendError(conn, re, "not_found", "no such node");
			return;
		}
	}
	else
	{
		UIB_SendError(conn, re, "unknown", "unknown event");
		return;
	}
	if (!strcmp(ev, "action"))
	{
		const char *a = UIBJ_Str(m, "action", "");
		if (!*a || strlen(a) > 120 || !node_has_action(n, a))
		{
			UIB_SendError(conn, re, "denied", "that node has no such action");
			return;
		}
		if (*mod) { UIBMod_Event(mod, raw, rawlen); return; }
		uibbuf_t b = {0};
		uibj_t *p = UIBJ_Get(m, "payload");
		if (p && p->type == UIBJ_OBJ)
			UIBJ_Write(&b, p);
		UIBM_Event("action", nodeid, a, b.p ? b.p : "");
		UIBB_Free(&b);
		return;
	}
	if (!strcmp(ev, "value") || !strcmp(ev, "submit"))
	{
		uibj_t *v = UIBJ_Get(m, "value");
		size_t bl;
		const char *bj = UIBS_PropJSON(n, "bind", &bl);
		char val[UIB_MAX_STRING + 1];
		const char *why = "bad value";
		if (!v)
		{
			UIB_SendError(conn, re, "bad_request", "value missing");
			return;
		}
		if (!strcmp(ev, "submit") && strcmp(n->type, "field"))
		{
			UIB_SendError(conn, re, "denied", "submit is for fields");
			return;
		}
		if (!bj && strcmp(ev, "submit"))
		{
			UIB_SendError(conn, re, "denied", "that node is not bound");
			return;
		}
		if (!check_value(n, v, val, sizeof(val), &why))
		{
			UIB_SendError(conn, re, "bad_request", why);
			// push the engine's value back so an optimistic renderer corrects itself
			n->gen = UIBS_Rev() + 1;
			UIBS_SetProp(n, "error", "\"\"", 2);
			UIBS_SetProp(n, "error", NULL, 0);
			return;
		}
		char bind[160] = "";
		if (bj && bl >= 2)
			dpsnprintf(bind, sizeof(bind), "%.*s", (int)(bl - 2), bj + 1);
		if (*mod)
		{
			if (strncmp(bind, "mod:", 4))
				return;
			uibbuf_t b = {0};
			UIBJ_Write(&b, v);
			UIBS_SetProp(n, "value", b.p, b.n);
			UIBB_Free(&b);
			UIBMod_Event(mod, raw, rawlen);
			return;
		}
		if (!strncmp(bind, "cvar:", 5) && !UIBJ_Bool(n->props ? NULL : NULL, "x", 0))
		{
			cvar_t *cv = Cvar_FindVar(&cvars_all, bind + 5, ~0u);
			if (!cv || (cv->flags & (CF_READONLY | CF_PRIVATE)))
			{
				UIB_SendError(conn, re, "denied", "that setting cannot be changed here");
				return;
			}
			Cvar_SetQuick(cv, val);
			// the watch tick turns the new cvar string into the value prop; do it now
			UIBS_WatchTick();
		}
		else if (!strncmp(bind, "vm:", 3) || !strcmp(ev, "submit"))
		{
			// the menu VM keeps the value and rebuilds; reflect it now for other renderers
			int priv = UIBS_PropJSON(n, "private", NULL) != NULL;
			if (!priv && v->type != UIBJ_NULL)
			{
				uibbuf_t b = {0};
				UIBJ_Write(&b, v);
				UIBS_SetProp(n, "value", b.p, b.n);
				UIBB_Free(&b);
			}
		}
		uibbuf_t b = {0};
		UIBJ_Write(&b, v);
		UIBM_Event(ev, nodeid, val, b.p);
		UIBB_Free(&b);
		return;
	}
	if (!strcmp(ev, "row"))
	{
		const char *row = UIBJ_Str(m, "row", "");
		const char *kind = UIBJ_Str(m, "kind", "activate");
		if (strcmp(n->type, "list"))
		{
			UIB_SendError(conn, re, "denied", "row events are for lists");
			return;
		}
		if (strlen(row) > 255)
			return;
		char arg[400];
		dpsnprintf(arg, sizeof(arg), "%s", row);
		uibbuf_t b = {0};
		UIBB_Printf(&b, "{\"kind\":\"%s\"", !strcmp(kind, "select") ? "select" : !strcmp(kind, "action") ? "action" : "activate");
		const char *a = UIBJ_Str(m, "action", NULL);
		if (a)
		{
			if (!node_has_action(n, a)) { UIBB_Free(&b); UIB_SendError(conn, re, "denied", "no such row action"); return; }
			UIBB_Puts(&b, ",\"action\":");
			UIBB_Str(&b, a, strlen(a), 120, 0);
		}
		UIBB_Putc(&b, '}');
		if (*mod) UIBMod_Event(mod, raw, rawlen);
		else UIBM_Event("row", nodeid, arg, b.p);
		UIBB_Free(&b);
		return;
	}
	if (!strcmp(ev, "visible"))
	{
		char arg[64];
		dpsnprintf(arg, sizeof(arg), "%d %d", (int)UIBJ_Num(m, "offset", 0), (int)UIBJ_Num(m, "limit", 0));
		UIBM_Event("visible", nodeid, arg, "");
		return;
	}
	// shown, hidden, dismiss
	if (*mod && strcmp(ev, "dismiss"))
	{
		UIBMod_Event(mod, raw, rawlen);
		return;
	}
	if (!strcmp(ev, "dismiss") && !strcmp(n->type, "toast"))
	{
		UIBS_Remove(nodeid);
		return;
	}
	UIBM_Event(ev, nodeid, "", "");
}

void UIB_Handle(int conn, const char *json, size_t len)
{
	char err[160];
	if (uib_debug.integer)
		Con_Printf("uib <- %s: %.*s\n", conn >= 0 && uib.conns[conn] ? uib.conns[conn]->name : "console",
			uib_debug.integer > 1 ? (int)len : (int)(len < 120 ? len : 120), json);
	uibjdoc_t *d = UIBJ_Parse(json, len, err, sizeof(err));
	uibj_t *m = UIBJ_Root(d);
	if (!m || m->type != UIBJ_OBJ)
	{
		UIB_SendError(conn, 0, "bad_request", m ? "a message is a JSON object" : err);
		UIBJ_Free(d);
		return;
	}
	int64_t re = (int64_t)UIBJ_Num(m, "id", 0);
	const char *t = UIBJ_Str(m, "t", "");
	if (UIBJ_Num(m, "uib", 0) != TOUCHUIB_PROTOCOL)
	{
		UIB_SendError(conn, re, "version", "this engine speaks uib 1");
		UIBJ_Free(d);
		return;
	}
	touchuib_conn_t *c = conn >= 0 ? uib.conns[conn] : NULL;
	if (!strcmp(t, "hello"))
	{
		const char *schema = UIBJ_Str(m, "schema", "");
		if (strncmp(schema, "1.", 2))
		{
			UIB_SendError(conn, re, "version", "schema 1.x expected");
			UIBJ_Free(d);
			return;
		}
		uibj_t *caps = UIBJ_Get(m, "caps");
		uibbuf_t b = {0};
		if (caps)
			UIBJ_Write(&b, caps);
		if (c)
		{
			dpsnprintf(c->caps, sizeof(c->caps), "%s", b.p ? b.p : "{}");
			const char *rn = UIBJ_Str(UIBJ_Get(m, "renderer"), "name", NULL);
			if (rn)
				dpsnprintf(c->name, sizeof(c->name), "%s", rn);
			c->hello = 1;
		}
		uib.touched = 1;
		// let the menu VM build (or adapt) its screens before the snapshot
		UIBM_Event("attach", c ? c->name : "console", "", b.p ? b.p : "{}");
		UIBB_Free(&b);
		UIBS_Commit();
		send_hello(conn, re);
		if (c)
			send_snapshot(conn);
	}
	else if (!strcmp(t, "caps"))
	{
		uibbuf_t b = {0};
		uibj_t *caps = UIBJ_Get(m, "caps");
		if (caps)
			UIBJ_Write(&b, caps);
		if (c && b.p)
			dpsnprintf(c->caps, sizeof(c->caps), "%s", b.p);
		UIBM_Event("caps", c ? c->name : "", "", b.p ? b.p : "{}");
		UIBB_Free(&b);
	}
	else if (!c || !c->hello)
		UIB_SendError(conn, re, "bad_request", "send hello first");
	else if (!strcmp(t, "sync"))
	{
		UIBS_Commit();
		send_snapshot(conn);
	}
	else if (!strcmp(t, "ping"))
		send_simple(conn, "pong", re);
	else if (!strcmp(t, "bye"))
		TouchUIB_Close(c);
	else if (!strcmp(t, "window"))
	{
		uibbuf_t b = {0};
		const char *filter = UIBJ_Str(m, "filter", NULL);
		UIBSrc_Window(&b, UIB_NextMsgId(conn), re, UIBJ_Str(m, "source", ""), (int)UIBJ_Num(m, "offset", 0),
			(int)UIBJ_Num(m, "limit", 50), UIBJ_Str(m, "sort", NULL), UIBJ_Bool(m, "desc", 0), filter ? filter : "", filter != NULL);
		UIB_SendTo(conn, b.p, b.n);
		UIBB_Free(&b);
	}
	else if (!strcmp(t, "image"))
	{
		const char *as = UIBJ_Str(m, "as", "path");
		UIBImg_Request(conn, re, UIBJ_Str(m, "ref", ""), (int)UIBJ_Num(m, "w", 0), (int)UIBJ_Num(m, "h", 0), !strcmp(as, "bytes"));
	}
	else if (!strcmp(t, "ev"))
		handle_event(conn, re, m, json, len);
	else
		UIB_SendError(conn, re, "unknown", "unknown message type");
	UIBJ_Free(d);
}

// ---------------------------------------------------------------- frame

static void send_changes(void)
{
	int64_t rev = UIBS_Rev();
	for (int i = 0; i < UIB_MAX_CONNS; i++)
	{
		touchuib_conn_t *c = uib.conns[i];
		if (!c || c->closed || !c->hello || c->local)
			continue;
		if (c->overflow)
		{
			send_snapshot(i);
			continue;
		}
		if (c->sent_rev == rev)
			continue;
		size_t len;
		char *s = UIBS_Diff(c->sent_rev, &len, UIB_NextMsgId(i));
		if (!s)
		{
			send_snapshot(i);
			continue;
		}
		UIB_SendTo(i, s, len);
		free(s);
		c->sent_rev = rev;
	}
}

// app.reserved: what the engine draws over the UI in its own window (the
// console button, touch_shellbtn.c), in dp, so renderers lay out around it.
static void UIB_WatchReserved(void)
{
	char json[160];
	float x, y, w, h, d;
	// not as a back end (-uibridge): the engine's window is then hidden behind
	// the app's own, and nothing of the engine's lies over the app's UI
	if (cls.state != ca_dedicated && !UIBStream_BackendMode() && TouchShell_Rect(&x, &y, &w, &h, &d) && d > 0)
	{
		dpsnprintf(json, sizeof(json), "[{\"id\":\"console\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}]",
			(int)floor(x / d), (int)floor(y / d), (int)ceil(w / d), (int)ceil(h / d));
		UIBS_SetReserved(json, strlen(json));
	}
	else
		UIBS_SetReserved(NULL, 0);
}

void TouchUIB_Frame(void)
{
	if (!uib.up)
		return;
	UIBStream_Frame();
	int any = 0;
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		any |= uib.conns[i] != NULL;
	if (!any && !uib.touched)
		return;  // nothing attached and the menu VM never described anything: zero cost
	for (int i = 0; i < UIB_MAX_CONNS; i++)
	{
		touchuib_conn_t *c = uib.conns[i];
		if (!c)
			continue;
		for (;;)
		{
			Thread_LockMutex(c->mutex);
			int closed = c->closed;
			qmsg_t *m = closed ? NULL : qpop(&c->in_head, &c->in_tail);
			if (m)
			{
				c->in_count--;
				c->in_bytes -= m->len;
			}
			Thread_UnlockMutex(c->mutex);
			if (closed)
			{
				conn_free(c);
				break;
			}
			if (!m)
				break;
			UIB_Handle(i, m->data, m->len);
			free(m);
			if (!uib.conns[i])
				break;
		}
	}
	double now = Sys_DirtyTime();
	if (now >= uib.next_watch)
	{
		uib.next_watch = now + 0.1;
		UIB_WatchReserved();
		UIBS_WatchTick();
		if (uibm_tick_wanted || UIB_ConnCount())
			UIBM_Event("tick", "", "", "");
	}
	UIBSrc_Frame();
	UIBImg_Frame();
	if (UIBS_Commit())
		send_changes();
	else
		for (int i = 0; i < UIB_MAX_CONNS; i++)
			if (uib.conns[i] && uib.conns[i]->hello && uib.conns[i]->overflow)
				send_snapshot(i);
	UIBStream_Frame();
}

// The QuakeC renderer's events: the same path as a native renderer's
void UIB_LocalPost(const char *json, size_t len)
{
	static touchuib_conn_t *qc;
	if (!qc || uib.conns[qc->slot] != qc)
	{
		qc = TouchUIB_Open("qc", 2);
		if (!qc)
			return;
		qc->local = 1;
		qc->hello = 1;
		qc->sent_rev = UIBS_Rev();
	}
	uibbuf_t b = {0};
	const char *body = json;
	while (*body == ' ') body++;
	if (*body == '{') body++, len -= (size_t)(body - json);
	UIBB_Puts(&b, "{\"uib\":1,\"t\":\"ev\",\"id\":1,");
	UIBB_Put(&b, body, len);
	TouchUIB_Post(qc, b.p, b.n);
	UIBB_Free(&b);
	uib.touched = 1;
}

// The handshake caps of the most recent native renderer (for the menu VM)
const char *UIB_LastCaps(void)
{
	for (int i = UIB_MAX_CONNS - 1; i >= 0; i--)
		if (uib.conns[i] && uib.conns[i]->hello && !uib.conns[i]->local && uib.conns[i]->caps[0])
			return uib.conns[i]->caps;
	return "";
}

// called by the builtins: the menu VM is describing screens
void UIB_Touched(void) { uib.touched = 1; }

// ---------------------------------------------------------------- pull helpers

char *TouchUIB_SnapshotJSON(size_t *len)
{
	UIBS_Commit();
	return UIBS_Snapshot(len, 0);
}

char *TouchUIB_DiffJSON(int64_t since_rev, size_t *len)
{
	UIBS_Commit();
	return UIBS_Diff(since_rev, len, 0);
}

int64_t TouchUIB_Rev(void) { return UIBS_Rev(); }

// ---------------------------------------------------------------- mods

typedef struct modpage_s
{
	char mod[64], page[64];
	char *json;
	size_t len;
	char title[128];
} modpage_t;

typedef struct modval_s
{
	char mod[64], key[64];
	char *json;
} modval_t;

#define MAX_MODPAGES 64
#define MAX_MODVALS 512
static modpage_t modpages[MAX_MODPAGES];
static int nmodpages;
static modval_t modvals[MAX_MODVALS];
static int nmodvals;

static int valid_modid(const char *s)
{
	size_t n = strlen(s);
	if (!n || n > 63)
		return 0;
	for (; *s; s++)
		if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-'))
			return 0;
	return 1;
}

static const char *mod_types[] = {"section", "row", "text", "divider", "image", "card", "switch", "slider", "stepper",
	"choice", "field", "button", "colour", "keybind", "progress", "status", NULL};

typedef struct
{
	const char *mod;
	int nodes, images;
	char *err;
	size_t errlen;
} modcheck_t;

static int mod_check_value(modcheck_t *mc, const char *key, uibj_t *v, int depth)
{
	if (depth > 6)
	{
		dpsnprintf(mc->err, mc->errlen, "prop %s nested too deep", key);
		return 0;
	}
	if (v->type == UIBJ_STR)
	{
		if (v->len > UIB_MOD_STRING)
		{
			dpsnprintf(mc->err, mc->errlen, "string in %s longer than %d bytes", key, UIB_MOD_STRING);
			return 0;
		}
		if (!strcmp(key, "image") || !strcmp(key, "ref") || !strcmp(key, "background") || !strcmp(key, "preview"))
		{
			char pre[80];
			dpsnprintf(pre, sizeof(pre), "mod:%s/", mc->mod);
			if (strncmp(v->str, pre, strlen(pre)) || strstr(v->str, ".."))
			{
				dpsnprintf(mc->err, mc->errlen, "image %s outside the mod", v->str);
				return 0;
			}
			if (++mc->images > UIB_MOD_IMAGES)
			{
				dpsnprintf(mc->err, mc->errlen, "more than %d images", UIB_MOD_IMAGES);
				return 0;
			}
		}
		if (!strcmp(key, "bind"))
		{
			char pre[80];
			dpsnprintf(pre, sizeof(pre), "mod:%s.", mc->mod);
			if (strncmp(v->str, pre, strlen(pre)))
			{
				dpsnprintf(mc->err, mc->errlen, "bind %s is not the mod's own value", v->str);
				return 0;
			}
		}
		if (!strcmp(key, "kind") && strcmp(v->str, "normal") && strcmp(v->str, "link"))
		{
			dpsnprintf(mc->err, mc->errlen, "action kind %s not allowed for mods", v->str);
			return 0;
		}
		if (!strcmp(key, "url") && strncmp(v->str, "https://", 8))
		{
			dpsnprintf(mc->err, mc->errlen, "links from mods must be https");
			return 0;
		}
	}
	if (v->type == UIBJ_OBJ || v->type == UIBJ_ARR)
	{
		if (v->len > 64)
		{
			dpsnprintf(mc->err, mc->errlen, "prop %s has more than 64 members", key);
			return 0;
		}
		for (uibj_t *m = v->child; m; m = m->next)
			if (!mod_check_value(mc, m->key ? m->key : key, m, depth + 1))
				return 0;
	}
	return 1;
}

// Validate a page: bounds, types, binds, actions. 1 ok.
static int mod_validate(const char *mod, uibj_t *root, char *err, size_t errlen)
{
	modcheck_t mc = {mod, 0, 0, err, errlen};
	uibj_t *nodes = UIBJ_Get(root, "nodes");
	if (!nodes || nodes->type != UIBJ_ARR || !nodes->len)
	{
		dpsnprintf(err, errlen, "a page needs a nodes array");
		return 0;
	}
	if (nodes->len > UIB_MOD_NODES)
	{
		dpsnprintf(err, errlen, "more than %d nodes", UIB_MOD_NODES);
		return 0;
	}
	const char *title = UIBJ_Str(root, "title", "");
	if (strlen(title) > 120)
	{
		dpsnprintf(err, errlen, "title too long");
		return 0;
	}
	for (uibj_t *n = nodes->child; n; n = n->next)
	{
		const char *id = UIBJ_Str(n, "id", NULL), *type = UIBJ_Str(n, "type", NULL);
		if (!id || !type || strlen(id) > 64)
		{
			dpsnprintf(err, errlen, "every node needs a short id and a type");
			return 0;
		}
		int depth = 0;
		for (const char *s = id; *s; s++)
			depth += *s == '/';
		if (depth >= UIB_MOD_DEPTH)
		{
			dpsnprintf(err, errlen, "node %s deeper than %d", id, UIB_MOD_DEPTH);
			return 0;
		}
		int ok = 0;
		for (int i = 0; mod_types[i]; i++)
			ok |= !strcmp(type, mod_types[i]);
		if (!ok)
		{
			dpsnprintf(err, errlen, "node type %s not allowed in mod pages", type);
			return 0;
		}
		uibj_t *props = UIBJ_Get(n, "props");
		if (props && props->type == UIBJ_OBJ)
			for (uibj_t *p = props->child; p; p = p->next)
				if (!mod_check_value(&mc, p->key, p, 0))
					return 0;
	}
	return 1;
}

static uibj_t *page_node(uibj_t *nodes, const char *id)
{
	for (uibj_t *n = nodes->child; n; n = n->next)
		if (!strcmp(UIBJ_Str(n, "id", ""), id))
			return n;
	return NULL;
}

static const char *modval_get(const char *mod, const char *key)
{
	for (int i = 0; i < nmodvals; i++)
		if (!strcmp(modvals[i].mod, mod) && !strcmp(modvals[i].key, key))
			return modvals[i].json;
	return NULL;
}

static void mod_build_node(const char *mod, uibj_t *nodes, uibj_t *n, int depth)
{
	const char *id = UIBJ_Str(n, "id", "");
	const char *key = strrchr(id, '/');
	key = key ? key + 1 : id;
	if (depth > UIB_MOD_DEPTH || !UIBS_Open(key, UIBJ_Str(n, "type", "text")))
	{
		UIBS_Close();
		return;
	}
	uibj_t *props = UIBJ_Get(n, "props");
	uibbuf_t b = {0};
	if (props && props->type == UIBJ_OBJ)
		for (uibj_t *p = props->child; p; p = p->next)
		{
			if (!strcmp(p->key, "origin") || !strcmp(p->key, "value"))
				continue;
			b.n = 0;
			UIBJ_Write(&b, p);
			UIBS_Set(p->key, b.p, b.n);
		}
	// the current value: the mod's latest, else what the page said
	const char *bind = props ? UIBJ_Str(props, "bind", NULL) : NULL;
	const char *cur = NULL;
	char vkey[80];
	if (bind)
	{
		dpsnprintf(vkey, sizeof(vkey), "%s", bind + 5 + strlen(mod));  // mod:<id>.<key>
		cur = modval_get(mod, vkey);
	}
	if (cur)
		UIBS_Set("value", cur, strlen(cur));
	else if (props && UIBJ_Get(props, "value") && !UIBJ_Bool(props, "private", 0))
	{
		b.n = 0;
		UIBJ_Write(&b, UIBJ_Get(props, "value"));
		UIBS_Set("value", b.p, b.n);
	}
	UIBB_Free(&b);
	uibj_t *kids = UIBJ_Get(n, "children");
	if (kids && kids->type == UIBJ_ARR)
		for (uibj_t *k = kids->child; k; k = k->next)
		{
			uibj_t *cn = k->type == UIBJ_STR ? page_node(nodes, k->str) : NULL;
			if (cn && cn != n)
				mod_build_node(mod, nodes, cn, depth + 1);
		}
	UIBS_Close();
}

int UIBMod_Materialize(const char *screenid)
{
	// mods.page.<mod>.<page>
	for (int i = 0; i < nmodpages; i++)
	{
		char id[160];
		dpsnprintf(id, sizeof(id), "mods.page.%s.%s", modpages[i].mod, modpages[i].page);
		if (strcmp(id, screenid))
			continue;
		uibjdoc_t *d = UIBJ_Parse(modpages[i].json, modpages[i].len, NULL, 0);
		uibj_t *root = UIBJ_Root(d);
		if (!root)
		{
			UIBJ_Free(d);
			return 0;
		}
		char origin[80];
		dpsnprintf(origin, sizeof(origin), "mod:%s", modpages[i].mod);
		uib.touched = 1;
		UIBS_BuildBegin(id, "screen", origin);
		uibbuf_t b = {0};
		UIBB_Str(&b, modpages[i].title, strlen(modpages[i].title), 120, 1);
		UIBS_Set("title", b.p, b.n);
		b.n = 0;
		UIBB_Puts(&b, "\"page\"");
		UIBS_Set("kind", b.p, b.n);
		UIBB_Free(&b);
		uibj_t *nodes = UIBJ_Get(root, "nodes");
		// roots: nodes no other node lists as a child
		for (uibj_t *n = nodes->child; n; n = n->next)
		{
			const char *nid = UIBJ_Str(n, "id", "");
			int child = 0;
			for (uibj_t *o = nodes->child; o && !child; o = o->next)
			{
				uibj_t *ks = UIBJ_Get(o, "children");
				for (uibj_t *k = ks ? ks->child : NULL; k; k = k->next)
					if (k->type == UIBJ_STR && !strcmp(k->str, nid))
						child = 1;
			}
			if (!child)
				mod_build_node(modpages[i].mod, nodes, n, 1);
		}
		UIBS_BuildEnd();
		UIBJ_Free(d);
		return 1;
	}
	return 0;
}

int TouchUIB_ModSetPage(const char *mod_id, const char *page_key, const char *json, size_t len, char *err, size_t errlen)
{
	char e[200];
	if (!err || !errlen)
		err = e, errlen = sizeof(e);
	err[0] = 0;
	if (!mod_id || !page_key || !valid_modid(mod_id) || !valid_modid(page_key))
	{
		dpsnprintf(err, errlen, "bad mod id or page key");
		return -1;
	}
	if (len > UIB_MOD_BYTES)
	{
		dpsnprintf(err, errlen, "page larger than %u bytes", UIB_MOD_BYTES);
		return -2;
	}
	uibjdoc_t *d = UIBJ_Parse(json, len, err, errlen);
	uibj_t *root = UIBJ_Root(d);
	if (!root || root->type != UIBJ_OBJ)
	{
		if (root)
			dpsnprintf(err, errlen, "a page is a JSON object");
		UIBJ_Free(d);
		return -3;
	}
	if (!mod_validate(mod_id, root, err, errlen))
	{
		UIBJ_Free(d);
		Con_DPrintf("uibridge: mod %s page %s refused: %s\n", mod_id, page_key, err);
		return -4;
	}
	modpage_t *p = NULL;
	for (int i = 0; i < nmodpages; i++)
		if (!strcmp(modpages[i].mod, mod_id) && !strcmp(modpages[i].page, page_key))
			p = &modpages[i];
	if (!p)
	{
		if (nmodpages >= MAX_MODPAGES)
		{
			UIBJ_Free(d);
			dpsnprintf(err, errlen, "too many mod pages");
			return -5;
		}
		p = &modpages[nmodpages++];
		memset(p, 0, sizeof(*p));
		dpsnprintf(p->mod, sizeof(p->mod), "%s", mod_id);
		dpsnprintf(p->page, sizeof(p->page), "%s", page_key);
	}
	free(p->json);
	p->json = (char *)malloc(len + 1);
	memcpy(p->json, json, len);
	p->json[len] = 0;
	p->len = len;
	UIB_CleanText(UIBJ_Str(root, "title", page_key), strlen(UIBJ_Str(root, "title", page_key)), p->title, sizeof(p->title));
	UIBJ_Free(d);
	char id[160];
	dpsnprintf(id, sizeof(id), "mods.page.%s.%s", mod_id, page_key);
	if (UIBS_Find(id))
		UIBMod_Materialize(id);
	UIBM_Event("modpages", "", "", "");
	return 0;
}

void TouchUIB_ModRemove(const char *mod_id, const char *page_key)
{
	for (int i = 0; i < nmodpages;)
	{
		if (!strcmp(modpages[i].mod, mod_id) && (!page_key || !strcmp(modpages[i].page, page_key)))
		{
			char id[160];
			dpsnprintf(id, sizeof(id), "mods.page.%s.%s", modpages[i].mod, modpages[i].page);
			UIBS_Remove(id);
			free(modpages[i].json);
			modpages[i] = modpages[--nmodpages];
			continue;
		}
		i++;
	}
	UIBM_Event("modpages", "", "", "");
}

int TouchUIB_ModSetValue(const char *mod_id, const char *key, const char *json_value, size_t len)
{
	char err[64];
	uibjdoc_t *d = UIBJ_Parse(json_value, len, err, sizeof(err));
	if (!d || len > UIB_MOD_STRING + 2 || strlen(key) > 60)
	{
		UIBJ_Free(d);
		return -1;
	}
	UIBJ_Free(d);
	modval_t *v = NULL;
	for (int i = 0; i < nmodvals; i++)
		if (!strcmp(modvals[i].mod, mod_id) && !strcmp(modvals[i].key, key))
			v = &modvals[i];
	if (!v)
	{
		if (nmodvals >= MAX_MODVALS)
			return -2;
		v = &modvals[nmodvals++];
		memset(v, 0, sizeof(*v));
		dpsnprintf(v->mod, sizeof(v->mod), "%s", mod_id);
		dpsnprintf(v->key, sizeof(v->key), "%s", key);
	}
	free(v->json);
	v->json = (char *)malloc(len + 1);
	memcpy(v->json, json_value, len);
	v->json[len] = 0;
	// live nodes bound to it
	char bind[160];
	dpsnprintf(bind, sizeof(bind), "\"mod:%s.%s\"", mod_id, key);
	char prefix[80];
	dpsnprintf(prefix, sizeof(prefix), "mods.page.%s.", mod_id);
	for (int i = 0; i < nmodpages; i++)
	{
		char id[160];
		dpsnprintf(id, sizeof(id), "mods.page.%s.%s", modpages[i].mod, modpages[i].page);
		if (strcmp(modpages[i].mod, mod_id) || !UIBS_Find(id))
			continue;
		UIBMod_Materialize(id);
	}
	return 0;
}

void TouchUIB_ModSetEventHandler(touchuib_mod_event_fn fn, void *ctx)
{
	uib.modfn = fn;
	uib.modctx = ctx;
}

void UIBMod_Event(const char *mod_id, const char *json, size_t len)
{
	if (uib.modfn)
		uib.modfn(mod_id, json, len, uib.modctx);
	else
		Con_DPrintf("uibridge: event for mod %s with no handler\n", mod_id);
}

// for the menu VM: "mod page title\n" lines
size_t UIBMod_List(char *out, size_t size)
{
	size_t o = 0;
	out[0] = 0;
	for (int i = 0; i < nmodpages && o + 1 < size; i++)
		o += dpsnprintf(out + o, size - o, "%s %s %s\n", modpages[i].mod, modpages[i].page, modpages[i].title);
	return o;
}

// ---------------------------------------------------------------- console

static void UIB_Dump_f(cmd_state_t *cmd)
{
	size_t len;
	char *s = TouchUIB_SnapshotJSON(&len);
	if (Cmd_Argc(cmd) > 1)
	{
		const char *fn = Cmd_Argv(cmd, 1);
		if (FS_CheckNastyPath(fn, false))
		{
			Con_Printf("uib_dump: bad file name\n");
		}
		else
		{
			FS_WriteFile(fn, s, len);
			Con_Printf("uib_dump: wrote %s (%u bytes, rev %lld, %d nodes)\n", fn, (unsigned)len, (long long)UIBS_Rev(), UIBS_NodeCount());
		}
	}
	else
		Con_Printf("%s\n", s);
	free(s);
}

static int loopback_conn(void)
{
	if (uib.loopback >= 0 && uib.conns[uib.loopback])
		return uib.loopback;
	touchuib_conn_t *c = TouchUIB_Open("console", 7);
	if (!c)
		return -1;
	c->print = 1;
	c->hello = 1;
	c->sent_rev = UIBS_Rev();
	uib.loopback = c->slot;
	return c->slot;
}

static void UIB_Send_f(cmd_state_t *cmd)
{
	const char *a = Cmd_Args(cmd);
	int c = loopback_conn();
	if (c < 0)
		return;
	uib.touched = 1;
	UIB_Handle(c, a, strlen(a));
}

static void UIB_Event_f(cmd_state_t *cmd)
{
	// uib_event {"ev":"action","node":"home/hero","action":"play"}: the envelope is added
	const char *a = Cmd_Args(cmd);
	int c = loopback_conn();
	if (c < 0)
		return;
	uibbuf_t b = {0};
	const char *body = a;
	while (*body == ' ') body++;
	if (*body == '{') body++;
	UIBB_Puts(&b, "{\"uib\":1,\"t\":\"ev\",\"id\":1,");
	UIBB_Puts(&b, body);
	uib.touched = 1;
	UIB_Handle(c, b.p, b.n);
	UIBB_Free(&b);
}

static void UIB_Stats_f(cmd_state_t *cmd)
{
	size_t len = 0;
	char *s = TouchUIB_SnapshotJSON(&len);
	free(s);
	Con_Printf("uibridge: rev %lld, %d nodes, snapshot %u bytes, %d renderer(s), stream %s, native %d\n",
		(long long)UIBS_Rev(), UIBS_NodeCount(), (unsigned)len, UIB_ConnCount(), UIBStream_Active() ? "connected" : "-", TouchUIB_NativeAttached());
	for (int i = 0; i < UIB_MAX_CONNS; i++)
		if (uib.conns[i])
			Con_Printf("  [%d] %s hello=%d rev=%lld\n", i, uib.conns[i]->name, uib.conns[i]->hello, (long long)uib.conns[i]->sent_rev);
}

static void UIB_ModPage_f(cmd_state_t *cmd)
{
	// uib_modpage <mod> <page> <json>   (tests and mod authors)
	if (Cmd_Argc(cmd) < 4)
	{
		Con_Printf("usage: uib_modpage <mod> <page> <json> | uib_modpage <mod> <page> -\n");
		return;
	}
	const char *json = Cmd_Args(cmd);
	// skip the two words
	for (int w = 0; w < 2; w++)
	{
		while (*json == ' ') json++;
		while (*json && *json != ' ') json++;
	}
	while (*json == ' ') json++;
	if (!strcmp(json, "-"))
	{
		TouchUIB_ModRemove(Cmd_Argv(cmd, 1), Cmd_Argv(cmd, 2));
		return;
	}
	char err[256];
	int r = TouchUIB_ModSetPage(Cmd_Argv(cmd, 1), Cmd_Argv(cmd, 2), json, strlen(json), err, sizeof(err));
	Con_Printf("uib_modpage: %s%s\n", r ? "refused: " : "ok", r ? err : "");
}

// uib_testrows <source> <count> servers|maps [seed]: synthetic rows for stress tests
static void UIB_TestRows_f(cmd_state_t *cmd)
{
	static const char *maps[] = {"afterslime", "atelier", "boil", "bromine", "darkzone", "erbium", "finalrage", "fuse",
		"glowplant", "implosion", "opium", "runningman", "silentsiege", "solarium", "stormkeep", "trident", "warfare", "xoylent"};
	static const char *modes[] = {"dm", "tdm", "ctf", "ca", "duel", "ft", "kh", "lms"};
	if (Cmd_Argc(cmd) < 4)
	{
		Con_Printf("usage: uib_testrows <source> <count> servers|maps [seed]\n");
		return;
	}
	int n = atoi(Cmd_Argv(cmd, 2));
	int maprows = !strcmp(Cmd_Argv(cmd, 3), "maps");
	unsigned seed = (unsigned)atoi(Cmd_Argv(cmd, 4)) * 2654435761u + 1;
	UIBSrc_StaticBegin(Cmd_Argv(cmd, 1));
	for (int i = 0; i < n && i < 65536; i++)
	{
		char key[64], cells[512], img[64];
		const char *m = maps[i % (int)(sizeof(maps) / sizeof(*maps))];
		seed = seed * 1103515245u + 12345u;
		dpsnprintf(img, sizeof(img), "pk3:maps/%s", m);
		if (maprows)
		{
			dpsnprintf(key, sizeof(key), "%s_%d", m, i);
			dpsnprintf(cells, sizeof(cells), "{\"name\":\"%s\",\"title\":\"%s %d\",\"author\":\"Test author %d\"}", key, m, i, i % 37);
		}
		else
		{
			int ping = 5 + (int)((seed >> 8) % 400);
			dpsnprintf(key, sizeof(key), "203.0.%d.%d:%d", (i / 250) % 250, i % 250, 26000 + i % 5);
			dpsnprintf(cells, sizeof(cells), "{\"name\":\"Test server %05d\",\"map\":\"%s\",\"mode\":\"%s\",\"players\":%d,\"max\":16,"
				"\"bots\":0,\"ping\":%d,\"lock\":%s,\"fav\":false,\"address\":\"%s\",\"net\":\"internet\"}",
				i, m, modes[i % 8], (int)(seed >> 20) % 17, ping, (i % 9) ? "false" : "true", key);
		}
		UIBSrc_StaticRow(key, cells, img);
	}
	UIBSrc_StaticEnd();
	uib.touched = 1;
	Con_Printf("uib_testrows: %d rows in %s\n", n, Cmd_Argv(cmd, 1));
}

void TouchUIB_Init(void)
{
	Cvar_RegisterVariable(&menu_touch_uib);
	Cvar_RegisterVariable(&uib_version);
	Cvar_RegisterVariable(&uib_debug);
	Cmd_AddCommand(CF_CLIENT, "uib_dump", UIB_Dump_f, "UI bridge: write the current snapshot to the console or a file (uib_dump <file>)");
	Cmd_AddCommand(CF_CLIENT, "uib_send", UIB_Send_f, "UI bridge: deliver a renderer message (JSON) on the console loopback and print the replies");
	Cmd_AddCommand(CF_CLIENT, "uib_event", UIB_Event_f, "UI bridge: deliver an event, e.g. uib_event {\"ev\":\"action\",\"node\":\"home/hero\",\"action\":\"play\"}");
	Cmd_AddCommand(CF_CLIENT, "uib_stats", UIB_Stats_f, "UI bridge: node count, rev, snapshot size, renderers");
	Cmd_AddCommand(CF_CLIENT, "uib_testrows", UIB_TestRows_f, "UI bridge: synthetic rows for stress tests (uib_testrows <source> <count> servers|maps [seed])");
	Cmd_AddCommand(CF_CLIENT, "uib_modpage", UIB_ModPage_f, "UI bridge: register a mod page (uib_modpage <mod> <page> <json>) or remove it (-)");
	uib.mutex = Thread_CreateMutex();
	uib.loopback = -1;
	uib.up = 1;
	UIBSrc_Init();
	UIBImg_Init();
	UIBStream_Init();
}

void TouchUIB_Shutdown(void)
{
	if (!uib.up)
		return;
	static const char bye[] = "{\"uib\":1,\"t\":\"bye\",\"id\":0,\"reason\":\"engine shutting down\"}";
	UIB_Broadcast(bye, sizeof(bye) - 1);
	UIBStream_Frame();
	UIBStream_Shutdown();
	uib.up = 0;
}

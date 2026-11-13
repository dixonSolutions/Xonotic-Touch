/*
Godnotic UI bridge: the menu VM side.

Builtins #1180.. (declared in qcsrc/menu/xonotic/touch_uib_api.qh, registered by one
include line in mvm_cmds.c): the builder that writes screens into the store, the reads
the QuakeC renderer draws from, and the queue it sends events through. And
UIBM_Event, which calls the menu's UIB_Event(ev, node, arg, json) for renderer events.

The menu checks cvar uib_version before calling any of these, so a newer menu.dat on an
older engine never calls a builtin that is not there.
*/
#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "libcurl.h"
#include <setjmp.h>
#include "touch_uib_int.h"
#include "touch_uib_mvm.h"

#ifdef CONFIG_MENU
extern jmp_buf mp_abort;
#endif

int uibm_tick_wanted;

// ---------------------------------------------------------------- calling the menu

#ifdef CONFIG_MENU
static const mfunction_t *ev_func(prvm_prog_t *prog)
{
	static const void *cached_for;
	static const mfunction_t *f;
	if (cached_for != prog->functions)
	{
		cached_for = prog->functions;
		f = PRVM_ED_FindFunction(prog, "UIB_Event");
	}
	return f;
}

int UIBM_Ready(void)
{
	prvm_prog_t *prog = MVM_prog;
	return prog->loaded && ev_func(prog) != NULL;
}

float UIBM_Event(const char *ev, const char *node, const char *arg, const char *json)
{
	prvm_prog_t *prog = MVM_prog;
	if (!prog->loaded)
		return 0;
	const mfunction_t *f = ev_func(prog);
	if (!f)
		return 0;
	if (prog->depth > 0)
	{
		Con_DPrintf("uibridge: event %s while the menu VM runs; dropped\n", ev);
		return 0;
	}
	jmp_buf saved;
	memcpy(&saved, &mp_abort, sizeof(jmp_buf));
	float r = 0;
	if (!setjmp(mp_abort))
	{
		PRVM_G_INT(OFS_PARM0) = PRVM_SetTempString(prog, ev, strlen(ev));
		PRVM_G_INT(OFS_PARM1) = PRVM_SetTempString(prog, node, strlen(node));
		PRVM_G_INT(OFS_PARM2) = PRVM_SetTempString(prog, arg, strlen(arg));
		PRVM_G_INT(OFS_PARM3) = PRVM_SetTempString(prog, json ? json : "", json ? strlen(json) : 0);
		prog->ExecuteProgram(prog, f - prog->functions, "UIB_Event");
		r = PRVM_G_FLOAT(OFS_RETURN);
	}
	memcpy(&mp_abort, &saved, sizeof(jmp_buf));
	return r;
}
#else
// dedicated server: no menu VM
int UIBM_Ready(void) { return 0; }
float UIBM_Event(const char *ev, const char *node, const char *arg, const char *json) { return 0; }
#endif

// ---------------------------------------------------------------- builder builtins

static void ret_string(prvm_prog_t *prog, const char *s)
{
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, s, strlen(s));
}

// float uib_begin(string rootid, string type)
void VM_uib_begin(prvm_prog_t *prog)
{
	UIB_Touched();
	PRVM_G_FLOAT(OFS_RETURN) = UIBS_BuildBegin(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1), NULL);
}

// float uib_node(string key, string type)
void VM_uib_node(prvm_prog_t *prog)
{
	PRVM_G_FLOAT(OFS_RETURN) = UIBS_Open(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1));
}

void VM_uib_end(prvm_prog_t *prog) { UIBS_Close(); }
void VM_uib_done(prvm_prog_t *prog) { UIBS_BuildEnd(); }

static int long_text_key(const char *k)
{
	return !strcmp(k, "body");
}

// void uib_s(string key, string value): a display string (colour codes removed)
void VM_uib_s(prvm_prog_t *prog)
{
	const char *k = PRVM_G_STRING(OFS_PARM0), *v = PRVM_G_STRING(OFS_PARM1);
	uibbuf_t b = {0};
	// ids, binds, refs and enums are not display text: keep them verbatim
	int verbatim = !strcmp(k, "bind") || !strcmp(k, "image") || !strcmp(k, "ref") || !strcmp(k, "background") ||
	               !strcmp(k, "source") || !strcmp(k, "icon") || !strcmp(k, "key");
	UIBB_Str(&b, v, strlen(v), long_text_key(k) ? UIB_MAX_BODY : UIB_MAX_STRING, !verbatim);
	UIBS_Set(k, b.p, b.n);
	UIBB_Free(&b);
}

// void uib_f(string key, float value)
void VM_uib_f(prvm_prog_t *prog)
{
	uibbuf_t b = {0};
	UIBB_Num(&b, PRVM_G_FLOAT(OFS_PARM1));
	UIBS_Set(PRVM_G_STRING(OFS_PARM0), b.p, b.n);
	UIBB_Free(&b);
}

// void uib_b(string key, float value)
void VM_uib_b(prvm_prog_t *prog)
{
	int v = PRVM_G_FLOAT(OFS_PARM1) != 0;
	UIBS_Set(PRVM_G_STRING(OFS_PARM0), v ? "true" : "false", v ? 4 : 5);
}

static int checked_json(const char *s, uibbuf_t *out)
{
	char err[96];
	uibjdoc_t *d = UIBJ_Parse(s, strlen(s), err, sizeof(err));
	if (!d)
	{
		Con_Printf("uibridge: menu VM sent bad JSON (%s): %.80s\n", err, s);
		return 0;
	}
	UIBJ_Write(out, UIBJ_Root(d));  // canonical, compact
	UIBJ_Free(d);
	return 1;
}

// void uib_j(string key, string json)
void VM_uib_j(prvm_prog_t *prog)
{
	uibbuf_t b = {0};
	if (checked_json(PRVM_G_STRING(OFS_PARM1), &b))
		UIBS_Set(PRVM_G_STRING(OFS_PARM0), b.p, b.n);
	UIBB_Free(&b);
}

// void uib_push(string key, string json)
void VM_uib_push(prvm_prog_t *prog)
{
	uibbuf_t b = {0};
	if (checked_json(PRVM_G_STRING(OFS_PARM1), &b))
		UIBS_Push(PRVM_G_STRING(OFS_PARM0), b.p, b.n);
	UIBB_Free(&b);
}

// string uib_q(string s): a JSON string literal of display text
void VM_uib_q(prvm_prog_t *prog)
{
	const char *s = PRVM_G_STRING(OFS_PARM0);
	uibbuf_t b = {0};
	UIBB_Str(&b, s, strlen(s), 2048, 1);
	ret_string(prog, b.p);
	UIBB_Free(&b);
}

// string uib_qv(string s): a JSON string literal, verbatim (ids, refs, urls)
void VM_uib_qv(prvm_prog_t *prog)
{
	const char *s = PRVM_G_STRING(OFS_PARM0);
	uibbuf_t b = {0};
	UIBB_Str(&b, s, strlen(s), 2048, 0);
	ret_string(prog, b.p);
	UIBB_Free(&b);
}

// void uib_nav(string stack, string modal, float canback, string context)
void VM_uib_nav(prvm_prog_t *prog)
{
	UIB_Touched();
	UIBS_SetNav(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1), PRVM_G_FLOAT(OFS_PARM2) != 0, PRVM_G_STRING(OFS_PARM3));
}

// void uib_busy(string json): "" when not busy
void VM_uib_busy(prvm_prog_t *prog)
{
	const char *s = PRVM_G_STRING(OFS_PARM0);
	uibbuf_t b = {0};
	if (*s && checked_json(s, &b))
		UIBS_SetBusy(b.p, b.n);
	else
		UIBS_SetBusy(NULL, 0);
	UIBB_Free(&b);
}

// void uib_toast(string text, string kind, string icon, float timeout)
void VM_uib_toast(prvm_prog_t *prog)
{
	UIBS_Toast(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1), PRVM_G_STRING(OFS_PARM2), PRVM_G_FLOAT(OFS_PARM3), NULL);
}

void VM_uib_remove(prvm_prog_t *prog) { UIBS_Remove(PRVM_G_STRING(OFS_PARM0)); }
void VM_uib_rows_begin(prvm_prog_t *prog) { UIBSrc_StaticBegin(PRVM_G_STRING(OFS_PARM0)); }

void VM_uib_row(prvm_prog_t *prog)
{
	UIBSrc_StaticRow(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1), PRVM_G_STRING(OFS_PARM2));
}

void VM_uib_rows_end(prvm_prog_t *prog) { UIBSrc_StaticEnd(); }

void VM_uib_param(prvm_prog_t *prog)
{
	UIBSrc_Param(PRVM_G_STRING(OFS_PARM0), PRVM_G_STRING(OFS_PARM1), PRVM_G_STRING(OFS_PARM2));
}

// float uib_native(): 1 while a native renderer shows the descriptions (the menu then does not draw)
void VM_uib_native(prvm_prog_t *prog)
{
	PRVM_G_FLOAT(OFS_RETURN) = TouchUIB_NativeAttached() ? 1 : UIBStream_BackendMode() ? 2 : 0;
}

void VM_uib_reset(prvm_prog_t *prog) { UIBS_Reset(); }
void VM_uib_rev(prvm_prog_t *prog) { PRVM_G_FLOAT(OFS_RETURN) = (float)UIBS_Rev(); }

// ---------------------------------------------------------------- reads for the QuakeC renderer

// float uib_count(string id): children, -1 if no such node
void VM_uib_count(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	PRVM_G_FLOAT(OFS_RETURN) = n ? n->nkids : -1;
}

void VM_uib_child(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	int i = (int)PRVM_G_FLOAT(OFS_PARM1);
	ret_string(prog, n && i >= 0 && i < n->nkids ? n->kids[i]->id : "");
}

void VM_uib_type(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	ret_string(prog, n ? n->type : "");
}

// string uib_get(string id, string path): "title", "primary.label", "actions.1.id", "options.#"
void VM_uib_get(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	const char *path = PRVM_G_STRING(OFS_PARM1);
	char out[VM_TEMPSTRING_MAXSIZE];
	out[0] = 0;
	if (n)
	{
		char key[64];
		const char *dot = strchr(path, '.');
		size_t kl = dot ? (size_t)(dot - path) : strlen(path);
		if (kl < sizeof(key))
		{
			memcpy(key, path, kl);
			key[kl] = 0;
			size_t l;
			const char *j = UIBS_PropJSON(n, key, &l);
			if (j)
			{
				if (!dot && j[0] == '"')
				{
					// the common case without a parse
					uibjdoc_t *d = UIBJ_Parse(j, l, NULL, 0);
					if (d)
						dp_strlcpy(out, UIBJ_Root(d)->str, sizeof(out));
					UIBJ_Free(d);
				}
				else if (!dot && (j[0] == 't' || j[0] == 'f'))
					dp_strlcpy(out, j[0] == 't' ? "1" : "0", sizeof(out));
				else
					UIBJ_PathText(j, l, dot ? dot + 1 : "", out, sizeof(out));
			}
		}
	}
	ret_string(prog, out);
}

void VM_uib_has(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	const char *path = PRVM_G_STRING(OFS_PARM1);
	int found = 0;
	if (n)
	{
		char key[64], out[16];
		const char *dot = strchr(path, '.');
		size_t kl = dot ? (size_t)(dot - path) : strlen(path);
		if (kl < sizeof(key))
		{
			memcpy(key, path, kl);
			key[kl] = 0;
			size_t l;
			const char *j = UIBS_PropJSON(n, key, &l);
			found = j && (!dot || UIBJ_PathText(j, l, dot + 1, out, sizeof(out)));
		}
	}
	PRVM_G_FLOAT(OFS_RETURN) = found;
}

// float uib_window(string source, float offset, float limit, string sort, float desc, string filter)
void VM_uib_window(prvm_prog_t *prog)
{
	PRVM_G_FLOAT(OFS_RETURN) = UIBSrc_LocalWindow(PRVM_G_STRING(OFS_PARM0), (int)PRVM_G_FLOAT(OFS_PARM1), (int)PRVM_G_FLOAT(OFS_PARM2),
		PRVM_G_STRING(OFS_PARM3), PRVM_G_FLOAT(OFS_PARM4) != 0, PRVM_G_STRING(OFS_PARM5));
}

void VM_uib_wtotal(prvm_prog_t *prog) { PRVM_G_FLOAT(OFS_RETURN) = UIBSrc_LocalTotal(); }
void VM_uib_wkey(prvm_prog_t *prog) { ret_string(prog, UIBSrc_LocalKey((int)PRVM_G_FLOAT(OFS_PARM0))); }
void VM_uib_wimage(prvm_prog_t *prog) { ret_string(prog, UIBSrc_LocalImage((int)PRVM_G_FLOAT(OFS_PARM0))); }

void VM_uib_wcell(prvm_prog_t *prog)
{
	ret_string(prog, UIBSrc_LocalCell((int)PRVM_G_FLOAT(OFS_PARM0), PRVM_G_STRING(OFS_PARM1)));
}

// string uib_imgpath(string ref): a path drawpic can load ("" if the ref is unknown)
void VM_uib_imgpath(prvm_prog_t *prog)
{
	char path[256];
	UIBImg_Resolve(PRVM_G_STRING(OFS_PARM0), path, sizeof(path));
	ret_string(prog, path);
}

void VM_uib_srcrev(prvm_prog_t *prog) { PRVM_G_FLOAT(OFS_RETURN) = (float)UIBSrc_Rev(PRVM_G_STRING(OFS_PARM0)); }

void VM_uib_modpages(prvm_prog_t *prog)
{
	char out[VM_TEMPSTRING_MAXSIZE];
	UIBMod_List(out, sizeof(out));
	ret_string(prog, out);
}

// string uib_caps(string path): from the native renderer's handshake ("widthClass", "insets.left")
void VM_uib_caps(prvm_prog_t *prog)
{
	const char *caps = UIB_LastCaps();
	char out[256];
	out[0] = 0;
	if (caps && *caps)
		UIBJ_PathText(caps, strlen(caps), PRVM_G_STRING(OFS_PARM0), out, sizeof(out));
	ret_string(prog, out);
}

void VM_uib_tick(prvm_prog_t *prog) { uibm_tick_wanted = PRVM_G_FLOAT(OFS_PARM0) != 0; }

void VM_uib_nodegen(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	PRVM_G_FLOAT(OFS_RETURN) = n ? (float)n->gen : -1;
}

void VM_uib_origin(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	ret_string(prog, n && n->origin ? n->origin : "");
}

// void uib_event(string json): the QuakeC renderer's events take the same path as a
// native renderer's (validated, cvars applied), handled at the start of the next frame
void VM_uib_event(prvm_prog_t *prog)
{
	const char *s = PRVM_G_STRING(OFS_PARM0);
	UIB_LocalPost(s, strlen(s));
}

void VM_uib_parent(prvm_prog_t *prog)
{
	uibnode_t *n = UIBS_Find(PRVM_G_STRING(OFS_PARM0));
	ret_string(prog, n && n->parent ? n->parent->id : "");
}


// string uib_loadinfo(): "<state> <signon> <progress> <speed> \"<file>\" <downloads>"
// state: menu, connecting, downloading, loading, game
void VM_uib_loadinfo(prvm_prog_t *prog)
{
	const char *state = "menu";
	double progress = 0, speed = 0;
	char file[MAX_QPATH] = "";
	int nd = 0;
#ifdef CONFIG_MENU
	Curl_downloadinfo_t *dl = Curl_GetDownloadInfo(&nd, NULL, NULL, 0);
	if (dl)
	{
		for (int i = 0; i < nd; i++)
			if (!dl[i].queued)
			{
				progress = dl[i].progress;
				speed = dl[i].speed;
				dp_strlcpy(file, dl[i].filename, sizeof(file));
				break;
			}
		Z_Free(dl);
	}
	if (cls.state == ca_connected)
	{
		if (cls.signon < SIGNONS)
			state = nd ? "downloading" : "loading";
		else
			state = "game";
		if (cls.qw_downloadname[0])
		{
			state = "downloading";
			progress = cls.qw_downloadpercent / 100.0;
			speed = cls.qw_downloadspeedrate;
			dp_strlcpy(file, cls.qw_downloadname, sizeof(file));
		}
	}
	else if (cls.connect_remainingtries > 0)
		state = "connecting";
	else if (nd)
		state = "downloading";
#endif
	char out[MAX_QPATH + 128];
	for (char *q = file; *q; q++)
		if (*q == '"' || *q == '\\')
			*q = '_';
	dpsnprintf(out, sizeof(out), "%s %d %.3f %.0f \"%s\" %d", state,
#ifdef CONFIG_MENU
		cls.signon,
#else
		0,
#endif
		progress, speed, file, nd);
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, out, strlen(out));
}

// string uib_thumb(string ref, float w, float h): a cached thumbnail's path for drawpic
int UIBImg_Thumb(const char *ref, int w, int h, char *out, size_t outsize);
void VM_uib_thumb(prvm_prog_t *prog)
{
	char out[160];
	UIBImg_Thumb(PRVM_G_STRING(OFS_PARM0), (int)PRVM_G_FLOAT(OFS_PARM1), (int)PRVM_G_FLOAT(OFS_PARM2), out, sizeof(out));
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, out, strlen(out));
}

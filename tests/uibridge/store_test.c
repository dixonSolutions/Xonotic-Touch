/*
Godnotic UI bridge: unit and property tests of the description store and the JSON code,
without the engine (engine/darkplaces/touch_uib_store.c, touch_uib_json.c).

  cc -O1 -g -fsanitize=address,undefined -Iengine/darkplaces tests/uibridge/store_test.c \
     engine/darkplaces/touch_uib_store.c engine/darkplaces/touch_uib_json.c -lm -o store_test && ./store_test

Property: after any sequence of random rebuilds, applying every diff in order to the
first snapshot gives exactly the latest snapshot (docs/UI_BRIDGE.md 3.4).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "touch_uib_int.h"

// ---- stubs for the engine side
static char cvar_val[64] = "1";
void *UIB_CvarFind(const char *name) { return strcmp(name, "test_cvar") ? NULL : (void *)cvar_val; }
const char *UIB_CvarString(void *cv) { return (const char *)cv; }
static int events;
float UIBM_Event(const char *ev, const char *node, const char *arg, const char *json) { (void)ev; (void)node; (void)arg; (void)json; events++; return 0; }
static double fake_time;
double UIB_Time(void) { return fake_time; }
int UIBMod_Materialize(const char *screenid) { (void)screenid; return 0; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

// ---- a client: id -> canonical node JSON
#define CMAX 20000
static char *cid[CMAX], *cjs[CMAX];
static int cn;
static long long crev = -1;

static void cclear(void)
{
	for (int i = 0; i < cn; i++) { free(cid[i]); free(cjs[i]); }
	cn = 0;
}

static int cfind(const char *id)
{
	for (int i = 0; i < cn; i++)
		if (!strcmp(cid[i], id))
			return i;
	return -1;
}

static void cset(uibj_t *node)
{
	const char *id = UIBJ_Str(node, "id", "");
	uibbuf_t b = {0};
	UIBJ_Write(&b, node);
	int k = cfind(id);
	if (k < 0)
	{
		k = cn++;
		cid[k] = strdup(id);
	}
	else
		free(cjs[k]);
	cjs[k] = UIBB_Detach(&b, NULL);
}

static void cdel(const char *id)
{
	int k = cfind(id);
	if (k < 0)
		return;
	free(cid[k]);
	free(cjs[k]);
	cid[k] = cid[cn - 1];
	cjs[k] = cjs[cn - 1];
	cn--;
}

static void capply(const char *msg)
{
	char err[128];
	uibjdoc_t *d = UIBJ_Parse(msg, strlen(msg), err, sizeof(err));
	CHECK(d != NULL, "engine message does not parse: %s", err);
	if (!d) return;
	uibj_t *m = UIBJ_Root(d);
	const char *t = UIBJ_Str(m, "t", "");
	if (!strcmp(t, "snapshot"))
	{
		cclear();
		for (uibj_t *n = UIBJ_Get(m, "nodes")->child; n; n = n->next)
			cset(n);
		crev = (long long)UIBJ_Num(m, "rev", 0);
	}
	else if (!strcmp(t, "diff"))
	{
		CHECK((long long)UIBJ_Num(m, "base", -2) == crev, "diff base %lld != held %lld", (long long)UIBJ_Num(m, "base", -2), crev);
		for (uibj_t *x = UIBJ_Get(m, "del")->child; x; x = x->next)
			cdel(x->str);
		for (uibj_t *n = UIBJ_Get(m, "set")->child; n; n = n->next)
			cset(n);
		crev = (long long)UIBJ_Num(m, "rev", 0);
	}
	UIBJ_Free(d);
}

// the client's store, restricted to nodes reachable from app, equals the snapshot
static void ccompare(int round)
{
	size_t len;
	char *snap = UIBS_Snapshot(&len, 0);
	uibjdoc_t *d = UIBJ_Parse(snap, len, NULL, 0);
	uibj_t *nodes = UIBJ_Get(UIBJ_Root(d), "nodes");
	int sn = 0;
	for (uibj_t *n = nodes->child; n; n = n->next, sn++)
	{
		const char *id = UIBJ_Str(n, "id", "");
		int k = cfind(id);
		uibbuf_t b = {0};
		UIBJ_Write(&b, n);
		CHECK(k >= 0, "round %d: client lacks %s", round, id);
		if (k >= 0)
			CHECK(!strcmp(cjs[k], b.p), "round %d: node %s differs\n  client %s\n  engine %s", round, id, cjs[k], b.p);
		UIBB_Free(&b);
	}
	// the client may hold unreachable leftovers only if the engine never told it to drop them
	int reachable = 0;
	for (int i = 0; i < cn; i++)
	{
		int found = 0;
		for (uibj_t *n = nodes->child; n && !found; n = n->next)
			found = !strcmp(UIBJ_Str(n, "id", ""), cid[i]);
		reachable += found;
		CHECK(found, "round %d: client still holds %s", round, cid[i]);
	}
	CHECK(reachable == sn, "round %d: %d vs %d nodes", round, reachable, sn);
	UIBJ_Free(d);
	free(snap);
}

static unsigned rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void random_subtree(int depth, int budget)
{
	int n = rnd(5);
	for (int i = 0; i < n && budget > 0; i++, budget--)
	{
		char key[16];
		static const char *types[] = {"section", "card", "text", "switch", "row", "list", "slider"};
		const char *type = types[rnd(7)];
		snprintf(key, sizeof(key), "k%u", rnd(8));
		if (!UIBS_Open(key, type))
		{
			UIBS_Close();  // refused (duplicate key): balance it
			continue;
		}
		char v[64];
		snprintf(v, sizeof(v), "\"title %u\"", rnd(4));
		UIBS_Set("title", v, strlen(v));
		if (!strcmp(type, "switch"))
			UIBS_Set("bind", "\"cvar:test_cvar\"", 16);
		if (rnd(3) == 0)
		{
			snprintf(v, sizeof(v), "{\"id\":\"a%u\",\"label\":\"x\"}", rnd(3));
			UIBS_Push("actions", v, strlen(v));
		}
		if (depth < 5)
			random_subtree(depth + 1, budget / 2);
		UIBS_Close();
	}
}

static void build_screen(const char *id)
{
	UIBS_BuildBegin(id, "screen", NULL);
	UIBS_Set("title", "\"T\"", 3);
	random_subtree(0, 60);
	UIBS_BuildEnd();
}

static void test_property(void)
{
	static const char *screens[] = {"home", "sp", "mp", "settings", "credits"};
	char stack[256] = "home";
	UIBS_Reset();
	build_screen("home");
	UIBS_SetNav(stack, "", 0, "menu");
	UIBS_Commit();
	size_t len;
	char *s = UIBS_Snapshot(&len, 1);
	capply(s);
	free(s);
	long long held = UIBS_Rev();
	for (int round = 0; round < 3000; round++)
	{
		int what = rnd(10);
		if (what < 6)
		{
			// rebuild a screen in the stack
			int n = 0;
			char copy[256], *ids[8];
			snprintf(copy, sizeof(copy), "%s", stack);
			for (char *t = strtok(copy, " "); t && n < 8; t = strtok(NULL, " "))
				ids[n++] = t;
			build_screen(ids[rnd(n)]);
		}
		else if (what < 8)
		{
			// push or pop
			int n = 1;
			for (char *p = stack; *p; p++) n += *p == ' ';
			if (n > 1 && rnd(2))
				*strrchr(stack, ' ') = 0;
			else if (n < 5)
			{
				const char *sc = screens[1 + rnd(4)];
				if (!strstr(stack, sc))
				{
					build_screen(sc);
					strcat(stack, " ");
					strcat(stack, sc);
				}
			}
			UIBS_SetNav(stack, "", 1, "menu");
		}
		else if (what < 9)
		{
			// the bound cvar changes from elsewhere
			snprintf(cvar_val, sizeof(cvar_val), "%u", rnd(2));
			UIBS_WatchTick();
		}
		else
		{
			fake_time += 1;
			if (rnd(3) == 0)
				UIBS_Toast("hello ^1red^7 \xee\x80\x81", "info", "check", 1, NULL);
		}
		if (UIBS_Commit())
		{
			char *dj = UIBS_Diff(held, &len, 2);
			CHECK(dj != NULL, "no diff from %lld", held);
			if (dj)
			{
				capply(dj);
				free(dj);
			}
			held = UIBS_Rev();
		}
		if (round % 50 == 0)
			ccompare(round);
	}
	ccompare(-1);
	printf("property: 3000 rounds, rev %lld, %d nodes, %d change events\n", held, UIBS_NodeCount(), events);
}

static void test_noop_rebuild(void)
{
	UIBS_Reset();
	build_screen("home");
	UIBS_SetNav("home", "", 0, "menu");
	UIBS_Commit();
	long long r = UIBS_Rev();
	rng = 777;
	unsigned save = rng;
	build_screen("home");
	UIBS_Commit();
	r = UIBS_Rev();
	rng = save;
	build_screen("home");   // the same nodes again
	CHECK(!UIBS_Commit(), "an identical rebuild must not change the rev");
	CHECK(UIBS_Rev() == r, "rev moved");
	CHECK(!UIBS_Commit(), "an idle commit must not change the rev");
}

static void test_json(void)
{
	char err[128];
	const char *good[] = {"{}", "[]", "{\"a\":[1,2.5,-3e2,true,false,null,\"x\\u00e9\\n\\\"\"]}", "\"\\ud83d\\ude00\"", "  {\"k\" : { } }  "};
	const char *bad[] = {"", "{", "{\"a\":}", "[1,]", "{'a':1}", "\"\\x\"", "tru", "01x", "{\"a\":1}x", "\"\x01\""};
	for (size_t i = 0; i < sizeof(good) / sizeof(*good); i++)
	{
		uibjdoc_t *d = UIBJ_Parse(good[i], strlen(good[i]), err, sizeof(err));
		CHECK(d != NULL, "good JSON refused: %s (%s)", good[i], err);
		UIBJ_Free(d);
	}
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
	{
		uibjdoc_t *d = UIBJ_Parse(bad[i], strlen(bad[i]), err, sizeof(err));
		CHECK(d == NULL, "bad JSON accepted: %s", bad[i]);
		UIBJ_Free(d);
	}
	// deep nesting is refused, not a stack overflow
	char deep[400];
	memset(deep, '[', 200);
	memset(deep + 200, ']', 200);
	uibjdoc_t *d = UIBJ_Parse(deep, 400, err, sizeof(err));
	CHECK(d == NULL, "200-deep JSON accepted");
	UIBJ_Free(d);
	// random bytes never crash
	char buf[256];
	for (int i = 0; i < 20000; i++)
	{
		int n = rnd(sizeof(buf));
		static const char alphabet[] = "{}[]\",:01aeltrunfs\\ \x80\xff";
		for (int k = 0; k < n; k++) buf[k] = alphabet[rnd(sizeof(alphabet) - 1)];
		UIBJ_Free(UIBJ_Parse(buf, n, NULL, 0));
	}
	// escaping and display cleaning
	uibbuf_t b = {0};
	UIBB_Str(&b, "a\"b\\c\nd\x01^1red^x1f0X^^", 30, 0, 1);
	CHECK(!strcmp(b.p, "\"a\\\"b\\\\c\\nd" "redX^\""), "escape/clean gave %s", b.p);
	UIBB_Free(&b);
	UIBB_Str(&b, "\xc3\xa9\xc3\xa9\xc3\xa9", 6, 3, 0);   // truncation at a UTF-8 boundary
	CHECK(!strcmp(b.p, "\"\xc3\xa9\""), "UTF-8 truncation gave %s", b.p);
	UIBB_Free(&b);
	UIBB_Str(&b, "\xff\xfe", 2, 0, 0);   // invalid bytes become U+FFFD
	CHECK(!strcmp(b.p, "\"\xef\xbf\xbd\xef\xbf\xbd\""), "invalid UTF-8 gave %s", b.p);
	UIBB_Free(&b);
	char out[64];
	CHECK(UIBJ_PathText("{\"a\":[{\"b\":\"x\"},{\"b\":7}]}", 25, "a.1.b", out, sizeof(out)) && !strcmp(out, "7"), "path a.1.b = %s", out);
	CHECK(UIBJ_PathText("{\"a\":[1,2,3]}", 13, "a.#", out, sizeof(out)) && !strcmp(out, "3"), "path a.# = %s", out);
	printf("json: ok\n");
}

static void test_bounds(void)
{
	UIBS_Reset();
	UIBS_BuildBegin("big", "screen", NULL);
	int refused = 0;
	for (int i = 0; i < 2000; i++)
	{
		char key[16];
		snprintf(key, sizeof(key), "n%d", i);
		if (!UIBS_Open(key, "text")) refused++;
		UIBS_Close();
	}
	UIBS_BuildEnd();
	CHECK(refused == 2000 - UIB_MAX_CHILDREN, "children bound: %d refused", refused);
	// depth bound
	UIBS_BuildBegin("deep", "screen", NULL);
	int opened = 0;
	for (int i = 0; i < 60; i++) opened += UIBS_Open("d", "section");
	for (int i = 0; i < 60; i++) UIBS_Close();
	UIBS_BuildEnd();
	CHECK(opened < UIB_MAX_DEPTH, "depth bound: %d levels opened", opened);
	// bad keys
	UIBS_BuildBegin("keys", "screen", NULL);
	CHECK(!UIBS_Open("a/b", "text"), "slash in key accepted");
	UIBS_Close();
	CHECK(!UIBS_Open("", "text"), "empty key accepted");
	UIBS_Close();
	UIBS_BuildEnd();
	// private values never leave the store
	UIBS_BuildBegin("pw", "screen", NULL);
	UIBS_Open("f", "field");
	UIBS_Set("private", "true", 4);
	UIBS_Set("value", "\"secret\"", 8);
	UIBS_Close();
	UIBS_BuildEnd();
	UIBS_SetNav("pw", "", 0, "menu");
	UIBS_Commit();
	size_t len;
	char *s = UIBS_Snapshot(&len, 0);
	CHECK(!strstr(s, "secret"), "a private value reached the snapshot");
	free(s);
	printf("bounds: ok\n");
}

static void bench(void)
{
	UIBS_Reset();
	clock_t t0 = clock();
	for (int i = 0; i < 200; i++)
	{
		UIBS_BuildBegin("settings", "screen", NULL);
		for (int s = 0; s < 12; s++)
		{
			char k[16];
			snprintf(k, sizeof(k), "s%d", s);
			UIBS_Open(k, "section");
			for (int c = 0; c < 20; c++)
			{
				char kk[16], v[32];
				snprintf(kk, sizeof(kk), "c%d", c);
				UIBS_Open(kk, "slider");
				snprintf(v, sizeof(v), "%d", (c == 3 && s == 2) ? i : 1);
				UIBS_Set("value", v, strlen(v));
				UIBS_Set("label", "\"A setting\"", 11);
				UIBS_Close();
			}
			UIBS_Close();
		}
		UIBS_BuildEnd();
		UIBS_SetNav("settings", "", 0, "menu");
		UIBS_Commit();
	}
	double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 200;
	size_t sl, dl;
	char *s = UIBS_Snapshot(&sl, 0);
	char *d = UIBS_Diff(UIBS_Rev() - 1, &dl, 0);
	printf("bench: rebuild of a 253-node screen with one change %.3f ms; snapshot %zu bytes, diff %zu bytes\n", ms, sl, dl);
	free(s);
	free(d);
}

int main(void)
{
	test_json();
	test_noop_rebuild();
	test_bounds();
	test_property();
	bench();
	if (failures)
	{
		printf("%d FAILURES\n", failures);
		return 1;
	}
	printf("store_test: all passed\n");
	return 0;
}

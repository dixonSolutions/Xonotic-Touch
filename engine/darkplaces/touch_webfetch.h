/*
Godnotic: downloads into memory on the web build, through the browser's fetch().

The web build has no libcurl, so Lua mod installs from a link and xt.http had
nothing to download with. These calls stand in for libcurl's ToMemory ones,
with the same callback and status codes (libcurl.h), so touch_lua_pkg.c and
touch_lua_api.c keep one code path:

  TouchWeb_Begin_ToMemory_POST  starts a GET (post NULL) or a POST
  TouchWeb_Cancel_ToMemory      drops a transfer; its callback then says ABORTED
  TouchWeb_Frame                hands finished transfers to their callbacks;
                                call it once per frame (it is cheap when idle)

Rules, the same as for libcurl transfers:
- https:// only, redirects included; plain http:// only to 127.0.0.1 or
  localhost and only when the caller allows it (tests), and never as the
  target of a redirect.
- no cookies or credentials, no referrer: a mod's request carries nothing of
  the player's.
- a body over bufsize stops the transfer: FAILED, with length bufsize + 1.

What the browser adds: a host has to allow requests from other sites (CORS).
Many do (raw.githubusercontent.com, most APIs meant for the web); GitHub's
release download links do not. Those fail as a network error, and the caller
says so.

One file defines the implementation: #define TOUCH_WEBFETCH_IMPL before
including this (touch_lua_pkg.c). Off the web every call fails, so callers
can stay free of #ifdefs where they want.
*/

#ifndef TOUCH_WEBFETCH_H
#define TOUCH_WEBFETCH_H

// needs libcurl.h first (curl_callback_t, CURLCBSTATUS_*); it has no include guard

qbool TouchWeb_Available(void);
qbool TouchWeb_Begin_ToMemory_POST(const char *url, const char *post_content_type, const unsigned char *post, size_t postsize, unsigned char *buf, size_t bufsize, qbool allow_loopback_http, curl_callback_t callback, void *cbdata);
void TouchWeb_Cancel_ToMemory(curl_callback_t callback, void *cbdata);
void TouchWeb_Frame(void);
/// The browser's reason for the last failure ("" when there is none): a CORS
/// refusal and a dead network look the same from here, so this is a hint.
const char *TouchWeb_LastError(void);

#ifdef TOUCH_WEBFETCH_IMPL

#define TOUCHWEB_MAX 40
#define TOUCHWEB_RUNNING (-100) // touchweb_js_take: not finished yet

typedef struct touchweb_s
{
	qbool used, cancelled;
	int id;
	unsigned char *buf;
	size_t bufsize;
	curl_callback_t callback;
	void *cbdata;
} touchweb_t;
static touchweb_t touchweb[TOUCHWEB_MAX];
static char touchweb_error[256];

const char *TouchWeb_LastError(void)
{
	return touchweb_error;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Module.xtFetch[id] = { done, status, bytes, error, ctrl }
EM_JS(int, touchweb_js_start, (const char *url, const char *type, const unsigned char *post, int postsize, int max, int loopback), {
	var M = Module;
	M.xtFetch = M.xtFetch || {};
	M.xtFetchNext = (M.xtFetchNext || 0) + 1;
	var id = M.xtFetchNext;
	var u = UTF8ToString(url);
	var slot = { done: false, status: -1, bytes: null, error: "", ctrl: null };
	M.xtFetch[id] = slot;
	var isHttps = function (x) { return x.slice(0, 8) === "https:" + "/" + "/"; };
	var local = false;
	try { var pu = new URL(u); local = pu.protocol === "http:" && (pu.hostname === "127.0.0.1" || pu.hostname === "localhost") && !pu.username; } catch (e) { local = false; }
	if (!isHttps(u) && !(loopback && local)) {
		slot.done = true; slot.error = "only https:// links"; return id;
	}
	var init = { method: "GET", credentials: "omit", cache: "no-store", redirect: "follow", referrerPolicy: "no-referrer", mode: "cors" };
	if (post) {
		init.method = "POST";
		init.body = HEAPU8.slice(post, post + postsize);
		init.headers = { "Content-Type": type ? UTF8ToString(type) : "application/octet-stream" };
	}
	try { slot.ctrl = new AbortController(); init.signal = slot.ctrl.signal; } catch (e) { slot.ctrl = null; }
	fetch(u, init).then(function (res) {
		// Followed redirects must stay on https, as libcurl's do.
		if (res.redirected && !isHttps(res.url)) {
			slot.status = -1; slot.error = "a redirect away from https"; slot.done = true;
			try { if (slot.ctrl) slot.ctrl.abort(); } catch (e) {}
			return;
		}
		var code = res.ok ? 0 : (res.status || -3);
		var len = +(res.headers.get("Content-Length") || 0);
		if (len > max) {
			slot.status = -1; slot.bytes = new Uint8Array(0); slot.over = true; slot.done = true;
			try { if (slot.ctrl) slot.ctrl.abort(); } catch (e) {}
			return;
		}
		var parts = [], got = 0;
		var reader = res.body && res.body.getReader ? res.body.getReader() : null;
		function finish() {
			var all = new Uint8Array(Math.min(got, max)), at = 0;
			for (var i = 0; i < parts.length && at < all.length; i++) {
				var p = parts[i].subarray(0, all.length - at);
				all.set(p, at); at += p.length;
			}
			slot.bytes = all; slot.status = code; slot.done = true;
		}
		if (!reader) {
			return res.arrayBuffer().then(function (ab) { parts.push(new Uint8Array(ab)); got = ab.byteLength;
				if (got > max) { slot.status = -1; slot.over = true; slot.bytes = new Uint8Array(0); slot.done = true; return; }
				finish(); });
		}
		function pump() {
			return reader.read().then(function (r) {
				if (slot.done) return;
				if (r.done) { finish(); return; }
				got += r.value.length;
				if (got > max) {
					slot.status = -1; slot.over = true; slot.bytes = new Uint8Array(0); slot.done = true;
					try { reader.cancel(); } catch (e) {}
					return;
				}
				parts.push(r.value);
				return pump();
			});
		}
		return pump();
	}).catch(function (e) {
		if (slot.done) return;
		slot.status = slot.cancelled ? -2 : -1;
		slot.error = (e && e.name === "TypeError")
			? "the site refused a request from a web page (CORS), or the network failed"
			: String(e && e.message || e);
		slot.done = true;
	});
	return id;
});

// TOUCHWEB_RUNNING while running; otherwise the status, with the length in *len (the body
// is copied into buf when it fits) and the slot gone
EM_JS(int, touchweb_js_take, (int id, unsigned char *buf, int bufsize, int *len, char *err, int errsize), {
	var M = Module, slot = M.xtFetch && M.xtFetch[id];
	if (!slot) { HEAP32[len >> 2] = 0; return -4; }
	if (!slot.done) return -100;
	delete M.xtFetch[id];
	var n = slot.bytes ? slot.bytes.length : 0;
	if (slot.over) n = bufsize + 1;
	else if (n > 0) HEAPU8.set(slot.bytes.subarray(0, Math.min(n, bufsize)), buf);
	HEAP32[len >> 2] = n;
	if (errsize > 0) stringToUTF8(slot.error || "", err, errsize);
	return slot.status;
});

EM_JS(void, touchweb_js_cancel, (int id), {
	var slot = Module.xtFetch && Module.xtFetch[id];
	if (!slot) return;
	slot.cancelled = true;
	try { if (slot.ctrl) slot.ctrl.abort(); } catch (e) {}
	delete Module.xtFetch[id];
});

qbool TouchWeb_Available(void)
{
	return true;
}

qbool TouchWeb_Begin_ToMemory_POST(const char *url, const char *post_content_type, const unsigned char *post, size_t postsize, unsigned char *buf, size_t bufsize, qbool allow_loopback_http, curl_callback_t callback, void *cbdata)
{
	int i;
	touchweb_t *t = NULL;
	for (i = 0; i < TOUCHWEB_MAX; i++)
	{
		if (touchweb[i].used && touchweb[i].callback == callback && touchweb[i].cbdata == cbdata)
			return false; // that one is already downloading, as libcurl refuses it
		if (!touchweb[i].used && !t)
			t = &touchweb[i];
	}
	if (!t || !buf || !bufsize || bufsize > 0x7ffffff0)
		return false;
	memset(t, 0, sizeof(*t));
	t->used = true;
	t->buf = buf;
	t->bufsize = bufsize;
	t->callback = callback;
	t->cbdata = cbdata;
	t->id = touchweb_js_start(url, post_content_type, post, post ? (int)postsize : 0, (int)bufsize, allow_loopback_http);
	return true;
}

void TouchWeb_Cancel_ToMemory(curl_callback_t callback, void *cbdata)
{
	int i;
	for (i = 0; i < TOUCHWEB_MAX; i++)
	{
		touchweb_t *t = &touchweb[i];
		if (!t->used || t->callback != callback || t->cbdata != cbdata)
			continue;
		touchweb_js_cancel(t->id);
		t->used = false;
		// as libcurl does: the owner hears about it, then may free its buffer
		callback(CURLCBSTATUS_ABORTED, 0, t->buf, cbdata);
	}
}

void TouchWeb_Frame(void)
{
	static unsigned lastframe;
	int i;
	if (lastframe == host.framecount)
		return;
	lastframe = host.framecount;
	for (i = 0; i < TOUCHWEB_MAX; i++)
	{
		touchweb_t *t = &touchweb[i];
		int status, len = 0;
		char err[sizeof(touchweb_error)];
		curl_callback_t cb;
		if (!t->used)
			continue;
		err[0] = 0;
		status = touchweb_js_take(t->id, t->buf, (int)t->bufsize, &len, err, sizeof(err));
		if (status == TOUCHWEB_RUNNING)
			continue;
		t->used = false;
		if (err[0])
			dp_strlcpy(touchweb_error, err, sizeof(touchweb_error));
		cb = t->callback;
		cb(status, (size_t)len, t->buf, t->cbdata);
	}
}

#else // not the web build

qbool TouchWeb_Available(void) { return false; }
qbool TouchWeb_Begin_ToMemory_POST(const char *url, const char *post_content_type, const unsigned char *post, size_t postsize, unsigned char *buf, size_t bufsize, qbool allow_loopback_http, curl_callback_t callback, void *cbdata)
{
	(void)url; (void)post_content_type; (void)post; (void)postsize; (void)buf; (void)bufsize; (void)allow_loopback_http; (void)callback; (void)cbdata;
	return false;
}
void TouchWeb_Cancel_ToMemory(curl_callback_t callback, void *cbdata) { (void)callback; (void)cbdata; }
void TouchWeb_Frame(void) {}

#endif // __EMSCRIPTEN__
#endif // TOUCH_WEBFETCH_IMPL
#endif // TOUCH_WEBFETCH_H

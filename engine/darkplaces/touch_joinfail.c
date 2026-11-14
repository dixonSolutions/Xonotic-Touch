/*
Godnotic: a join that fails says why, and can be retried.

The engine reports a failed join only in cl_connect_status and the console;
the loading screen simply goes away. This file watches a join from the first
getchallenge to the last signon, without a hook of its own (touch_lan.c calls
it from TouchLAN_Frame, at the top of NetConn_ClientFrame), and when it ends
short of the game:

  - classifies it (touch_joinfail_logic.h): no answer, answered but not let
    in, refused with the server's text, dropped during the signon, missing
    files, no network;
  - publishes it for every front end: _join_fail_serial (counts up),
    _join_fail_kind, _join_fail_text, _join_fail_detail, _join_fail_address,
    and the command join_retry;
  - keeps cl_connect_status' "Connect: failed" / "Connect: rejected" prefix
    (the native front ends key on it) but with the real reason after it;
  - opens the touch menu's "Could not join" dialog (dialog_touch_joinfail.qc)
    unless touch_joinfail_dialog is 0 (a native front end shows its own).

It also keeps the client from locking itself out: see TOUCHJOIN_BACKOFF_* in
touch_joinfail_logic.h.
*/
#include "quakedef.h"
#include "netconn.h"
#include "touch_joinfail.h"
#include "touch_joinfail_logic.h"
#include "touch_nativefe.h"
#include "touch_datafetch.h"

static cvar_t join_fail_serial = {CF_CLIENT | CF_READONLY, "_join_fail_serial", "0", "Godnotic: counts the joins that failed this session; the other _join_fail_* cvars describe the last one"};
static cvar_t join_fail_kind = {CF_CLIENT | CF_READONLY, "_join_fail_kind", "", "Godnotic: why the last join failed: timeout, busy, refused, network, dropped, files, error"};
static cvar_t join_fail_text = {CF_CLIENT | CF_READONLY, "_join_fail_text", "", "Godnotic: the last failed join in one sentence"};
static cvar_t join_fail_detail = {CF_CLIENT | CF_READONLY, "_join_fail_detail", "", "Godnotic: the server's own words for the last failed join, or what to do about it"};
static cvar_t join_fail_address = {CF_CLIENT | CF_READONLY, "_join_fail_address", "", "Godnotic: the address of the last failed join (join_retry connects to it again)"};
static cvar_t touch_joinfail_dialog = {CF_CLIENT, "touch_joinfail_dialog", "1", "Godnotic: open the menu's Could not join dialog when a join fails (a native front end that shows its own sets 0)"};
static cvar_t net_connect_backoff = {CF_CLIENT | CF_ARCHIVE, "net_connect_backoff", "1", "Godnotic: when a server answers the challenge but ignores the connect three times, wait out its 5 second connect flood block once instead of renewing it every second"};

static struct
{
	qbool active;       // a join is being watched
	qbool was_trying;
	qbool connected;    // the accept came
	qbool backed_off;
	qbool in_reply;     // cl_connect_status is the "replying to challenge" line
	int challenges;
	qbool ipv6;
	char address[128];
	char last_status[MAX_QPATH];
	// a drop is reported a moment later, unless a reconnect took over
	double pending_time;
	int pending_kind;
	char pending_text[200], pending_detail[256];
} jf;

qbool TouchJoinFail_Joining(void)
{
	return cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS && !cls.demoplayback);
}

static void report(int kind, const char *text, const char *detail)
{
	char buf[256];
	Cvar_SetQuick(&join_fail_kind, TouchJoinFail_KindName(kind));
	Cvar_SetQuick(&join_fail_text, text);
	Cvar_SetQuick(&join_fail_detail, detail);
	Cvar_SetQuick(&join_fail_address, jf.address);
	Cvar_SetValueQuick(&join_fail_serial, join_fail_serial.integer + 1);
	Con_Printf(CON_ERROR "Join failed (%s): %s. %s\n", TouchJoinFail_KindName(kind), text, detail);
#ifdef CONFIG_MENU
	if (touch_joinfail_dialog.integer && !TouchNativeFE_Active())
	{
		// The same road the password prompt takes (touch_lan.c). The address is
		// LHNETADDRESS_ToString's; the sentences travel in the cvars.
		dpsnprintf(buf, sizeof(buf), "\ntogglemenu 1; menu_cmd directmenu TouchJoinFailed %s \"%s\"\n", TouchJoinFail_KindName(kind), jf.address);
		Cbuf_AddText(cmd_local, buf);
	}
#else
	(void)buf;
#endif
}

static void finish(const char *status)
{
	char text[200], detail[256];
	int kind = TouchJoinFail_Classify(status, jf.challenges, jf.connected, jf.ipv6, jf.address, text, sizeof(text), detail, sizeof(detail));
	jf.active = false;
	if (!kind)
		return;
	if (kind == TOUCHJOINFAIL_TIMEOUT)
		dpsnprintf(cl_connect_status, sizeof(cl_connect_status), "Connect: failed, no answer from %s", jf.address);
	else if (kind == TOUCHJOINFAIL_BUSY)
		dpsnprintf(cl_connect_status, sizeof(cl_connect_status), "Connect: failed, %s answered but did not accept the join", jf.address);
	if (kind == TOUCHJOINFAIL_DROPPED || kind == TOUCHJOINFAIL_ERROR || kind == TOUCHJOINFAIL_FILES)
	{
		// touch_net.c joins again by itself after some drops: give it a moment
		jf.pending_kind = kind;
		jf.pending_time = host.realtime + 0.5;
		dp_strlcpy(jf.pending_text, text, sizeof(jf.pending_text));
		dp_strlcpy(jf.pending_detail, detail, sizeof(jf.pending_detail));
		return;
	}
	report(kind, text, detail);
}

void TouchJoinFail_Frame(void)
{
	qbool trying;
	if (cls.state == ca_dedicated)
		return;
	trying = cls.connect_trying;

	if (jf.pending_kind && host.realtime >= jf.pending_time)
	{
		int kind = jf.pending_kind;
		jf.pending_kind = 0;
		if (!trying && cls.state != ca_connected && !TouchData_JoinPending()) // the map is still downloading: touch_datafetch.c joins again
			report(kind, jf.pending_text, jf.pending_detail);
	}

	if (trying && !jf.was_trying)
	{
		// a new join (or the same one again)
		jf.active = true;
		jf.connected = jf.backed_off = jf.in_reply = false;
		jf.challenges = 0;
		jf.pending_kind = 0;
		LHNETADDRESS_ToString(&cls.connect_address, jf.address, sizeof(jf.address), true);
		jf.ipv6 = LHNETADDRESS_GetAddressType(&cls.connect_address) == LHNETADDRESSTYPE_INET6;
	}
	jf.was_trying = trying;

	// "no network" never sets connect_trying: CL_EstablishConnection gives up at once
	if (!jf.active && !trying && strcmp(cl_connect_status, jf.last_status) && !strcmp(cl_connect_status, "Connect: failed, no network"))
	{
		char text[200], detail[256];
		// CL_EstablishConnection parsed the address before it found no socket for it
		if (cls.state != ca_connected)
		{
			LHNETADDRESS_ToString(&cls.connect_address, jf.address, sizeof(jf.address), true);
			jf.ipv6 = LHNETADDRESS_GetAddressType(&cls.connect_address) == LHNETADDRESSTYPE_INET6;
			report(TouchJoinFail_Classify(cl_connect_status, 0, 0, jf.ipv6, jf.address, text, sizeof(text), detail, sizeof(detail)), text, detail);
		}
	}
	dp_strlcpy(jf.last_status, cl_connect_status, sizeof(jf.last_status));

	if (!jf.active)
		return;
	if (trying)
	{
		// The challenge handler leaves this line; the next request replaces it.
		qbool reply = !strcmp(cl_connect_status, "Connect: replying to challenge...");
		if (reply && !jf.in_reply)
			jf.challenges++;
		jf.in_reply = reply;
		if (net_connect_backoff.integer && cls.connect_nextsendtime < host.realtime + 0.05
			&& TouchJoin_ShouldBackOff(jf.challenges, jf.backed_off))
		{
			jf.backed_off = true;
			cls.connect_nextsendtime = host.realtime + TOUCHJOIN_BACKOFF_SECONDS;
			if (cls.connect_remainingtries < TOUCHJOIN_BACKOFF_TRIES)
				cls.connect_remainingtries = TOUCHJOIN_BACKOFF_TRIES;
			dp_strlcpy(cl_connect_status, "Connect: the server is busy, asking again in a few seconds...", sizeof(cl_connect_status));
			jf.in_reply = false;
			Con_Printf("Join: %s answers but ignores the connect; waiting %.1f s for its connect flood block to end\n", jf.address, TOUCHJOIN_BACKOFF_SECONDS);
		}
		return;
	}
	if (cls.state == ca_connected)
	{
		jf.connected = true;
		if (cls.signon >= SIGNONS)
			jf.active = false; // in the game: a later drop is touch_net.c's business
		return;
	}
	finish(cl_connect_status);
}

static void TouchJoinFail_Retry_f(cmd_state_t *cmd)
{
	char line[200];
	if (!join_fail_address.string[0])
	{
		Con_Print("join_retry: no join has failed\n");
		return;
	}
	dpsnprintf(line, sizeof(line), "\nconnect \"%s\"\n", join_fail_address.string);
	Cbuf_AddText(cmd_local, line);
}

void TouchJoinFail_Init(void)
{
	Cvar_RegisterVariable(&join_fail_serial);
	Cvar_RegisterVariable(&join_fail_kind);
	Cvar_RegisterVariable(&join_fail_text);
	Cvar_RegisterVariable(&join_fail_detail);
	Cvar_RegisterVariable(&join_fail_address);
	Cvar_RegisterVariable(&touch_joinfail_dialog);
	Cvar_RegisterVariable(&net_connect_backoff);
	Cmd_AddCommand(CF_CLIENT, "join_retry", TouchJoinFail_Retry_f, "Godnotic: join the server of the last failed join again (_join_fail_address)");
}

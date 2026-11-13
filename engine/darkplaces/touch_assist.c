/*
Server switch for the touch assists.

A server running this engine tells each joining client which of the touch
assists it allows, in a stufftext that sets _cl_touch_assist to the bits of
sv_touch_assist (TOUCH_ASSIST_* in touch_assist.h): aim pull, auto-shoot and
automatic weapon switching. A Touch client obeys it until it disconnects. A
stock client only gets a cvar it never reads.

A server that says nothing (every stock Xonotic server) leaves the assists as
the player set them: _cl_touch_assist stays -1.

The message goes with the server info, which every client gets when it joins
and again at each map change, so a change to sv_touch_assist reaches everyone
at the next map. The server cvar is registered on first use, from that same
hook: a dedicated server never initialises the client side, and this keeps the
upstream server code to one added line. A value set in server.cfg before that
(set sv_touch_assist 0) is kept when it registers.

This is a request the client honours, not something the server can enforce.
*/

#include "quakedef.h"
#include "touch_assist.h"

static cvar_t sv_touch_assist = {CF_SERVER, "sv_touch_assist", "7",
	"Touch clients' assists this server allows (bits): 1 aim assist, 2 auto-shoot, 4 automatic weapon switching; 7 all, 0 none. "
	"Sent to each player when they join and at each map change"};

static cvar_t cl_touch_assist_server = {CF_CLIENT | CF_READONLY, "_cl_touch_assist", "-1",
	"Set by the server: the touch assists it allows (sv_touch_assist bits), -1 when it has said nothing"};

void TouchAssist_SendServerinfo(client_t *client)
{
	static qbool registered;
	char text[64];
	int bits;
	if (!registered)
	{
		Cvar_RegisterVariable(&sv_touch_assist);
		registered = true;
	}
	if (!client || !client->netconnection)
		return;
	bits = sv_touch_assist.integer & TOUCH_ASSIST_ALL;
	dpsnprintf(text, sizeof(text), "\n_touch_assist_allowed %d\n", bits);
	MSG_WriteByte(&client->netconnection->message, svc_stufftext);
	MSG_WriteString(&client->netconnection->message, text);
}

// The stufftext lands here: a read-only cvar cannot be set by "set" from a
// server, and only this command (with one number) changes it.
static void TouchAssist_Allowed_f(cmd_state_t *cmd)
{
	int bits, old = cl_touch_assist_server.integer;
	if (Cmd_Argc(cmd) != 2)
		return;
	bits = atoi(Cmd_Argv(cmd, 1)) & TOUCH_ASSIST_ALL;
	if (cls.state != ca_connected)
		return;
	Cvar_SetValueQuick(&cl_touch_assist_server, bits);
	if (bits != old && bits != TOUCH_ASSIST_ALL)
	{
		static const char *const names[3] = { "aim assist", "auto-shoot", "automatic weapon switching" };
		const char *off[3];
		int i, n = 0;
		for (i = 0; i < 3; i++)
			if (!(bits & (1 << i)))
				off[n++] = names[i];
		if (n == 1)
			Con_Printf("This server turns off %s for touch controls\n", off[0]);
		else if (n == 2)
			Con_Printf("This server turns off %s and %s for touch controls\n", off[0], off[1]);
		else
			Con_Printf("This server turns off %s, %s and %s for touch controls\n", off[0], off[1], off[2]);
	}
}

void TouchAssist_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_assist_server);
	Cmd_AddCommand(CF_CLIENT | CF_CLIENT_FROM_SERVER, "_touch_assist_allowed", TouchAssist_Allowed_f,
		"set by the server: which touch assists it allows (sv_touch_assist bits)");
}

// Before this frame's packets are read (CL_Input): a new connection, or none,
// starts from "the server has said nothing", also when `connect` went from one
// server straight to another within a frame.
void TouchAssist_Frame(void)
{
	static const void *netcon;
	if ((cls.state != ca_connected || (const void *)cls.netcon != netcon) && cl_touch_assist_server.integer != -1)
		Cvar_SetValueQuick(&cl_touch_assist_server, -1);
	netcon = cls.netcon;
}

qbool TouchAssist_Allowed(int bit)
{
	return cl_touch_assist_server.integer < 0 || (cl_touch_assist_server.integer & bit) != 0;
}

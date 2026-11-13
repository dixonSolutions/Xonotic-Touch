// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_build.qh"

bool UIB_Screen(string id, string title)
{
	if (!uib_begin(id, "screen"))
		return false;
	uib_s("title", title);
	return true;
}

void UIB_Done() { uib_done(); }
bool UIB_Open(string key, string type) { return uib_node(key, type); }
void UIB_Close() { uib_end(); }

string UIB_A(string id, string label, string icon)
{
	if (icon == "")
		return sprintf("{\"id\":%s,\"label\":%s}", uib_qv(id), uib_q(label));
	return sprintf("{\"id\":%s,\"label\":%s,\"icon\":%s}", uib_qv(id), uib_q(label), uib_qv(icon));
}

string UIB_AS(string id, string label, string icon, string style)
{
	return sprintf("{\"id\":%s,\"label\":%s,\"icon\":%s,\"style\":%s}", uib_qv(id), uib_q(label), uib_qv(icon), uib_qv(style));
}

string UIB_AC(string id, string label, string icon, string style, string title, string ok, bool destructive)
{
	return sprintf("{\"id\":%s,\"label\":%s,\"icon\":%s,\"style\":%s,\"confirm\":{\"title\":%s,\"ok\":%s,\"destructive\":%s}}",
		uib_qv(id), uib_q(label), uib_qv(icon), uib_qv(style), uib_q(title), uib_q(ok), destructive ? "true" : "false");
}

string UIB_ALink(string id, string label, string icon, string url)
{
	return sprintf("{\"id\":%s,\"label\":%s,\"icon\":%s,\"kind\":\"link\",\"url\":%s}", uib_qv(id), uib_q(label), uib_qv(icon), uib_qv(url));
}

string UIB_AText(string id, string label, string icon, string kind, string text)
{
	return sprintf("{\"id\":%s,\"label\":%s,\"icon\":%s,\"kind\":%s,\"text\":%s}", uib_qv(id), uib_q(label), uib_qv(icon), uib_qv(kind), uib_qv(text));
}

string UIB_Chip(string icon, string text)
{
	return sprintf("{\"icon\":%s,\"text\":%s}", uib_qv(icon), uib_q(text));
}

string UIB_Badge(string icon, string text, string tone)
{
	return sprintf("{\"icon\":%s,\"text\":%s,\"tone\":%s}", uib_qv(icon), uib_q(text), uib_qv(tone));
}

string UIB_Opt(string value, string label)
{
	return sprintf("{\"value\":%s,\"label\":%s}", uib_qv(value), uib_q(label));
}

string UIB_Attr(string label, string value, string icon)
{
	return sprintf("{\"label\":%s,\"value\":%s,\"icon\":%s}", uib_q(label), uib_q(value), uib_qv(icon));
}

string UIB_Col(string key, string type, string label, string width, float priority, bool sortable, bool alignend)
{
	return sprintf("{\"key\":%s,\"type\":%s,\"label\":%s,\"width\":%s,\"priority\":%d,\"sortable\":%s%s}",
		uib_qv(key), uib_qv(type), uib_q(label), uib_qv(width), priority, (sortable ? "true" : "false"), (alignend ? ",\"align\":\"end\"" : ""));
}

void UIB_Switch(string key, string label, string bind, string help)
{
	uib_node(key, "switch");
	uib_s("label", label);
	uib_s("bind", bind);
	if (help != "") uib_s("help", help);
	if (substring(bind, 0, 3) == "vm:") uib_b("value", UIB_VMf(substring(bind, 3, -1)));
	uib_end();
}

void UIB_Check(string key, string label, string bind, string icon)
{
	uib_node(key, "switch");
	uib_s("label", label);
	uib_s("style", "checkbox");
	uib_s("bind", bind);
	if (icon != "") uib_s("icon", icon);
	if (substring(bind, 0, 3) == "vm:") uib_b("value", UIB_VMf(substring(bind, 3, -1)));
	uib_end();
}

void UIB_Slider(string key, string label, string bind, float lo, float hi, float step, string fmt, string help)
{
	uib_node(key, "slider");
	uib_s("label", label);
	uib_s("bind", bind);
	uib_f("min", lo);
	uib_f("max", hi);
	uib_f("step", step);
	uib_s("format", fmt);
	if (help != "") uib_s("help", help);
	if (substring(bind, 0, 3) == "vm:") uib_f("value", UIB_VMf(substring(bind, 3, -1)));
	uib_end();
}

void UIB_Stepper(string key, string label, string bind, float lo, float hi, float step, string unit)
{
	uib_node(key, "stepper");
	uib_s("label", label);
	uib_s("bind", bind);
	uib_f("min", lo);
	uib_f("max", hi);
	uib_f("step", step);
	uib_s("format", "int");
	if (unit != "") uib_s("unit", unit);
	if (substring(bind, 0, 3) == "vm:") uib_f("value", UIB_VMf(substring(bind, 3, -1)));
	uib_end();
}

void UIB_Choice(string key, string label, string bind, string style, string optsjson, string help)
{
	uib_node(key, "choice");
	uib_s("label", label);
	uib_s("bind", bind);
	uib_s("style", style);
	uib_j("options", optsjson);
	if (help != "") uib_s("help", help);
	if (substring(bind, 0, 3) == "vm:") uib_s("value", UIB_VM(substring(bind, 3, -1)));
	uib_end();
}

void UIB_Button(string key, string label, string actionjson, string style)
{
	uib_node(key, "button");
	uib_s("label", label);
	uib_j("action", actionjson);
	uib_s("style", style);
	uib_end();
}

void UIB_Text(string key, string body, string style)
{
	uib_node(key, "text");
	uib_s("body", body);
	if (style != "") uib_s("style", style);
	uib_end();
}

void UIB_Section(string key, string title, bool sticky)
{
	uib_node(key, "section");
	if (title != "") uib_s("title", title);
	if (sticky) uib_b("sticky", 1);
}

void UIB_Apply(string how) { uib_s("apply", how); }

string UIB_VMCvar(string name)
{
	return strcat("_uib_", strreplace(".", "_", name));
}

string UIB_VM(string name)
{
	return cvar_string(UIB_VMCvar(name));
}

float UIB_VMf(string name)
{
	return stof(cvar_string(UIB_VMCvar(name)));
}

void UIB_VMSet(string name, string value)
{
	string c = UIB_VMCvar(name);
	registercvar(c, "", 0);
	cvar_set(c, value);
}

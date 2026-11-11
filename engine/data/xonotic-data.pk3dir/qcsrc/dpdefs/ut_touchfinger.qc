// Xonotic Touch — CSQC builtin (engine/darkplaces: gettouchfinger, DP_UT_TOUCHFINGER)
#ifndef UT_TOUCHFINGER_QC
#define UT_TOUCHFINGER_QC
#pragma noref 1
vector(float slot) gettouchfinger = #643;
// A circle, capsule or rounded rectangle drawn as antialiased geometry: centre,
// half extents, corner radius, border width inside the edge (0 fills), colour,
// alpha. Borders stay the same width at any size. Extension
// XONOTIC_TOUCH_DRAWROUNDEDBOX; touch_shape.qc falls back to masks without it.
void(vector center, vector half, float radius, float stroke, vector rgb, float alpha) drawroundedbox = #644;
#pragma noref 0
#endif

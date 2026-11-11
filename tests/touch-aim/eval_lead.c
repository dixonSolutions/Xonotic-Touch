/*
Lead evaluator: replays recorded player tracks (XTRK lines from
cl_touch_aim_dumptracks: time entnum x y z vx vy vz) through the lead the
engine uses and scores how far the predicted position is from where the
player really was when the shot arrived.

  eval_lead PING TRACKS...

For every player, every 0.25 s and four shooter directions, the tracker
state the client would have (smoothed velocity, strafe reversals across the
line of sight, in the air or not) predicts the position after the horizon
h = ping + 0.05 (interpolation) + flight time, for flight times of the
projectiles at 500 / 900 / 1500 units. The error is the horizontal distance
to the real position at that time; a hit is an error under 16 units (the
body's half width) for a direct shot.

Models: "old" = full constant-velocity lead (v1.2.136); "new" = the share
TouchWep_LeadScale gives a dodger; "k=x" = a fixed share x for everyone in
that class (to tune).
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool qbool;
#include "touch_weapons.h"

#define MAXS 400000
typedef struct { double t; int n; float p[3], v[3]; } sample_t;
static sample_t *S;
static int ns;

static int cmp(const void *a, const void *b)
{
	const sample_t *x = a, *y = b;
	if (x->n != y->n)
		return x->n - y->n;
	return x->t < y->t ? -1 : x->t > y->t;
}

// where player samples [a, b) put the player at time t (linear between samples)
static int at(int a, int b, double t, float out[3])
{
	int i;
	if (t < S[a].t || t > S[b - 1].t)
		return 0;
	for (i = a; i < b - 1; i++)
		if (S[i + 1].t >= t)
		{
			double f = S[i + 1].t > S[i].t ? (t - S[i].t) / (S[i + 1].t - S[i].t) : 0;
			int k;
			for (k = 0; k < 3; k++)
				out[k] = S[i].p[k] + (float)f * (S[i + 1].p[k] - S[i].p[k]);
			return 1;
		}
	return 0;
}

#define NMODEL 7
static const char *mname[NMODEL] = { "old", "new", "k=0.5", "k=0.75", "k=1", "k=1.25", "h-share" };
#define NCLASS 4
static double err_sum[NCLASS][3][NMODEL], hits[NCLASS][3][NMODEL], near60[NCLASS][3][NMODEL];
static long cnt[NCLASS][3];

int main(int argc, char **argv)
{
	double ping = argc > 1 ? atof(argv[1]) : 0.25;
	int a, i, f;
	static const float dists[3] = { 500, 900, 1500 };
	S = malloc(sizeof(sample_t) * MAXS);
	for (f = 2; f < argc; f++)
	{
		FILE *fp = fopen(argv[f], "r");
		char line[256];
		while (fp && fgets(line, sizeof(line), fp) && ns < MAXS)
		{
			sample_t s;
			if (sscanf(line, "XTRK %lf %d %f %f %f %f %f %f", &s.t, &s.n, &s.p[0], &s.p[1], &s.p[2], &s.v[0], &s.v[1], &s.v[2]) == 8)
			{
				s.n += f * 1000; // players of different recordings apart
				S[ns++] = s;
			}
		}
		if (fp)
			fclose(fp);
	}
	qsort(S, ns, sizeof(sample_t), cmp);
	for (a = 0; a < ns;)
	{
		int b = a, dir;
		while (b < ns && S[b].n == S[a].n)
			b++;
		for (dir = 0; dir < 4; dir++)
		{
			float u[2] = { cosf(dir * 1.5708f + 0.4f), sinf(dir * 1.5708f + 0.4f) }; // from shooter to target
			float vel[3] = { 0, 0, 0 };
			int lat_sign = 0;
			double rev[4] = { 0, 0, 0, 0 }, next = S[a].t + 1, lastt = S[a].t;
			for (i = a; i < b; i++)
			{
				float dt = (float)(S[i].t - lastt), fr, lat, vh;
				int k, nrev = 0, cls, d;
				lastt = S[i].t;
				if (dt <= 0)
					continue;
				// the engine's tracker: smoothed velocity, reversals, class
				fr = dt / 0.08f;
				fr = fr > 1 ? 1 : fr;
				for (k = 0; k < 3; k++)
					vel[k] += fr * (S[i].v[k] - vel[k]);
				lat = u[0] * vel[1] - u[1] * vel[0];
				if (fabsf(lat) > 120)
				{
					int sign = lat > 0 ? 1 : -1;
					if (lat_sign && sign != lat_sign)
					{
						for (k = 3; k > 0; k--)
							rev[k] = rev[k - 1];
						rev[0] = S[i].t;
					}
					lat_sign = sign;
				}
				for (k = 0; k < 4; k++)
					if (rev[k] > 0 && S[i].t - rev[k] < 1.5)
						nrev++;
				vh = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
				cls = fabsf(S[i].v[2]) > 50 ? TW_CLASS_AIR : nrev >= 2 ? TW_CLASS_ERRATIC : vh < 60 ? TW_CLASS_STAND : TW_CLASS_STEADY;
				if (S[i].t < next)
					continue;
				next = S[i].t + 0.25;
				for (d = 0; d < 3; d++)
				{
					// flight of a 2200 u/s shot (Hagar class) at that distance
					float tof = dists[d] / 2200.0f, h = (float)(ping + 0.05) + tof, real[3], period = 0;
					int m, mm = 0;
					if (!at(a, b, S[i].t + h, real))
						continue;
					for (k = 1; k < 4; k++)
						if (rev[k] > 0 && S[i].t - rev[k] < 3.0)
							mm = k;
					if (mm >= 1)
						period = (float)((rev[0] - rev[mm]) / mm);
					for (m = 0; m < NMODEL; m++)
					{
						float sh = 1, e[2];
						if (m == 1)
							sh = cls == TW_CLASS_ERRATIC ? TouchWep_LeadScale(h, period, 0) : 1;
						else if (m >= 2 && m <= 5)
							sh = 0.5f + (m - 2) * 0.25f;
						else if (m == 6)
							sh = TouchWep_LeadShare(cls, h, period);
						// only the sideways part is scaled; the part along the line of sight is kept
						{
							float along = vel[0] * u[0] + vel[1] * u[1];
							float pv[2] = { u[0] * along + sh * (vel[0] - u[0] * along), u[1] * along + sh * (vel[1] - u[1] * along) };
							e[0] = S[i].p[0] + pv[0] * h - real[0];
							e[1] = S[i].p[1] + pv[1] * h - real[1];
						}
						{
							float err = sqrtf(e[0] * e[0] + e[1] * e[1]);
							err_sum[cls][d][m] += err;
							hits[cls][d][m] += err < 16;
							near60[cls][d][m] += err < 60;
						}
					}
					cnt[cls][d]++;
				}
			}
		}
		a = b;
	}
	printf("ping %.0f ms, %d samples\n", ping * 1000, ns);
	for (i = 0; i < NCLASS; i++)
		for (a = 0; a < 3; a++)
		{
			int m;
			if (!cnt[i][a])
				continue;
			printf("%-13s %4.0f u n=%-5ld", touch_wep_class_name[i], dists[a], cnt[i][a]);
			for (m = 0; m < NMODEL; m++)
				printf(" | %s %3.0f%% %3.0f%%", mname[m], 100 * hits[i][a][m] / cnt[i][a], 100 * near60[i][a][m] / cnt[i][a]);
			printf("\n");
		}
	return 0;
}

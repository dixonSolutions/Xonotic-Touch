/* touch_srvlist_logic.h: the saved list's line format, the query budget, the
   tries per server and the percentiles. Run by scripts/test-srvlist.sh. */
#include "touch_srvlist_logic.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

int main(void)
{
	touchsl_record_t r, q;
	touchsl_bucket_t b;
	char line[700], bad[128];
	float t[10] = {0.9f, 0.1f, 0.5f, 0.3f, 0.7f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
	int n, i, total;
	double now;

	// a record survives the file
	memset(&r, 0, sizeof(r));
	strcpy(r.cname, "[2001:db8::1]:26000");
	r.ping = 87; r.numplayers = 9; r.numbots = 4; r.maxplayers = 16; r.protocol = 3; r.gameversion = 806;
	r.seen = 1794000000LL; r.fails = 0;
	strcpy(r.name, "A\tserver\nname ^1red"); strcpy(r.map, "stormkeep"); strcpy(r.mod, "data"); strcpy(r.game, "Xonotic");
	strcpy(r.qcstatus, "ctf:git:P9:S16:F7:MXonotic::score!!");
	n = TouchSL_FormatRecord(line, sizeof(line), &r);
	CHECK(n > 0 && line[n - 1] == '\n', "a record is one line");
	line[n - 1] = 0;
	CHECK(TouchSL_ParseRecord(line, &q), "and parses back");
	CHECK(!strcmp(q.cname, r.cname) && q.ping == 87 && q.numplayers == 9 && q.numbots == 4 && q.maxplayers == 16 && q.gameversion == 806 && q.seen == r.seen, "numbers survive");
	CHECK(!strcmp(q.name, "A server name ^1red") && !strcmp(q.map, "stormkeep") && !strcmp(q.qcstatus, r.qcstatus), "strings survive, without tabs or newlines");
	// lines that are not records
	strcpy(bad, "garbage"); CHECK(!TouchSL_ParseRecord(bad, &q), "garbage is refused");
	strcpy(bad, "1.2.3.4:26000\t10\t1"); CHECK(!TouchSL_ParseRecord(bad, &q), "a short line is refused");
	strcpy(bad, "evil;quit:1\t10\t1\t0\t8\t3\t806\t1\t0\tn\tm\td\tg\tq"); CHECK(!TouchSL_ParseRecord(bad, &q), "an address with other characters is refused");
	strcpy(bad, "1.2.3.4:26000\t-5\t1\t0\t8\t3\t806\t1\t0\tn\tm\td\tg\tq"); CHECK(!TouchSL_ParseRecord(bad, &q), "a negative ping is refused");
	strcpy(bad, "1.2.3.4:26000\t50\t3\t9\t8\t3\t806\t1\t0\tn\tm\td\tg\tq"); CHECK(TouchSL_ParseRecord(bad, &q) && q.numbots == 3, "more bots than players is clamped");
	// what is shown at once, what is kept
	r.fails = 0; r.seen = 1000000;
	CHECK(TouchSL_RecordShown(&r, 1000000 + 3600), "answered last time: shown");
	CHECK(!TouchSL_RecordShown(&r, 1000000 + TOUCHSL_MAX_AGE + 1), "a week old: not shown");
	r.fails = 1;
	CHECK(!TouchSL_RecordShown(&r, 1000000 + 60) && TouchSL_RecordKept(&r, 1000000 + 60), "failed once: not shown, still known");
	r.fails = TOUCHSL_MAX_FAILS;
	CHECK(!TouchSL_RecordKept(&r, 1000000 + 60), "failed three times: forgotten");

	// the budget: never more than the rate over a second, never more than a burst at once
	memset(&b, 0, sizeof(b));
	total = 0;
	for (i = 0, now = 100.0; i < 240; i++, now += 1.0 / 60) // four seconds of 60 fps frames
	{
		n = TouchSL_Budget(&b, now, 300, 20);
		CHECK(n <= 20, "a burst at most per frame");
		TouchSL_Spend(&b, n);
		total += n;
	}
	CHECK(total <= 300 * 4 + 20 && total >= 300 * 4 - 40, "the rate holds over four seconds at 60 fps");
	memset(&b, 0, sizeof(b));
	total = 0;
	for (i = 0, now = 100.0; i < 20; i++, now += 0.2) // 5 fps: the frame rate does not set the pace beyond the burst
	{
		n = TouchSL_Budget(&b, now, 300, 20);
		TouchSL_Spend(&b, n);
		total += n;
	}
	CHECK(total == 20 * 20, "at 5 fps every frame sends one burst and no more");
	n = TouchSL_Budget(&b, now + 30, 300, 20);
	CHECK(n == 20, "a stall does not pile up more than a burst");

	// tries
	CHECK(TouchSL_Tries(TOUCHSL_KNOWN_ALIVE, 3, 0) == 3, "a server that was up gets every try");
	CHECK(TouchSL_Tries(TOUCHSL_UNKNOWN, 3, 0) == 2, "an unknown one two");
	CHECK(TouchSL_Tries(TOUCHSL_KNOWN_DEAD, 3, 0) == 1, "one that failed before one");
	CHECK(TouchSL_Tries(TOUCHSL_KNOWN_ALIVE, 3, 1) == 2, "the background refresh at most two");
	CHECK(TouchSL_Tries(TOUCHSL_UNKNOWN, 1, 0) == 1 && TouchSL_Tries(TOUCHSL_KNOWN_ALIVE, 99, 0) == 8, "net_slist_maxtries bounds");
	CHECK(TOUCHSL_RETRY_INTERVAL > 1.0, "a retry waits out the server's 1 s getstatus block");
	CHECK(TouchSL_FinalWait(420) > 0.42 && TouchSL_FinalWait(420) < 0.6, "the last wait is maxping and a frame");

	// percentiles
	CHECK(TouchSL_Percentile(t, 10, 50) == 0.5f, "half of ten answers were in by the fifth");
	CHECK(TouchSL_Percentile(t, 10, 95) == 1.0f, "95% of ten needs the tenth");
	CHECK(TouchSL_Percentile(t, 1, 50) == 0.1f && TouchSL_Percentile(t, 0, 50) == 0, "one answer, none");

	TouchSL_Summary(line, sizeof(line), 212, 37);
	CHECK(!strcmp(line, "212 servers \xC2\xB7 37 players"), "summary");
	TouchSL_Summary(line, sizeof(line), 1, 1);
	CHECK(!strcmp(line, "1 server \xC2\xB7 1 player"), "summary, singular");

	if (fails)
		return 1;
	printf("ok   srvlist logic\n");
	return 0;
}

/*
Checks how the engine reads a server's vote prints (server/command/vote.qc)
back into vote events (engine/darkplaces/touch_vote_parse.h): the lines are
written here byte for byte as the server sends them, colour codes and the \1
chat-sound marker included.

Run through scripts/test-touch-vote.sh.
*/

#include <stdio.h>
#include <string.h>

#include "touch_vote_parse.h"

static int failures;

#define CHECK_INT(expected, actual, what) do { \
	int e_ = (expected), a_ = (actual); \
	if (e_ != a_) { \
		printf("FAIL %s: expected %d, got %d\n", (what), e_, a_); \
		failures++; \
	} \
} while (0)

#define CHECK_STR(expected, actual, what) do { \
	if (strcmp((expected), (actual))) { \
		printf("FAIL %s: expected \"%s\", got \"%s\"\n", (what), (expected), (actual)); \
		failures++; \
	} \
} while (0)

static void check_line(const char *what, const char *line, int kind, const char *caller, const char *text)
{
	touchvote_msg_t m;
	CHECK_INT(kind, TouchVote_ParseLine(line, &m), what);
	CHECK_INT(kind, m.kind, what);
	if (caller)
		CHECK_STR(caller, m.caller, what);
	if (text)
		CHECK_STR(text, m.text, what);
}

static void check_results(const char *what, const char *line, int yes, int no, int needed, int abstain, int notvoted)
{
	touchvote_msg_t m;
	CHECK_INT(TOUCHVOTE_MSG_RESULTS, TouchVote_ParseLine(line, &m), what);
	CHECK_INT(yes, m.yes, what);
	CHECK_INT(no, m.no, what);
	CHECK_INT(needed, m.needed, what);
	CHECK_INT(abstain, m.abstain, what);
	CHECK_INT(notvoted, m.notvoted, what);
}

int main(void)
{
	char buf[128];
	touchvote_msg_t m;

	TouchVote_StripColors("\001^2* ^3emb^xF80ryo^7 ^^ok^ ", buf, sizeof(buf));
	CHECK_STR("* embryo ^ok^", buf, "colour codes, ^^ and the chat-sound byte");
	TouchVote_StripColors("^1abcdefgh", buf, 5);
	CHECK_STR("abcd", buf, "output truncated to its size");

	// VoteCommand_call, VoteCommand_master
	check_line("call", "\001^2* ^3embryo^2 calls a vote for ^1gotomap revdm3\n", TOUCHVOTE_MSG_CALL, "embryo", "gotomap revdm3");
	check_line("call, coloured name and kick display", "\001^2* ^3^1Bad^7Guy^2 calls a vote for ^1kick #3 ^7Some^3One^1 camping",
		TOUCHVOTE_MSG_CALL, "BadGuy", "kick #3 SomeOne camping");
	check_line("master call", "\001^2* ^3Caller^2 calls a vote to become ^3master^2.", TOUCHVOTE_MSG_CALL, "Caller", "master");
	check_line("DEBUG MODE line", "\001^2* ^3^6DEBUG MODE ACTIVE: bots can vote too", TOUCHVOTE_MSG_NONE, NULL, NULL);

	// VoteAccept, VoteReject, VoteTimeout
	check_line("accepted", "\001^2* ^3embryo^2's vote for ^1gotomap revdm3^2 was accepted\n", TOUCHVOTE_MSG_ACCEPTED, "embryo", "gotomap revdm3");
	check_line("rejected", "\001^2* ^3Caller^2's vote for ^1extendmatchtime^2 was rejected", TOUCHVOTE_MSG_REJECTED, "Caller", "extendmatchtime");
	check_line("timed out", "\001^2* ^3Caller^2's vote for ^1endmatch^2 timed out", TOUCHVOTE_MSG_TIMEOUT, "Caller", "endmatch");

	// VoteStop
	check_line("canceled", "\001^2* ^3Caller^2's vote was canceled", TOUCHVOTE_MSG_STOPPED, "Caller", "");
	check_line("stopped by caller", "\001^2* ^3Caller^2 stopped their vote", TOUCHVOTE_MSG_STOPPED, "Caller", "");
	check_line("stopped by another", "\001^2* ^3Admin^2 stopped ^3Caller^2's vote", TOUCHVOTE_MSG_STOPPED, "Caller", "");

	// VoteSpam: the owner's report, a pass on timeout, a plain pass
	check_results("results with needed", "\001^2* vote results: ^11^2:^10^2 (^11^2 needed), ^10^2 didn't care, ^11^2 didn't vote\n", 1, 0, 1, 0, 1);
	check_results("results without needed", "\001^2* vote results: ^12^2:^11^2, ^10^2 didn't care, ^13^2 didn't have to vote", 2, 1, -1, 0, 3);
	check_results("double digits", "\001^2* vote results: ^112^2:^110^2 (^112^2 needed), ^14^2 didn't care, ^10^2 didn't vote", 12, 10, 12, 4, 0);
	check_line("results garbage", "\001^2* vote results: none", TOUCHVOTE_MSG_NONE, NULL, NULL);

	// print_to replies to this player's own vote commands
	CHECK_INT(TOUCHVOTE_MSG_MINE, TouchVote_ParseLine("^1You accepted the vote.\n", &m), "yes reply");
	CHECK_INT(TOUCHVOTE_YES, m.choice, "yes reply choice");
	CHECK_INT(TOUCHVOTE_MSG_MINE, TouchVote_ParseLine("^1You rejected the vote.", &m), "no reply");
	CHECK_INT(TOUCHVOTE_NO, m.choice, "no reply choice");
	CHECK_INT(TOUCHVOTE_MSG_MINE, TouchVote_ParseLine("^1You abstained from your vote.", &m), "abstain reply");
	CHECK_INT(TOUCHVOTE_ABSTAIN, m.choice, "abstain reply choice");
	check_line("already voted", "^1You have already voted.", TOUCHVOTE_MSG_ALREADY, NULL, NULL);
	check_line("no vote", "^1No vote called.", TOUCHVOTE_MSG_NOVOTE, NULL, NULL);

	// lines that only look like votes
	check_line("chat", "\001^7embryo^7: ^7* vote results soon", TOUCHVOTE_MSG_NONE, NULL, NULL);
	check_line("chat quoting a call", "\001jiji: calls a vote for fun", TOUCHVOTE_MSG_NONE, NULL, NULL);
	check_line("empty", "", TOUCHVOTE_MSG_NONE, NULL, NULL);
	check_line("frag", "\001^2* ^3A^2 was gibbed by B", TOUCHVOTE_MSG_NONE, NULL, NULL);

	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("vote print parsing: all checks passed\n");
	return 0;
}

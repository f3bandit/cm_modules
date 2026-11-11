/* The front ports' card bus lock (cmod_mc_lock, CMOD_MC_BUSY), and the shell modules' mc_lock agreeing with it.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md). */
#include "cmmod.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failed = 1; } } while (0)

int main(void)
{
	char who[120];
	unlink(CMOD_MC_BUSY);

	/* free; ours; busy for others, who say who has it */
	CHECK(cmod_mc_busy(who, sizeof who) == 0 && !who[0]);
	CHECK(cmod_mc_lock("test one", 0, who, sizeof who) == 0);
	CHECK(cmod_mc_busy(who, sizeof who) == 1 && !strcmp(who, "test one"));
	pid_t p = fork();
	if (p == 0) _exit(cmod_mc_lock("test two", 300, who, sizeof who) == 1 && !strcmp(who, "test one") ? 0 : 1);
	int st;
	waitpid(p, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	/* another process can't give it back; we can */
	p = fork();
	if (p == 0) { cmod_mc_unlock(); _exit(0); }
	waitpid(p, &st, 0);
	CHECK(cmod_mc_busy(NULL, 0) == 1);
	cmod_mc_unlock();
	CHECK(cmod_mc_busy(NULL, 0) == 0);

	/* the next one waits for it to be given back */
	p = fork();
	if (p == 0) { cmod_mc_lock("holder", 0, NULL, 0); usleep(400000); cmod_mc_unlock(); _exit(0); }
	usleep(100000);
	CHECK(cmod_mc_lock("waiter", 2000, who, sizeof who) == 0);
	waitpid(p, &st, 0);
	cmod_mc_unlock();

	/* an owner that died (crashed) leaves it free */
	p = fork();
	if (p == 0) { cmod_mc_lock("crashes", 0, NULL, 0); kill(getpid(), SIGKILL); }
	waitpid(p, &st, 0);
	CHECK(cmod_mc_busy(who, sizeof who) == 0);
	CHECK(cmod_mc_lock("after a crash", 0, NULL, 0) == 0);
	cmod_mc_unlock();

	/* the shell modules' mc_lock: the same lock both ways */
	CHECK(system("bash -c '. modules/lib/common.sh; mc_lock script 0 && sleep 0.6; mc_unlock' &") == 0);
	usleep(300000);
	CHECK(cmod_mc_busy(who, sizeof who) == 1 && !strcmp(who, "script"));
	CHECK(cmod_mc_lock("c after script", 3000, who, sizeof who) == 0);
	CHECK(system("bash -c '. modules/lib/common.sh; mc_lock script2 0'") != 0);   /* ours now: a script can't */
	cmod_mc_unlock();
	CHECK(system("bash -c '. modules/lib/common.sh; mc_lock script3 0 && mc_unlock'") == 0);
	CHECK(cmod_mc_busy(NULL, 0) == 0);

	printf(failed ? "card bus lock: FAILED\n" : "card bus lock: ok\n");
	return failed;
}

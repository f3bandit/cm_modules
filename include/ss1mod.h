/* ss1mod: the SuperStation One's front-port and drive tools, as modules a front end (Console Mode) can call.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE); Retro Remake: see LICENSE-RETRO-REMAKE.md.
 * The controller tester and the memory card manager are f3bandit's idea and design.
 *
 * No screens here: every module gives the front end data and actions, the front end draws them.
 *
 *   memory cards     the SS1 Memory Card Tool core reads, checks and writes the card in front port 1;
 *                    the card format and the backup library are ps1_memcard_lib (vendor/)
 *   controllers      the SS1 Controller Tester core reads both front ports (buttons, sticks, card present)
 *   drive repair     the offline SD card repair: a copy of the SS1's Linux on a USB/NVMe drive repairs the SD
 *                    card at the next warm restart (scripts/sd_offline_repair.sh, the same as SS1 Tool's)
 *
 * While one of the cores runs, the front end isn't running (the core has the screen). So a job is handed
 * to ss1jobd (tools/ss1jobd.c), which starts the core, follows it, brings the front end back
 * ("load_core menu.rbf") and leaves the result in a key=value file the front end reads when it starts
 * again: ss1_job_start() / ss1_job_result(). */
#ifndef SS1MOD_H
#define SS1MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ paths */

#define SS1_FAT          "/media/fat"
#define SS1_HOME         SS1_FAT "/ConsoleMode/ss1"          /* ss1jobd, the scripts, results */
#define SS1_JOBD         SS1_HOME "/ss1jobd"
#define SS1_RESULT       SS1_HOME "/last_job.txt"            /* the last job's result (key=value) */
#define SS1_MC_DIR       SS1_FAT "/games/SS1_MemCard"        /* the card core's files */
#define SS1_PAD_DIR      SS1_FAT "/games/SS1_Controller_Tester"
#define SS1_MC_PREFIX    "SS1_MemCard_"                      /* _Utility/SS1_MemCard_<date>.rbf */
#define SS1_PAD_PREFIX   "SS1_Controller_Tester_"

/* ------------------------------------------------------------------ MiSTer */

/* the newest installed core whose file name starts with prefix (in _Utility); 0, or -1 when there's none */
int ss1_core_installed(const char *prefix, char *path, size_t n);
/* the core on screen (/tmp/CORENAME, "MENU" for the menu core); "" when unknown */
void ss1_core_running(char *name, size_t n);
/* load_core on /dev/MiSTer_cmd: an .rbf or an .mgl; 0 when MiSTer took the command */
int ss1_load(const char *what);
/* back to the front end: load_core menu.rbf until the core named `core` has closed (a few tries) */
int ss1_back_to_menu(const char *core);

/* ------------------------------------------------------------------ memory cards (SS1 Memory Card Tool core) */

enum { SS1_MC_BACKUP = 1, SS1_MC_VERIFY = 2, SS1_MC_WRITE = 3 };
/* results the core reports */
enum { SS1_MC_OK = 1, SS1_MC_OK_RETRIED = 2, SS1_MC_FAILED = 3, SS1_MC_NO_CARD = 4, SS1_MC_DIFFERS = 5,
       SS1_MC_BAD_FILE = 6, SS1_MC_READ_ONLY = 7, SS1_MC_SAVED_FAILED = 8, SS1_MC_WRITTEN = 9 };
/* the card map: one byte per sector (1024) */
enum { SS1_MAP_NONE = 0, SS1_MAP_OK = 1, SS1_MAP_RETRIED = 2, SS1_MAP_FAILED = 3, SS1_MAP_DIFFERS = 4, SS1_MAP_WRITTEN = 5 };
#define SS1_MC_JOB_SIZE 1536
#define SS1_MC_STEP_COPY1 1   /* flags: this backup is step 1 of a copy (shown on the TV) */
#define SS1_MC_STEP_COPY2 2   /*        this write is step 2 */

typedef struct {
	int finished;             /* 1: the report ("SMCR"); 0: progress ("SMCP") */
	int version, job, result;
	int card;                 /* 0 none, 1 formatted, 2 not formatted */
	uint32_t id;
	int ok, retried, failed, differ, retries, done;
	int slow, phase, sector, written;   /* phase: 2 loading the file, 3 reading the card, 4 saving the file */
	uint8_t map[1024];
} ss1_mc_report;

/* the job file's request: job, flags, id; force: sectors a write writes even where they match */
void ss1_mc_request(uint8_t req[SS1_MC_JOB_SIZE], int job, int flags, uint32_t id, const int *force, int nforce);
/* reads the job file as the core left it: 1 progress or report for this id, 0 nothing yet, -1 not a job file */
int ss1_mc_parse(const uint8_t *buf, size_t len, uint32_t id, ss1_mc_report *r);
/* a result in a few words ("Card backed up", "No card in front port 1"...) */
const char *ss1_mc_result_text(int job, int result);

/* ------------------------------------------------------------------ controllers (SS1 Controller Tester core) */

/* buttons as the core reports them, pressed = 1 */
enum {
	SS1_PAD_SELECT = 0x0001, SS1_PAD_L3 = 0x0002, SS1_PAD_R3 = 0x0004, SS1_PAD_START = 0x0008,
	SS1_PAD_UP = 0x0010, SS1_PAD_RIGHT = 0x0020, SS1_PAD_DOWN = 0x0040, SS1_PAD_LEFT = 0x0080,
	SS1_PAD_L2 = 0x0100, SS1_PAD_R2 = 0x0200, SS1_PAD_L1 = 0x0400, SS1_PAD_R1 = 0x0800,
	SS1_PAD_TRIANGLE = 0x1000, SS1_PAD_CIRCLE = 0x2000, SS1_PAD_CROSS = 0x4000, SS1_PAD_SQUARE = 0x8000
};
#define SS1_PAD_REPORT_SIZE 512

typedef struct {
	int present;              /* a controller answered */
	int id;                   /* its ID: 0x41 digital, 0x73 DualShock analog, 0x53 analog joystick, 0x23 NeGcon... */
	int analog;               /* sticks are valid (ID's low nibble >= 3) */
	uint16_t buttons;         /* SS1_PAD_* */
	int lx, ly, rx, ry;       /* 0..255, 128 in the middle */
	int reads;                /* reads so far (wraps at 256): a changing value means it's live */
	int card;                 /* memory card in this port: 0 none, 1 formatted, 2 not formatted */
	int analog_asked;         /* the core asked a DualShock for analog mode */
} ss1_pad_port;

typedef struct {
	int version;
	uint32_t id;
	ss1_pad_port port[2];
	int analog_switches;
	uint8_t pins_port1, pins_port2, pins_sel6;   /* wiring check: USER_IN pins pulled low (bit n = USER_IN[n]) */
	int sel6_answered, sel6_id;                   /* a controller answered with USER_OUT[6] as the select */
} ss1_pad_report;

void ss1_pad_request(uint8_t req[SS1_PAD_REPORT_SIZE], uint32_t id);
/* 1 a report for this id, 0 nothing yet, -1 not a report file */
int ss1_pad_parse(const uint8_t *buf, size_t len, uint32_t id, ss1_pad_report *r);
const char *ss1_pad_type(int id);           /* "Digital pad", "Analog (DualShock)"... */
const char *ss1_pad_button_name(uint16_t bit);

/* ------------------------------------------------------------------ jobs (ss1jobd) */

/* what ss1jobd runs; the front end starts it with ss1_job_start(), then MiSTer closes the front end */
typedef enum {
	SS1_JOB_MC_BACKUP,        /* card -> file: arg = where the 128 KB image goes */
	SS1_JOB_MC_VERIFY,        /* card vs file: arg = the image to compare with */
	SS1_JOB_MC_WRITE,         /* file -> card: arg = the image to write (back the card up first!) */
	SS1_JOB_PADTEST           /* the controller tester until Select+Start is held 2 s on either pad */
} ss1_job;

/* starts ss1jobd detached for that job (it outlives the caller) and returns at once: 0 started */
int ss1_job_start(ss1_job job, const char *arg, int flags, const int *force, int nforce);
/* the last job's result (SS1_RESULT), one key at a time: 0 found. Keys: job, state (running/done/failed),
 * result, message, card, ok, retried, failed, differ, written, file, at (unix time), error */
int ss1_job_result(const char *key, char *val, size_t n);

/* ------------------------------------------------------------------ drive repair (sd_offline_repair.sh) */

/* runs scripts/sd_offline_repair.sh with a command (status, setup <drive>, arm, disarm, restart, result,
 * remove) and collects its key=value output into out (n bytes, NUL-terminated): 0 when it ran */
int ss1_repair_run(const char *cmd, const char *arg, char *out, size_t n);
/* one key from such output ("armed", "drive", "last_result"...): 0 found */
int ss1_kv(const char *text, const char *key, char *val, size_t n);

#ifdef __cplusplus
}
#endif
#endif

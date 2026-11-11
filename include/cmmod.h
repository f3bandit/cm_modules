/* cmmod: the SuperStation One's front-port and drive tools, as modules a front end (Console Mode) can call.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md).
 * The controller tester and the memory card manager are f3bandit's idea and design.
 *
 * No screens here: every module gives the front end data and actions, the front end draws them.
 *
 *   memory cards     the Memory Card Tool core reads, checks and writes the card in front port 1;
 *                    the card format and the backup library are ps1_memcard_lib (vendor/)
 *   controllers      the Controller Tester core reads both front ports (buttons, sticks, card present)
 *   drive repair     the offline SD card repair: a copy of the SuperStation's Linux on a USB/NVMe drive repairs the SD
 *                    card at the next warm restart (scripts/sd_offline_repair.sh)
 *
 * While one of the cores runs, the front end isn't running (the core has the screen). So a job is handed
 * to cmjobd (tools/cmjobd.c), which starts the core, follows it, brings the front end back
 * ("load_core menu.rbf") and leaves the result in a key=value file the front end reads when it starts
 * again: cmod_job_start() / cmod_job_result(). */
#ifndef CMMOD_H
#define CMMOD_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ paths */

#define CMOD_FAT          "/media/fat"
#define CMOD_HOME         CMOD_FAT "/ConsoleMode/tools"          /* cmjobd, the scripts, results */
#define CMOD_JOBD         CMOD_HOME "/cmjobd"
#define CMOD_RESULT       CMOD_HOME "/last_job.txt"            /* the last job's result (key=value) */

/* every backup the modules and the apps make on the SuperStation, in one folder, one subfolder per kind */
#define CMOD_BACKUPS          CMOD_HOME "/backups"
#define CMOD_BK_RESTORE       CMOD_BACKUPS "/restore-points"   /* MiSTer's Main, menu core, kernel, Linux (update-guard) */
#define CMOD_BK_LINUX         CMOD_BACKUPS "/linux-settings"   /* settings inside Linux kept for after a Linux update */
#define CMOD_BK_MEMCARDS      CMOD_BACKUPS "/memory-cards"     /* the memory card library: mclib_set_dir(CMOD_BK_MEMCARDS) */
#define CMOD_BK_SAVES         CMOD_BACKUPS "/saves"
#define CMOD_BK_CONTROLLERS   CMOD_BACKUPS "/controllers"
#define CMOD_BK_WIFI          CMOD_BACKUPS "/wifi"             /* wpa_supplicant.conf before each WiFi change (the last 10) */
#define CMOD_MC_DIR       CMOD_FAT "/games/SS1_MemCard"        /* the card core's files */
#define CMOD_PAD_DIR      CMOD_FAT "/games/SS1_Controller_Tester"
#define CMOD_MC_PREFIX    "SS1_MemCard_"                      /* _Utility/SS1_MemCard_<date>.rbf */
#define CMOD_PAD_PREFIX   "SS1_Controller_Tester_"

/* ------------------------------------------------------------------ MiSTer */

/* the newest installed core whose file name starts with prefix (in _Utility); 0, or -1 when there's none */
int cmod_core_installed(const char *prefix, char *path, size_t n);
/* the core on screen (/tmp/CORENAME, "MENU" for the menu core); "" when unknown */
void cmod_core_running(char *name, size_t n);
/* load_core on /dev/MiSTer_cmd: an .rbf or an .mgl; 0 when MiSTer took the command */
int cmod_load(const char *what);
/* back to the front end: load_core menu.rbf until the core named `core` has closed (a few tries) */
int cmod_back_to_menu(const char *core);

/* ------------------------------------------------------------------ memory cards (Memory Card Tool core) */

enum { CMOD_MC_BACKUP = 1, CMOD_MC_VERIFY = 2, CMOD_MC_WRITE = 3 };
/* results the core reports */
enum { CMOD_MC_OK = 1, CMOD_MC_OK_RETRIED = 2, CMOD_MC_FAILED = 3, CMOD_MC_NO_CARD = 4, CMOD_MC_DIFFERS = 5,
       CMOD_MC_BAD_FILE = 6, CMOD_MC_READ_ONLY = 7, CMOD_MC_SAVED_FAILED = 8, CMOD_MC_WRITTEN = 9 };
/* the card map: one byte per sector (1024) */
enum { CMOD_MAP_NONE = 0, CMOD_MAP_OK = 1, CMOD_MAP_RETRIED = 2, CMOD_MAP_FAILED = 3, CMOD_MAP_DIFFERS = 4, CMOD_MAP_WRITTEN = 5 };
#define CMOD_MC_JOB_SIZE 1536
#define CMOD_MC_STEP_COPY1 1   /* flags: this backup is step 1 of a copy (shown on the TV) */
#define CMOD_MC_STEP_COPY2 2   /*        this write is step 2 */

typedef struct {
	int finished;             /* 1: the report ("SMCR"); 0: progress ("SMCP") */
	int version, job, result;
	int card;                 /* 0 none, 1 formatted, 2 not formatted */
	uint32_t id;
	int ok, retried, failed, differ, retries, done;
	int slow, phase, sector, written;   /* phase: 2 loading the file, 3 reading the card, 4 saving the file */
	uint8_t map[1024];
} cmod_mc_report;

/* the job file's request: job, flags, id; force: sectors a write writes even where they match */
void cmod_mc_request(uint8_t req[CMOD_MC_JOB_SIZE], int job, int flags, uint32_t id, const int *force, int nforce);
/* reads the job file as the core left it: 1 progress or report for this id, 0 nothing yet, -1 not a job file */
int cmod_mc_parse(const uint8_t *buf, size_t len, uint32_t id, cmod_mc_report *r);
/* a result in a few words ("Card backed up", "No card in front port 1"...) */
const char *cmod_mc_result_text(int job, int result);

/* ------------------------------------------------------------------ controllers (Controller Tester core) */

/* buttons as the core reports them, pressed = 1 */
enum {
	CMOD_PAD_SELECT = 0x0001, CMOD_PAD_L3 = 0x0002, CMOD_PAD_R3 = 0x0004, CMOD_PAD_START = 0x0008,
	CMOD_PAD_UP = 0x0010, CMOD_PAD_RIGHT = 0x0020, CMOD_PAD_DOWN = 0x0040, CMOD_PAD_LEFT = 0x0080,
	CMOD_PAD_L2 = 0x0100, CMOD_PAD_R2 = 0x0200, CMOD_PAD_L1 = 0x0400, CMOD_PAD_R1 = 0x0800,
	CMOD_PAD_TRIANGLE = 0x1000, CMOD_PAD_CIRCLE = 0x2000, CMOD_PAD_CROSS = 0x4000, CMOD_PAD_SQUARE = 0x8000
};
#define CMOD_PAD_REPORT_SIZE 512

typedef struct {
	int present;              /* a controller answered */
	int id;                   /* its ID: 0x41 digital, 0x73 DualShock analog, 0x53 analog joystick, 0x23 NeGcon... */
	int analog;               /* sticks are valid (ID's low nibble >= 3) */
	uint16_t buttons;         /* CMOD_PAD_* */
	int lx, ly, rx, ry;       /* 0..255, 128 in the middle */
	int reads;                /* reads so far (wraps at 256): a changing value means it's live */
	int card;                 /* memory card in this port: 0 none, 1 formatted, 2 not formatted */
	int analog_asked;         /* the core asked a DualShock for analog mode */
} cmod_pad_port;

typedef struct {
	int version;
	uint32_t id;
	cmod_pad_port port[2];
	int analog_switches;
	uint8_t pins_port1, pins_port2, pins_sel6;   /* wiring check: USER_IN pins pulled low (bit n = USER_IN[n]) */
	int sel6_answered, sel6_id;                   /* a controller answered with USER_OUT[6] as the select */
} cmod_pad_report;

void cmod_pad_request(uint8_t req[CMOD_PAD_REPORT_SIZE], uint32_t id);
/* 1 a report for this id, 0 nothing yet, -1 not a report file */
int cmod_pad_parse(const uint8_t *buf, size_t len, uint32_t id, cmod_pad_report *r);
const char *cmod_pad_type(int id);           /* "Digital pad", "Analog (DualShock)"... */
const char *cmod_pad_button_name(uint16_t bit);

/* ------------------------------------------------------------------ jobs (cmjobd) */

/* what cmjobd runs; the front end starts it with cmod_job_start(), then MiSTer closes the front end */
typedef enum {
	CMOD_JOB_MC_BACKUP,        /* card -> file: arg = where the 128 KB image goes */
	CMOD_JOB_MC_VERIFY,        /* card vs file: arg = the image to compare with */
	CMOD_JOB_MC_WRITE,         /* file -> card: arg = the image to write (back the card up first!) */
	CMOD_JOB_PADTEST           /* the controller tester until Select+Start is held 2 s on either pad */
} cmod_job;

/* starts cmjobd detached for that job (it outlives the caller) and returns at once: 0 started */
int cmod_job_start(cmod_job job, const char *arg, int flags, const int *force, int nforce);
/* the last job's result (CMOD_RESULT), one key at a time: 0 found. Keys: job, state (running/done/failed),
 * result, message, card, ok, retried, failed, differ, written, file, at (unix time), error */
int cmod_job_result(const char *key, char *val, size_t n);

/* ------------------------------------------------------------------ drive repair (sd_offline_repair.sh) */

/* runs scripts/sd_offline_repair.sh with a command (status, setup <drive>, arm, disarm, restart, result,
 * remove) and collects its key=value output into out (n bytes, NUL-terminated): 0 when it ran */
int cmod_repair_run(const char *cmd, const char *arg, char *out, size_t n);
/* one key from such output ("armed", "drive", "last_result"...): 0 found */
int cmod_kv(const char *text, const char *key, char *val, size_t n);

/* ------------------------------------------------------------------ the app link (cmremote) */

/* The phone and PC apps reach the game library over the home network through cmremote, a small program
 * that keeps running (remote/). Not CM Link: that's a developer tool for debugging and testing. */
#define CMOD_REMOTE        CMOD_HOME "/cmremote"
#define CMOD_REMOTE_SOCK   "/tmp/cmremote.sock"

/* starts cmremote if it isn't running (call it when the front end starts): 0 running */
int cmod_remote_start(void);
/* one command, its key=value answer into out (read keys with cmod_kv()): 0 answered.
 *   "info"         on, port, listening, error, code (the pairing code, while open), code_left (seconds),
 *                  paired (the device that just paired), devices (count), device=<id> <paired> <last used> <name>
 *                  (one line each), address=<ip:port> (one line per network connection)
 *   "on" / "off"   the app link on or off (remembered); answers result=ok, then info
 *   "pair"         opens pairing for 2 minutes: show code= on the TV; paired= is set once a device paired
 *   "pair_stop"    closes it
 *   "forget <id>"  forgets a device (id: from its device= line)
 *   "rescan"       looks at the games again (otherwise every 10 minutes) */
int cmod_remote_cmd(const char *cmd, char *out, size_t n);
/* any app-link command, the same JSON the apps send ({"cmd":"wifi","do":"scan"}, {"cmd":"bluetooth"},
   {"cmd":"controllers"}, {"cmd":"ua"}, {"cmd":"guard"}, {"cmd":"dashboard"}...: remote/*.c list them all); the JSON
   reply in out. One call for the TV's screens to reach everything the PC app can. 0 ok. */
int cmod_remote_api(const char *json, char *out, size_t n, int timeout_s);

/* ---------- WiFi (src/cmwifi.c) ----------
 * Scan, connect, forget, order and undo on MiSTer's own WiFi setup: the networks live in
 * /media/fat/linux/wpa_supplicant.conf (the file Console Mode's WiFi menu uses too), wpa_supplicant is told
 * to read it again, dhcpcd picks the connection up. Each change backs the file up first (CMOD_BK_WIFI) and
 * cmod_wifi_undo() puts the last copy back. Passwords are stored as WPA keys, never as text, never returned.
 * Thread-safe. A front end's WiFi screen: status at the top, Scan, the networks found (strongest first,
 * the saved ones marked), Connect asks for the password when secure, saved networks: Forget, Move up/down. */
#define CMOD_WIFI_SSID 33
#define CMOD_WIFI_MAX 48
#define CMOD_WIFI_SAVED_MAX 32
typedef struct {
	char iface[16], ssid[CMOD_WIFI_SSID], ip[48];
	int present, connected, signal, freq, supplicant;    /* signal: dBm; freq: MHz */
} cmod_wifi_status_t;
typedef struct {
	char ssid[CMOD_WIFI_SSID];
	int signal;            /* dBm, the strongest access point with this name */
	int bands;             /* 1 = 2.4 GHz, 2 = 5 GHz, 3 = both */
	int secure, psk, wpa3, enterprise;
} cmod_wifi_net_t;
typedef struct { char ssid[CMOD_WIFI_SSID]; int priority, open; } cmod_wifi_saved_t;

void cmod_wifi_status(cmod_wifi_status_t *s);
int cmod_wifi_scan_start(void);                   /* in the background (a few seconds); 1 if already scanning */
/* the last scan's networks (count); busy: still scanning; at: when it finished (0: never) */
int cmod_wifi_scan_results(cmod_wifi_net_t *out, int max, int *busy, time_t *at, char *err, size_t errn);
int cmod_wifi_saved(cmod_wifi_saved_t *out, int max);   /* the first is tried first */
int cmod_wifi_connect(const char *ssid, const char *password, int hidden, char *err, size_t errn);   /* 0 ok */
int cmod_wifi_forget(const char *ssid, char *err, size_t errn);
int cmod_wifi_move(const char *ssid, int dir, char *err, size_t errn);   /* dir -1 up (tried sooner), 1 down */
int cmod_wifi_undo(char *err, size_t errn);
void cmod_wifi_start(void);                       /* optional: starts wpa_supplicant once a late USB adapter shows up */
void cmod_wifi_set_log(void (*fn)(const char *msg));    /* where it says what it changed (none by default) */


/* ---------- Bluetooth controllers (src/cmbt.c) ----------
 * Pairing runs MiSTer's own btctl pair (what Console Mode's pairing and MiSTer's OSD run): it finds a controller in
 * pairing mode, pairs, trusts and connects it, stopped after the first one (or 90 s, or cancel). The list comes from
 * bluetoothctl; forget and disconnect use btctl. Pairings stay where MiSTer keeps them (linux/bluetooth). A pairing
 * already running (Console Mode's own) is waited for, never doubled. Thread-safe. */
#define CMOD_BT_MAX 24
typedef struct {
	char mac[18], name[64], icon[32];     /* icon: bluetoothctl's (input-gaming, input-keyboard...) */
	int connected, input;
} cmod_bt_dev_t;
typedef struct {
	int present;           /* a Bluetooth adapter */
	int busy;              /* pairing running */
	int ok;                /* the last pairing worked (when !busy and at) */
	time_t at;             /* when the last pairing ended, 0: none yet */
	char found[64];        /* the controller it found ("name (mac)") */
	char message[160];     /* what's happening / what happened */
} cmod_bt_state_t;
void cmod_bt_state(cmod_bt_state_t *s);
int cmod_bt_devices(cmod_bt_dev_t *out, int max);    /* the paired devices (refreshed in the background) */
void cmod_bt_refresh_soon(void);
int cmod_bt_pair_start(void);                         /* background, up to 90 s; 1 already pairing, -1 no adapter, -2 Console Mode is pairing, -3 SS1 Tool is changing the pairings */
void cmod_bt_pair_cancel(void);
int cmod_bt_forget(const char *mac, char *err, size_t n);
int cmod_bt_disconnect(const char *mac, char *err, size_t n);
void cmod_bt_set_log(void (*fn)(const char *msg));

#ifdef __cplusplus
}
#endif

#endif

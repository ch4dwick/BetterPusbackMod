/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2022 Saso Kiselkov. All rights reserved.
 * Copyright 2024 Robert Wellinger. All rights reserved.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <XPLMMenus.h>
#include <XPLMUtilities.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>

#include <acfutils/assert.h>
#include <acfutils/core.h>
#include <acfutils/crc64.h>
#include <acfutils/dr_cmd_reg.h>
#include <acfutils/glew.h>
#include <acfutils/helpers.h>
#include <acfutils/intl.h>
#include <acfutils/log.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/wav.h>
#include <acfutils/time.h>

#include "bp.h"
#include "bp_cam.h"
#include "cab_view.h"
#include "cfg.h"
#include "ff_a320_intf.h"
#include "msg.h"
#include "tug.h"
#include "xplane.h"
#include "wed2route.h"

/* Enables leaving bp_tug_name set to facilitate local master/slave debug */
/*#define	SLAVE_DEBUG*/

#define STATUS_CHECK_INTVAL 1 /* second */
enum
{
    COUPLED_STATE_OFF = 0,       /* disconnected */
    COUPLED_STATE_SLAVE = 1,     /* connected and we're slave */
    COUPLED_STATE_MASTER = 2,    /* connected and we're master */
    COUPLED_STATE_PASSENGER = -1 /* connected and we're passenger */
};

static bool_t inited = B_FALSE;

XPLMCommandRef start_pb, start_cam, conn_first, stop_pb;
static XPLMCommandRef stop_cam;
static XPLMCommandRef cab_cam, recreate_routes;
static XPLMCommandRef abort_push, pref_cmd;
static XPLMCommandRef manual_push_start, manual_push_start_no_yoke;
static XPLMCommandRef manual_push_left, manual_push_right, manual_push_reverse;
static XPLMMenuID root_menu;
static int plugins_menu_item;
static int start_pb_plan_menu_item, stop_pb_plan_menu_item;
static int start_pb_menu_item, stop_pb_menu_item, conn_first_menu_item;
static int cab_cam_menu_item, prefs_menu_item;
static bool_t prefs_enable, stop_pb_plan_enable,
    stop_pb_enable, conn_first_enable, cab_cam_enable;
bool_t start_pb_plan_enable, start_pb_enable;

static bool_t pref_widget_active_status = B_FALSE;
bool_t hide_main_intf = B_FALSE;

bool_t get_pref_widget_status(void);
void set_pref_widget_status(bool_t active);

static int start_pb_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int stop_pb_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int start_cam_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int stop_cam_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int conn_first_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int cab_cam_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int recreate_routes_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int abort_push_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static int manual_push_left_handler(XPLMCommandRef, XPLMCommandPhase, void *);
static int manual_push_right_handler(XPLMCommandRef, XPLMCommandPhase, void *);
static int manual_push_reverse_handler(XPLMCommandRef, XPLMCommandPhase, void *);
static int manual_push_start_handler(XPLMCommandRef, XPLMCommandPhase, void *);
static int manual_push_start_no_yoke_handler(XPLMCommandRef, XPLMCommandPhase, void *);
static int manual_push_reverse_handler(XPLMCommandRef, XPLMCommandPhase, void *);

static bool_t start_after_cam = B_FALSE;

static char xpdir[512];
static char plugindir[512];
const char *const bp_xpdir = xpdir;
const char *const bp_plugindir = plugindir;

static bool_t smartcopilot_present;
static dr_t smartcopilot_state;
static bool_t sharedflight_present;
static dr_t sharedflight_state;

int bp_xp_ver, bp_xplm_ver;
XPLMHostApplicationID bp_host_id;
airportdb_t *airportdb = NULL;

static bool_t bp_priv_enable(void);

static void bp_priv_disable(void);

static float bp_do_reload(float, float, int, void *);

static bool_t reload_rqst = B_FALSE;
static XPLMCreateFlightLoop_t reload_floop = {
    .structSize = sizeof(reload_floop),
    .phase = xplm_FlightLoop_Phase_AfterFlightModel,
    .callbackFunc = bp_do_reload,
    .refcon = NULL};
static XPLMFlightLoopID reload_floop_ID = NULL;

/*
 * These datarefs are for syncing two instances of BetterPushback over the
 * net via syncing addons such as smartcopilot and Shared Flight.
 * This works as follows:
 * 1) Master/slave must not be switched during pushback (undefined behavior
 *	may result). It's possible to observe if BetterPushback is running by
 *	monitoring the read-only boolean "bp/started" dataref. This dataref
 *	must NOT be synced, it's only a hint to smartcopilot whether
 *	switching is safe.
 * 2) The boolean dataref "bp/slave_mode" must be set to 0 on the master and
 *	1 on the slave.
 * 3) The boolean dataref "bp/op_complete" must be synced from master to
 *	slave. This signals to bp_run() that the pushback stage needs to
 *	progress to either PB_STEP_STOPPING if the tug has already attached,
 *	or immediately to bp_complete() if it has not. The master sets this
 *	dataref in response to either a "Stop" command request, or when it
 *	has processed all pushback segments. The slave cannot set this
 *	(master controls when pushback stops).
 * 4) The boolean dataref "bp/plan_complete" must be synced from master to
 *	slave. This is a signal from the master machine to the slave that
 *	if late_plan_requested was in effect, the slave can continue with
 *	the state transitions past the late_plan_requested limit. This is
 *	needed because we don't transfer the route to the slave, so the
 *	slave cannot use the presence of a route as a condition to continue.
 * 5) The string dataref "bp/tug_name" must be synced from master to slave.
 *	This string identifies which tug model the master selected (since tug
 *	selection is non-deterministic). The slave then instances its tug
 *	object using tug_alloc_man. Both master and slave must have identical
 *	tug libraries, otherwise sync fails.
 * 6) The command "BetterPushback/start" should be synced from mater to slave.
 *	There's no need to sync any other commands. The planning GUI is
 *	disabled on the slave machine and stopping of the pushback can only be
 *	performed by the master machine.
 * 7) The dataref bp/parking_brake_override can be set to 1 on slaves and
 *  then bp/parking_brake_set will be used instead of the local parking brake.
 */
static dr_t bp_started_dr, bp_connected_dr, slave_mode_dr, op_complete_dr;
static dr_t plan_complete_dr, bp_tug_name_dr;
static dr_t pb_set_remote_dr, pb_set_override_dr;
bool_t bp_started = B_FALSE;
bool_t bp_connected = B_FALSE;
bool_t slave_mode = B_FALSE;
bool_t op_complete = B_FALSE;
bool_t plan_complete = B_FALSE;
bool_t pb_set_remote = B_FALSE;
bool_t pb_set_override = B_FALSE;
char bp_tug_name[64] = {0};

/*
 * Hides or unhides the default X-Plane 11 tug. This is done by renaming the
 * original OBJ file containing the tug to some temporary filename and putting
 * an empty OBJ file in its place. To unhide the tug, we simply undo this
 * operation. We hide the tug while starting up and before X-Plane attempts to
 * load the tug OBJ and later undo this when shutting down.
 */
static void
set_xp11_tug_hidden(bool_t flag)
{
    static bool_t hidden = B_FALSE;
    char *filename, *filename_backup;

    if (flag == hidden)
    {
        return;
    }
    else if (bp_xp_ver < 11000 || bp_xp_ver >= 12000)
    {
        logMsg(BP_WARN_LOG "Hidden xp11 default tug not supported. XP Version %d", bp_xp_ver);
        return;
    }

    ASSERT3U(bp_xp_ver, >=, 11000);

    filename = mkpathname(bp_xpdir, "Resources", "default scenery",
                          "sim objects", "apt_vehicles", "pushback", "Tug_GT110.obj", NULL);
    filename_backup = mkpathname(bp_xpdir, "Resources", "default scenery",
                                 "sim objects", "apt_vehicles", "pushback",
                                 "Tug_GT110-BetterPushback-backup.obj", NULL);

    if (flag)
    {
        FILE *fp;

        if (!file_exists(filename, NULL))
        {
            logMsg(BP_WARN_LOG "Failed to hide default X-Plane 11 tug: "
                               "original tug file doesn't exist.");
            goto out;
        }
        if (file_exists(filename_backup, NULL))
        {
            logMsg(BP_WARN_LOG "Failed to hide default X-Plane 11 tug: "
                               "backup tug file already exists.");
            goto out;
        }
        if (rename(filename, filename_backup) != 0)
        {
            logMsg(BP_WARN_LOG "Failed to hide default X-Plane 11 tug: "
                               "cannot rename original tug file: %s.",
                   strerror(errno));
            goto out;
        }
        fp = fopen(filename, "wb");
        if (fp == NULL)
        {
            logMsg(BP_WARN_LOG "Failed to hide default X-Plane 11 tug: "
                               "cannot write substitute tug object file.");
            rename(filename_backup, filename);
            goto out;
        }
        fprintf(fp, "A\n800\nOBJ\n");
        fclose(fp);
    }
    else
    {
        if (!file_exists(filename, NULL) ||
            !file_exists(filename_backup, NULL))
        {
            logMsg(BP_WARN_LOG "Failed to unhide default X-Plane 11 "
                               "tug: subtitute file or backup file don't exist");
            goto out;
        }
        if (!remove_file(filename, B_FALSE))
        {
            logMsg(BP_WARN_LOG "Failed to unhide default X-Plane 11 "
                               "tug: cannot remove subtitute file");
            goto out;
        }
        if (rename(filename_backup, filename) != 0)
        {
            logMsg(BP_WARN_LOG "Failed to unhide default X-Plane 11 "
                               "tug: couldn't rename original file: %s",
                   strerror(errno));
            goto out;
        }
    }

out:
    hidden = flag;
    free(filename);
    free(filename_backup);
}

static void
init_core_state(void)
{
    bp_started = B_FALSE;
    bp_connected = B_FALSE;
    slave_mode = B_FALSE;
    op_complete = B_FALSE;
    plan_complete = B_FALSE;
    cab_view_init();
}

static void
enable_menu_items()
{
    XPLMEnableMenuItem(root_menu, prefs_menu_item, prefs_enable);
    XPLMEnableMenuItem(root_menu, start_pb_menu_item, start_pb_enable);
    XPLMEnableMenuItem(root_menu, stop_pb_menu_item, stop_pb_enable);
    XPLMEnableMenuItem(root_menu, start_pb_plan_menu_item, start_pb_plan_enable);
    XPLMEnableMenuItem(root_menu, stop_pb_plan_menu_item, stop_pb_plan_enable);
    XPLMEnableMenuItem(root_menu, conn_first_menu_item, conn_first_enable);
    XPLMEnableMenuItem(root_menu, cab_cam_menu_item, cab_cam_enable);
}

static int
start_pb_handler_(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);

    if (phase != xplm_CommandEnd )
        return (1);

   if (tug_pending_mode) {
        tug_pending_mode = B_FALSE;
        return (1);
    }
    if (phase != xplm_CommandEnd || !bp_init())
        return (1);

    if (get_pref_widget_status()) // do nothing if preference widget is active
        return (1);

    if (!start_pb_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/start\" is currently disabled");
        if (push_manual.requested) {
            manual_bp_stop();
        }
        return (1);
    }

    if (push_manual.requested) {
        manual_bp_start();
    }
 

    stop_cam_handler(NULL, xplm_CommandEnd, NULL); // synchronously stop a possible open cam

    // if late_plan_requested always present plan for final review
    if ((late_plan_requested || bp_num_segs() == 0) && !slave_mode)
    {
        if (!push_manual.active) {
            if (!bp_cam_start())
            return (1);
        }
        prefs_enable = B_FALSE;
        start_pb_plan_enable = B_FALSE;
        stop_pb_plan_enable = B_TRUE;
        start_pb_enable = B_FALSE;
        conn_first_enable = B_FALSE;
        stop_pb_enable = B_FALSE;
        enable_menu_items();
        start_after_cam = B_TRUE;
        if (!push_manual.active) {
            msg_play(MSG_PLAN_START);
            return (1);
        }
    }
    op_complete = B_FALSE;
    late_plan_requested = B_FALSE;
    if (!bp_start())
        return (1);

    prefs_enable = B_FALSE;
    start_pb_plan_enable = B_FALSE;
    stop_pb_plan_enable = B_FALSE;
    start_pb_enable = B_FALSE;
    conn_first_enable = B_FALSE;
    stop_pb_enable = !slave_mode;
    enable_menu_items();
    return (1);
}

static int
start_pb_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    if (phase != xplm_CommandEnd )
    return (1);

    bool_t always_connect_tug_first = B_FALSE;
    (void)conf_get_b(bp_conf, "always_connect_tug_first", &always_connect_tug_first);

    if ( ((tug_auto_start && tug_starts_next_plane) || always_connect_tug_first) && !bp_started)
    {
        return conn_first_handler(cmd, phase, refcon);
    }
    else
    {
        return start_pb_handler_(cmd, phase, refcon);
    }
}

static int
manual_push_start_handler_(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon, bool_t with_yoke)
{
    if (phase != xplm_CommandEnd )
        return (1);

    if (!push_manual.active) {
        manual_bp_request(with_yoke);
        return start_pb_handler_(cmd, phase, refcon);
    } else {
        push_manual.pause = !push_manual.pause;
        logMsg("Manual push: Status %s", push_manual.pause ? "paused" : "pushing");
        return (1);
    }
}

static int
manual_push_start_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    if (phase != xplm_CommandEnd )
        return (1);
    
    return manual_push_start_handler_(cmd, phase, refcon, true);
}

static int
manual_push_start_no_yoke_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    if (phase != xplm_CommandEnd )
        return (1);
    
    return manual_push_start_handler_(cmd, phase, refcon, false);
}

static int
stop_pb_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);

    if (slave_mode || phase != xplm_CommandEnd || !bp_init())
        return (1);

    if (!stop_pb_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/stop\" is currently disabled");
        return (1);
    }

    if (push_manual.active) {
        manual_bp_stop();
    }

    (void)bp_stop();
    op_complete = B_TRUE;
    if (!slave_mode)
    {
        /* Reset the menu back */
        late_plan_requested = B_FALSE;
        start_pb_enable = B_TRUE;
        conn_first_enable = B_TRUE;
        prefs_enable = B_TRUE;
        enable_menu_items();
    }
    return (1);
}

static int
manual_push_reverse_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);

    if (push_manual.active)  {
        if ((bp_ls.tug->pos.spd > 0.1) || (bp_ls.tug->pos.spd < -0.1)) {
            logMsg("Manual push:  in progress, tug is moving to fast toggling direction is not yet possibe");
            return (1);
        }
        push_manual.forward_direction = !push_manual.forward_direction;
        logMsg("Manual push:  Toggling direction to %s", push_manual.forward_direction ? "forward" : "backward");
    } else {
        logMsg("Manual push:  Not in progress, toggling direction is disabled");
    }
    return (1);
}


static int
start_cam_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);

    if (get_pref_widget_status()) // do nothing if preference widget is active
        return (1);

    if (slave_mode || late_plan_requested)
        return (1);

    if (phase != xplm_CommandEnd || !bp_init())
        return (1);

    if (!start_pb_plan_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/start_planner\" is currently disabled");
        return (1);
    }

    if (!bp_cam_start())
    {
        start_after_cam = B_FALSE;
        return (1);
    }

    prefs_enable = B_FALSE;
    start_pb_plan_enable = B_FALSE;
    stop_pb_plan_enable = B_TRUE;
    start_pb_enable = B_FALSE;
    conn_first_enable = B_TRUE;
    stop_pb_enable = B_FALSE;
    enable_menu_items();
    return (1);
}

static int
stop_cam_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);

    if (slave_mode || phase != xplm_CommandEnd || !bp_init())
        return (1);

    if (!stop_pb_plan_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/stop_planner\" is currently disabled");
        return (1);
    }

    if (!bp_cam_stop())
        return (1);

    prefs_enable = B_TRUE;
    start_pb_plan_enable = B_TRUE;
    stop_pb_plan_enable = B_FALSE;
    start_pb_enable = B_TRUE;
    conn_first_enable = B_TRUE;
    stop_pb_enable = B_FALSE;
    enable_menu_items();

    if (late_plan_requested)
    {
        prefs_enable = B_FALSE;
        start_pb_plan_enable = B_FALSE;
        stop_pb_plan_enable = B_FALSE;
        start_pb_enable = (bp_num_segs() == 0);
        conn_first_enable = B_FALSE;
        stop_pb_enable = B_TRUE;
        enable_menu_items();
    }
    else if (start_after_cam)
    {
        if (bp_num_segs() != 0)
            XPLMCommandOnce(start_pb);
    }
    else if (bp_can_start(NULL))
    {
        msg_play(MSG_PLAN_END);
    }

    start_after_cam = B_FALSE;

    return (1);
}

static int
conn_first_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd || !bp_init() || bp_started || slave_mode)
        return (0);

    if (!conn_first_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/connect_first\" is currently disabled");
        return (0);
    }

    if (get_pref_widget_status())
    { // do nothing if preference widget is active
        return (1);
    }

    late_plan_requested = B_TRUE;
    (void)bp_cam_stop();

    /*
     * The conn_first procedure results in 2 calls to bp_start(). First here to get the tug connected,
     * then the second is issued by the user to get things moving.
     * An *active* preplanned route interferes with the holding point after lift in
     * pb_step_lift() / late_plan_end_cond() .
     * So we save an active preplanned route here and clear it.
     * It will be loaded again when the planner is started for the final review in the
     * user invocation of bp_start() so the planning is not lost but it may be modified.
     */
    if (bp_num_segs())
    {
        route_save(&bp.segs);
        bp_delete_all_segs();
    }

    if (!bp_start())
    {
        late_plan_requested = B_FALSE;
        return (1);
    }

    if (!slave_mode)
    {
        start_pb_plan_enable = B_FALSE;
        stop_pb_plan_enable = B_FALSE;
        start_pb_enable = (bp_num_segs() == 0);
        stop_pb_enable = B_TRUE;
        conn_first_enable = B_FALSE;
        prefs_enable = B_FALSE;
        enable_menu_items();
    }
    return (1);
}

static int
cab_cam_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);

    if (!cab_cam_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/cab_cam\" is currently disabled");
        return (0);
    }

    if (!cab_view_start())
    {
        XPLMSpeakString(_("ERROR: Unable to select pushback tug view at this time."));
        return (0);
    }

    tug_view_callback_is_alive = B_TRUE;
    return (1);
}

static int
recreate_routes_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);
    xlate_wedroutes();
    return (1);
}

static int
preference_window_handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);

    if (!prefs_enable)
    {
        logMsg(BP_WARN_LOG "Command \"BetterPushback/preference\" is currently disabled");
        return (0);
    }

    bp_conf_open();
    return (1);
}

static void
menu_cb(void *inMenuRef, void *inItemRef)
{
    UNUSED(inMenuRef);
    if (inItemRef == NULL)
        return;
    else
        XPLMCommandOnce((XPLMCommandRef)inItemRef);
}

void bp_done_notify(void)
{
    if (!slave_mode)
    {
        prefs_enable = B_TRUE;
        start_pb_enable = B_TRUE;
        conn_first_enable = B_TRUE;
        stop_pb_enable = B_FALSE;
        start_pb_plan_enable = B_TRUE;
        stop_pb_plan_enable = B_FALSE;
        enable_menu_items();
    }

#ifndef SLAVE_DEBUG
    bp_tug_name[0] = '\0';
#endif
}

/*
 * Notification from BP engine that a reconnect has been requested at the
 * appropriate time. Behave as if the user had hit "connect first" and on
 * the master's machine invoke the planner.
 */
void bp_reconnect_notify(void)
{
    if (slave_mode)
        return;

    late_plan_requested = B_TRUE;
    VERIFY(bp_cam_start());
    msg_play(MSG_PLAN_START);
    start_pb_plan_enable = B_FALSE;
    stop_pb_plan_enable = B_TRUE;
    start_pb_enable = B_FALSE;
    conn_first_enable = B_TRUE;
    stop_pb_enable = B_TRUE;
    enable_menu_items();
}

const char *
bp_get_lang(void)
{
    const char *c;
    if (conf_get_str(bp_conf, "lang", &c))
        return (c);
    return (acfutils_xplang2code(XPLMGetLanguage()));
}

static void
coupled_state_change()
{
    /* If we were in slave mode, reenable the menu items. */
    start_pb_enable = slave_mode ? 1 : 0;
    conn_first_enable = start_pb_enable;
    start_pb_plan_enable = start_pb_enable;

    stop_pb_enable = B_FALSE;
    stop_pb_plan_enable = B_FALSE;
    enable_menu_items();
}

void slave_mode_cb(dr_t *dr, void *unused)
{
    UNUSED(unused);
    UNUSED(dr);
    VERIFY(!bp_started);

    if (slave_mode)
        bp_fini();

    coupled_state_change();
}

void pb_set_remote_cb(dr_t *dr, void *unused)
{
    UNUSED(unused);
    UNUSED(dr);
}

void pb_set_override_cb(dr_t *dr, void *unused)
{
    UNUSED(unused);
    UNUSED(dr);
}

static float
status_check(float elapsed, float elapsed2, int counter, void *refcon)
{
    UNUSED(elapsed);
    UNUSED(elapsed2);
    UNUSED(counter);
    UNUSED(refcon);

    cab_cam_enable = cab_view_can_start();
    enable_menu_items();

    if (!tug_view_callback_is_alive && tug_cam_started)
    {
        cab_view_stop();
    }
    tug_view_callback_is_alive = B_FALSE;

    if ((!setup_view_callback_is_alive) && (get_pref_widget_status()))
    {
        set_pref_widget_status(B_FALSE);
    }
    setup_view_callback_is_alive = B_FALSE;

    if (bp_plan_callback_is_alive > 0)
    {
        bp_plan_callback_is_alive--;
    }

    if (!conf_get_b_per_acf("hide_magic_squares", &hide_main_intf))
    {
        hide_main_intf = B_FALSE;
    }

    main_intf(hide_main_intf);

    // Status check only needed if we have a known system of coupling installed...
    if (!smartcopilot_present && !sharedflight_present)
        return (1);

    if (smartcopilot_present && dr_geti(&smartcopilot_state) == COUPLED_STATE_SLAVE &&
        !slave_mode)
    {
        if (bp_started)
        {
            XPLMSpeakString(_("Pushback failure: smartcopilot "
                              "attempted to switch master/slave or network "
                              "connection lost. Stopping operation."));
        }
        /*
         * If we were in master mode, stop the camera, flush out all
         * pushback segments and inhibit all menu items. The master
         * will control us.
         */
        bp_fini();
        slave_mode = B_TRUE;
        coupled_state_change();
    }
    else if (sharedflight_present && dr_geti(&sharedflight_state) == COUPLED_STATE_SLAVE &&
             !slave_mode)
    {
        if (bp_started)
        {
            XPLMSpeakString(_("Pushback failure: Shared Flight "
                              "attempted to switch pilot flying or network "
                              "connection lost. Stopping operation."));
        }
        bp_fini();
        slave_mode = B_TRUE;
        coupled_state_change();
    }
    else if ((smartcopilot_present && dr_geti(&smartcopilot_state) != COUPLED_STATE_SLAVE) &&
             (!sharedflight_present || dr_geti(&sharedflight_state) != COUPLED_STATE_SLAVE) &&
             slave_mode)
    {
        if (bp_started)
        {
            XPLMSpeakString(_("Pushback failure: smartcopilot "
                              "attempted to switch master/slave or network "
                              "connection lost. Stopping operation."));
        }
        bp_fini();
        slave_mode = B_FALSE;
        coupled_state_change();
    }
    else if ((sharedflight_present && dr_geti(&sharedflight_state) != COUPLED_STATE_SLAVE && dr_geti(&sharedflight_state) != COUPLED_STATE_PASSENGER) &&
             (!smartcopilot_present || dr_geti(&smartcopilot_state) != COUPLED_STATE_SLAVE) &&
             slave_mode)
    {
        if (bp_started)
        {
            XPLMSpeakString(_("Pushback failure: Shared Flight "
                              "attempted to switch pilot flying or network "
                              "connection lost. Stopping operation."));
        }
        bp_fini();
        slave_mode = B_FALSE;
        coupled_state_change();
    }

    return (STATUS_CHECK_INTVAL);
}

static void
xlate_init(void)
{
    char *po_file = mkpathname(xpdir, plugindir, "data", "po",
                               bp_get_lang(), "strings.po", NULL);
    (void)acfutils_xlate_init(po_file);
    free(po_file);
}

PLUGIN_API int
XPluginStart(char *name, char *sig, char *desc)
{
    char *p;
    GLenum err;

    log_init(XPLMDebugString, "BetterPushback");
    logMsg(BP_INFO_LOG "This is Better Pushback (MOD) -" BP_PLUGIN_VERSION " libacfutils-%s - for X-Plane 11/12",
           libacfutils_version);

    crc64_init();
    crc64_srand(microclock());

    err = glewInit();
    if (err != GLEW_OK)
    {
        /* Problem: glewInit failed, something is seriously wrong. */
        logMsg(BP_FATAL_LOG "cannot initialize libGLEW: %s", glewGetErrorString(err));
        return (0);
    }

    /* Always use Unix-native paths on the Mac! */
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    XPLMGetSystemPath(xpdir);
    XPLMGetPluginInfo(XPLMGetMyID(), NULL, plugindir, NULL, NULL);

#if IBM
    fix_pathsep(xpdir);
    fix_pathsep(plugindir);
#endif /* IBM */

    /* cut off the trailing path component (our filename) */
    if ((p = strrchr(plugindir, DIRSEP)) != NULL)
        *p = '\0';
    /* cut off an optional '32' or '64' trailing component */
    if ((p = strrchr(plugindir, DIRSEP)) != NULL)
    {
        if (strcmp(p + 1, "64") == 0 || strcmp(p + 1, "32") == 0 ||
            strcmp(p + 1, "win_x64") == 0 ||
            strcmp(p + 1, "mac_x64") == 0 ||
            strcmp(p + 1, "lin_x64") == 0)
        {
            *p = '\0';
        }
    }

    /*
     * Now we strip a leading xpdir from plugindir, so that now plugindir
     * will be relative to X-Plane's root directory.
     */
    if (strstr(plugindir, xpdir) == plugindir)
    {
        int xpdir_len = strlen(xpdir);
        int plugindir_len = strlen(plugindir);
        memmove(plugindir, &plugindir[xpdir_len],
                plugindir_len - xpdir_len + 1);
    }

    strcpy(name, BP_PLUGIN_NAME);
    strcpy(sig, BP_PLUGIN_SIG);
    strcpy(desc, BP_PLUGIN_DESCRIPTION);

    dcr_init();

    /* We need the configuration very early to be able to pick the lang */
    if (!bp_conf_init())
        return (0);

    /* We need the i18n support really early, so init early */
    xlate_init();

    /* We can't delete commands, so put their creation here */
    start_pb = XPLMCreateCommand("BetterPushback/start",
                                 _("Start pushback"));
    stop_pb = XPLMCreateCommand("BetterPushback/stop",
                                _("Stop pushback"));
    start_cam = XPLMCreateCommand("BetterPushback/start_planner",
                                  _("Start pushback planner"));
    stop_cam = XPLMCreateCommand("BetterPushback/stop_planner",
                                 _("Stop pushback planner"));
    conn_first = XPLMCreateCommand("BetterPushback/connect_first",
                                   _("Connect tug before entering pushback plan"));
    cab_cam = XPLMCreateCommand("BetterPushback/cab_camera",
                                _("View from tug's cab."));
    recreate_routes = XPLMCreateCommand(
        "BetterPushback/recreate_scenery_routes",
        _("Recreate scenery routes from WED files."));
    pref_cmd = XPLMCreateCommand(
        "BetterPushback/preference",
        _("Open preference window."));

    abort_push = XPLMCreateCommand("BetterPushback/abort_push",
                                   _("Abort pushback during coupled push"));

    manual_push_left = XPLMCreateCommand("BetterPushback/manual_push_left",
    _("Turn the tug to the left"));
    manual_push_right = XPLMCreateCommand("BetterPushback/manual_push_right",
        _("Turn the tug to the right"));
    manual_push_reverse = XPLMCreateCommand("BetterPushback/manual_push_toggle",
        _("Toggle the trajectory of the push back"));
    manual_push_start = XPLMCreateCommand("BetterPushback/manual_push_start",
        _("Start/Pause the manual push back with yoke"));
    manual_push_start_no_yoke = XPLMCreateCommand("BetterPushback/manual_push_start_no_yoke",
        _("Start/Pause the manual push back (yoke not used)"));    
        
    bp_boot_init();

    dr_create_i(&bp_started_dr, (int *)&bp_started, B_FALSE,
                "bp/started");
    dr_create_i(&bp_connected_dr, (int *)&bp_connected, B_FALSE,
                "bp/connected");
    dr_create_i(&slave_mode_dr, (int *)&slave_mode, B_TRUE,
                "bp/slave_mode");
    slave_mode_dr.write_cb = slave_mode_cb;
    dr_create_i(&op_complete_dr, (int *)&op_complete, B_TRUE,
                "bp/op_complete");
    dr_create_i(&plan_complete_dr, (int *)&plan_complete, B_TRUE,
                "bp/plan_complete");
    dr_create_b(&bp_tug_name_dr, bp_tug_name, sizeof(bp_tug_name),
                B_TRUE, "bp/tug_name");

    dr_create_i(&pb_set_remote_dr, (int *)&pb_set_remote, B_FALSE,
                "bp/parking_brake_set");
    pb_set_remote_dr.write_cb = pb_set_remote_cb;
    dr_create_i(&pb_set_override_dr, (int *)&pb_set_override, B_FALSE,
                "bp/parking_brake_override");
    pb_set_override_dr.write_cb = pb_set_override_cb;

    XPLMGetVersions(&bp_xp_ver, &bp_xplm_ver, &bp_host_id);

    reload_floop_ID = XPLMCreateFlightLoop(&reload_floop);

    return (1);
}

PLUGIN_API void
XPluginStop(void)
{
    cfg_cleanup();
    bp_conf_fini();
    acfutils_xlate_fini();
    tug_glob_fini();
    bp_shut_fini();
    dr_delete(&bp_started_dr);
    dr_delete(&slave_mode_dr);
    dr_delete(&op_complete_dr);
    dr_delete(&bp_tug_name_dr);
    dr_delete(&pb_set_remote_dr);
    dr_delete(&pb_set_override_dr);
    dcr_fini();

    if (reload_floop_ID != NULL)
    {
        XPLMDestroyFlightLoop(reload_floop_ID);
        reload_floop_ID = NULL;
    }
    logMsg(BP_INFO_LOG "Unloading BetterPushBack");
}

/*
 * The actual enable/disable bootstrapping code is in bp_priv_{enable,disable}.
 * This is to allow these routines to be called from our configuration code
 * as well, but using the XPlugin{Enable,Disable} interface could result in
 * linking problems, since every plugin in the system needs to have these
 * functions externally exported.
 */
PLUGIN_API int
XPluginEnable(void)
{
    return (bp_priv_enable());
}

PLUGIN_API void
XPluginDisable(void)
{
    bp_priv_disable();
}

PLUGIN_API void
XPluginReceiveMessage(XPLMPluginID from, int msg, void *param)
{
    UNUSED(from);
    UNUSED(param);

    switch (msg)
    {
    case XPLM_MSG_AIRPORT_LOADED:
    case XPLM_MSG_PLANE_LOADED:
        /* Force a reinit to re-read aircraft size params */
        smartcopilot_present = dr_find(&smartcopilot_state, "scp/api/ismaster");
        stop_cam_handler(NULL, xplm_CommandEnd, NULL);
        sharedflight_present = dr_find(&sharedflight_state,
                                       "SharedFlight/is_pilot_flying");
        bp_fini();
        cab_view_fini();
#ifndef SLAVE_DEBUG
        bp_tug_name[0] = '\0';
#endif
        init_core_state();
        break;
    }

    if (msg == XPLM_MSG_PLANE_LOADED)
        (void)ff_a320_intf_init();
    else if (msg == XPLM_MSG_PLANE_UNLOADED)
        ff_a320_intf_fini();
}

static bool_t
bp_priv_enable(void)
{
    char *cachedir = mkpathname(xpdir, "Output", "caches",
                                "BetterPushbackAirports.cache", NULL);
    bool_t dont_hide_xp_tug = B_FALSE;

    ASSERT(!inited);
    XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);
    /*
     * Reinit translations & config to allow switching languages on
     * the fly.
     */
    acfutils_xlate_fini();
    xlate_init();
    bp_conf_fini();
    if (!bp_conf_init())
        return (0);
    init_core_state();

    airportdb = safe_calloc(1, sizeof(*airportdb));
    airportdb_create(airportdb, bp_xpdir, cachedir);

    if (!recreate_cache(airportdb) || !tug_glob_init())
        goto errout;

    XPLMRegisterCommandHandler(start_pb, start_pb_handler, 1, NULL);
    XPLMRegisterCommandHandler(stop_pb, stop_pb_handler, 1, NULL);
    XPLMRegisterCommandHandler(start_cam, start_cam_handler, 1, NULL);
    XPLMRegisterCommandHandler(stop_cam, stop_cam_handler, 1, NULL);
    XPLMRegisterCommandHandler(conn_first, conn_first_handler, 1, NULL);
    XPLMRegisterCommandHandler(cab_cam, cab_cam_handler, 1, NULL);
    XPLMRegisterCommandHandler(recreate_routes, recreate_routes_handler,
                               1, NULL);
    XPLMRegisterCommandHandler(pref_cmd, preference_window_handler,
                               1, NULL);
    XPLMRegisterCommandHandler(abort_push, abort_push_handler, 1, NULL);

    XPLMRegisterCommandHandler(manual_push_left, manual_push_left_handler, 1, NULL);
    XPLMRegisterCommandHandler(manual_push_right, manual_push_right_handler, 1, NULL);
    XPLMRegisterCommandHandler(manual_push_reverse, manual_push_reverse_handler, 1, NULL);
    XPLMRegisterCommandHandler(manual_push_start, manual_push_start_handler, 1, NULL);
    XPLMRegisterCommandHandler(manual_push_start_no_yoke, manual_push_start_no_yoke_handler, 1, NULL);

    plugins_menu_item = XPLMAppendMenuItem(XPLMFindPluginsMenu(),
                                           "Better Pushback", NULL, 1);
    root_menu = XPLMCreateMenu("Better Pushback", XPLMFindPluginsMenu(),
                               plugins_menu_item, menu_cb, NULL);

    start_pb_plan_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                            _("Pre-plan pushback"), start_cam);
    stop_pb_plan_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                           _("Close pushback planner"), stop_cam);
    conn_first_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                         _("Connect tug first"), conn_first);
    start_pb_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                       _("Start pushback"), start_pb);
    stop_pb_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                      _("Stop pushback"), stop_pb);
    cab_cam_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                      _("Tug cab view"), cab_cam);

    XPLMAppendMenuSeparator(root_menu);
    prefs_menu_item = XPLMAppendMenuItemWithCommand(root_menu,
                                                    _("Preferences..."), pref_cmd);

    tug_starts_next_plane = B_FALSE;
    (void) conf_get_b(bp_conf,"tug_starts_next_plane", &tug_starts_next_plane);

    tug_auto_start = B_FALSE;
    // feature disabled for now.. waiting for a better UI paradigm
    //(void) conf_get_b(bp_conf,"tug_auto_start", &tug_auto_start);
    //

    prefs_enable = B_TRUE;
    start_pb_enable = B_TRUE;
    conn_first_enable = B_TRUE;
    stop_pb_enable = B_FALSE;
    start_pb_plan_enable = B_TRUE;
    stop_pb_plan_enable = B_FALSE;
    cab_cam_enable = B_FALSE;
    enable_menu_items();

    XPLMRegisterFlightLoopCallback(status_check, STATUS_CHECK_INTVAL, NULL);

    /* If the user OK'd it, remove the default tug */
    (void)conf_get_b(bp_conf, "dont_hide_xp11_tug", &dont_hide_xp_tug);
    if ((!dont_hide_xp_tug) && (bp_xp_ver >= 11000) && (bp_xp_ver < 12000))
        set_xp11_tug_hidden(B_TRUE);

    inited = B_TRUE;

    free(cachedir);

    return (1);

errout:
    if (cachedir != NULL)
        free(cachedir);
    if (airportdb != NULL)
    {
        airportdb_destroy(airportdb);
        free(airportdb);
        airportdb = NULL;
    }
    tug_glob_fini();

    return (0);
}

static void
bp_priv_disable(void)
{
    if (!inited)
        return;

    set_xp11_tug_hidden(B_FALSE);

    XPLMUnregisterCommandHandler(start_pb, start_pb_handler, 1, NULL);
    XPLMUnregisterCommandHandler(stop_pb, stop_pb_handler, 1, NULL);
    XPLMUnregisterCommandHandler(start_cam, start_cam_handler, 1, NULL);
    XPLMUnregisterCommandHandler(stop_cam, stop_cam_handler, 1, NULL);
    XPLMUnregisterCommandHandler(conn_first, conn_first_handler, 1, NULL);
    XPLMUnregisterCommandHandler(cab_cam, cab_cam_handler, 1, NULL);
    XPLMUnregisterCommandHandler(recreate_routes, recreate_routes_handler,
                                 1, NULL);
    XPLMUnregisterCommandHandler(abort_push, abort_push_handler, 1, NULL);
    XPLMUnregisterCommandHandler(manual_push_left, manual_push_left_handler, 1, NULL);
    XPLMUnregisterCommandHandler(manual_push_right, manual_push_right_handler, 1, NULL);
    XPLMUnregisterCommandHandler(manual_push_reverse, manual_push_reverse_handler, 1, NULL);
    XPLMUnregisterCommandHandler(manual_push_start, manual_push_start_handler, 1, NULL);
    XPLMUnregisterCommandHandler(manual_push_start_no_yoke, manual_push_start_no_yoke_handler, 1, NULL);
    XPLMUnregisterCommandHandler(pref_cmd, preference_window_handler, 1, NULL);

    bp_fini();
    tug_glob_fini();
    cab_view_fini();

    airportdb_destroy(airportdb);
    free(airportdb);
    airportdb = NULL;

    XPLMDestroyMenu(root_menu);
    XPLMRemoveMenuItem(XPLMFindPluginsMenu(), plugins_menu_item);
    XPLMUnregisterFlightLoopCallback(status_check, NULL);

    inited = B_FALSE;
}

static float
bp_do_reload(float u1, float u2, int u3, void *u4)
{
    UNUSED(u1);
    UNUSED(u2);
    UNUSED(u3);
    UNUSED(u4);
    if (reload_rqst)
    {
        bp_priv_disable();
        VERIFY(bp_priv_enable());
        reload_rqst = B_FALSE;
    }
    return (0);
}

void bp_sched_reload(void)
{
    reload_rqst = B_TRUE;
    ASSERT(reload_floop_ID != NULL);
    XPLMScheduleFlightLoop(reload_floop_ID, -1, 1);
}

static int
abort_push_handler(XPLMCommandRef cmd, XPLMCommandPhase phase,
                   void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);
    bp_fini();
    logMsg("bp_fini called from abort_push_handler, bp_started = %d", bp_started);
    slave_mode = B_FALSE;
    coupled_state_change();
    return (1);
}

static void manual_push_handler(bool_t to_the_left)
{
// simulation joystick position    
#define STEER_INCR 10
#define STEER_MAX 100

    if (push_manual.active) {
        if (push_manual.with_yoke) {
            logMsg("Manual push:  Manual nose tug rotation disabled (yoke support enabled)");
            return;
        }
        float angle =  to_the_left ? push_manual.angle - STEER_INCR : push_manual.angle + STEER_INCR;

        if (angle > STEER_MAX) {
            angle = STEER_MAX;
        }
        if (angle < -STEER_MAX) {
            angle = -STEER_MAX;
        }
        push_manual.angle = angle;
        logMsg("Manual push: New steer angle %f", angle);
        return;
    } else {
        logMsg("Manual push: Manual nose tug rotation disabled (manual push not active)");
    }
}

static int
manual_push_left_handler(XPLMCommandRef cmd, XPLMCommandPhase phase,
                   void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);
    manual_push_handler(true);
    return (1);
}

static int
manual_push_right_handler(XPLMCommandRef cmd, XPLMCommandPhase phase,
                   void *refcon)
{
    UNUSED(cmd);
    UNUSED(refcon);
    if (phase != xplm_CommandEnd)
        return (0);
    manual_push_handler(false);
    return (1);
}

#if IBM
BOOL WINAPI
DllMain(HINSTANCE hinst, DWORD reason, LPVOID resvd)
{
    UNUSED(hinst);
    UNUSED(resvd);
    lacf_glew_dllmain_hook(reason);
    return (TRUE);
}
#endif /* IBM */

void set_pref_widget_status(bool_t active)
{
    pref_widget_active_status = active;
    start_pb_enable = !active;
    conn_first_enable = !active;
    start_pb_plan_enable = !active;
    prefs_enable = !active;
    enable_menu_items();
}

bool_t
get_pref_widget_status(void)
{
    return pref_widget_active_status;
}

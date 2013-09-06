/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2009, Digium, Inc.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 *
 * \brief Channel Event Logging API
 *
 * \author Steve Murphy <murf@digium.com>
 * \author Russell Bryant <russell@digium.com>
 */

/*! \li \ref cel.c uses the configuration file \ref cel.conf
 * \addtogroup configuration_file Configuration Files
 */

/*!
 * \page cel.conf cel.conf
 * \verbinclude cel.conf.sample
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/_private.h"

#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/cel.h"
#include "asterisk/logger.h"
#include "asterisk/linkedlists.h"
#include "asterisk/utils.h"
#include "asterisk/config.h"
#include "asterisk/config_options.h"
#include "asterisk/cli.h"
#include "asterisk/astobj2.h"
#include "asterisk/stasis_message_router.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/stasis_bridges.h"
#include "asterisk/bridge.h"
#include "asterisk/parking.h"
#include "asterisk/pickup.h"
#include "asterisk/core_local.h"

/*** DOCUMENTATION
	<configInfo name="cel" language="en_US">
		<configFile name="cel.conf">
			<configObject name="general">
				<synopsis>Options that apply globally to Channel Event Logging (CEL)</synopsis>
				<configOption name="enable">
					<synopsis>Determines whether CEL is enabled</synopsis>
				</configOption>
				<configOption name="dateformat">
					<synopsis>The format to be used for dates when logging</synopsis>
				</configOption>
				<configOption name="apps">
					<synopsis>List of apps for CEL to track</synopsis>
					<description><para>A case-insensitive, comma-separated list of applications
					to track when one or both of APP_START and APP_END events are flagged for
					tracking</para></description>
				</configOption>
				<configOption name="events">
					<synopsis>List of events for CEL to track</synopsis>
					<description><para>A case-sensitive, comma-separated list of event names
					to track. These event names do not include the leading <literal>AST_CEL</literal>.
					</para>
					<enumlist>
						<enum name="ALL">
							<para>Special value which tracks all events.</para>
						</enum>
						<enum name="CHAN_START"/>
						<enum name="CHAN_END"/>
						<enum name="ANSWER"/>
						<enum name="HANGUP"/>
						<enum name="APP_START"/>
						<enum name="APP_END"/>
						<enum name="PARK_START"/>
						<enum name="PARK_END"/>
						<enum name="USER_DEFINED"/>
						<enum name="BRIDGE_ENTER"/>
						<enum name="BRIDGE_EXIT"/>
						<enum name="BLINDTRANSFER"/>
						<enum name="ATTENDEDTRANSFER"/>
						<enum name="PICKUP"/>
						<enum name="FORWARD"/>
						<enum name="LINKEDID_END"/>
						<enum name="LOCAL_OPTIMIZE"/>
					</enumlist>
					</description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/

/*! Message router for state that CEL needs to know about */
static struct stasis_message_router *cel_state_router;

/*! Topic for CEL-specific messages */
static struct stasis_topic *cel_topic;

/*! Aggregation topic for all topics CEL needs to know about */
static struct stasis_topic *cel_aggregation_topic;

/*! Subscription for forwarding the channel caching topic */
static struct stasis_subscription *cel_channel_forwarder;

/*! Subscription for forwarding the channel caching topic */
static struct stasis_subscription *cel_bridge_forwarder;

/*! Subscription for forwarding the parking topic */
static struct stasis_subscription *cel_parking_forwarder;

/*! Subscription for forwarding the CEL-specific topic */
static struct stasis_subscription *cel_cel_forwarder;

struct stasis_message_type *cel_generic_type(void);
STASIS_MESSAGE_TYPE_DEFN(cel_generic_type);

/*! Container for CEL backend information */
static struct ao2_container *cel_backends;

/*! The number of buckets into which backend names will be hashed */
#define BACKEND_BUCKETS 13

/*! Container for dial end multichannel blobs for holding on to dial statuses */
static struct ao2_container *cel_dialstatus_store;

/*!
 * \brief Maximum possible CEL event IDs
 * \note This limit is currently imposed by the eventset definition
 */
#define CEL_MAX_EVENT_IDS 64

/*!
 * \brief Number of buckets for the appset container
 */
#define NUM_APP_BUCKETS		97

/*!
 * \brief Number of buckets for the dialstatus container
 */
#define NUM_DIALSTATUS_BUCKETS	251

/*!
 * \brief Container of Asterisk application names
 *
 * The apps in this container are the applications that were specified
 * in the configuration as applications that CEL events should be generated
 * for when they start and end on a channel.
 */
static struct ao2_container *linkedids;

/*! \brief Destructor for cel_config */
static void cel_general_config_dtor(void *obj)
{
	struct ast_cel_general_config *cfg = obj;
	ast_string_field_free_memory(cfg);
	ao2_cleanup(cfg->apps);
	cfg->apps = NULL;
}

void *ast_cel_general_config_alloc(void)
{
	RAII_VAR(struct ast_cel_general_config *, cfg, NULL, ao2_cleanup);

	if (!(cfg = ao2_alloc(sizeof(*cfg), cel_general_config_dtor))) {
		return NULL;
	}

	if (ast_string_field_init(cfg, 64)) {
		return NULL;
	}

	if (!(cfg->apps = ast_str_container_alloc(NUM_APP_BUCKETS))) {
		return NULL;
	}

	ao2_ref(cfg, +1);
	return cfg;
}

/*! \brief A container that holds all config-related information */
struct cel_config {
	struct ast_cel_general_config *general;
};


static AO2_GLOBAL_OBJ_STATIC(cel_configs);

/*! \brief Destructor for cel_config */
static void cel_config_dtor(void *obj)
{
	struct cel_config *cfg = obj;
	ao2_cleanup(cfg->general);
	cfg->general = NULL;
}

static void *cel_config_alloc(void)
{
	RAII_VAR(struct cel_config *, cfg, NULL, ao2_cleanup);

	if (!(cfg = ao2_alloc(sizeof(*cfg), cel_config_dtor))) {
		return NULL;
	}

	if (!(cfg->general = ast_cel_general_config_alloc())) {
		return NULL;
	}

	ao2_ref(cfg, +1);
	return cfg;
}

/*! \brief An aco_type structure to link the "general" category to the ast_cel_general_config type */
static struct aco_type general_option = {
	.type = ACO_GLOBAL,
	.name = "general",
	.item_offset = offsetof(struct cel_config, general),
	.category_match = ACO_WHITELIST,
	.category = "^general$",
};

/*! \brief The config file to be processed for the module. */
static struct aco_file cel_conf = {
	.filename = "cel.conf",                  /*!< The name of the config file */
	.types = ACO_TYPES(&general_option),     /*!< The mapping object types to be processed */
	.skip_category = "(^manager$|^radius$)", /*!< Config sections used by existing modules. Do not add to this list. */
};

static int cel_pre_apply_config(void);

CONFIG_INFO_CORE("cel", cel_cfg_info, cel_configs, cel_config_alloc,
	.files = ACO_FILES(&cel_conf),
	.pre_apply_config = cel_pre_apply_config,
);

static int cel_pre_apply_config(void)
{
	struct cel_config *cfg = aco_pending_config(&cel_cfg_info);

	if (!cfg->general) {
		return -1;
	}

	if (!ao2_container_count(cfg->general->apps)) {
		return 0;
	}

	if (cfg->general->events & ((int64_t) 1 << AST_CEL_APP_START)) {
		return 0;
	}

	if (cfg->general->events & ((int64_t) 1 << AST_CEL_APP_END)) {
		return 0;
	}

	ast_log(LOG_ERROR, "Applications are listed to be tracked, but APP events are not tracked\n");
	return -1;
}

static struct aco_type *general_options[] = ACO_TYPES(&general_option);

/*!
 * \brief Map of ast_cel_event_type to strings
 */
static const char * const cel_event_types[CEL_MAX_EVENT_IDS] = {
	[0]                        = "ALL",
	[AST_CEL_CHANNEL_START]    = "CHAN_START",
	[AST_CEL_CHANNEL_END]      = "CHAN_END",
	[AST_CEL_ANSWER]           = "ANSWER",
	[AST_CEL_HANGUP]           = "HANGUP",
	[AST_CEL_APP_START]        = "APP_START",
	[AST_CEL_APP_END]          = "APP_END",
	[AST_CEL_PARK_START]       = "PARK_START",
	[AST_CEL_PARK_END]         = "PARK_END",
	[AST_CEL_USER_DEFINED]     = "USER_DEFINED",
	[AST_CEL_BRIDGE_ENTER]       = "BRIDGE_ENTER",
	[AST_CEL_BRIDGE_EXIT]        = "BRIDGE_EXIT",
	[AST_CEL_BLINDTRANSFER]    = "BLINDTRANSFER",
	[AST_CEL_ATTENDEDTRANSFER] = "ATTENDEDTRANSFER",
	[AST_CEL_PICKUP]           = "PICKUP",
	[AST_CEL_FORWARD]          = "FORWARD",
	[AST_CEL_LINKEDID_END]     = "LINKEDID_END",
	[AST_CEL_LOCAL_OPTIMIZE]   = "LOCAL_OPTIMIZE",
};

struct cel_backend {
	ast_cel_backend_cb callback; /*!< Callback for this backend */
	char name[0];                /*!< Name of this backend */
};

/*! \brief Hashing function for cel_backend */
static int cel_backend_hash(const void *obj, int flags)
{
	const struct cel_backend *backend;
	const char *name;

	switch (flags & (OBJ_POINTER | OBJ_KEY | OBJ_PARTIAL_KEY)) {
	case OBJ_POINTER:
		backend = obj;
		name = backend->name;
		break;
	case OBJ_KEY:
		name = obj;
		break;
	default:
		/* Hash can only work on something with a full key. */
		ast_assert(0);
		return 0;
	}

	return ast_str_hash(name);
}

/*! \brief Comparator function for cel_backend */
static int cel_backend_cmp(void *obj, void *arg, int flags)
{
	struct cel_backend *backend2, *backend1 = obj;
	const char *backend2_id, *backend1_id = backend1->name;

	switch (flags & (OBJ_POINTER | OBJ_KEY | OBJ_PARTIAL_KEY)) {
	case OBJ_POINTER:
		backend2 = arg;
		backend2_id = backend2->name;
		break;
	case OBJ_KEY:
		backend2_id = arg;
		break;
	default:
		/* Hash can only work on something with a full key. */
		ast_assert(0);
		return 0;
	}

	return !strcmp(backend1_id, backend2_id) ? CMP_MATCH | CMP_STOP : 0;
}

static const char *get_caller_uniqueid(struct ast_multi_channel_blob *blob)
{
	struct ast_channel_snapshot *caller = ast_multi_channel_blob_get_channel(blob, "caller");
	if (!caller) {
		return NULL;
	}

	return caller->uniqueid;
}

/*! \brief Hashing function for dialstatus container */
static int dialstatus_hash(const void *obj, int flags)
{
	struct ast_multi_channel_blob *blob = (void *) obj;
	const char *uniqueid = obj;
	if (!(flags & OBJ_KEY)) {
		uniqueid = get_caller_uniqueid(blob);
	}

	return ast_str_hash(uniqueid);
}

/*! \brief Comparator function for dialstatus container */
static int dialstatus_cmp(void *obj, void *arg, int flags)
{
	struct ast_multi_channel_blob *blob1 = obj, *blob2 = arg;
	const char *blob2_id = arg, *blob1_id = get_caller_uniqueid(blob1);
	if (!(flags & OBJ_KEY)) {
		blob2_id = get_caller_uniqueid(blob2);
	}

	return !strcmp(blob1_id, blob2_id) ? CMP_MATCH | CMP_STOP : 0;
}

unsigned int ast_cel_check_enabled(void)
{
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	if (!cfg || !cfg->general) {
		return 0;
	}

	return cfg->general->enable;
}

static int print_app(void *obj, void *arg, int flags)
{
	struct ast_cli_args *a = arg;

	ast_cli(a->fd, "CEL Tracking Application: %s\n", (const char *) obj);

	return 0;
}

static int event_desc_cb(void *obj, void *arg, int flags)
{
	struct ast_cli_args *a = arg;
	struct cel_backend *backend = obj;

	ast_cli(a->fd, "CEL Event Subscriber: %s\n", backend->name);
	return 0;
}

static char *handle_cli_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	unsigned int i;
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	switch (cmd) {
	case CLI_INIT:
		e->command = "cel show status";
		e->usage =
			"Usage: cel show status\n"
			"       Displays the Channel Event Logging system status.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	case CLI_HANDLER:
		break;
	}

	if (a->argc > 3) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "CEL Logging: %s\n", ast_cel_check_enabled() ? "Enabled" : "Disabled");

	if (!cfg || !cfg->general) {
		return CLI_SUCCESS;
	}

	if (!cfg->general->enable) {
		return CLI_SUCCESS;
	}

	for (i = 0; i < (sizeof(cfg->general->events) * 8); i++) {
		const char *name;

		if (!(cfg->general->events & ((int64_t) 1 << i))) {
			continue;
		}

		name = ast_cel_get_type_name(i);
		if (strcasecmp(name, "Unknown")) {
			ast_cli(a->fd, "CEL Tracking Event: %s\n", name);
		}
	}

	ao2_callback(cfg->general->apps, OBJ_NODATA, print_app, a);
	ao2_callback(cel_backends, OBJ_MULTIPLE | OBJ_NODATA, event_desc_cb, a);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_status = AST_CLI_DEFINE(handle_cli_status, "Display the CEL status");

enum ast_cel_event_type ast_cel_str_to_event_type(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(cel_event_types); i++) {
		if (!cel_event_types[i]) {
			continue;
		}

		if (!strcasecmp(name, cel_event_types[i])) {
			return i;
		}
	}

	return -1;
}

static int ast_cel_track_event(enum ast_cel_event_type et)
{
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	if (!cfg || !cfg->general) {
		return 0;
	}

	return (cfg->general->events & ((int64_t) 1 << et));
}

static int events_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct ast_cel_general_config *cfg = obj;
	char *events = ast_strdupa(var->value);
	char *cur_event;

	while ((cur_event = strsep(&events, ","))) {
		enum ast_cel_event_type event_type;

		cur_event = ast_strip(cur_event);
		if (ast_strlen_zero(cur_event)) {
			continue;
		}

		event_type = ast_cel_str_to_event_type(cur_event);

		if (event_type == 0) {
			/* All events */
			cfg->events = (int64_t) -1;
		} else if (event_type == -1) {
			ast_log(LOG_ERROR, "Unknown event name '%s'\n", cur_event);
			return -1;
		} else {
			cfg->events |= ((int64_t) 1 << event_type);
		}
	}

	return 0;
}

static int apps_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct ast_cel_general_config *cfg = obj;
	char *apps = ast_strdupa(var->value);
	char *cur_app;

	while ((cur_app = strsep(&apps, ","))) {
		cur_app = ast_strip(cur_app);
		if (ast_strlen_zero(cur_app)) {
			continue;
		}

		cur_app = ast_str_to_lower(cur_app);
		ast_str_container_add(cfg->apps, cur_app);
	}

	return 0;
}

static int do_reload(void)
{
	if (aco_process_config(&cel_cfg_info, 1) == ACO_PROCESS_ERROR) {
		return -1;
	}

	ast_verb(3, "CEL logging %sabled.\n", ast_cel_check_enabled() ? "en" : "dis");

	return 0;
}

const char *ast_cel_get_type_name(enum ast_cel_event_type type)
{
	return S_OR(cel_event_types[type], "Unknown");
}

static int cel_track_app(const char *const_app)
{
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);
	RAII_VAR(char *, app, NULL, ao2_cleanup);
	char *app_lower;

	if (!cfg || !cfg->general) {
		return 0;
	}

	app_lower = ast_str_to_lower(ast_strdupa(const_app));
	app = ao2_find(cfg->general->apps, app_lower, OBJ_KEY);
	if (!app) {
		return 0;
	}

	return 1;
}

static int cel_linkedid_ref(const char *linkedid);
struct ast_event *ast_cel_create_event(struct ast_channel_snapshot *snapshot,
		enum ast_cel_event_type event_type, const char *userdefevname,
		struct ast_json *extra)
{
	struct timeval eventtime = ast_tvnow();
	RAII_VAR(char *, extra_txt, NULL, ast_json_free);
	if (extra) {
		extra_txt = ast_json_dump_string(extra);
	}
	return ast_event_new(AST_EVENT_CEL,
		AST_EVENT_IE_CEL_EVENT_TYPE, AST_EVENT_IE_PLTYPE_UINT, event_type,
		AST_EVENT_IE_CEL_EVENT_TIME, AST_EVENT_IE_PLTYPE_UINT, eventtime.tv_sec,
		AST_EVENT_IE_CEL_EVENT_TIME_USEC, AST_EVENT_IE_PLTYPE_UINT, eventtime.tv_usec,
		AST_EVENT_IE_CEL_USEREVENT_NAME, AST_EVENT_IE_PLTYPE_STR, S_OR(userdefevname, ""),
		AST_EVENT_IE_CEL_CIDNAME, AST_EVENT_IE_PLTYPE_STR, snapshot->caller_name,
		AST_EVENT_IE_CEL_CIDNUM, AST_EVENT_IE_PLTYPE_STR, snapshot->caller_number,
		AST_EVENT_IE_CEL_CIDANI, AST_EVENT_IE_PLTYPE_STR, snapshot->caller_ani,
		AST_EVENT_IE_CEL_CIDRDNIS, AST_EVENT_IE_PLTYPE_STR, snapshot->caller_rdnis,
		AST_EVENT_IE_CEL_CIDDNID, AST_EVENT_IE_PLTYPE_STR, snapshot->caller_dnid,
		AST_EVENT_IE_CEL_EXTEN, AST_EVENT_IE_PLTYPE_STR, snapshot->exten,
		AST_EVENT_IE_CEL_CONTEXT, AST_EVENT_IE_PLTYPE_STR, snapshot->context,
		AST_EVENT_IE_CEL_CHANNAME, AST_EVENT_IE_PLTYPE_STR, snapshot->name,
		AST_EVENT_IE_CEL_APPNAME, AST_EVENT_IE_PLTYPE_STR, snapshot->appl,
		AST_EVENT_IE_CEL_APPDATA, AST_EVENT_IE_PLTYPE_STR, snapshot->data,
		AST_EVENT_IE_CEL_AMAFLAGS, AST_EVENT_IE_PLTYPE_UINT, snapshot->amaflags,
		AST_EVENT_IE_CEL_ACCTCODE, AST_EVENT_IE_PLTYPE_STR, snapshot->accountcode,
		AST_EVENT_IE_CEL_PEERACCT, AST_EVENT_IE_PLTYPE_STR, snapshot->peeraccount,
		AST_EVENT_IE_CEL_UNIQUEID, AST_EVENT_IE_PLTYPE_STR, snapshot->uniqueid,
		AST_EVENT_IE_CEL_LINKEDID, AST_EVENT_IE_PLTYPE_STR, snapshot->linkedid,
		AST_EVENT_IE_CEL_USERFIELD, AST_EVENT_IE_PLTYPE_STR, snapshot->userfield,
		AST_EVENT_IE_CEL_EXTRA, AST_EVENT_IE_PLTYPE_STR, S_OR(extra_txt, ""),
		AST_EVENT_IE_CEL_PEER, AST_EVENT_IE_PLTYPE_STR, "",
		AST_EVENT_IE_END);
}

static int cel_backend_send_cb(void *obj, void *arg, int flags)
{
	struct cel_backend *backend = obj;

	backend->callback(arg);
	return 0;
}

static int cel_report_event(struct ast_channel_snapshot *snapshot,
		enum ast_cel_event_type event_type, const char *userdefevname,
		struct ast_json *extra)
{
	struct ast_event *ev;
	char *linkedid = ast_strdupa(snapshot->linkedid);
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	if (!cfg || !cfg->general) {
		return 0;
	}

	if (!cfg->general->enable) {
		return 0;
	}

	/* Record the linkedid of new channels if we are tracking LINKEDID_END even if we aren't
	 * reporting on CHANNEL_START so we can track when to send LINKEDID_END */
	if (ast_cel_track_event(AST_CEL_LINKEDID_END) && event_type == AST_CEL_CHANNEL_START && linkedid) {
		if (cel_linkedid_ref(linkedid)) {
			return -1;
		}
	}

	if (!ast_cel_track_event(event_type)) {
		return 0;
	}

	if ((event_type == AST_CEL_APP_START || event_type == AST_CEL_APP_END)
		&& !cel_track_app(snapshot->appl)) {
		return 0;
	}

	ev = ast_cel_create_event(snapshot, event_type, userdefevname, extra);
	if (!ev) {
		return -1;
	}

	/* Distribute event to backends */
	ao2_callback(cel_backends, OBJ_MULTIPLE | OBJ_NODATA, cel_backend_send_cb, ev);
	ast_event_destroy(ev);

	return 0;
}

/* called whenever a channel is destroyed or a linkedid is changed to
 * potentially emit a CEL_LINKEDID_END event */
static void check_retire_linkedid(struct ast_channel_snapshot *snapshot)
{
	char *lid;

	/* make sure we need to do all this work */

	if (ast_strlen_zero(snapshot->linkedid) || !ast_cel_track_event(AST_CEL_LINKEDID_END)) {
		return;
	}

	if (!(lid = ao2_find(linkedids, (void *) snapshot->linkedid, OBJ_POINTER))) {
		ast_log(LOG_ERROR, "Something weird happened, couldn't find linkedid %s\n", snapshot->linkedid);
		return;
	}

	/* We have a ref for each channel with this linkedid, the link and the above find, so if
	 * before unreffing the channel we have a refcount of 3, we're done. Unlink and report. */
	if (ao2_ref(lid, -1) == 3) {
		ast_str_container_remove(linkedids, lid);
		cel_report_event(snapshot, AST_CEL_LINKEDID_END, NULL, NULL);
	}
	ao2_ref(lid, -1);
}

/* Note that no 'chan_fixup' function is provided for this datastore type,
 * because the channels that will use it will never be involved in masquerades.
 */
static const struct ast_datastore_info fabricated_channel_datastore = {
	.type = "CEL fabricated channel",
	.destroy = ast_free_ptr,
};

struct ast_channel *ast_cel_fabricate_channel_from_event(const struct ast_event *event)
{
	struct varshead *headp;
	struct ast_var_t *newvariable;
	const char *mixed_name;
	char timebuf[30];
	struct ast_channel *tchan;
	struct ast_cel_event_record record = {
		.version = AST_CEL_EVENT_RECORD_VERSION,
	};
	struct ast_datastore *datastore;
	char *app_data;
	RAII_VAR(struct cel_config *, cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	if (!cfg || !cfg->general) {
		return NULL;
	}

	/* do not call ast_channel_alloc because this is not really a real channel */
	if (!(tchan = ast_dummy_channel_alloc())) {
		return NULL;
	}

	headp = ast_channel_varshead(tchan);

	/* first, get the variables from the event */
	if (ast_cel_fill_record(event, &record)) {
		ast_channel_unref(tchan);
		return NULL;
	}

	/* next, fill the channel with their data */
	mixed_name = (record.event_type == AST_CEL_USER_DEFINED)
		? record.user_defined_name : record.event_name;
	if ((newvariable = ast_var_assign("eventtype", mixed_name))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}

	if (ast_strlen_zero(cfg->general->date_format)) {
		snprintf(timebuf, sizeof(timebuf), "%ld.%06ld", (long) record.event_time.tv_sec,
				(long) record.event_time.tv_usec);
	} else {
		struct ast_tm tm;
		ast_localtime(&record.event_time, &tm, NULL);
		ast_strftime(timebuf, sizeof(timebuf), cfg->general->date_format, &tm);
	}

	if ((newvariable = ast_var_assign("eventtime", timebuf))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}

	if ((newvariable = ast_var_assign("eventenum", record.event_name))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}
	if ((newvariable = ast_var_assign("userdeftype", record.user_defined_name))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}
	if ((newvariable = ast_var_assign("eventextra", record.extra))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}

	ast_channel_caller(tchan)->id.name.valid = 1;
	ast_channel_caller(tchan)->id.name.str = ast_strdup(record.caller_id_name);
	ast_channel_caller(tchan)->id.number.valid = 1;
	ast_channel_caller(tchan)->id.number.str = ast_strdup(record.caller_id_num);
	ast_channel_caller(tchan)->ani.number.valid = 1;
	ast_channel_caller(tchan)->ani.number.str = ast_strdup(record.caller_id_ani);
	ast_channel_redirecting(tchan)->from.number.valid = 1;
	ast_channel_redirecting(tchan)->from.number.str = ast_strdup(record.caller_id_rdnis);
	ast_channel_dialed(tchan)->number.str = ast_strdup(record.caller_id_dnid);

	ast_channel_exten_set(tchan, record.extension);
	ast_channel_context_set(tchan, record.context);
	ast_channel_name_set(tchan, record.channel_name);
	ast_channel_uniqueid_set(tchan, record.unique_id);
	ast_channel_linkedid_set(tchan, record.linked_id);
	ast_channel_accountcode_set(tchan, record.account_code);
	ast_channel_peeraccount_set(tchan, record.peer_account);
	ast_channel_userfield_set(tchan, record.user_field);

	if ((newvariable = ast_var_assign("BRIDGEPEER", record.peer))) {
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}

	ast_channel_amaflags_set(tchan, record.amaflag);

	/* We need to store an 'application name' and 'application
	 * data' on the channel for logging purposes, but the channel
	 * structure only provides a place to store pointers, and it
	 * expects these pointers to be pointing to data that does not
	 * need to be freed. This means that the channel's destructor
	 * does not attempt to free any storage that these pointers
	 * point to. However, we can't provide data in that form directly for
	 * these structure members. In order to ensure that these data
	 * elements have a lifetime that matches the channel's
	 * lifetime, we'll put them in a datastore attached to the
	 * channel, and set's the channel's pointers to point into the
	 * datastore.  The datastore will then be automatically destroyed
	 * when the channel is destroyed.
	 */

	if (!(datastore = ast_datastore_alloc(&fabricated_channel_datastore, NULL))) {
		ast_channel_unref(tchan);
		return NULL;
	}

	if (!(app_data = ast_malloc(strlen(record.application_name) + strlen(record.application_data) + 2))) {
		ast_datastore_free(datastore);
		ast_channel_unref(tchan);
		return NULL;
	}

	ast_channel_appl_set(tchan, strcpy(app_data, record.application_name));
	ast_channel_data_set(tchan, strcpy(app_data + strlen(record.application_name) + 1,
		record.application_data));

	datastore->data = app_data;
	ast_channel_datastore_add(tchan, datastore);

	return tchan;
}

static int cel_linkedid_ref(const char *linkedid)
{
	char *lid;

	if (ast_strlen_zero(linkedid)) {
		ast_log(LOG_ERROR, "The linkedid should never be empty\n");
		return -1;
	}

	if (!(lid = ao2_find(linkedids, (void *) linkedid, OBJ_POINTER))) {
		if (!(lid = ao2_alloc(strlen(linkedid) + 1, NULL))) {
			return -1;
		}
		strcpy(lid, linkedid);
		if (!ao2_link(linkedids, lid)) {
			ao2_ref(lid, -1);
			return -1;
		}
		/* Leave both the link and the alloc refs to show a count of 1 + the link */
	}
	/* If we've found, go ahead and keep the ref to increment count of how many channels
	 * have this linkedid. We'll clean it up in check_retire */
	return 0;
}

int ast_cel_fill_record(const struct ast_event *e, struct ast_cel_event_record *r)
{
	if (r->version != AST_CEL_EVENT_RECORD_VERSION) {
		ast_log(LOG_ERROR, "Module ABI mismatch for ast_cel_event_record.  "
				"Please ensure all modules were compiled for "
				"this version of Asterisk.\n");
		return -1;
	}

	r->event_type = ast_event_get_ie_uint(e, AST_EVENT_IE_CEL_EVENT_TYPE);

	r->event_time.tv_sec = ast_event_get_ie_uint(e, AST_EVENT_IE_CEL_EVENT_TIME);
	r->event_time.tv_usec = ast_event_get_ie_uint(e, AST_EVENT_IE_CEL_EVENT_TIME_USEC);

	r->event_name = ast_cel_get_type_name(r->event_type);
	if (r->event_type == AST_CEL_USER_DEFINED) {
		r->user_defined_name = ast_event_get_ie_str(e, AST_EVENT_IE_CEL_USEREVENT_NAME);
	} else {
		r->user_defined_name = "";
	}

	r->caller_id_name   = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CIDNAME), "");
	r->caller_id_num    = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CIDNUM), "");
	r->caller_id_ani    = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CIDANI), "");
	r->caller_id_rdnis  = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CIDRDNIS), "");
	r->caller_id_dnid   = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CIDDNID), "");
	r->extension        = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_EXTEN), "");
	r->context          = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CONTEXT), "");
	r->channel_name     = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_CHANNAME), "");
	r->application_name = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_APPNAME), "");
	r->application_data = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_APPDATA), "");
	r->account_code     = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_ACCTCODE), "");
	r->peer_account     = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_ACCTCODE), "");
	r->unique_id        = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_UNIQUEID), "");
	r->linked_id        = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_LINKEDID), "");
	r->amaflag          = ast_event_get_ie_uint(e, AST_EVENT_IE_CEL_AMAFLAGS);
	r->user_field       = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_USERFIELD), "");
	r->peer             = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_PEER), "");
	r->extra            = S_OR(ast_event_get_ie_str(e, AST_EVENT_IE_CEL_EXTRA), "");

	return 0;
}

/*! \brief Typedef for callbacks that get called on channel snapshot updates */
typedef void (*cel_channel_snapshot_monitor)(
	struct ast_channel_snapshot *old_snapshot,
	struct ast_channel_snapshot *new_snapshot);

static struct ast_multi_channel_blob *get_dialstatus_blob(const char *uniqueid)
{
	return ao2_find(cel_dialstatus_store, uniqueid, OBJ_KEY | OBJ_UNLINK);
}

static const char *get_blob_variable(struct ast_multi_channel_blob *blob, const char *varname)
{
	struct ast_json *json = ast_multi_channel_blob_get_json(blob);
	if (!json) {
		return NULL;
	}

	json = ast_json_object_get(json, varname);
	if (!json) {
		return NULL;
	}

	return ast_json_string_get(json);
}

/*! \brief Handle channel state changes */
static void cel_channel_state_change(
	struct ast_channel_snapshot *old_snapshot,
	struct ast_channel_snapshot *new_snapshot)
{
	int is_hungup, was_hungup;

	if (!new_snapshot) {
		cel_report_event(old_snapshot, AST_CEL_CHANNEL_END, NULL, NULL);
		check_retire_linkedid(old_snapshot);
		return;
	}

	if (!old_snapshot) {
		cel_report_event(new_snapshot, AST_CEL_CHANNEL_START, NULL, NULL);
		return;
	}

	was_hungup = ast_test_flag(&old_snapshot->flags, AST_FLAG_DEAD) ? 1 : 0;
	is_hungup = ast_test_flag(&new_snapshot->flags, AST_FLAG_DEAD) ? 1 : 0;

	if (!was_hungup && is_hungup) {
		RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);
		RAII_VAR(struct ast_multi_channel_blob *, blob, get_dialstatus_blob(new_snapshot->uniqueid), ao2_cleanup);
		const char *dialstatus = "";
		if (blob && !ast_strlen_zero(get_blob_variable(blob, "dialstatus"))) {
			dialstatus = get_blob_variable(blob, "dialstatus");
		}
		extra = ast_json_pack("{s: i, s: s, s: s}",
			"hangupcause", new_snapshot->hangupcause,
			"hangupsource", new_snapshot->hangupsource,
			"dialstatus", dialstatus);
		cel_report_event(new_snapshot, AST_CEL_HANGUP, NULL, extra);
		return;
	}

	if (old_snapshot->state != new_snapshot->state && new_snapshot->state == AST_STATE_UP) {
		cel_report_event(new_snapshot, AST_CEL_ANSWER, NULL, NULL);
		return;
	}
}

static void cel_channel_linkedid_change(
	struct ast_channel_snapshot *old_snapshot,
	struct ast_channel_snapshot *new_snapshot)
{
	if (!old_snapshot || !new_snapshot) {
		return;
	}

	ast_assert(!ast_strlen_zero(new_snapshot->linkedid));
	ast_assert(!ast_strlen_zero(old_snapshot->linkedid));

	if (strcmp(old_snapshot->linkedid, new_snapshot->linkedid)) {
		cel_linkedid_ref(new_snapshot->linkedid);
		check_retire_linkedid(old_snapshot);
	}
}

static void cel_channel_app_change(
	struct ast_channel_snapshot *old_snapshot,
	struct ast_channel_snapshot *new_snapshot)
{
	if (new_snapshot && old_snapshot
		&& !strcmp(old_snapshot->appl, new_snapshot->appl)) {
		return;
	}

	/* old snapshot has an application, end it */
	if (old_snapshot && !ast_strlen_zero(old_snapshot->appl)) {
		cel_report_event(old_snapshot, AST_CEL_APP_END, NULL, NULL);
	}

	/* new snapshot has an application, start it */
	if (new_snapshot && !ast_strlen_zero(new_snapshot->appl)) {
		cel_report_event(new_snapshot, AST_CEL_APP_START, NULL, NULL);
	}
}

/* \brief Handlers for channel snapshot changes.
 * \note Order of the handlers matters. Application changes must come before state
 * changes to ensure that hangup notifications occur after application changes.
 * Linkedid checking should always come last.
 */
cel_channel_snapshot_monitor cel_channel_monitors[] = {
	cel_channel_app_change,
	cel_channel_state_change,
	cel_channel_linkedid_change,
};

static int cel_filter_channel_snapshot(struct ast_channel_snapshot *snapshot)
{
	if (!snapshot) {
		return 0;
	}
	return snapshot->tech_properties & AST_CHAN_TP_INTERNAL;
}

static void cel_snapshot_update_cb(void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct stasis_cache_update *update = stasis_message_data(message);
	if (ast_channel_snapshot_type() == update->type) {
		struct ast_channel_snapshot *old_snapshot;
		struct ast_channel_snapshot *new_snapshot;
		size_t i;

		old_snapshot = stasis_message_data(update->old_snapshot);
		new_snapshot = stasis_message_data(update->new_snapshot);

		if (cel_filter_channel_snapshot(old_snapshot) || cel_filter_channel_snapshot(new_snapshot)) {
			return;
		}

		for (i = 0; i < ARRAY_LEN(cel_channel_monitors); ++i) {
			cel_channel_monitors[i](old_snapshot, new_snapshot);
		}
	}
}

static void cel_bridge_enter_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_bridge_blob *blob = stasis_message_data(message);
	struct ast_bridge_snapshot *snapshot = blob->bridge;
	struct ast_channel_snapshot *chan_snapshot = blob->channel;
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);

	if (cel_filter_channel_snapshot(chan_snapshot)) {
		return;
	}

	extra = ast_json_pack("{s: s}", "bridge_id", snapshot->uniqueid);
	if (!extra) {
		return;
	}

	cel_report_event(chan_snapshot, AST_CEL_BRIDGE_ENTER, NULL, extra);
}

static void cel_bridge_leave_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_bridge_blob *blob = stasis_message_data(message);
	struct ast_bridge_snapshot *snapshot = blob->bridge;
	struct ast_channel_snapshot *chan_snapshot = blob->channel;
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);

	if (cel_filter_channel_snapshot(chan_snapshot)) {
		return;
	}

	extra = ast_json_pack("{s: s}", "bridge_id", snapshot->uniqueid);
	if (!extra) {
		return;
	}

	cel_report_event(chan_snapshot, AST_CEL_BRIDGE_EXIT, NULL, extra);
}

static void cel_parking_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_parked_call_payload *parked_payload = stasis_message_data(message);
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);
	const char *reason = NULL;

	switch (parked_payload->event_type) {
	case PARKED_CALL:
		extra = ast_json_pack("{s: s, s: s}",
			"parker_dial_string", parked_payload->parker_dial_string,
			"parking_lot", parked_payload->parkinglot);
		if (extra) {
			cel_report_event(parked_payload->parkee, AST_CEL_PARK_START, NULL, extra);
		}
		return;
	case PARKED_CALL_TIMEOUT:
		reason = "ParkedCallTimeOut";
		break;
	case PARKED_CALL_GIVEUP:
		reason = "ParkedCallGiveUp";
		break;
	case PARKED_CALL_UNPARKED:
		reason = "ParkedCallUnparked";
		break;
	case PARKED_CALL_FAILED:
		reason = "ParkedCallFailed";
		break;
	case PARKED_CALL_SWAP:
		reason = "ParkedCallSwap";
		break;
	}

	extra = ast_json_pack("{s: s}", "reason", reason);
	if (extra) {
		cel_report_event(parked_payload->parkee, AST_CEL_PARK_END, NULL, extra);
	}
}

static void save_dialstatus(struct ast_multi_channel_blob *blob)
{
	ao2_link(cel_dialstatus_store, blob);
}

static void cel_dial_cb(void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_multi_channel_blob *blob = stasis_message_data(message);

	if (cel_filter_channel_snapshot(ast_multi_channel_blob_get_channel(blob, "caller"))) {
		return;
	}

	if (!get_caller_uniqueid(blob)) {
		return;
	}

	if (!ast_strlen_zero(get_blob_variable(blob, "forward"))) {
		struct ast_channel_snapshot *caller = ast_multi_channel_blob_get_channel(blob, "caller");
		RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);
		if (!caller) {
			return;
		}

		extra = ast_json_pack("{s: s}", "forward", get_blob_variable(blob, "forward"));
		if (extra) {
			cel_report_event(caller, AST_CEL_FORWARD, NULL, extra);
		}
	}

	if (ast_strlen_zero(get_blob_variable(blob, "dialstatus"))) {
		return;
	}

	save_dialstatus(blob);
}

static void cel_generic_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_channel_blob *obj = stasis_message_data(message);
	int event_type = ast_json_integer_get(ast_json_object_get(obj->blob, "event_type"));
	struct ast_json *event_details = ast_json_object_get(obj->blob, "event_details");

	switch (event_type) {
	case AST_CEL_USER_DEFINED:
		{
			const char *event = ast_json_string_get(ast_json_object_get(event_details, "event"));
			struct ast_json *extra = ast_json_object_get(event_details, "extra");
			cel_report_event(obj->snapshot, event_type, event, extra);
			break;
		}
	default:
		ast_log(LOG_ERROR, "Unhandled %s event blob\n", ast_cel_get_type_name(event_type));
		break;
	}
}

static void cel_blind_transfer_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_bridge_blob *obj = stasis_message_data(message);
	struct ast_channel_snapshot *chan_snapshot = obj->channel;
	struct ast_bridge_snapshot *bridge_snapshot = obj->bridge;
	struct ast_json *blob = obj->blob;
	struct ast_json *json_result = ast_json_object_get(blob, "result");
	struct ast_json *json_exten;
	struct ast_json *json_context;
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);
	const char *exten, *context;
	enum ast_transfer_result result;

	if (!json_result) {
		return;
	}

	result = ast_json_integer_get(json_result);
	if (result != AST_BRIDGE_TRANSFER_SUCCESS) {
		return;
	}

	json_exten = ast_json_object_get(blob, "exten");
	json_context = ast_json_object_get(blob, "context");

	if (!json_exten || !json_context) {
		return;
	}

	exten = ast_json_string_get(json_exten);
	context = ast_json_string_get(json_context);
	if (!exten || !context) {
		return;
	}

	extra = ast_json_pack("{s: s, s: s, s: s}",
		"extension", exten,
		"context", context,
		"bridge_id", bridge_snapshot->uniqueid);

	if (extra) {
		cel_report_event(chan_snapshot, AST_CEL_BLINDTRANSFER, NULL, extra);
	}
}

static void cel_attended_transfer_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_attended_transfer_message *xfer = stasis_message_data(message);
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);
	struct ast_bridge_snapshot *bridge1, *bridge2;
	struct ast_channel_snapshot *channel1, *channel2;

	/* Make sure bridge1 is always non-NULL */
	if (!xfer->to_transferee.bridge_snapshot) {
		bridge1 = xfer->to_transfer_target.bridge_snapshot;
		bridge2 = xfer->to_transferee.bridge_snapshot;
		channel1 = xfer->to_transfer_target.channel_snapshot;
		channel2 = xfer->to_transferee.channel_snapshot;
	} else {
		bridge1 = xfer->to_transferee.bridge_snapshot;
		bridge2 = xfer->to_transfer_target.bridge_snapshot;
		channel1 = xfer->to_transferee.channel_snapshot;
		channel2 = xfer->to_transfer_target.channel_snapshot;
	}

	switch (xfer->dest_type) {
	case AST_ATTENDED_TRANSFER_DEST_FAIL:
		return;
		/* handle these three the same */
	case AST_ATTENDED_TRANSFER_DEST_BRIDGE_MERGE:
	case AST_ATTENDED_TRANSFER_DEST_LINK:
	case AST_ATTENDED_TRANSFER_DEST_THREEWAY:
		extra = ast_json_pack("{s: s, s: s, s: s}",
			"bridge1_id", bridge1->uniqueid,
			"channel2_name", channel2->name,
			"bridge2_id", bridge2->uniqueid);

		if (!extra) {
			return;
		}
		break;
	case AST_ATTENDED_TRANSFER_DEST_APP:
		extra = ast_json_pack("{s: s, s: s, s: s}",
			"bridge1_id", bridge1->uniqueid,
			"channel2_name", channel2->name,
			"app", xfer->dest.app);

		if (!extra) {
			return;
		}
		break;
	}
	cel_report_event(channel1, AST_CEL_ATTENDEDTRANSFER, NULL, extra);
}

static void cel_pickup_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_multi_channel_blob *obj = stasis_message_data(message);
	struct ast_channel_snapshot *channel = ast_multi_channel_blob_get_channel(obj, "channel");
	struct ast_channel_snapshot *target = ast_multi_channel_blob_get_channel(obj, "target");
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);

	if (!channel || !target) {
		return;
	}

	extra = ast_json_pack("{s: s}", "pickup_channel", channel->name);
	if (!extra) {
		return;
	}

	cel_report_event(target, AST_CEL_PICKUP, NULL, extra);
}

static void cel_local_cb(
	void *data, struct stasis_subscription *sub,
	struct stasis_topic *topic,
	struct stasis_message *message)
{
	struct ast_multi_channel_blob *obj = stasis_message_data(message);
	struct ast_channel_snapshot *localone = ast_multi_channel_blob_get_channel(obj, "1");
	struct ast_channel_snapshot *localtwo = ast_multi_channel_blob_get_channel(obj, "2");
	RAII_VAR(struct ast_json *, extra, NULL, ast_json_unref);

	if (!localone || !localtwo) {
		return;
	}

	extra = ast_json_pack("{s: s}", "local_two", localtwo->name);
	if (!extra) {
		return;
	}

	cel_report_event(localone, AST_CEL_LOCAL_OPTIMIZE, NULL, extra);
}

static void ast_cel_engine_term(void)
{
	aco_info_destroy(&cel_cfg_info);
	ao2_global_obj_release(cel_configs);
	stasis_message_router_unsubscribe_and_join(cel_state_router);
	cel_state_router = NULL;
	ao2_cleanup(cel_aggregation_topic);
	cel_aggregation_topic = NULL;
	ao2_cleanup(cel_topic);
	cel_topic = NULL;
	cel_channel_forwarder = stasis_unsubscribe_and_join(cel_channel_forwarder);
	cel_bridge_forwarder = stasis_unsubscribe_and_join(cel_bridge_forwarder);
	cel_parking_forwarder = stasis_unsubscribe_and_join(cel_parking_forwarder);
	cel_cel_forwarder = stasis_unsubscribe_and_join(cel_cel_forwarder);
	ast_cli_unregister(&cli_status);
	ao2_cleanup(cel_dialstatus_store);
	cel_dialstatus_store = NULL;
	ao2_cleanup(linkedids);
	linkedids = NULL;
	ao2_cleanup(cel_backends);
	cel_backends = NULL;
	STASIS_MESSAGE_TYPE_CLEANUP(cel_generic_type);
}

int ast_cel_engine_init(void)
{
	int ret = 0;
	if (!(linkedids = ast_str_container_alloc(NUM_APP_BUCKETS))) {
		return -1;
	}

	if (!(cel_dialstatus_store = ao2_container_alloc(NUM_DIALSTATUS_BUCKETS, dialstatus_hash, dialstatus_cmp))) {
		return -1;
	}

	if (STASIS_MESSAGE_TYPE_INIT(cel_generic_type)) {
		return -1;
	}

	if (ast_cli_register(&cli_status)) {
		return -1;
	}

	cel_backends = ao2_container_alloc(BACKEND_BUCKETS, cel_backend_hash, cel_backend_cmp);
	if (!cel_backends) {
		return -1;
	}

	cel_aggregation_topic = stasis_topic_create("cel_aggregation_topic");
	if (!cel_aggregation_topic) {
		return -1;
	}

	cel_topic = stasis_topic_create("cel_topic");
	if (!cel_topic) {
		return -1;
	}

	cel_channel_forwarder = stasis_forward_all(
		ast_channel_topic_all_cached(),
		cel_aggregation_topic);
	if (!cel_channel_forwarder) {
		return -1;
	}

	cel_bridge_forwarder = stasis_forward_all(
		ast_bridge_topic_all_cached(),
		cel_aggregation_topic);
	if (!cel_bridge_forwarder) {
		return -1;
	}

	cel_parking_forwarder = stasis_forward_all(
		ast_parking_topic(),
		cel_aggregation_topic);
	if (!cel_parking_forwarder) {
		return -1;
	}

	cel_cel_forwarder = stasis_forward_all(
		ast_cel_topic(),
		cel_aggregation_topic);
	if (!cel_cel_forwarder) {
		return -1;
	}

	cel_state_router = stasis_message_router_create(cel_aggregation_topic);
	if (!cel_state_router) {
		return -1;
	}

	ret |= stasis_message_router_add(cel_state_router,
		stasis_cache_update_type(),
		cel_snapshot_update_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_channel_dial_type(),
		cel_dial_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_channel_entered_bridge_type(),
		cel_bridge_enter_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_channel_left_bridge_type(),
		cel_bridge_leave_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_parked_call_type(),
		cel_parking_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		cel_generic_type(),
		cel_generic_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_blind_transfer_type(),
		cel_blind_transfer_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_attended_transfer_type(),
		cel_attended_transfer_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_call_pickup_type(),
		cel_pickup_cb,
		NULL);

	ret |= stasis_message_router_add(cel_state_router,
		ast_local_optimization_end_type(),
		cel_local_cb,
		NULL);

	/* If somehow we failed to add any routes, just shut down the whole
	 * thing and fail it.
	 */
	if (ret) {
		ast_cel_engine_term();
		return -1;
	}

	if (aco_info_init(&cel_cfg_info)) {
		return -1;
	}

	aco_option_register(&cel_cfg_info, "enable", ACO_EXACT, general_options, "no", OPT_BOOL_T, 1, FLDSET(struct ast_cel_general_config, enable));
	aco_option_register(&cel_cfg_info, "dateformat", ACO_EXACT, general_options, "", OPT_STRINGFIELD_T, 0, STRFLDSET(struct ast_cel_general_config, date_format));
	aco_option_register_custom(&cel_cfg_info, "apps", ACO_EXACT, general_options, "", apps_handler, 0);
	aco_option_register_custom(&cel_cfg_info, "events", ACO_EXACT, general_options, "", events_handler, 0);

	if (aco_process_config(&cel_cfg_info, 0)) {
		RAII_VAR(struct cel_config *, cel_cfg, cel_config_alloc(), ao2_cleanup);

		if (!cel_cfg) {
			return -1;
		}

		/* If we couldn't process the configuration and this wasn't a reload,
		 * create a default config
		 */
		if (!aco_set_defaults(&general_option, "general", cel_cfg->general)) {
			ast_log(LOG_NOTICE, "Failed to process CEL configuration; using defaults\n");
			ao2_global_obj_replace_unref(cel_configs, cel_cfg);
		}
	}

	ast_register_cleanup(ast_cel_engine_term);

	return 0;
}

int ast_cel_engine_reload(void)
{
	return do_reload();
}

void ast_cel_publish_event(struct ast_channel *chan,
	enum ast_cel_event_type event_type,
	struct ast_json *blob)
{
	RAII_VAR(struct ast_channel_blob *, obj, NULL, ao2_cleanup);
	RAII_VAR(struct ast_json *, cel_blob, NULL, ast_json_unref);
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);
	cel_blob = ast_json_pack("{s: i, s: O}",
		"event_type", event_type,
		"event_details", blob);

	message = ast_channel_blob_create(chan, cel_generic_type(), cel_blob);
	if (message) {
		stasis_publish(ast_cel_topic(), message);
	}
}

struct stasis_topic *ast_cel_topic(void)
{
	return cel_topic;
}

struct ast_cel_general_config *ast_cel_get_config(void)
{
	RAII_VAR(struct cel_config *, mod_cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);

	if (!mod_cfg || !mod_cfg->general) {
		return NULL;
	}

	ao2_ref(mod_cfg->general, +1);
	return mod_cfg->general;
}

void ast_cel_set_config(struct ast_cel_general_config *config)
{
	RAII_VAR(struct cel_config *, mod_cfg, ao2_global_obj_ref(cel_configs), ao2_cleanup);
	RAII_VAR(struct ast_cel_general_config *, cleanup_config, mod_cfg->general, ao2_cleanup);

	if (mod_cfg) {
		mod_cfg->general = config;
		if (mod_cfg->general) {
			ao2_ref(mod_cfg->general, +1);
		}
	}
}

int ast_cel_backend_unregister(const char *name)
{
	RAII_VAR(struct cel_backend *, backend, NULL, ao2_cleanup);

	backend = ao2_find(cel_backends, name, OBJ_KEY | OBJ_UNLINK);
	if (!backend) {
		return -1;
	}

	return 0;
}

int ast_cel_backend_register(const char *name, ast_cel_backend_cb backend_callback)
{
	RAII_VAR(struct cel_backend *, backend, NULL, ao2_cleanup);

	if (ast_strlen_zero(name)) {
		return -1;
	}

	backend = ao2_alloc(sizeof(*backend) + 1 + strlen(name), NULL);
	if (!backend) {
		return -1;
	}

	/* safe strcpy */
	strcpy(backend->name, name);
	backend->callback = backend_callback;
	ao2_link(cel_backends, backend);

	return 0;
}

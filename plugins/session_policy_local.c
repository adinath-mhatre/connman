/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2012  BMW Car IT GbmH. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include <glib.h>

#include <gdbus.h>

#define CONNMAN_API_SUBJECT_TO_CHANGE
#include <connman/plugin.h>
#include <connman/log.h>
#include <connman/session.h>
#include <connman/dbus.h>
#include <connman/inotify.h>

#define POLICYDIR STORAGEDIR "/session_policy_local"

#define MODE		(S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | \
			S_IXGRP | S_IROTH | S_IXOTH)

static DBusConnection *connection;

static GHashTable *file_hash;    /* (filename, policy_file) */
static GHashTable *session_hash; /* (connman_session, policy_config) */

/* Global lookup table for mapping sessions to policies */
static GHashTable *selinux_hash; /* (lsm context, policy_group) */

struct create_data {
	struct connman_session *session;
};

/*
 * A instance of struct policy_file is created per file in
 * POLICYDIR.
 */
struct policy_file {
	/*
	 * A valid file is a keyfile with one ore more groups. All
	 * groups are keept in this list.
	 */
	GSList *groups;
};

struct policy_group {
	char *selinux;

	/*
	 * Each policy_group owns a config and is not shared with
	 * sessions. Instead each session copies the valued from this
	 * object.
	 */
	struct connman_session_config *config;

	/* All 'users' of this policy. */
	GSList *sessions;
};

/* A struct policy_config object is created and owned by a session. */
struct policy_config {
	char *selinux;

	/* The policy config owned by the session */
	struct connman_session_config *config;

	/* To which policy belongs this policy_config */
	struct connman_session *session;
	/*
	 * Points to the policy_group when a config has been applied
	 * from a file.
	 */
	struct policy_group *group;
};

static void copy_session_config(struct connman_session_config *dst,
			struct connman_session_config *src)
{
	g_slist_free(dst->allowed_bearers);
	dst->allowed_bearers = g_slist_copy(src->allowed_bearers);
	dst->ecall = src->ecall;
	dst->type = src->type;
	dst->roaming_policy = src->roaming_policy;
	dst->priority = src->priority;
}

static void set_policy(struct policy_config *policy,
			struct policy_group *group)
{
	DBG("policy %p group %p", policy, group);

	group->sessions = g_slist_prepend(group->sessions, policy);
	policy->group = group;

	copy_session_config(policy->config, group->config);
}

static char *parse_selinux_type(const char *context)
{
	char *ident, **tokens;

	/*
	 * SELinux combines Role-Based Access Control (RBAC), Type
	 * Enforcment (TE) and optionally Multi-Level Security (MLS).
	 *
	 * When SELinux is enabled all processes and files are labeled
	 * with a contex that contains information such as user, role
	 * type (and optionally a level). E.g.
	 *
	 * $ ls -Z
	 * -rwxrwxr-x. wagi wagi unconfined_u:object_r:haifux_exec_t:s0 session_ui.py
	 *
	 * For identifyng application we (ab)using the type
	 * information. In the above example the haifux_exec_t type
	 * will be transfered to haifux_t as defined in the domain
	 * transition and thus we are able to identify the application
	 * as haifux_t.
	 */

	tokens = g_strsplit(context, ":", 0);
	if (g_strv_length(tokens) < 2) {
		g_strfreev(tokens);
		return NULL;
	}

	/* Use the SELinux type as identification token. */
	ident = g_strdup(tokens[2]);

	g_strfreev(tokens);

	return ident;
}

static struct policy_config *create_policy(void)
{
	struct policy_config *policy;

	policy = g_new0(struct policy_config, 1);

	DBG("policy %p", policy);

	policy->config = connman_session_create_default_config();

	return policy;
}

static void selinux_context_reply(const unsigned char *context, void *user_data,
					int err)
{
	struct cb_data *cbd = user_data;
	connman_session_config_func_t cb = cbd->cb;
	struct create_data *data = cbd->data;
	struct policy_config *policy;
	struct policy_group *group;
	struct connman_session_config *config = NULL;
	char *ident = NULL;

	DBG("session %p", data->session);

	if (err < 0)
		goto done;

	DBG("SELinux context %s", context);

	ident = parse_selinux_type((const char*)context);
	if (ident == NULL) {
		err = -EINVAL;
		goto done;
	}

	policy = create_policy();
	policy->selinux = g_strdup(ident);
	policy->session = data->session;

	group = g_hash_table_lookup(selinux_hash, policy->selinux);
	if (group != NULL)
		set_policy(policy, group);

	g_hash_table_replace(session_hash, data->session, policy);
	config = policy->config;

done:
	(*cb)(data->session, config, cbd->user_data, err);

	g_free(cbd);
	g_free(data);
	g_free(ident);
}

static int policy_local_create(struct connman_session *session,
				connman_session_config_func_t cb,
				void *user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct create_data *data;
	const char *owner;
	int err;

	DBG("session %p", session);

	data = g_new0(struct create_data, 1);
	cbd->data = data;

	data->session = session;

	owner = connman_session_get_owner(session);

	err = connman_dbus_get_selinux_context(connection, owner,
					selinux_context_reply,
					cbd);
	if (err < 0) {
		connman_error("Could not get SELinux context");
		g_free(data);
		g_free(cbd);
		return err;
	}

	return 0;
}

static void policy_local_destroy(struct connman_session *session)
{
	struct policy_data *policy;

	DBG("session %p", session);

	policy = g_hash_table_lookup(session_hash, session);
	if (policy == NULL)
		return;

	g_hash_table_remove(session_hash, session);
}

static struct connman_session_policy session_policy_local = {
	.name = "session local policy configuration",
	.priority = CONNMAN_SESSION_POLICY_PRIORITY_DEFAULT,
	.create = policy_local_create,
	.destroy = policy_local_destroy,
};

static int load_keyfile(const char *pathname, GKeyFile **keyfile)
{
	GError *error = NULL;
	int err;

	*keyfile = g_key_file_new();

	if (g_key_file_load_from_file(*keyfile, pathname, 0, &error) == FALSE)
		goto err;

	return 0;

err:
	/*
	 * The fancy G_FILE_ERROR_* codes are identical to the native
	 * error codes.
	 */
	err = -error->code;

	DBG("Unable to load %s: %s", pathname, error->message);
	g_clear_error(&error);

	g_key_file_free(*keyfile);
	*keyfile = NULL;

	return err;
}

static int load_policy(GKeyFile *keyfile, const char *groupname,
			struct policy_group *group)
{
	struct connman_session_config *config = group->config;
	char *str, **tokens;
	int i, err = 0;

	group->selinux = g_key_file_get_string(keyfile, groupname,
						"selinux", NULL);
	if (group->selinux == NULL)
		return -EINVAL;

	config->priority = g_key_file_get_boolean(keyfile, groupname,
						"Priority", NULL);

	str = g_key_file_get_string(keyfile, groupname, "RoamingPolicy",
				NULL);
	if (str != NULL) {
		config->roaming_policy = connman_session_parse_roaming_policy(str);
		g_free(str);
	}

	str = g_key_file_get_string(keyfile, groupname, "ConnectionType",
				NULL);
	if (str != NULL) {
		config->type = connman_session_parse_connection_type(str);
		g_free(str);
	}

	config->ecall = g_key_file_get_boolean(keyfile, groupname,
						"EmergencyCall", NULL);

	str = g_key_file_get_string(keyfile, groupname, "AllowedBearers",
				NULL);
	if (str != NULL) {
		tokens = g_strsplit(str, " ", 0);

		for (i = 0; tokens[i] != NULL; i++) {
			err = connman_session_parse_bearers(tokens[i],
					&config->allowed_bearers);
			if (err < 0)
				break;
		}

		g_free(str);
		g_strfreev(tokens);
	}

	DBG("group %p selinux %s", group, group->selinux);

	return err;
}
static void update_session(struct policy_config *policy)
{
	DBG("policy %p session %p", policy, policy->session);

	if (policy->session == NULL)
		return;

	if (connman_session_config_update(policy->session) < 0)
		connman_session_destroy(policy->session);
}

static void set_default_config(gpointer user_data)
{
	struct policy_config *policy = user_data;

	connman_session_set_default_config(policy->config);
	policy->group = NULL;
	update_session(policy);
}

static void cleanup_config(gpointer user_data)
{
	struct policy_config *policy = user_data;

	DBG("policy %p group %p", policy, policy->group);

	if (policy->group != NULL)
		policy->group->sessions =
			g_slist_remove(policy->group->sessions, policy);

	g_slist_free(policy->config->allowed_bearers);
	g_free(policy->config);
	g_free(policy->selinux);
	g_free(policy);
}

static void cleanup_group(gpointer user_data)
{
	struct policy_group *group = user_data;

	DBG("group %p", group);

	g_slist_free_full(group->sessions, set_default_config);

	g_slist_free(group->config->allowed_bearers);
	g_free(group->config);
	if (group->selinux != NULL)
		g_hash_table_remove(selinux_hash, group->selinux);
	g_free(group->selinux);
	g_free(group);
}

static void cleanup_file(gpointer user_data)
{
	struct policy_file *file = user_data;

	DBG("file %p", file);

	g_slist_free_full(file->groups, cleanup_group);
	g_free(file);
}

static void recheck_sessions(void)
{
	GHashTableIter iter;
	gpointer value, key;
	struct policy_group *group = NULL;

	g_hash_table_iter_init(&iter, session_hash);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		struct policy_config *policy = value;

		if (policy->group != NULL)
			continue;

		group = g_hash_table_lookup(selinux_hash, policy->selinux);
		if (group != NULL) {
			set_policy(policy, group);
			update_session(policy);
		}
	}
}

static int load_file(const char *filename, struct policy_file *file)
{
	GKeyFile *keyfile;
	struct policy_group *group;
	char **groupnames;
	char *pathname;
	int err = 0, i;

	DBG("%s", filename);

	pathname = g_strdup_printf("%s/%s", POLICYDIR, filename);
	err = load_keyfile(pathname, &keyfile);
	g_free(pathname);

	if (err < 0)
		return err;

	groupnames = g_key_file_get_groups(keyfile, NULL);

	for (i = 0; groupnames[i] != NULL; i++) {
		group = g_new0(struct policy_group, 1);
		group->config = g_new0(struct connman_session_config, 1);

		err = load_policy(keyfile, groupnames[i], group);
		if (err < 0) {
			g_free(group->config);
			g_free(group);
			break;
		}
		g_hash_table_replace(selinux_hash, group->selinux, group);

		file->groups = g_slist_prepend(file->groups, group);
	}

	g_strfreev(groupnames);

	if (err < 0)
		g_slist_free_full(file->groups, cleanup_group);

	g_key_file_free(keyfile);

	return err;
}

static connman_bool_t is_filename_valid(const char *filename)
{
	if (filename == NULL)
		return FALSE;

	if (filename[0] == '.')
		return FALSE;

	return g_str_has_suffix(filename, ".policy");
}

static int read_policies()
{
	GDir *dir;
	const gchar *filename;
	struct policy_file *file;

	DBG("");

	dir = g_dir_open(POLICYDIR, 0, NULL);
	if (dir == NULL)
		return -EINVAL;

	while ((filename = g_dir_read_name(dir)) != NULL) {
		if (is_filename_valid(filename) == FALSE)
			continue;

		file = g_new0(struct policy_file, 1);
		if (load_file(filename, file) < 0) {
			g_free(file);
			continue;
		}

		g_hash_table_replace(file_hash, g_strdup(filename), file);
	}

	g_dir_close(dir);

	return 0;
}


static void notify_handler(struct inotify_event *event,
                                        const char *filename)
{
	struct policy_file *file;

	DBG("event %x file %s", event->mask, filename);

	if (event->mask & IN_CREATE)
		return;

	if (is_filename_valid(filename) == FALSE)
		return;

	/*
	 * load_file() will modify the global selinux/uid/gid hash
	 * tables. We need to remove the old entries first before
	 * else the table points to the wrong entries.
	 */
	g_hash_table_remove(file_hash, filename);

	if (event->mask & (IN_DELETE | IN_MOVED_FROM))
		return;

	if (event->mask & (IN_MOVED_TO | IN_MODIFY)) {
		connman_info("Policy update for '%s'", filename);

		file = g_new0(struct policy_file, 1);
		if (load_file(filename, file) < 0) {
			g_free(file);
			return;
		}

		g_hash_table_replace(file_hash, g_strdup(filename), file);
		recheck_sessions();
	}
}

static int session_policy_local_init(void)
{
	int err;

	DBG("");

	/* If the dir doesn't exist, create it */
	if (g_file_test(POLICYDIR, G_FILE_TEST_IS_DIR) == FALSE) {
		if (mkdir(POLICYDIR, MODE) < 0) {
			if (errno != EEXIST)
				return -errno;
		}
	}

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -EIO;

	file_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, cleanup_file);
	session_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal,
						NULL, cleanup_config);
	selinux_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
					NULL, NULL);

	err = connman_inotify_register(POLICYDIR, notify_handler);
	if (err < 0)
		goto err;

	err = connman_session_policy_register(&session_policy_local);
	if (err < 0)
		goto err_notify;

	read_policies();

	return 0;

err_notify:

	connman_inotify_unregister(POLICYDIR, notify_handler);

err:
	if (file_hash != NULL)
		g_hash_table_destroy(file_hash);

	if (session_hash != NULL)
		g_hash_table_destroy(session_hash);

	if (selinux_hash != NULL)
		g_hash_table_destroy(selinux_hash);

	connman_session_policy_unregister(&session_policy_local);

	dbus_connection_unref(connection);

	return err;
}

static void session_policy_local_exit(void)
{
	DBG("");

	g_hash_table_destroy(file_hash);
	g_hash_table_destroy(session_hash);
	g_hash_table_destroy(selinux_hash);

	connman_session_policy_unregister(&session_policy_local);

	dbus_connection_unref(connection);

	connman_inotify_unregister(POLICYDIR, notify_handler);
}

CONNMAN_PLUGIN_DEFINE(session_policy_local,
		"Session local file policy configuration plugin",
		VERSION, CONNMAN_PLUGIN_PRIORITY_DEFAULT,
		session_policy_local_init, session_policy_local_exit)

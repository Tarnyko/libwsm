/*
Wayland Security Module
Copyright (C) 2014 Martin Peres & Steve Dodier-Lazaro

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA	02110-1301	USA
*/

#include <sys/types.h>
#include <malloc.h>

#include <libwsm.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <wayland-util.h>
#include "config-parser.h"

#include "debug.h"

#define TMP_PREFIX					 	"../data/"
#define WSM_DEFAULT_POLICY_DIR			TMP_PREFIX"security/wsm/default"
#define WSM_DEFAULT_POLICY_PER_USER_DIR	TMP_PREFIX"security/wsm/per-user/default"

#define WSM_DEFAULT_DEFAULT_PATH		"*"
#define WSM_DEFAULT_DEFAULT_UID			0

#define WSM_SCREENSHOT "WSM_SCREENSHOT"
#define WSM_SCREENSHARING "WSM_SCREENSHARING"
#define WSM_VIRTUAL_KEYBOARD "WSM_VIRTUAL_KEYBOARD"
#define WSM_VIRTUAL_POINTING "WSM_VIRTUAL_POINTING"
#define WSM_GLOBAL_KEYBOARD_SEQUENCE "WSM_GLOBAL_KEYBOARD_SEQUENCE"
#define WSM_FORWARD_RESERVED_KEYBOARD_SEQUENCE "WSM_FORWARD_RESERVED_KEYBOARD_SEQUENCE"
#define WSM_CLIPBOARD_COPY "WSM_CLIPBOARD_COPY"
#define WSM_CLIPBOARD_PASTE "WSM_CLIPBOARD_PASTE"


struct wsm_default_t {
	/* List of policies for each application and UID */
	struct wl_list app_policies;
};

static struct wsm_default_t *_wsm_default_global = NULL;

struct wsm_default_client_t{
	/* Executable */
	wsm_client_info_t	*parent;
	char			 	*exe_path;
	pid_t			 	 pid;
	uid_t			 	 euid;
	struct wl_list		 caps;		 //TODO migrate to struct weston_config?
};

struct wsm_app_policy_t {
	unsigned int		 	 version;
	char					*exe_path;
	uid_t					 uid;
	struct weston_config	*config; //TODO migrate to a nicer type?
	struct wl_list		 	 link;
};

struct wsm_cap_t {
	const char	 	*cap_name;
	struct wl_list	 objects; /* wsm_objperm_t */
	struct wl_list	 link;
};

struct wsm_objperm_t {
	const char	 	*object;
	const char	 	*permission;
	struct wl_list	 link;
};

struct wsm_default_client_t *wsm_default_client_lookup (int socket);
struct wsm_default_client_t *wsm_default_client_new (int socket);
void wsm_default_client_delete (struct wsm_default_client_t *app);
void wsm_default_client_change_permission (struct wsm_default_client_t *app, const char *cap_name, void *object, void *permission);

struct wsm_app_policy_t *wsm_app_policy_new (struct wsm_default_t *global, const char *path, const uid_t uid, short * const existed);
void wsm_app_policy_register (struct wsm_default_t *global, struct wsm_app_policy_t *policy);
void wsm_app_policy_free (struct wsm_app_policy_t *policy);
struct wsm_app_policy_t *wsm_app_policy_lookup (struct wsm_default_t *global, const char *exe_path, const uid_t uid);

int _string_starts_with (const char *str, const char *prefix)
{
	size_t pre_len = strlen(prefix);
	size_t str_len = strlen(str);

	if (pre_len > str_len)
		return 0;
	else
		return (strncmp (prefix, str, pre_len) == 0);
}

struct wsm_app_policy_t *wsm_app_policy_new (struct wsm_default_t *global, const char *path, const uid_t uid, short * const existed)
{
	if (path == NULL)
		return NULL;

	// Factorising for later, will set to 1 when a policy is found to exist
	if (existed)
		*existed=0;

	struct weston_config *config;
	struct weston_config_section *section;
	char *exe_path = NULL;

	config = weston_config_parse(path);
	if (config != NULL) {
		fprintf (stdout, "Using config file '%s'\n", weston_config_get_full_path(config));
	} else {
		fprintf(stderr, "wsm_app_policy_new: Could not parse policy file '%s'.\n", path);
	}
	section = weston_config_get_section(config, "Wayland Security Entry", NULL, NULL);
	weston_config_section_get_string(section, "Exec", &exe_path, NULL);

	if (!exe_path) {
		fprintf (stderr, "wsm_app_policy_new: Policy file '%s' is missing an executable path and will be discarded.\n", path);
		weston_config_destroy (config);
		return NULL;
	}

	struct wsm_app_policy_t *existing;

	if ((existing = wsm_app_policy_lookup (global, exe_path, uid)) != NULL)
	{
		printf ("wsm_app_policy_new: Found an existing policy for '%s;%d'.\n", exe_path, uid);
		if (existed)
			*existed=1;

		weston_config_destroy (config);
		free (exe_path);
		return existing;
	} else {
		printf ("wsm_app_policy_new: Created a new policy for '%s;%d'.\n", exe_path, uid);
		struct wsm_app_policy_t *policy = malloc(sizeof (struct wsm_app_policy_t));
		policy->version = (const unsigned int) 1;
		policy->exe_path = exe_path;
		policy->uid = (const uid_t) uid;
		policy->config = config;

		wsm_app_policy_register (global, policy);

		return policy;
	}
}

void wsm_app_policy_register (struct wsm_default_t *global, struct wsm_app_policy_t *policy)
{
	wl_list_insert(&global->app_policies, &policy->link);
}

void wsm_app_policy_free (struct wsm_app_policy_t *policy)
{
	if (!policy)
		return;

	free(policy->exe_path);
	weston_config_destroy(policy->config);
	free(policy);
}

struct wsm_app_policy_t *wsm_app_policy_lookup (struct wsm_default_t *global, const char *exe_path, const uid_t uid)
{
	struct wsm_app_policy_t *policy;

	wl_list_for_each(policy, &global->app_policies, link) {
		if (exe_path)
			if (strcmp(policy->exe_path, exe_path) == 0 && policy->uid == uid)
				return policy;
	}

	return NULL;
}

static int _filter_uid (const struct dirent *dir)
{
	if (dir == NULL)
		return 0;

	if (dir->d_type != DT_DIR)
		return 0;

	if (dir->d_name[0] == '.')
		return 0;

	errno = 0;
	long int uid = strtol (dir->d_name, 0, 10);
	if (errno)
	{
		perror ("Error when scanning for a user policy directory: folder name does not look like an UID.");
		return 0;
	} else
		return uid > 0;
}

static int _filter_all_files (const struct dirent *dir)
{
	if (dir == NULL)
		return 0;

	if (dir->d_type != DT_REG)
		return 0;

		return 1;
}

int scan_policy_folder (struct wsm_default_t *global, const char *path, const uid_t uid)
{
	int created_files = 0;

	if (uid)
		printf ("Scanning directory '%s' for user '%d' policies...\n", path, uid);
	else
		printf ("Scanning directory '%s' for policies...\n", path);

	struct dirent **namelist = NULL;
	int nb_files = scandir (path, &namelist, _filter_all_files, alphasort);

	if (nb_files == -1 && errno != ENOENT)
		perror ("Error when scanning a policy directory");
	else
		printf ("%d %s found in '%s'.\n", nb_files, (nb_files!=1? "policies":"policy"), path);

	if (nb_files > 0) {
		size_t pathlen = strlen (path);

		int i=0;
		for(i=0; i<nb_files; ++i) {
			struct dirent *ent = namelist[i];
			char *ini_path = malloc (pathlen + strlen(ent->d_name) + 2);
			sprintf (ini_path, "%s/%s", path, ent->d_name);

			short existed;
			struct wsm_app_policy_t *policy = wsm_app_policy_new (global, ini_path, uid, &existed);
			if (policy && !existed)
				++created_files;

			free(ini_path);
			free(namelist[i]);
		}
		free (namelist);
	}

	return created_files;
}

static void _free_policy_list(struct wsm_default_t *global)
{
	struct wsm_app_policy_t *policy;

	wl_list_for_each(policy, &global->app_policies, link) {
		wsm_app_policy_free(policy);
	}
}

static int _init_policy_list(struct wsm_default_t *global)
{
	if(!global)
		return -1;

	wl_list_init (&global->app_policies);
	int total_policies = 0;

	DEBUG ("Scanning user directories for per-user policies...\n");
	struct dirent **namelist = NULL;
	int nb_users = scandir (WSM_DEFAULT_POLICY_PER_USER_DIR, &namelist, _filter_uid, alphasort);
	if (nb_users == -1 && errno != ENOENT) {
		DEBUG ("An error occurred when scanning user directories in '%s'.\n", WSM_DEFAULT_POLICY_PER_USER_DIR);
	} else
		DEBUG ("%d %s found.\n", nb_users, (nb_users!=1? "user directories":"user directory"));

	if (nb_users > 0) {	
		size_t pathlen = strlen (WSM_DEFAULT_POLICY_PER_USER_DIR);
		int i;
		for (i=0; i<nb_users; ++i)
		{
			struct dirent *ent = namelist[i];
			char *folder_path = malloc (pathlen + strlen(ent->d_name) + 2);
			sprintf (folder_path, "%s/%s", WSM_DEFAULT_POLICY_PER_USER_DIR, ent->d_name);
			long int uid = strtol(ent->d_name, 0, 10);
		
			int nb_policies = scan_policy_folder (global, folder_path, uid);
			if (nb_policies < 0)
				fprintf (stderr, "An error occurred when looking for policies in '%s'.\n", folder_path);
			else
				total_policies += nb_policies;

			free(folder_path);
			free(namelist[i]);
		}
		free (namelist);
	}
	printf ("\n");

	printf ("Scanning the default policy directory...\n");
	int nb_policies = scan_policy_folder (global, WSM_DEFAULT_POLICY_DIR, 0);
	if (nb_policies < 0)
		fprintf (stderr, "An error occurred when looking for policies in '%s'.\n", WSM_DEFAULT_POLICY_DIR);
	else
		total_policies += nb_policies;
	printf ("\n");

	printf ("%d %s were loaded in total.\n", total_policies, (total_policies!=1? "policies":"policy"));

	return total_policies;
}

void dtor(void *global)
{
	if(!global) {
		DEBUG("WSM Default Backend: libwsm attempted to have me delete my internal data by passing a NULL pointer. This is a bug, please report it to the libwsm developers.\n");
		return;
	}

	_free_policy_list(global);
	free(global);
}

void *ctor(void)
{
	void *global = malloc(sizeof(struct wsm_default_t));
	if(!global)
		return NULL;

	if (!_init_policy_list(global)) {
		DEBUG("WSM Default Backend: could not find any policy file, please check you installed them at the right location.\nI expected to find default policies at '%s' and per-user policies at '%s' (in folders named after users' UIDs).\n", WSM_DEFAULT_POLICY_DIR, WSM_DEFAULT_POLICY_PER_USER_DIR);
		free(global);
		return NULL;
	}

	if(_wsm_default_global)
		dtor(_wsm_default_global);
	_wsm_default_global = global;

	return global;
}

const char* get_module_name()
{
	return "default";
}

unsigned int get_ABI_version()
{
	return 1;
}

void *client_new(wsm_client_info_t info)
{
	if(!_wsm_default_global) {
		DEBUG("WSM Default Backend: libwsm attempted to have me initialise a policy for a new client but I am not initialised or I have been deleted. This is a bug, please report it to the libwsm developers.\n");
		return NULL;
	}

	struct wsm_default_t *global = _wsm_default_global;

	fprintf (stderr, "UNABLE TO PROCEED: I NEED THE COMPOSITOR'S IDENTITY.\n");
	return NULL;

	struct wsm_default_client_t *client = malloc(sizeof(struct wsm_default_client_t));

	if (info.fullpath == NULL || info.uid < 0 || info.pid <= 0) {
		DEBUG("Default module: I was asked to create a new client with invalid information. This should be a bug in libwsm. Path:'%s'\tUID:%d\tPID:%d.\n", info.fullpath, info.uid, info.pid);
		return NULL;
	}

	client->exe_path = strndup(info.fullpath, PATH_MAX);
	client->pid = info.pid;
	client->euid = info.uid;

	struct wsm_app_policy_t *pol = NULL;

	pol = wsm_app_policy_lookup(global, client->exe_path, client->euid);

	if (!pol)
		pol = wsm_app_policy_lookup(global, client->exe_path, WSM_DEFAULT_DEFAULT_UID);

	if (!pol)
		pol = wsm_app_policy_lookup(global, WSM_DEFAULT_DEFAULT_PATH, client->euid);

	if (!pol)
		pol = wsm_app_policy_lookup(global, WSM_DEFAULT_DEFAULT_PATH, WSM_DEFAULT_DEFAULT_UID);

	//TODO initialise the client caps
	//struct wl_list	caps;


	return (void *) client;
}

void client_free(void *client)
{
	free((struct wsm_default_client_t *)client);
}

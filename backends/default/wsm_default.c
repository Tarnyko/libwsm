/*
Wayland Security Module - Default Backend
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

#include <wsm/debug.h>

#define WSM_DEFAULT_POLICY_DIR			WSM_DEFAULT_POLICY_PREFIX"/wsm/default"
#define WSM_DEFAULT_POLICY_PER_USER_DIR		WSM_DEFAULT_POLICY_PREFIX"/wsm/per-user/default"
#define WSM_DEFAULT_ALL_COMPOSITORS		"All Compositors"

#define WSM_DEFAULT_DEFAULT_PATH		"*"
#define WSM_DEFAULT_TEMPLATE_PATH		"?"
#define WSM_DEFAULT_DEFAULT_UID			-1

#define WSM_DEFAULT_MAIN_SECTION_KEY	"Wayland Security Entry"
#define WSM_DEFAULT_KEY_EXECUTABLE		"Exec"
#define WSM_DEFAULT_KEY_TEMPLATE		"Template"

#define WSM_SCREENSHOT "WSM_SCREENSHOT"
#define WSM_SCREENSHARING "WSM_SCREENSHARING"
#define WSM_VIRTUAL_KEYBOARD "WSM_VIRTUAL_KEYBOARD"
#define WSM_VIRTUAL_POINTING "WSM_VIRTUAL_POINTING"
#define WSM_GLOBAL_KEYBOARD_SEQUENCE "WSM_GLOBAL_KEYBOARD_SEQUENCE"
#define WSM_FORWARD_RESERVED_KEYBOARD_SEQUENCE "WSM_FORWARD_RESERVED_KEYBOARD_SEQUENCE"
#define WSM_CLIPBOARD_COPY "WSM_CLIPBOARD_COPY"
#define WSM_CLIPBOARD_PASTE "WSM_CLIPBOARD_PASTE"


struct wsm_default_t {
	/* Name of the current compositor */
	char *compositor_name;

	/* List of policies for each application and UID */
	struct wl_list app_policies;
};

static struct wsm_default_t *_wsm_default_global = NULL;

struct wsm_default_client_t{
	wsm_client_info_t		 info;
	struct weston_config	*policy;
};

struct wsm_app_policy_t {
	unsigned int		 	 version;
	char					*exe_path;
	char					*template_name;
	signed long				 uid;		/* Can be -1 for the default UID */
	struct weston_config	*config;
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

struct wsm_default_client_t *wsm_default_client_lookup(int socket);
struct wsm_default_client_t *wsm_default_client_new(int socket);
void wsm_default_client_delete(struct wsm_default_client_t *app);
void wsm_default_client_change_permission(struct wsm_default_client_t *app, const char *cap_name, void *object, void *permission);

struct wsm_app_policy_t *wsm_app_policy_new(struct wsm_default_t *global, const char *path, const signed long uid, short * const existed);
void wsm_app_policy_register(struct wsm_default_t *global, struct wsm_app_policy_t *policy);
void wsm_app_policy_free(struct wsm_app_policy_t *policy);
struct wsm_app_policy_t *wsm_app_policy_lookup(struct wsm_default_t *global, const char *exe_path, const signed long uid);
struct wsm_app_policy_t *wsm_app_template_lookup(struct wsm_default_t *global, const char *template_name, const signed long uid);

static int _string_starts_with(const char *str, const char *prefix)
{
	size_t pre_len = strlen(prefix);
	size_t str_len = strlen(str);

	if (pre_len > str_len)
		return 0;
	else
		return (strncmp(prefix, str, pre_len) == 0);
}

static int _string_ends_with(const char *str, const char *suffix)
{
	size_t suf_len = strlen(suffix);
	size_t str_len = strlen(str);

	if (suf_len > str_len)
		return 0;
	else
	{
		size_t delta = str_len - suf_len;
		return (strncmp(suffix, str+delta, suf_len) == 0);
	}
}

static struct wsm_app_policy_t *_wsm_app_template_new(struct wsm_default_t *global, struct weston_config *config, const char *path, char *exe_path, char *template_name, const signed long uid, short * const existed)
{
	struct wsm_app_policy_t	*existing = NULL;

	if (!template_name) {
		DEBUG("Default Backend: wsm_app_policy_new: Template policy file '%s' is missing a name and will be discarded.\n", path);
		weston_config_destroy(config);
		free(exe_path);
		return NULL;
	}

	if ((existing = wsm_app_template_lookup(global, template_name, uid)) != NULL)
	{
		DEBUG("Default Backend: wsm_app_policy_new: Found an existing template for template name '%s;%ld'.\n", template_name, uid);
		if (existed)
			*existed=1;

		free(exe_path);
		if (template_name) free(template_name);
		weston_config_destroy(config);
		return existing;
	} else {
		if (existed)
			*existed=0;
		struct wsm_app_policy_t *policy = malloc(sizeof(struct wsm_app_policy_t));
		policy->version = (const unsigned int) 1;
		policy->uid = uid;
		policy->exe_path = exe_path;
		policy->template_name = template_name;
		policy->config = config;
		DEBUG("Default Backend: wsm_app_policy_new: Created a new template for '%s;%ld'.\n", template_name, uid);

		wsm_app_policy_register(global, policy);
		return policy;
	}
}

//TODO static struct wsm_app_policy_t *_wsm_app_override_new(...);

static struct wsm_app_policy_t *_wsm_app_policy_new(struct wsm_default_t *global, struct weston_config *config, const char *path, char *exe_path, char *template_name, const signed long uid, short * const existed)
{
	if (!_string_starts_with(exe_path, "/") && strcmp(exe_path, WSM_DEFAULT_DEFAULT_PATH)) {
		DEBUG("Default Backend: wsm_app_policy_new: Policy file '%s' 's executable path should be an absolute path or the wildcard '%s' (which matches all executables).\n", path, WSM_DEFAULT_DEFAULT_PATH);
		free(exe_path);
		if (template_name) free(template_name);
		weston_config_destroy(config);
		return NULL;
	}

	struct wsm_app_policy_t *existing;

	if ((existing = wsm_app_policy_lookup(global, exe_path, uid)) != NULL)
	{
		DEBUG("Default Backend: wsm_app_policy_new: Found an existing policy for '%s;%ld'.\n", exe_path, uid);
		if (existed)
			*existed=1;

		free(exe_path);
		if (template_name) free(template_name);
		weston_config_destroy(config);
		return existing;
	} else {
		if (existed)
			*existed=0;
		struct wsm_app_policy_t *policy = malloc(sizeof(struct wsm_app_policy_t));
		policy->version = (const unsigned int) 1;
		policy->exe_path = exe_path;
		policy->uid = uid;

		if (template_name) {
			DEBUG("Default Backend: wsm_app_policy_new: Created a new policy for '%s;%ld' after template '%s'.\n", exe_path, uid, template_name);
			weston_config_destroy(config);
			policy->config = NULL;
			policy->template_name = template_name;
		} else {
			DEBUG("Default Backend: wsm_app_policy_new: Created a new policy for '%s;%ld'.\n", exe_path, uid);
			policy->config = config;
			policy->template_name = NULL;
		}

		wsm_app_policy_register(global, policy);
		return policy;
	}
}

/* DO NOT MODIFY THIS FUNCTION without adapting _wsm_app_*_new. Pay particular
 * attention to the fact that the ownership of exe_path, template_name and
 * config is transferred to said functions. */
struct wsm_app_policy_t *wsm_app_policy_new(struct wsm_default_t *global, const char *path, const signed long uid, short * const existed)
{
	if (path == NULL)
		return NULL;

	struct weston_config *config;
	struct weston_config_section *section;
	char *exe_path = NULL;
	char *template_name = NULL;

	config = weston_config_parse(path);
	if (config != NULL) {
		DEBUG("Default Backend: wsm_app_policy_new: Using config file '%s'\n", weston_config_get_full_path(config));
	} else {
		DEBUG("Default Backend: wsm_app_policy_new: Could not parse policy file '%s'.\n", path);
	}
	section = weston_config_get_section(config, WSM_DEFAULT_MAIN_SECTION_KEY, NULL, NULL);

	weston_config_section_get_string(section, WSM_DEFAULT_KEY_EXECUTABLE, &exe_path, NULL);
	if (!exe_path) {
		DEBUG("Default Backend: wsm_app_policy_new: Policy file '%s' is missing an executable path and will be discarded.\n", path);
		weston_config_destroy(config);
		return NULL;
	}

	weston_config_section_get_string(section, WSM_DEFAULT_KEY_TEMPLATE, &template_name, NULL);

	/* Template file */
	if (strcmp(exe_path, WSM_DEFAULT_TEMPLATE_PATH) == 0) {
		DEBUG("Default Backend: wsm_app_policy_new: Policy file '%s' is a policy template named '%s'.\n", path, template_name);
		return _wsm_app_template_new(global, config, path, exe_path, template_name, uid, existed);
	}
	/* Override file (TODO) */
	else if (0) {
		//TODO create override
		//TODO check if template_name and exe_path need to be freed
		//return _wsm_app_override_new(global, config, path, exe_path, uid, existed);
	}
	/* Normal file (either app-specific or default policy */
	else
		return _wsm_app_policy_new(global, config, path, exe_path, template_name, uid, existed);
}

void wsm_app_policy_register(struct wsm_default_t *global, struct wsm_app_policy_t *policy)
{
	wl_list_insert(&global->app_policies, &policy->link);
}

void wsm_app_policy_free(struct wsm_app_policy_t *policy)
{
	if (!policy)
		return;

	if (policy->exe_path)
		free(policy->exe_path);
	if (policy->template_name)
		free(policy->template_name);
	if (policy->config)
		weston_config_destroy(policy->config);

	free(policy);
}

struct wsm_app_policy_t *wsm_app_policy_lookup(struct wsm_default_t *global, const char *exe_path, const signed long uid)
{
	if(!exe_path || (uid < 0 && uid != WSM_DEFAULT_DEFAULT_UID))
		return NULL;

	struct wsm_app_policy_t *policy;

	wl_list_for_each(policy, &global->app_policies, link) {
		if (strcmp(policy->exe_path, exe_path) == 0 && policy->uid == uid)
			return policy;
	}

	return NULL;
}

struct wsm_app_policy_t *wsm_app_template_lookup(struct wsm_default_t *global, const char *template_name, const signed long uid)
{
	if(!template_name || (uid < 0 && uid != WSM_DEFAULT_DEFAULT_UID))
		return NULL;

	struct wsm_app_policy_t *policy;

	wl_list_for_each(policy, &global->app_policies, link) {
		if (policy->template_name && strcmp(policy->template_name, template_name) == 0 && (strcmp(policy->exe_path, WSM_DEFAULT_TEMPLATE_PATH) == 0) && (policy->uid == uid))
			return policy;
	}

	return NULL;
}

static int _filter_uid(const struct dirent *dir)
{
	if (dir == NULL)
		return 0;

	if (dir->d_type != DT_DIR)
		return 0;

	if (dir->d_name[0] == '.')
		return 0;

	errno = 0;
	long int uid = strtol(dir->d_name, 0, 10);
	if (errno)
	{
		DEBUG("Default Backend: Error when scanning for a user policy directory: folder name does not look like an UID (%s).", strerror(errno));
		return 0;
	} else
		return uid > 0;
}

static int _filter_ini_files(const struct dirent *dir)
{
	if (dir == NULL)
		return 0;

	if (dir->d_type != DT_REG)
		return 0;

	return _string_ends_with (dir->d_name, ".ini");
}

int scan_policy_folder(struct wsm_default_t *global, const char *path, const signed long uid)
{
	int created_files = 0;

	if (uid != WSM_DEFAULT_DEFAULT_UID) {
		DEBUG("Default Backend: Scanning directory '%s' for user '%ld' policies...\n", path, uid);
	} else {
		DEBUG("Default Backend: Scanning directory '%s' for policies...\n", path);
	}

	struct dirent **namelist = NULL;
	int nb_files = scandir(path, &namelist, _filter_ini_files, alphasort);

	if (nb_files == -1 && errno != ENOENT) {
		DEBUG("Default Backend: Error when scanning a policy directory");
	} else {
		DEBUG("Default Backend: %d %s found in '%s'.\n", nb_files, (nb_files!=1? "policies":"policy"), path);
	}

	if (nb_files > 0) {
		size_t pathlen = strlen(path);

		int i=0;
		for(i=0; i<nb_files; ++i) {
			struct dirent *ent = namelist[i];
			char *ini_path = malloc(pathlen + strlen(ent->d_name) + 2);
			sprintf(ini_path, "%s/%s", path, ent->d_name);

			short existed;
			struct wsm_app_policy_t *policy = wsm_app_policy_new(global, ini_path, uid, &existed);
			if (policy && !existed)
				++created_files;

			free(ini_path);
			free(namelist[i]);
		}
		free(namelist);
	}

	return created_files;
}

static void _free_policy_list(struct wsm_default_t *global)
{
	struct wsm_app_policy_t *pol, *next_pol;

	if (global == NULL)
		return;

	wl_list_for_each_safe(pol, next_pol, &global->app_policies, link) {
		wsm_app_policy_free(pol);
	}
}

static int _init_policy_list(struct wsm_default_t *global)
{
	if(!global)
		return -1;

	wl_list_init(&global->app_policies);
	int total_policies = 0;

	DEBUG("Default Backend: Scanning user directories for per-user policies...\n");
	struct dirent **namelist = NULL;
	int nb_users = scandir(WSM_DEFAULT_POLICY_PER_USER_DIR, &namelist, _filter_uid, alphasort);
	if (nb_users == -1 && errno != ENOENT) {
		DEBUG("Default Backend: An error occurred when scanning user directories in '%s'.\n", WSM_DEFAULT_POLICY_PER_USER_DIR);
	} else
		DEBUG("Default Backend: %d %s found.\n", nb_users, (nb_users!=1? "user directories":"user directory"));

	if (nb_users > 0) {
		size_t pathlen = strlen(WSM_DEFAULT_POLICY_PER_USER_DIR);
		int i;
		for (i=0; i<nb_users; ++i) {
			struct dirent *ent = namelist[i];
			char *folder_path = malloc(pathlen + strlen(ent->d_name) + 2);
			sprintf(folder_path, "%s/%s", WSM_DEFAULT_POLICY_PER_USER_DIR, ent->d_name);
			long uid = strtol(ent->d_name, 0, 10);

			int nb_policies = scan_policy_folder(global, folder_path, uid);
			if (nb_policies < 0) {
				DEBUG("Default Backend: An error occurred when looking for policies in '%s'.\n", folder_path);
			} else {
				total_policies += nb_policies;
			}

			free(folder_path);
			free(namelist[i]);
		}
		free(namelist);
	}

	DEBUG("Default Backend: Scanning the default policy directory...\n");
	int nb_policies = scan_policy_folder(global, WSM_DEFAULT_POLICY_DIR, WSM_DEFAULT_DEFAULT_UID);
	if (nb_policies < 0) {
		DEBUG("Default Backend: An error occurred when looking for policies in '%s'.\n", WSM_DEFAULT_POLICY_DIR);
	} else {
		total_policies += nb_policies;
	}

	DEBUG("Default Backend: %d %s were loaded in total.\n", total_policies, (total_policies!=1? "policies":"policy"));

	return total_policies;
}

void dtor(void *p_global)
{
	struct wsm_default_t *global = p_global;

	if(!global) {
		DEBUG("Default Backend: dtor: libwsm attempted to have me delete my internal data by passing a NULL pointer. This is a bug, please report it to the libwsm developers.\n");
		return;
	}

	_free_policy_list(global);
	free(global->compositor_name);
	free(global);
}

void *ctor(const char *compositor_name)
{
	struct wsm_default_t *global = malloc(sizeof(struct wsm_default_t));
	if(!global)
		return NULL;

	if (!_init_policy_list(global)) {
		DEBUG("Default Backend: ctor: could not find any policy file, please check you installed them at the right location.\nI expected to find default policies at '%s' and per-user policies at '%s' (in folders named after users' UIDs).\n", WSM_DEFAULT_POLICY_DIR, WSM_DEFAULT_POLICY_PER_USER_DIR);
		free(global);
		return NULL;
	}

	global->compositor_name = strdup(compositor_name);

	if(_wsm_default_global)
		dtor(_wsm_default_global);
	_wsm_default_global = global;

	return (void *)global;
}

const char* get_backend_name()
{
	return "default";
}

unsigned int get_ABI_version()
{
	return 1;
}

static struct wsm_app_policy_t *_wsm_app_policy_lookup_and_resolve(struct wsm_default_t *global, const char *exe_path, const signed long uid)
{
	struct wsm_app_policy_t *pol = wsm_app_policy_lookup(global, exe_path, uid);

	if (!pol)
		return NULL;

	if (pol->template_name)
		return wsm_app_template_lookup(global, pol->template_name, pol->uid);
	else
		return pol;
}

void *client_create(wsm_client_info_t info)
{
	if(!_wsm_default_global) {
		DEBUG("Default Backend: client_new: libwsm attempted to have the default backend initialise a policy for a new client but it is not initialised or has been deleted. This is a bug, please report it to the libwsm developers.\n");
		return NULL;
	}

	if (info.fullpath == NULL || info.uid < 0 || info.pid <= 0) {
		DEBUG("Default Backend: client_new: the default backend was asked to create a new client with invalid information. This should be a bug in libwsm. Path:'%s'\tUID:%d\tPID:%d.\n", info.fullpath, info.uid, info.pid);
		return NULL;
	}

	struct wsm_default_t		*global	= _wsm_default_global;
	struct wsm_default_client_t	*client	= NULL;
	struct wsm_app_policy_t		*pol	= NULL;

	client = malloc(sizeof(struct wsm_default_client_t));
	if (!client) {
		DEBUG("Default Backend: client_new: ran out of memory whilst creating a new client. Aborting.\n");
		return NULL;
	}

	if ((pol = _wsm_app_policy_lookup_and_resolve(global, info.fullpath, info.uid)) != NULL) {
	} else if ((pol = _wsm_app_policy_lookup_and_resolve(global, info.fullpath, WSM_DEFAULT_DEFAULT_UID)) != NULL) {
	} else if ((pol = _wsm_app_policy_lookup_and_resolve(global, WSM_DEFAULT_DEFAULT_PATH, info.uid)) != NULL) {
	} else if ((pol = _wsm_app_policy_lookup_and_resolve(global, WSM_DEFAULT_DEFAULT_PATH, WSM_DEFAULT_DEFAULT_UID)) != NULL) {
	}

	if (!pol) {
		DEBUG("Default Backend: No policy could be found for client '%s\tUID:%d\tPID:%d', this is probably a bug in the backend or a mistake in your system configuration.\n", info.fullpath, info.uid, info.pid);
		free(client);
		return NULL;
	}

	if (pol->template_name) {
		DEBUG("Default Backend: The policy for client '%s\tUID:%d\tPID:%d' is a template named '%s'.\n", info.fullpath, info.uid, info.pid, pol->template_name);
	}

	client->info = info;
	client->policy = wsm_weston_config_copy(pol->config);

	return (void *) client;
}

void client_destroy(void *generic_client)
{
	struct wsm_default_client_t *client = (struct wsm_default_client_t *)generic_client;

	weston_config_destroy(client->policy);
	free(client);
}

static char *_get_permission(void *generic_client, const char *capability, const char *object)
{
	if(!_wsm_default_global) {
		DEBUG("Default Backend: get_permission: libwsm attempted to ask the default backend to make a security decision before initialising the backend. This is a bug, please report it to the libwsm developers.\n");
		return WSM_DECISION_ERROR;
	}

	struct wsm_default_t			*global		= _wsm_default_global;
	struct wsm_default_client_t		*client		= (struct wsm_default_client_t *)generic_client;
	struct weston_config_section	*section	= NULL;

	if (!client) {
		DEBUG("Default Backend: Was asked to retrieve a permission for a non-existent client (capability was '%s' and object '%s').\n", capability, object);
		return NULL;
	}

	if (!capability) {
		DEBUG("Default Backend: Was asked to retrieve a permission but was not told for which capability (client was '%s:%d' and object '%s').\n", client->info.fullpath, client->info.pid, object);
		return NULL;
	}

	/* object can legitimately be NULL for many capabilities */

	section = weston_config_get_section_with_key(client->policy, global->compositor_name, capability);
	if (!section)
		section = weston_config_get_section_with_key(client->policy, WSM_DEFAULT_ALL_COMPOSITORS, capability);
	if (!section) {
		DEBUG("Default Backend: get_permission: Client '%s:%d' asked to perform '%s' on object '%s', and it was denied by default because no relevant policy could be found.\n", client->info.fullpath, client->info.pid, capability, object);
		return strdup(WSM_SOFT_DENY_KEY); /* no compatible policy for this compositor */
	} else {
		char *value = NULL;
		if (!weston_config_section_get_string(section, capability, &value, NULL)) {
			int is_array = 0; //FIXME implement support for array capabilities
			//TODO parse to verify of it's an array.
			if(is_array) {
				//TODO find object-matching value
				//TODO strdup matching value into a new variable
				free(value);
				//TODO clean up other things
				//TODO return matching value variable as is
			} else {
				DEBUG("Default Backend: get_permission: Client '%s:%d' asked to perform '%s' on object '%s', and permission '%s' was read from the policy.\n", client->info.fullpath, client->info.pid, capability, object, value);
				return value;
			}
		}
	}

	DEBUG("Default Backend: get_permission: Client '%s:%d' asked to perform '%s' on object '%s', and it was denied by default because no relevant policy could be found.\n", client->info.fullpath, client->info.pid, capability, object);
	return strdup(WSM_SOFT_DENY_KEY); /* no policy for this capability */
}

/* TODO: refactor config to add integers to keys at file loading time for performance */
wsm_decision_t get_permission(void *generic_client, const char *capability, const char *object)
{
	char			*perm	= _get_permission(generic_client, capability, object);
	wsm_decision_t	 dec	= WSM_DECISION_ERROR;

	if (strcmp (perm, WSM_DENY_KEY) == 0)
		dec = WSM_DECISION_DENY;
	else if (strcmp (perm, WSM_SOFT_DENY_KEY) == 0)
		dec = WSM_DECISION_SOFT_DENY;
	else if (strcmp (perm, WSM_SOFT_ALLOW_KEY) == 0)
		dec = WSM_DECISION_SOFT_ALLOW;
	else if (strcmp (perm, WSM_ALLOW_KEY) == 0)
		dec = WSM_DECISION_ALLOW;

	free (perm);
	return dec;
}

char *get_custom_permission(void *generic_client, const char *capability, const char *object)
{
	return _get_permission(generic_client, capability, object);
}

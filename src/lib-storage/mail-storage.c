/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "llist.h"
#include "mail-storage.h"
#include "str.h"
#include "str-parse.h"
#include "sha1.h"
#include "unichar.h"
#include "hex-binary.h"
#include "fs-api.h"
#include "file-dotlock.h"
#include "file-create-locked.h"
#include "istream.h"
#include "eacces-error.h"
#include "mkdir-parents.h"
#include "time-util.h"
#include "wildcard-match.h"
#include "settings.h"
#include "dsasl-client.h"
#include "imap-date.h"
#include "mail-index-private.h"
#include "mail-index-alloc-cache.h"
#include "mailbox-tree.h"
#include "mailbox-list-private.h"
#include "mail-storage-private.h"
#include "mail-storage-service.h"
#include "mail-storage-settings.h"
#include "mail-namespace.h"
#include "mail-search.h"
#include "mail-search-register.h"
#include "mail-search-mime-register.h"
#include "mailbox-search-result-private.h"
#include "mailbox-guid-cache.h"
#include "mail-cache.h"
#include "utc-mktime.h"

#include <ctype.h>

#define MAILBOX_DELETE_RETRY_SECS 30
#define MAILBOX_MAX_HIERARCHY_NAME_LENGTH 255

extern struct mail_search_register *mail_search_register_imap4rev2;
extern struct mail_search_register *mail_search_register_imap4rev1;
extern struct mail_search_register *mail_search_register_human;

struct event_category event_category_storage = {
	.name = "storage",
};
struct event_category event_category_mailbox = {
	.parent = &event_category_storage,
	.name = "mailbox",
};
struct event_category event_category_mail = {
	.parent = &event_category_mailbox,
	.name = "mail",
};

struct mail_storage_module_register mail_storage_module_register = { 0 };
struct mail_module_register mail_module_register = { 0 };

struct mail_storage_mail_index_module mail_storage_mail_index_module =
	MODULE_CONTEXT_INIT(&mail_index_module_register);
ARRAY_TYPE(mail_storage) mail_storage_classes;

static int mail_storage_init_refcount = 0;

static const char *
mailbox_get_name_without_prefix(struct mail_namespace *ns,
				const char *vname)
{
	if (ns->prefix_len > 0 &&
	    strncmp(ns->prefix, vname, ns->prefix_len-1) == 0) {
		if (vname[ns->prefix_len-1] == mail_namespace_get_sep(ns))
			vname += ns->prefix_len;
		else if (vname[ns->prefix_len-1] == '\0') {
			/* namespace prefix itself */
			vname = "";
		}
	}
	return vname;
}

void mail_storage_init(void)
{
	if (mail_storage_init_refcount++ > 0)
		return;
	dsasl_clients_init();
	mailbox_attributes_init();
	mailbox_lists_init();
	mail_storage_hooks_init();
	i_array_init(&mail_storage_classes, 8);
	mail_storage_register_all();
	mailbox_list_register_all();
	settings_info_register(&mail_storage_setting_parser_info);
}

void mail_storage_deinit(void)
{
	i_assert(mail_storage_init_refcount > 0);
	if (--mail_storage_init_refcount > 0)
		return;
	if (mail_search_register_human != NULL)
		mail_search_register_deinit(&mail_search_register_human);
	if (mail_search_register_imap4rev1 != NULL)
		mail_search_register_deinit(&mail_search_register_imap4rev1);
	if (mail_search_register_imap4rev2 != NULL)
		mail_search_register_deinit(&mail_search_register_imap4rev2);
	mail_search_mime_register_deinit();
	if (array_is_created(&mail_storage_classes))
		array_free(&mail_storage_classes);
	mail_storage_hooks_deinit();
	mailbox_lists_deinit();
	mailbox_attributes_deinit();
	dsasl_clients_deinit();
}

void mail_storage_class_register(struct mail_storage *storage_class)
{
	i_assert(mail_storage_find_class(storage_class->name) == NULL);

	if (storage_class->set_info != NULL)
		settings_info_register(storage_class->set_info);

	/* append it after the list, so the autodetection order is correct */
	array_push_back(&mail_storage_classes, &storage_class);
}

void mail_storage_class_unregister(struct mail_storage *storage_class)
{
	unsigned int i;

	if (!array_lsearch_ptr_idx(&mail_storage_classes, storage_class, &i))
		i_unreached();
	array_delete(&mail_storage_classes, i, 1);
}

struct mail_storage *mail_storage_find_class(const char *name)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	i_assert(name != NULL);

	classes = array_get(&mail_storage_classes, &count);
	for (i = 0; i < count; i++) {
		if (strcasecmp(classes[i]->name, name) == 0)
			return classes[i];
	}
	return NULL;
}

static struct mail_storage *
mail_storage_autodetect(const struct mail_namespace *ns,
			const struct mail_storage_settings *mail_set,
			const char **root_path_override,
			const char **inbox_path_override)
{
	struct mail_storage *const *classes;
	const char *root_path, *inbox_path = NULL;
	unsigned int i, count;

	classes = array_get(&mail_storage_classes, &count);
	for (i = 0; i < count; i++) {
		if (classes[i]->v.autodetect != NULL) {
			if (classes[i]->v.autodetect(ns, mail_set,
						     &root_path, &inbox_path)) {
				*root_path_override = root_path;
				*inbox_path_override = inbox_path;
				return classes[i];
			}
		}
	}
	return NULL;
}

static struct mail_storage *
mail_storage_get_class(struct mail_namespace *ns, const char *driver,
		       struct event *set_event,
		       const char **root_path_override,
		       const char **inbox_path_override, const char **error_r)
{
	struct mail_storage *storage_class = NULL;
	const char *home;

	if (driver[0] == '\0') {
		/* empty mail_driver setting, autodetect */
	} else if (strcmp(driver, "auto") == 0) {
		/* explicit autodetection with "auto" driver. */
	} else {
		storage_class = mail_user_get_storage_class(ns->user, driver);
		if (storage_class == NULL) {
			*error_r = t_strdup_printf(
				"Unknown mail storage driver %s", driver);
			return NULL;
		}
	}

	if (storage_class != NULL)
		return storage_class;

	const struct mail_storage_settings *mail_set;
	if (settings_get(set_event, &mail_storage_setting_parser_info, 0,
			 &mail_set, error_r) < 0)
		return NULL;

	storage_class = mail_storage_autodetect(ns, mail_set, root_path_override,
						inbox_path_override);
	if (storage_class != NULL) {
		settings_free(mail_set);
		return storage_class;
	}

	(void)mail_user_get_home(ns->user, &home);
	if (home == NULL || *home == '\0') home = "(not set)";

	*error_r = t_strdup_printf(
		"Mail storage autodetection failed (home=%s, mail_path=%s) - "
		"Set mail_driver explicitly",
		home, mail_set->mail_path);
	settings_free(mail_set);
	return NULL;
}

static int
mail_storage_verify_root(const char *root_dir, const char *dir_type,
			 const char **error_r)
{
	struct stat st;

	if (stat(root_dir, &st) == 0) {
		/* exists */
		if (S_ISDIR(st.st_mode))
			return 0;
		*error_r = t_strdup_printf(
			"Root mail directory is a file: %s", root_dir);
		return -1;
	} else if (ENOACCESS(errno)) {
		*error_r = mail_error_eacces_msg("stat", root_dir);
		return -1;
	} else if (errno != ENOENT) {
		*error_r = t_strdup_printf("stat(%s) failed: %m", root_dir);
		return -1;
	} else {
		*error_r = t_strdup_printf(
			"Root %s directory doesn't exist: %s", dir_type, root_dir);
		return -1;
	}
}

static int
mail_storage_create_root(struct mailbox_list *list,
			 enum mail_storage_flags flags, const char **error_r)
{
	const char *root_dir, *type_name, *error;
	enum mailbox_list_path_type type;

	if (list->mail_set->mailbox_list_iter_from_index_dir) {
		type = MAILBOX_LIST_PATH_TYPE_INDEX;
		type_name = "index";
	} else {
		type = MAILBOX_LIST_PATH_TYPE_MAILBOX;
		type_name = "mail";
	}
	if (!mailbox_list_get_root_path(list, type, &root_dir)) {
		/* storage doesn't use directories (e.g. shared root) */
		return 0;
	}

	if ((flags & MAIL_STORAGE_FLAG_NO_AUTOVERIFY) != 0) {
		if (!event_want_debug_log(list->event))
			return 0;

		/* we don't need to verify, but since debugging is
		   enabled, check and log if the root doesn't exist */
		if (mail_storage_verify_root(root_dir, type_name, &error) < 0) {
			e_debug(list->event,
				"Namespace %s: Creating storage despite: %s",
				list->ns->set->name, error);
		}
		return 0;
	}

	if ((flags & MAIL_STORAGE_FLAG_NO_AUTOCREATE) == 0) {
		/* If the directories don't exist, we'll just autocreate them
		   later. */
		return 0;
	}
	return mail_storage_verify_root(root_dir, type_name, error_r);
}

static bool
mail_storage_match_class(struct mail_storage *storage,
			 const struct mail_storage *storage_class,
			 const struct mail_storage_settings *mail_set)
{
	if (strcmp(storage->name, storage_class->name) != 0)
		return FALSE;

	if ((storage->class_flags & MAIL_STORAGE_CLASS_FLAG_UNIQUE_ROOT) != 0 &&
	    strcmp(storage->unique_root_dir, mail_set->mail_path) != 0)
		return FALSE;

	if (strcmp(storage->name, "shared") == 0) {
		/* allow multiple independent shared namespaces */
		return FALSE;
	}
	return TRUE;
}

static struct mail_storage *
mail_storage_find(struct mail_user *user,
		  const struct mail_storage *storage_class,
		  const struct mail_storage_settings *mail_set)
{
	struct mail_storage *storage = user->storages;

	for (; storage != NULL; storage = storage->next) {
		if (mail_storage_match_class(storage, storage_class, mail_set))
			return storage;
	}
	return NULL;
}

static void
mail_storage_create_ns_instance(struct mail_namespace *ns,
				struct event *set_event)
{
	if (ns->_set_instance != NULL)
		return;

	struct settings_instance *set_instance =
		mail_storage_service_user_get_settings_instance(
			ns->user->service_user);
	ns->_set_instance = settings_instance_dup(set_instance);
	event_set_ptr(set_event, SETTINGS_EVENT_INSTANCE, ns->_set_instance);
}

static int
mail_storage_create_list(struct mail_namespace *ns,
			 struct mail_storage *storage_class,
			 struct event *parent_set_event,
			 enum mail_storage_flags flags,
			 const char *root_path_override,
			 const char *inbox_path_override,
			 const char **error_r)
{
	enum mailbox_list_flags list_flags = 0;
	if (mail_storage_is_mailbox_file(storage_class))
		list_flags |= MAILBOX_LIST_FLAG_MAILBOX_FILES;
	if ((storage_class->class_flags & MAIL_STORAGE_CLASS_FLAG_NO_ROOT) != 0)
		list_flags |= MAILBOX_LIST_FLAG_NO_MAIL_FILES;
	if ((storage_class->class_flags & MAIL_STORAGE_CLASS_FLAG_NO_LIST_DELETES) != 0)
		list_flags |= MAILBOX_LIST_FLAG_NO_DELETES;

	struct mailbox_list *list;
	struct event *set_event = event_create(parent_set_event);
	/* Lookup storage-specific settings, especially to get
	   storage-specific defaults for mailbox list settings. */
	settings_event_add_filter_name(set_event, storage_class->name);
	/* Set namespace, but don't overwrite if it already is set.
	   Shared storage uses the same shared namespace here also for the
	   user's root prefix="" namespace. */
	if (event_find_field_recursive(set_event, SETTINGS_EVENT_NAMESPACE_NAME) == NULL) {
		event_add_str(set_event, SETTINGS_EVENT_NAMESPACE_NAME, ns->set->name);
		settings_event_add_list_filter_name(set_event,
			SETTINGS_EVENT_NAMESPACE_NAME, ns->set->name);
	}

	if ((flags & MAIL_STORAGE_FLAG_SHARED_DYNAMIC) != 0) {
		mail_storage_create_ns_instance(ns, set_event);
		settings_override(ns->_set_instance,
				  "*/mailbox_list_layout", "shared",
				  SETTINGS_OVERRIDE_TYPE_CODE);
	}

	const struct mailbox_list_layout_settings *layout_set;
	if (settings_get(set_event, &mailbox_list_layout_setting_parser_info, 0,
			 &layout_set, error_r) < 0) {
		event_unref(&set_event);
		return -1;
	}

	/* Lookup also layout-specific settings, especially defaults */
	struct event *set_event2 = event_create(set_event);
	event_unref(&set_event);
	set_event = set_event2;
	settings_event_add_filter_name(set_event, t_strdup_printf("layout_%s",
		t_str_lcase(layout_set->mailbox_list_layout)));
	settings_free(layout_set);

	if (root_path_override != NULL) {
		mail_storage_create_ns_instance(ns, set_event);
		settings_override(ns->_set_instance, "*/mail_path",
				  root_path_override,
				  SETTINGS_OVERRIDE_TYPE_CODE);
	}
	if (inbox_path_override != NULL) {
		mail_storage_create_ns_instance(ns, set_event);
		settings_override(ns->_set_instance, "*/mail_inbox_path",
				  inbox_path_override,
				  SETTINGS_OVERRIDE_TYPE_CODE);
	}

	const struct mail_storage_settings *mail_set;
	if (settings_get(set_event, &mail_storage_setting_parser_info, 0,
			 &mail_set, error_r) < 0) {
		event_unref(&set_event);
		return -1;
	}

	if (mail_set->mail_path[0] == '\0') {
		/* no root directory given. is this allowed? */
		if ((flags & MAIL_STORAGE_FLAG_NO_AUTODETECTION) == 0) {
			/* autodetection should take care of this */
		} else if ((storage_class->class_flags & MAIL_STORAGE_CLASS_FLAG_NO_ROOT) != 0) {
			/* root not required for this storage */
		} else {
			*error_r = "Root mail directory not given";
			settings_free(mail_set);
			event_unref(&set_event);
			return -1;
		}
	}

	/* Use parent_set_event instead of set_event mainly to avoid
	   permanently having SETTINGS_EVENT_FILTER_NAME=storage_name in
	   mailbox_list->event. This would be wrong, since mailbox_list can
	   support multiple storages. */
	struct event *event = event_create(parent_set_event);
	event_add_str(event, SETTINGS_EVENT_NAMESPACE_NAME, ns->set->name);
	settings_event_add_list_filter_name(event,
		SETTINGS_EVENT_NAMESPACE_NAME, ns->set->name);
	int ret = mailbox_list_create(event, ns, mail_set, list_flags,
				      &list, error_r);
	if (ret < 0) {
		*error_r = t_strdup_printf("mailbox_list_layout %s: %s",
			mail_set->mailbox_list_layout, *error_r);
	}
	settings_free(mail_set);
	event_unref(&event);
	event_unref(&set_event);
	return ret;
}

static bool ATTR_PURE pop3_uidl_format_has_md5(const char *fmt)
{
	struct var_expand_program *prog;
	const char *error;
	if (var_expand_program_create(fmt, &prog, &error) < 0)
		i_fatal("Invalid pop3_uidl_format: %s", error);
	const char *const *vars = var_expand_program_variables(prog);
	bool has_md5 = str_array_find(vars, "md5");
	var_expand_program_free(&prog);
	return has_md5;
}

static int
mail_storage_create_real(struct mail_namespace *ns, struct event *set_event,
			 enum mail_storage_flags flags,
			 struct mail_storage **storage_r, const char **error_r)
{
	struct mail_storage *storage_class, *storage = NULL;
	const struct mail_driver_settings *driver_set;
	const char *driver = NULL;
	const char *inbox_path_override = NULL;
	const char *root_path_override = NULL;

	/* Lookup initial mailbox list settings. Once they're found, another
	   settings lookup is done with mailbox format as an additional
	   filter. */
	if (settings_get(set_event, &mail_driver_setting_parser_info, 0,
			 &driver_set, error_r) < 0)
		return -1;
	driver = driver_set->mail_driver;

	if ((flags & MAIL_STORAGE_FLAG_SHARED_DYNAMIC) != 0) {
		/* internal shared namespace */
		driver = MAIL_SHARED_STORAGE_NAME;
		root_path_override = ns->user->set->base_dir;
	}

	storage_class = mail_storage_get_class(ns, driver, set_event,
					       &root_path_override,
					       &inbox_path_override, error_r);
	settings_free(driver_set);
	if (storage_class == NULL)
		return -1;

	if (ns->list == NULL) {
		/* first storage for namespace */
		if (mail_storage_create_list(ns, storage_class, set_event,
					     flags, root_path_override,
					     inbox_path_override, error_r) < 0)
			return -1;
		if ((storage_class->class_flags & MAIL_STORAGE_CLASS_FLAG_NO_ROOT) == 0) {
			if (mail_storage_create_root(ns->list, flags, error_r) < 0)
				return -1;
		}
	}

	storage = mail_storage_find(ns->user, storage_class,
				    ns->list->mail_set);
	if (storage != NULL) {
		/* using an existing storage */
		storage->refcount++;
		mail_namespace_add_storage(ns, storage);
		*storage_r = storage;
		return 0;
	}

	if ((flags & MAIL_STORAGE_FLAG_KEEP_HEADER_MD5) == 0 &&
	    ns->list->mail_set->pop3_uidl_format != NULL) {
		/* if pop3_uidl_format contains %m, we want to keep the
		   header MD5 sums stored even if we're not running POP3
		   right now. */
		if (pop3_uidl_format_has_md5(ns->list->mail_set->pop3_uidl_format))
			flags |= MAIL_STORAGE_FLAG_KEEP_HEADER_MD5;
	}

	storage = storage_class->v.alloc();
	storage->refcount = 1;
	storage->storage_class = storage_class;
	storage->user = ns->user;
	storage->set = ns->list->mail_set;
	pool_ref(storage->set->pool);
	storage->flags = flags;
	/* Set to UINT32_MAX manually to denote 'unset', as the default 0 is
	   used for mails currently being saved. */
	storage->last_internal_error_mail_uid = UINT32_MAX;

	storage->event = event_create(ns->user->event);
	if (storage_class->event_category != NULL)
		event_add_category(storage->event, storage_class->event_category);
	event_set_append_log_prefix(
		storage->event, t_strdup_printf("%s: ", storage_class->name));
	p_array_init(&storage->module_contexts, storage->pool, 5);

	if (storage->v.create != NULL &&
	    storage->v.create(storage, ns, error_r) < 0) {
		*error_r = t_strdup_printf("%s: %s", storage->name, *error_r);
		storage->v.destroy(storage);
		settings_free(storage->set);
		event_unref(&storage->event);
		pool_unref(&storage->pool);
		return -1;
	}

	/* If storage supports list index rebuild,
	   provide default mailboxes_fs unless storage
	   wants to use its own. */
	if (storage->v.list_index_rebuild != NULL &&
	    storage->mailboxes_fs == NULL) {
		struct fs_parameters fs_params;
		const char *error;
		i_zero(&fs_params);
		mail_user_init_fs_parameters(storage->user, &fs_params);

		struct settings_instance *set_instance =
			mail_storage_service_user_get_settings_instance(
				storage->user->service_user);
		storage->mailboxes_fs_set_instance =
			settings_instance_dup(set_instance);
		settings_override(storage->mailboxes_fs_set_instance,
				  "*/fs", "__posix",
				  SETTINGS_OVERRIDE_TYPE_CODE);
		settings_override(storage->mailboxes_fs_set_instance,
				  "fs/__posix/fs_driver", "posix",
				  SETTINGS_OVERRIDE_TYPE_CODE);

		struct event *event = event_create(storage->event);
		event_set_ptr(event, SETTINGS_EVENT_INSTANCE,
			      storage->mailboxes_fs_set_instance);
		if (fs_init_auto(event, &fs_params, &storage->mailboxes_fs,
				 &error) <= 0) {
			*error_r = t_strdup_printf("fs_init(posix) failed: %s", error);
			event_unref(&event);
			storage->v.destroy(storage);
			settings_free(storage->set);
			event_unref(&storage->event);
			pool_unref(&storage->pool);
			return -1;
		}
		event_unref(&event);
	}

	T_BEGIN {
		hook_mail_storage_created(storage);
	} T_END;

	i_assert(storage->unique_root_dir != NULL ||
		 (storage->class_flags & MAIL_STORAGE_CLASS_FLAG_UNIQUE_ROOT) == 0);
	DLLIST_PREPEND(&ns->user->storages, storage);
	mail_namespace_add_storage(ns, storage);
	*storage_r = storage;
	return 0;
}

int mail_storage_create(struct mail_namespace *ns, struct event *set_event,
			enum mail_storage_flags flags,
			struct mail_storage **storage_r, const char **error_r)
{
	int ret;
	T_BEGIN {
		ret = mail_storage_create_real(ns, set_event, flags,
					       storage_r, error_r);
	} T_END_PASS_STR_IF(ret < 0, error_r);
	return ret;
}

void mail_storage_unref(struct mail_storage **_storage)
{
	struct mail_storage *storage = *_storage;

	i_assert(storage->refcount > 0);

	/* set *_storage=NULL only after calling destroy() callback.
	   for example mdbox wants to access ns->storage */
	if (--storage->refcount > 0) {
		*_storage = NULL;
		return;
	}

	if (storage->mailboxes != NULL) {
		i_panic("Trying to deinit storage without freeing mailbox %s",
			storage->mailboxes->vname);
	}
	if (storage->obj_refcount != 0)
		i_panic("Trying to deinit storage before freeing its objects");

	DLLIST_REMOVE(&storage->user->storages, storage);

	storage->v.destroy(storage);
	mail_storage_clear_error(storage);
	if (array_is_created(&storage->error_stack)) {
		i_assert(array_count(&storage->error_stack) == 0);
		array_free(&storage->error_stack);
	}
	fs_unref(&storage->mailboxes_fs);
	settings_instance_free(&storage->mailboxes_fs_set_instance);
	settings_free(storage->set);
	event_unref(&storage->event);

	*_storage = NULL;
	pool_unref(&storage->pool);

	mail_index_alloc_cache_destroy_unrefed();
}

void mail_storage_obj_ref(struct mail_storage *storage)
{
	i_assert(storage->refcount > 0);

	if (storage->obj_refcount++ == 0)
		mail_user_ref(storage->user);
}

void mail_storage_obj_unref(struct mail_storage *storage)
{
	i_assert(storage->refcount > 0);
	i_assert(storage->obj_refcount > 0);

	if (--storage->obj_refcount == 0) {
		struct mail_user *user = storage->user;
		mail_user_unref(&user);
	}
}

void mail_storage_clear_error(struct mail_storage *storage)
{
	i_free_and_null(storage->error_string);

	i_free(storage->last_internal_error);
	i_free(storage->last_internal_error_mailbox);
	storage->last_error_is_internal = FALSE;
	storage->error = MAIL_ERROR_NONE;
	storage->last_internal_error_mail_uid = UINT32_MAX;
}

void mail_storage_set_error(struct mail_storage *storage,
			    enum mail_error error, const char *string)
{
	if (storage->error_string != string) {
		i_free(storage->error_string);
		storage->error_string = i_strdup(string);
	}
	storage->last_error_is_internal = FALSE;
	storage->error = error;
	storage->last_internal_error_mail_uid = UINT32_MAX;
}

void mail_storage_set_internal_error(struct mail_storage *storage)
{
	const char *str;

	str = t_strflocaltime(MAIL_ERRSTR_CRITICAL_MSG_STAMP, ioloop_time);

	i_free(storage->error_string);
	storage->error_string = i_strdup(str);
	storage->error = MAIL_ERROR_TEMP;

	/* this function doesn't set last_internal_error, so
	   last_error_is_internal can't be TRUE. */
	storage->last_error_is_internal = FALSE;
	i_free(storage->last_internal_error);
	i_free(storage->last_internal_error_mailbox);
	storage->last_internal_error_mail_uid = UINT32_MAX;
}

static void
mail_storage_set_critical_error(struct mail_storage *storage, const char *str,
				const char *mailbox_vname, uint32_t mail_uid)
{
	char *old_error = storage->error_string;
	char *old_internal_error = storage->last_internal_error;
	char *old_internal_error_mailbox = storage->last_internal_error_mailbox;

	storage->error_string = NULL;
	storage->last_internal_error = NULL;
	storage->last_internal_error_mailbox = NULL;
	/* critical errors may contain sensitive data, so let user
	   see only "Internal error" with a timestamp to make it
	   easier to look from log files the actual error message. */
	mail_storage_set_internal_error(storage);

	storage->last_internal_error = i_strdup(str);
	storage->last_internal_error_mailbox = i_strdup(mailbox_vname);
	storage->last_internal_error_mail_uid = mail_uid;
	storage->last_error_is_internal = TRUE;

	/* free the old_error and old_internal_error only after the new error
	   is generated, because they may be one of the parameters. */
	i_free(old_error);
	i_free(old_internal_error);
	i_free(old_internal_error_mailbox);
}

void mail_storage_set_critical(struct mail_storage *storage,
			       const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	T_BEGIN {
		const char *str = t_strdup_vprintf(fmt, va);
		mail_storage_set_critical_error(storage, str, NULL, UINT32_MAX);
		e_error(storage->event, "%s", str);
	} T_END;
	va_end(va);
}

void mailbox_set_critical(struct mailbox *box, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	T_BEGIN {
		const char *str = t_strdup_vprintf(fmt, va);
		mail_storage_set_critical_error(box->storage, str, box->vname,
						UINT32_MAX);
		e_error(box->event, "%s", str);
	} T_END;
	va_end(va);
}

void mail_set_critical(struct mail *mail, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	T_BEGIN {
		const char *formatted_msg = t_strdup_vprintf(fmt, va);
		mail_storage_set_critical_error(mail->box->storage, formatted_msg,
						mail->box->vname, mail->uid);
		e_error(mail_event(mail), "%s", formatted_msg);
	} T_END;
	va_end(va);
}

/* Note: mail_storage_get_last_internal_error() will always include
	 the mailbox prefix, while mailbox_get_last_internal_error() and
	 mail_get_last_internal_error() usually will not. */
const char *mail_storage_get_last_internal_error(struct mail_storage *storage,
						 enum mail_error *error_r)
{
	if (error_r != NULL)
		*error_r = storage->error;
	if (storage->last_error_is_internal) {
		i_assert(storage->last_internal_error != NULL);

		bool is_mailbox_error_set = storage->last_internal_error_mailbox != NULL;
		bool is_mail_error_set =
			storage->last_internal_error_mail_uid != UINT32_MAX;

		if (is_mail_error_set) {
			i_assert(is_mailbox_error_set);
			return t_strdup_printf(
				"Mailbox %s: UID %u: %s",
				mailbox_name_sanitize(storage->last_internal_error_mailbox),
				storage->last_internal_error_mail_uid,
				storage->last_internal_error);
		}
		if (is_mailbox_error_set)
			return t_strdup_printf(
				"Mailbox %s: %s",
				mailbox_name_sanitize(storage->last_internal_error_mailbox),
				storage->last_internal_error);

		return storage->last_internal_error;
	}
	return mail_storage_get_last_error(storage, error_r);
}

/* Note: mailbox_get_last_internal_error() will include the mailbox prefix only
	 when mailbox->vname does not match last_internal_error_mailbox, which
	 might happen with e.g. virtual mailboxes logging about physical
	 mailboxes, while mail_storage_get_last_internal_error() always does. */
const char *mailbox_get_last_internal_error(struct mailbox *box,
					    enum mail_error *error_r)
{
	struct mail_storage *storage = mailbox_get_storage(box);
	const char *last_mailbox = storage->last_internal_error_mailbox;
	if (last_mailbox != NULL &&
	    strcmp(last_mailbox, box->vname) != 0)
		return mail_storage_get_last_internal_error(storage, error_r);

	if (error_r != NULL)
		*error_r = storage->error;
	if (storage->last_error_is_internal) {
		i_assert(storage->last_internal_error != NULL);
		if (storage->last_internal_error_mail_uid != UINT32_MAX)
			return t_strdup_printf("UID %u: %s",
					       storage->last_internal_error_mail_uid,
					       storage->last_internal_error);
		return storage->last_internal_error;
	}
	return mail_storage_get_last_error(storage, error_r);
}

/* Note: mail_get_last_internal_error() will include the mail prefix only when
	 mail->uid does not match last_internal_error_mail_uid, while
	 mail_storage_get_last_internal_error() always does. */
const char *
mail_get_last_internal_error(struct mail *mail, enum mail_error *error_r)
{
	struct mail_storage *storage = mailbox_get_storage(mail->box);
	const char *last_mailbox = storage->last_internal_error_mailbox;
	if (last_mailbox != NULL &&
	    strcmp(last_mailbox, mail->box->vname) != 0)
		return mail_storage_get_last_internal_error(storage, error_r);

	uint32_t last_mail_uid = storage->last_internal_error_mail_uid;
	if (last_mail_uid == UINT32_MAX || last_mail_uid != mail->uid)
		return mailbox_get_last_internal_error(mail->box, error_r);

	if (error_r != NULL)
		*error_r = storage->error;
	if (storage->last_error_is_internal) {
		i_assert(storage->last_internal_error != NULL);
		return storage->last_internal_error;
	}
	return mail_storage_get_last_error(storage, error_r);
}

void mail_storage_copy_error(struct mail_storage *dest,
			     struct mail_storage *src)
{
	const char *str;
	enum mail_error error;

	if (src == dest)
		return;

	str = mail_storage_get_last_error(src, &error);
	mail_storage_set_error(dest, error, str);
}

void mail_storage_copy_list_error(struct mail_storage *storage,
				  struct mailbox_list *list)
{
	const char *str;
	enum mail_error error;

	str = mailbox_list_get_last_error(list, &error);
	mail_storage_set_error(storage, error, str);
}

void mailbox_set_index_error(struct mailbox *box)
{
	if (mail_index_is_deleted(box->index)) {
		mailbox_set_deleted(box);
		mail_index_reset_error(box->index);
	} else {
		i_free(box->storage->last_internal_error_mailbox);
		box->storage->last_internal_error_mailbox = i_strdup(box->vname);
		mail_storage_set_index_error(box->storage, box->index);
	}
}

void mail_storage_set_index_error(struct mail_storage *storage,
				  struct mail_index *index)
{
	const char *index_error;

	mail_storage_set_internal_error(storage);
	/* use the lib-index's error as our internal error string */
	index_error = mail_index_get_last_error(index, NULL);
	if (index_error == NULL)
		index_error = "BUG: Unknown internal index error";
	storage->last_internal_error = i_strdup(index_error);
	storage->last_error_is_internal = TRUE;
	mail_index_reset_error(index);
}

const struct mail_storage_settings *
mail_storage_get_settings(struct mail_storage *storage)
{
	return storage->set;
}

struct mail_user *mail_storage_get_user(struct mail_storage *storage)
{
	return storage->user;
}

void mail_storage_set_callbacks(struct mail_storage *storage,
				struct mail_storage_callbacks *callbacks,
				void *context)
{
	storage->callbacks = *callbacks;
	storage->callback_context = context;
}

int mail_storage_purge(struct mail_storage *storage)
{
	if (storage->v.purge == NULL)
		return 0;

	int ret;
	T_BEGIN {
		ret = storage->v.purge(storage);
	} T_END;
	return ret;
}

const char *mail_storage_get_last_error(struct mail_storage *storage,
					enum mail_error *error_r)
{
	/* We get here only in error situations, so we have to return some
	   error. If storage->error is NONE, it means we forgot to set it at
	   some point.. */
	if (storage->error == MAIL_ERROR_NONE) {
		if (error_r != NULL)
			*error_r = MAIL_ERROR_TEMP;
		return storage->error_string != NULL ? storage->error_string :
			"BUG: Unknown internal error";
	}

	if (storage->error_string == NULL) {
		/* This shouldn't happen.. */
		storage->error_string =
			i_strdup_printf("BUG: Unknown 0x%x error",
					storage->error);
	}

	if (error_r != NULL)
		*error_r = storage->error;
	return storage->error_string;
}

const char *mailbox_get_last_error(struct mailbox *box,
				   enum mail_error *error_r)
{
	return mail_storage_get_last_error(box->storage, error_r);
}

enum mail_error mailbox_get_last_mail_error(struct mailbox *box)
{
	enum mail_error error;

	mail_storage_get_last_error(box->storage, &error);
	return error;
}

void mail_storage_last_error_push(struct mail_storage *storage)
{
	struct mail_storage_error *err;

	if (!array_is_created(&storage->error_stack))
		i_array_init(&storage->error_stack, 2);
	err = array_append_space(&storage->error_stack);
	err->error_string = i_strdup(storage->error_string);
	err->error = storage->error;
	err->last_error_is_internal = storage->last_error_is_internal;
	/* Initially set to UINT32_MAX manually to denote 'unset', as the
	   default 0 is used for mails currently being saved. If there is no
	   internal error, the attribute would not be set otherwise. */
	err->last_internal_error_mail_uid = UINT32_MAX;
	if (err->last_error_is_internal) {
		err->last_internal_error = i_strdup(storage->last_internal_error);
		err->last_internal_error_mailbox =
			i_strdup(storage->last_internal_error_mailbox);
		err->last_internal_error_mail_uid =
			storage->last_internal_error_mail_uid;
	}
}

void mail_storage_last_error_pop(struct mail_storage *storage)
{
	unsigned int count = array_count(&storage->error_stack);
	const struct mail_storage_error *err =
		array_idx(&storage->error_stack, count-1);

	i_free(storage->error_string);
	i_free(storage->last_internal_error);
	i_free(storage->last_internal_error_mailbox);
	storage->error_string = err->error_string;
	storage->error = err->error;
	storage->last_error_is_internal = err->last_error_is_internal;
	storage->last_internal_error = err->last_internal_error;
	storage->last_internal_error_mailbox = err->last_internal_error_mailbox;
	storage->last_internal_error_mail_uid = err->last_internal_error_mail_uid;
	array_delete(&storage->error_stack, count-1, 1);
}

bool mail_storage_is_mailbox_file(struct mail_storage *storage)
{
	return (storage->class_flags &
		MAIL_STORAGE_CLASS_FLAG_MAILBOX_IS_FILE) != 0;
}

bool mail_storage_set_error_from_errno(struct mail_storage *storage)
{
	const char *error_string;
	enum mail_error error;

	if (!mail_error_from_errno(&error, &error_string))
		return FALSE;
	if (event_want_debug_log(storage->event) && error != MAIL_ERROR_NOTFOUND) {
		/* debugging is enabled - admin may be debugging a
		   (permission) problem, so return FALSE to get the caller to
		   log the full error message. */
		return FALSE;
	}

	mail_storage_set_error(storage, error, error_string);
	return TRUE;
}

static int
mailbox_list_get_default_box_settings(struct mailbox_list *list,
				      const struct mailbox_settings **set_r,
				      const char **error_r)
{
	if (list->default_box_set == NULL) {
		if (settings_get(list->event,
				 &mailbox_setting_parser_info, 0,
				 &list->default_box_set, error_r) < 0)
			return -1;
	}
	pool_ref(list->default_box_set->pool);
	*set_r = list->default_box_set;
	return 1;
}

int mailbox_name_try_get_settings(struct mailbox_list *list, const char *vname,
				  const struct mailbox_settings **set_r,
				  const char **error_r)
{
	if (array_is_empty(&list->ns->set->mailboxes))
		return mailbox_list_get_default_box_settings(list, set_r, error_r);

	const char *vname_without_prefix =
		mailbox_get_name_without_prefix(list->ns, vname);
	unsigned int i, count;
	const struct mailbox_settings *set = NULL, *const *mailboxes =
		array_get(&list->ns->set->parsed_mailboxes, &count);

	for (i = 0; i < count; i++) {
		if (!wildcard_match(vname_without_prefix, mailboxes[i]->name))
			continue;

		if (set == NULL)
			set = mailboxes[i];
		else {
			/* multiple mailbox named list filters match - need to
			   lookup settings to get them merged. */
			return 0;
		}
	}
	if (set == NULL)
		return mailbox_list_get_default_box_settings(list, set_r, error_r);

	pool_ref(set->pool);
	*set_r = set;
	return 1;
}

struct mailbox *mailbox_alloc(struct mailbox_list *list, const char *vname,
			      enum mailbox_flags flags)
{
	struct mailbox_list *new_list = list;
	struct mail_storage *storage;
	struct mailbox *box;
	enum mail_error open_error = 0;
	const char *suffix, *errstr = NULL;

	i_assert(uni_utf8_str_is_valid(vname));

	if (str_begins_icase(vname, "INBOX", &suffix) &&
	    !str_begins_with(vname, "INBOX")) {
		/* make sure INBOX shows up in uppercase everywhere. do this
		   regardless of whether we're in inbox=yes namespace, because
		   clients expect INBOX to be case-insensitive regardless of
		   server's internal configuration. */
		if (suffix[0] == '\0')
			vname = "INBOX";
		else if (suffix[0] != mail_namespace_get_sep(list->ns))
			/* not INBOX prefix */ ;
		else if (strncasecmp(list->ns->prefix, vname, 6) == 0 &&
			 !str_begins_with(list->ns->prefix, "INBOX")) {
			mailbox_list_set_critical(list,
				"Invalid server configuration: "
				"Namespace %s: prefix=%s must be uppercase INBOX",
				list->ns->set->name, list->ns->prefix);
			open_error = MAIL_ERROR_TEMP;
		} else {
			vname = t_strconcat("INBOX", suffix, NULL);
		}
	}

	T_BEGIN {
		const char *error, *orig_vname = vname;
		enum mailbox_list_get_storage_flags storage_flags = 0;
		int ret;

		if ((flags & MAILBOX_FLAG_SAVEONLY) != 0)
			storage_flags |= MAILBOX_LIST_GET_STORAGE_FLAG_SAVEONLY;
		if (mailbox_list_get_storage(&new_list, &vname,
					     storage_flags, &storage) < 0) {
			/* do a delayed failure at mailbox_open() */
			storage = mail_namespace_get_default_storage(list->ns);
			errstr = mailbox_list_get_last_error(new_list, &open_error);
			errstr = t_strdup(errstr);
		}

		box = storage->v.mailbox_alloc(storage, new_list, vname, flags);
		if (open_error != 0) {
			box->open_error = open_error;
			mail_storage_set_error(storage, open_error, errstr);
		} else if ((ret = mailbox_name_try_get_settings(box->list,
					vname, &box->set, &error)) < 0) {
			mailbox_set_critical(box, "%s", error);
			box->open_error = box->storage->error;
		} else if (ret == 0) {
			if (settings_get(box->event,
					 &mailbox_setting_parser_info, 0,
					 &box->set, &error) < 0) {
				mailbox_set_critical(box, "%s", error);
				box->open_error = box->storage->error;
			}
		}
		if (strcmp(orig_vname, vname) != 0)
			box->mailbox_not_original = TRUE;
		hook_mailbox_allocated(box);
	} T_END;

	DLLIST_PREPEND(&box->storage->mailboxes, box);
	mail_storage_obj_ref(box->storage);
	return box;
}

struct mailbox *mailbox_alloc_guid(struct mailbox_list *list,
				   const guid_128_t guid,
				   enum mailbox_flags flags)
{
	struct mailbox *box = NULL;
	struct mailbox_metadata metadata;
	enum mail_error open_error = MAIL_ERROR_TEMP;
	const char *vname;

	if (mailbox_guid_cache_find(list, guid, &vname) < 0) {
		vname = NULL;
	} else if (vname != NULL) {
		box = mailbox_alloc(list, vname, flags);
		if (mailbox_get_metadata(box, MAILBOX_METADATA_GUID,
					 &metadata) < 0) {
		} else if (memcmp(metadata.guid, guid,
				  sizeof(metadata.guid)) != 0) {
			/* GUID mismatch, refresh cache and try again */
			mailbox_free(&box);
			mailbox_guid_cache_refresh(list);
			return mailbox_alloc_guid(list, guid, flags);
		} else {
			/* successfully opened the correct mailbox */
			return box;
		}
		e_error(list->event, "mailbox_alloc_guid(%s): "
			"Couldn't verify mailbox GUID: %s",
			guid_128_to_string(guid),
			mailbox_get_last_internal_error(box, NULL));
		vname = NULL;
		mailbox_free(&box);
	} else {
		vname = t_strdup_printf("(nonexistent mailbox with GUID=%s)",
					guid_128_to_string(guid));
		open_error = MAIL_ERROR_NOTFOUND;
	}

	if (vname == NULL) {
		vname = t_strdup_printf("(error in mailbox with GUID=%s)",
					guid_128_to_string(guid));
	}
	box = mailbox_alloc(list, vname, flags);
	box->open_error = open_error;
	return box;
}

static bool
str_contains_special_use(const char *str, const char *special_use)
{
	const char *const *uses;

	i_assert(special_use != NULL);
	if (*special_use != '\\')
		return FALSE;

	bool ret;
	T_BEGIN {
		uses = t_strsplit_spaces(str, " ");
		ret = str_array_icase_find(uses, special_use);
	} T_END;
	return ret;
}

static int
namespace_find_special_use(struct mail_namespace *ns, const char *special_use,
			   const char **vname_r, enum mail_error *error_code_r)
{
	struct mailbox_list *list = ns->list;
	struct mailbox_list_iterate_context *ctx;
	const struct mailbox_info *info;
	int ret = 0;

	*vname_r = NULL;
	*error_code_r = MAIL_ERROR_NONE;

	if (!ns->set->parsed_have_special_use_mailboxes)
		return 0;
	if (!HAS_ALL_BITS(ns->type, MAIL_NAMESPACE_TYPE_PRIVATE))
		return 0;

	ctx = mailbox_list_iter_init(list, "*",
		MAILBOX_LIST_ITER_SELECT_SPECIALUSE |
		MAILBOX_LIST_ITER_RETURN_SPECIALUSE);
	while ((info = mailbox_list_iter_next(ctx)) != NULL) {
		if ((info->flags &
		     (MAILBOX_NOSELECT | MAILBOX_NONEXISTENT)) != 0)
			continue;
		/* iter can only return mailboxes that have non-empty
		   special-use */
		i_assert(info->special_use != NULL &&
			 *info->special_use != '\0');

		if (str_contains_special_use(info->special_use, special_use)) {
			*vname_r = t_strdup(info->vname);
			ret = 1;
			break;
		}
	}
	if (mailbox_list_iter_deinit(&ctx) < 0) {
		const char *error;

		error = mailbox_list_get_last_error(ns->list, error_code_r);
		e_error(ns->list->event,
			"Namespace %s: Failed to find mailbox with SPECIAL-USE flag '%s': %s",
			ns->set->name, special_use, error);
		return -1;
	}
	return ret;
}

static int
namespaces_find_special_use(struct mail_namespace *namespaces,
			    const char *special_use,
			    struct mail_namespace **ns_r,
			    const char **vname_r, enum mail_error *error_code_r)
{
	struct mail_namespace *ns_inbox;
	int ret;

	*error_code_r = MAIL_ERROR_NONE;
	*vname_r = NULL;

	/* check user's INBOX namespace first */
	*ns_r = ns_inbox = mail_namespace_find_inbox(namespaces);
	ret = namespace_find_special_use(*ns_r, special_use,
					 vname_r, error_code_r);
	if (ret != 0)
		return ret;

	/* check other namespaces */
	for (*ns_r = namespaces; *ns_r != NULL; *ns_r = (*ns_r)->next) {
		if (*ns_r == ns_inbox) {
			/* already checked */
			continue;
		}
		ret = namespace_find_special_use(*ns_r, special_use,
						 vname_r, error_code_r);
		if (ret != 0)
			return ret;
	}

	*ns_r = ns_inbox;
	return 0;
}

struct mailbox *
mailbox_alloc_for_user(struct mail_user *user, const char *mname,
		       enum mailbox_flags flags)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	const char *vname;
	enum mail_error open_error = MAIL_ERROR_NONE;
	int ret;

	if (HAS_ALL_BITS(flags, MAILBOX_FLAG_SPECIAL_USE)) {
		ret = namespaces_find_special_use(user->namespaces, mname,
						  &ns, &vname, &open_error);
		if (ret < 0) {
			i_assert(open_error != MAIL_ERROR_NONE);
			vname = t_strdup_printf(
				"(error finding mailbox with SPECIAL-USE=%s)",
				mname);
		} else if (ret == 0) {
			i_assert(open_error == MAIL_ERROR_NONE);
			vname = t_strdup_printf(
				"(nonexistent mailbox with SPECIAL-USE=%s)",
				mname);
			open_error = MAIL_ERROR_NOTFOUND;
		}
	} else {
		vname = mname;
		ns = mail_namespace_find(user->namespaces, mname);
	}

	if (HAS_ALL_BITS(flags, MAILBOX_FLAG_POST_SESSION)) {
		flags |= MAILBOX_FLAG_SAVEONLY;

		if (strcmp(vname, ns->prefix) == 0 &&
		    (ns->flags & NAMESPACE_FLAG_INBOX_USER) != 0) {
			/* delivering to a namespace prefix means we actually
			   want to deliver to the INBOX instead */
			vname = "INBOX";
			ns = mail_namespace_find_inbox(user->namespaces);
		}

		if (strcasecmp(vname, "INBOX") == 0) {
			/* deliveries to INBOX must always succeed,
			   regardless of ACLs */
			flags |= MAILBOX_FLAG_IGNORE_ACLS;
		}
	}

	i_assert(ns != NULL);
	box = mailbox_alloc(ns->list, vname, flags);
	if (open_error != MAIL_ERROR_NONE)
		box->open_error = open_error;
	return box;
}

bool mailbox_is_autocreated(struct mailbox *box)
{
	if (box->inbox_user)
		return TRUE;
	if ((box->flags & MAILBOX_FLAG_AUTO_CREATE) != 0)
		return TRUE;
	return box->set != NULL &&
		strcmp(box->set->autocreate, MAILBOX_SET_AUTO_NO) != 0;
}

bool mailbox_is_autosubscribed(struct mailbox *box)
{
	if ((box->flags & MAILBOX_FLAG_AUTO_SUBSCRIBE) != 0)
		return TRUE;
	return box->set != NULL &&
		strcmp(box->set->autocreate, MAILBOX_SET_AUTO_SUBSCRIBE) == 0;
}

static int mailbox_autocreate(struct mailbox *box)
{
	const char *errstr;
	enum mail_error error;

	if (mailbox_create(box, NULL, FALSE) < 0) {
		errstr = mailbox_get_last_internal_error(box, &error);
		if (error == MAIL_ERROR_NOTFOUND && box->acl_no_lookup_right) {
			/* ACL prevents creating this mailbox */
			return -1;
		}
		if (error != MAIL_ERROR_EXISTS) {
			mailbox_set_critical(box,
				"Failed to autocreate mailbox: %s",
				errstr);
			return -1;
		}
	} else if (mailbox_is_autosubscribed(box)) {
		if (mailbox_set_subscribed(box, TRUE) < 0) {
			mailbox_set_critical(box,
				"Failed to autosubscribe to mailbox: %s",
				mailbox_get_last_internal_error(box, NULL));
			return -1;
		}
	}
	return 0;
}

static int mailbox_autocreate_and_reopen(struct mailbox *box)
{
	int ret;

	if (mailbox_autocreate(box) < 0)
		return -1;
	mailbox_close(box);

	ret = box->v.open(box);
	if (ret < 0 && box->inbox_user && !box->acl_no_lookup_right &&
	    !box->storage->user->inbox_open_error_logged) {
		box->storage->user->inbox_open_error_logged = TRUE;
		mailbox_set_critical(box,
			"Opening INBOX failed: %s",
			mailbox_get_last_internal_error(box, NULL));
	}
	return ret;
}

static bool
mailbox_name_verify_extra_separators(const char *vname, char sep,
				     const char **error_r)
{
	unsigned int i;
	bool prev_sep = FALSE;

	/* Make sure the vname doesn't have extra separators:

	   1) Must not have adjacent separators. If we allow these, these could
	   end up pointing to existing mailboxes due to kernel ignoring
	   duplicate '/' in paths. However, this might cause us to handle some
	   of our own checks wrong, such as skipping ACLs.

	   2) Must not end with separator. Similar reasoning as above.
	*/
	for (i = 0; vname[i] != '\0'; i++) {
		if (vname[i] == sep) {
			if (prev_sep) {
				*error_r = "Has adjacent hierarchy separators";
				return FALSE;
			}
			prev_sep = TRUE;
		} else {
			prev_sep = FALSE;
		}
	}
	if (prev_sep && i > 0) {
		*error_r = "Ends with hierarchy separator";
		return FALSE;
	}
	return TRUE;
}

static bool
mailbox_verify_name_prefix(struct mail_namespace *ns, const char **vnamep,
			   const char **error_r)
{
	const char *vname = *vnamep;

	if (ns->prefix_len == 0)
		return TRUE;

	/* vname is either "namespace/box" or "namespace" */
	if (strncmp(vname, ns->prefix, ns->prefix_len-1) != 0 ||
	    (vname[ns->prefix_len-1] != '\0' &&
	     vname[ns->prefix_len-1] != ns->prefix[ns->prefix_len-1])) {
		/* User input shouldn't normally be able to get us in
		   here. The main reason this isn't an assert is to
		   allow any input at all to mailbox_verify_*_name()
		   without crashing. */
		*error_r = t_strdup_printf("Missing namespace prefix '%s'",
					   ns->prefix);
		return FALSE;
	}
	vname += ns->prefix_len - 1;
	if (vname[0] != '\0') {
		i_assert(vname[0] == ns->prefix[ns->prefix_len-1]);
		vname++;

		if (vname[0] == '\0') {
			/* "namespace/" isn't a valid mailbox name. */
			*error_r = "Ends with hierarchy separator";
			return FALSE;
		}
	}
	*vnamep = vname;
	return TRUE;
}

static int mailbox_verify_name_int(struct mailbox *box)
{
	struct mail_namespace *ns = box->list->ns;
	const char *error, *vname = box->vname;
	char list_sep, ns_sep;

	if (box->inbox_user) {
		/* this is INBOX - don't bother with further checks */
		return 0;
	}

	/* Verify the namespace prefix here. Change vname to skip the prefix
	   for the following checks. */
	if (!mailbox_verify_name_prefix(box->list->ns, &vname, &error)) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
			t_strdup_printf("Invalid mailbox name '%s': %s",
					mailbox_name_sanitize(vname), error));
		return -1;
	}

	list_sep = mailbox_list_get_hierarchy_sep(box->list);
	ns_sep = mail_namespace_get_sep(ns);

	/* If namespace { separator } differs from the mailbox_list separator,
	   the list separator can't actually be used in the mailbox name
	   unless it's escaped with storage_name_escape_char. For example if
	   namespace separator is '/' and mailbox_list_layout=Maildir++ has '.'
	   as the separator, there's no way to use '.' in the mailbox name
	   (without escaping) because it would end up becoming a hierarchy
	   separator. */
	if (ns_sep != list_sep &&
	    box->list->mail_set->mailbox_list_storage_escape_char[0] == '\0' &&
	    strchr(vname, list_sep) != NULL) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS, t_strdup_printf(
			"Character not allowed in mailbox name: '%c'", list_sep));
		return -1;
	}
	/* vname must not begin with the hierarchy separator normally.
	   For example we don't want to allow accessing /etc/passwd. However,
	   if mail_full_filesystem_access=yes, we do actually want to allow
	   that. */
	if (vname[0] == ns_sep &&
	    !box->storage->set->mail_full_filesystem_access) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
			"Invalid mailbox name: Begins with hierarchy separator");
		return -1;
	}

	if (!mailbox_name_verify_extra_separators(vname, ns_sep, &error) ||
	    !mailbox_list_is_valid_name(box->list, box->name, &error)) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
			t_strdup_printf("Invalid mailbox name: %s", error));
		return -1;
	}
	return 0;
}

int mailbox_verify_name(struct mailbox *box)
{
	int ret;
	T_BEGIN {
		ret = mailbox_verify_name_int(box);
	} T_END;
	return ret;
}

static int mailbox_verify_existing_name_int(struct mailbox *box)
{
	const char *path;

	if (box->opened)
		return 0;

	if (mailbox_verify_name(box) < 0)
		return -1;

	/* Make sure box->_path is set, so mailbox_get_path() works from
	   now on. Note that this may also fail with some backends if the
	   mailbox doesn't exist. */
	if (mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_MAILBOX, &path) < 0) {
		if (box->storage->error != MAIL_ERROR_NOTFOUND ||
		    !mailbox_is_autocreated(box))
			return -1;
		/* if this is an autocreated mailbox, create it now */
		if (mailbox_autocreate(box) < 0)
			return -1;
		mailbox_close(box);
		if (mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_MAILBOX,
					&path) < 0)
			return -1;
	}
	return 0;
}

static int mailbox_verify_existing_name(struct mailbox *box)
{
	int ret;
	T_BEGIN {
		ret = mailbox_verify_existing_name_int(box);
	} T_END;
	return ret;
}

static bool mailbox_name_has_control_chars(const char *name)
{
	const char *p;

	for (p = name; *p != '\0'; p++) {
		if ((unsigned char)*p < ' ')
			return TRUE;
	}
	return FALSE;
}

void mailbox_skip_create_name_restrictions(struct mailbox *box, bool set)
{
	box->skip_create_name_restrictions = set;
}

int mailbox_verify_create_name(struct mailbox *box)
{
	/* mailbox_alloc() already checks that vname is valid UTF8,
	   so we don't need to verify that.

	   check vname instead of storage name, because vname is what is
	   visible to users, while storage name may be a fixed length GUID. */
	if (mailbox_verify_name(box) < 0)
		return -1;
	if (box->skip_create_name_restrictions)
		return 0;
	if (mailbox_name_has_control_chars(box->vname)) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
			"Control characters not allowed in new mailbox names");
		return -1;
	}
	if (strlen(box->vname) > MAILBOX_LIST_NAME_MAX_LENGTH) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       "Mailbox name too long");
		return -1;
	}
	/* check individual component names, too */
	const char *old_name = box->name;
	const char *name;
	const char sep = mailbox_list_get_hierarchy_sep(box->list);
	while((name = strchr(old_name, sep)) != NULL) {
		if (name - old_name > MAILBOX_MAX_HIERARCHY_NAME_LENGTH) {
			mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				"Mailbox name too long");
			return -1;
		}
		name++;
		old_name = name;
	}
	if (strlen(old_name) > MAILBOX_MAX_HIERARCHY_NAME_LENGTH) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       "Mailbox name too long");
		return -1;
	}
	return 0;
}

static bool have_listable_namespace_prefix(struct mail_namespace *ns,
					   const char *name)
{
	size_t name_len = strlen(name);

	for (; ns != NULL; ns = ns->next) {
		if ((ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) == 0)
			continue;

		if (ns->prefix_len <= name_len)
			continue;

		/* if prefix has multiple hierarchies, match
		   any of the hierarchies */
		if (strncmp(ns->prefix, name, name_len) == 0 &&
		    ns->prefix[name_len] == mail_namespace_get_sep(ns))
			return TRUE;
	}
	return FALSE;
}

int mailbox_exists(struct mailbox *box, bool auto_boxes,
		   enum mailbox_existence *existence_r)
{
	switch (box->open_error) {
	case 0:
		break;
	case MAIL_ERROR_NOTFOUND:
		*existence_r = MAILBOX_EXISTENCE_NONE;
		return 0;
	default:
		/* unsure if this exists or not */
		return -1;
	}
	if (mailbox_verify_name(box) < 0) {
		/* the mailbox name is invalid. we don't know if it currently
		   exists or not, but since it can never be accessed in any way
		   report it as if it didn't exist. */
		*existence_r = MAILBOX_EXISTENCE_NONE;
		return 0;
	}

	int ret;
	T_BEGIN {
		ret = box->v.exists(box, auto_boxes, existence_r);
	} T_END;
	if (ret < 0)
		return -1;

	if (!box->inbox_user && *existence_r == MAILBOX_EXISTENCE_NOSELECT &&
	    have_listable_namespace_prefix(box->storage->user->namespaces,
					   box->vname)) {
	       /* listable namespace prefix always exists. */
		*existence_r = MAILBOX_EXISTENCE_NOSELECT;
		return 0;
	}

	/* if this is a shared namespace with only INBOX and
	   mail_shared_explicit_inbox=no, we'll need to mark the namespace as
	   usable here since nothing else will. */
	box->list->ns->flags |= NAMESPACE_FLAG_USABLE;
	return 0;
}

static int ATTR_NULL(2)
mailbox_open_full(struct mailbox *box, struct istream *input)
{
	int ret;

	if (box->opened)
		return 0;

	switch (box->open_error) {
	case 0:
		e_debug(box->event, "Mailbox opened");
		break;
	case MAIL_ERROR_NOTFOUND:
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(box->vname));
		return -1;
	default:
		mail_storage_set_internal_error(box->storage);
		box->storage->error = box->open_error;
		return -1;
	}

	if (mailbox_verify_existing_name(box) < 0)
		return -1;

	if (input != NULL) {
		if ((box->storage->class_flags &
		     MAIL_STORAGE_CLASS_FLAG_OPEN_STREAMS) == 0) {
			mailbox_set_critical(box,
				"Storage doesn't support streamed mailboxes");
			return -1;
		}
		box->input = input;
		box->flags |= MAILBOX_FLAG_READONLY;
		i_stream_ref(box->input);
	}

	ret = box->v.open(box);
	if (ret < 0 && box->storage->error == MAIL_ERROR_NOTFOUND &&
	    !box->deleting && !box->creating &&
	    box->input == NULL && mailbox_is_autocreated(box)) T_BEGIN {
		ret = mailbox_autocreate_and_reopen(box);
	} T_END;

	if (ret < 0) {
		if (box->input != NULL)
			i_stream_unref(&box->input);
		return -1;
	}

	box->list->ns->flags |= NAMESPACE_FLAG_USABLE;
	return 0;
}

static bool mailbox_try_undelete(struct mailbox *box)
{
	time_t mtime;

	i_assert(!box->mailbox_undeleting);

	if ((box->flags & MAILBOX_FLAG_READONLY) != 0) {
		/* most importantly we don't do this because we want to avoid
		   a loop: mdbox storage rebuild -> mailbox_open() ->
		   mailbox_mark_index_deleted() -> mailbox_sync() ->
		   mdbox storage rebuild. */
		return FALSE;
	}
	if (mail_index_get_modification_time(box->index, &mtime) < 0)
		return FALSE;
	if (mtime + MAILBOX_DELETE_RETRY_SECS > time(NULL))
		return FALSE;

	box->mailbox_undeleting = TRUE;
	int ret = mailbox_mark_index_deleted(box, FALSE);
	box->mailbox_undeleting = FALSE;
	if (ret < 0)
		return FALSE;
	box->mailbox_deleted = FALSE;
	return TRUE;
}

int mailbox_open(struct mailbox *box)
{
	int ret;

	T_BEGIN {
		ret = mailbox_open_full(box, NULL);
	} T_END;
	if (ret < 0) {
		if (!box->mailbox_deleted || box->mailbox_undeleting)
			return -1;

		/* mailbox has been marked as deleted. if this deletion
		   started (and crashed) a long time ago, it can be confusing
		   to user that the mailbox can't be opened. so we'll just
		   undelete it and reopen. */
		if(!mailbox_try_undelete(box))
			return -1;

		/* make sure we close the mailbox in the middle. some backends
		   may not have fully opened the mailbox while it was being
		   undeleted. */
		mailbox_close(box);
		T_BEGIN {
			ret = mailbox_open_full(box, NULL);
		} T_END;
		if (ret < 0)
			return -1;
	}
	return 0;
}

static int mailbox_alloc_index_pvt(struct mailbox *box)
{
	const char *index_dir;
	int ret;

	if (box->index_pvt != NULL)
		return 1;

	ret = mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_INDEX_PRIVATE,
				  &index_dir);
	if (ret <= 0)
		return ret; /* error / no private indexes */

	if (mailbox_create_missing_dir(box, MAILBOX_LIST_PATH_TYPE_INDEX_PRIVATE) < 0)
		return -1;

	/* Note that this may cause box->event to live longer than box */
	box->index_pvt = mail_index_alloc_cache_get(box->event,
		NULL, index_dir, t_strconcat(box->index_prefix, ".pvt", NULL));
	mail_index_set_fsync_mode(box->index_pvt,
				  box->storage->set->parsed_fsync_mode, 0);
	mail_index_set_lock_method(box->index_pvt,
		box->storage->set->parsed_lock_method,
		mail_storage_get_lock_timeout(box->storage, UINT_MAX));
	return 1;
}

int mailbox_open_index_pvt(struct mailbox *box)
{
	enum mail_index_open_flags index_flags;
	int ret;

	if (box->view_pvt != NULL)
		return 1;
	if (mailbox_get_private_flags_mask(box) == 0)
		return 0;

	if ((ret = mailbox_alloc_index_pvt(box)) <= 0)
		return ret;
	index_flags = MAIL_INDEX_OPEN_FLAG_CREATE |
		mail_storage_settings_to_index_flags(box->storage->set);
	if ((box->flags & MAILBOX_FLAG_SAVEONLY) != 0)
		index_flags |= MAIL_INDEX_OPEN_FLAG_SAVEONLY;
	if (mail_index_open(box->index_pvt, index_flags) < 0)
		return -1;
	box->view_pvt = mail_index_view_open(box->index_pvt);
	return 1;
}

int mailbox_open_stream(struct mailbox *box, struct istream *input)
{
	return mailbox_open_full(box, input);
}

int mailbox_enable(struct mailbox *box, enum mailbox_feature features)
{
	if (mailbox_verify_name(box) < 0)
		return -1;

	int ret;
	T_BEGIN {
		ret = box->v.enable(box, features);
	} T_END;
	return ret;
}

enum mailbox_feature mailbox_get_enabled_features(struct mailbox *box)
{
	return box->enabled_features;
}

void mail_storage_free_binary_cache(struct mail_storage *storage)
{
	if (storage->binary_cache.box == NULL)
		return;

	timeout_remove(&storage->binary_cache.to);
	i_stream_destroy(&storage->binary_cache.input);
	i_zero(&storage->binary_cache);
}

void mailbox_close(struct mailbox *box)
{
	if (!box->opened)
		return;

	if (box->transaction_count != 0) {
		i_panic("Trying to close mailbox %s with open transactions",
			box->name);
	}
	T_BEGIN {
		box->v.close(box);
	} T_END;

	if (box->storage->binary_cache.box == box)
		mail_storage_free_binary_cache(box->storage);
	box->opened = FALSE;
	box->mailbox_deleted = FALSE;
	array_clear(&box->search_results);

	if (array_is_created(&box->recent_flags))
		array_free(&box->recent_flags);
	box->recent_flags_prev_uid = 0;
	box->recent_flags_count = 0;
}

void mailbox_free(struct mailbox **_box)
{
	struct mailbox *box = *_box;

	*_box = NULL;

	mailbox_close(box);
	box->v.free(box);

	if (box->attribute_iter_count != 0) {
		i_panic("Trying to free mailbox %s with %u open attribute iterators",
			box->name, box->attribute_iter_count);
	}

	DLLIST_REMOVE(&box->storage->mailboxes, box);
	mail_storage_obj_unref(box->storage);
	settings_free(box->set);
	pool_unref(&box->pool);
}

bool mailbox_equals(const struct mailbox *box1,
		    const struct mail_namespace *ns2, const char *vname2)
{
	struct mail_namespace *ns1 = mailbox_get_namespace(box1);
	const char *name1;

	if (ns1 != ns2)
		return FALSE;

	name1 = mailbox_get_vname(box1);
	if (strcmp(name1, vname2) == 0)
		return TRUE;

	return strcasecmp(name1, "INBOX") == 0 &&
		strcasecmp(vname2, "INBOX") == 0;
}

bool mailbox_is_any_inbox(struct mailbox *box)
{
	return box->inbox_any;
}

bool mailbox_has_special_use(struct mailbox *box, const char *special_use)
{
	if (box->set == NULL)
		return FALSE;
	return str_contains_special_use(t_array_const_string_join(&box->set->special_use, " "),
					special_use);
}

static void mailbox_copy_cache_decisions_from_inbox(struct mailbox *box)
{
	struct mail_namespace *ns =
		mail_namespace_find_inbox(box->storage->user->namespaces);
	struct mailbox *inbox =
		mailbox_alloc(ns->list, "INBOX", MAILBOX_FLAG_READONLY);
	enum mailbox_existence existence;

	/* this should be NoSelect but since inbox can never be
	   NoSelect we use EXISTENCE_NONE to avoid creating inbox by accident */
	if (mailbox_exists(inbox, FALSE, &existence) == 0 &&
	    existence != MAILBOX_EXISTENCE_NONE &&
	    mailbox_open(inbox) == 0 &&
	    mailbox_open(box) == 0) {
		/* we can't do much about errors here */
		(void)mail_cache_decisions_copy(inbox->cache, box->cache);
	}

	mailbox_free(&inbox);
}

int mailbox_create(struct mailbox *box, const struct mailbox_update *update,
		   bool directory)
{
	int ret;

	if (mailbox_verify_create_name(box) < 0)
		return -1;

	struct event_reason *reason = event_reason_begin("mailbox:create");

	/* Avoid race conditions by keeping mailbox list locked during changes.
	   This especially fixes a race during INBOX creation with
	   mailbox_list_layout=index
	   because it scans for missing mailboxes if INBOX doesn't exist. The
	   second process's scan can find a half-created INBOX and add it,
	   causing the first process to become confused. */
	if (mailbox_list_lock(box->list) < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		event_reason_end(&reason);
		return -1;
	}
	box->creating = TRUE;
	if ((box->list->props & MAILBOX_LIST_PROP_NO_NOSELECT) != 0) {
		/* Layout doesn't support creating \NoSelect mailboxes.
		   Switch to creating a selectable mailbox */
		directory = FALSE;
	}
	T_BEGIN {
		ret = box->v.create_box(box, update, directory);
	} T_END;
	box->creating = FALSE;
	mailbox_list_unlock(box->list);

	if (ret == 0) {
		box->list->guid_cache_updated = TRUE;
		if (!box->inbox_any) T_BEGIN {
			mailbox_copy_cache_decisions_from_inbox(box);
		} T_END;
	} else if (box->opened) {
		/* Creation failed after (partially) opening the mailbox.
		   It may not be in a valid state, so close it. */
		mail_storage_last_error_push(box->storage);
		mailbox_close(box);
		mail_storage_last_error_pop(box->storage);
	}
	event_reason_end(&reason);
	return ret;
}

int mailbox_update(struct mailbox *box, const struct mailbox_update *update)
{
	int ret;

	i_assert(update->min_next_uid == 0 ||
		 update->min_first_recent_uid == 0 ||
		 update->min_first_recent_uid <= update->min_next_uid);

	if (mailbox_verify_existing_name(box) < 0)
		return -1;

	struct event_reason *reason = event_reason_begin("mailbox:update");
	T_BEGIN {
		ret = box->v.update_box(box, update);
	} T_END;
	if (!guid_128_is_empty(update->mailbox_guid))
		box->list->guid_cache_invalidated = TRUE;
	event_reason_end(&reason);
	return ret;
}

int mailbox_mark_index_deleted(struct mailbox *box, bool del)
{
	struct mail_index_transaction *trans;
	enum mail_index_transaction_flags trans_flags = 0;
	enum mailbox_flags old_flag;
	int ret;

	e_debug(box->event, "Attempting to %s mailbox", del ?
		"delete" : "undelete");

	if (box->marked_deleted && del) {
		/* we already marked it deleted. this allows plugins to
		   "lock" the deletion earlier. */
		return 0;
	}

	old_flag = box->flags & MAILBOX_FLAG_OPEN_DELETED;
	box->flags |= MAILBOX_FLAG_OPEN_DELETED;
	ret = mailbox_open(box);
	box->flags = (box->flags & ENUM_NEGATE(MAILBOX_FLAG_OPEN_DELETED)) | old_flag;
	if (ret < 0)
		return -1;

	trans_flags = del ? 0 : MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL;
	trans = mail_index_transaction_begin(box->view, trans_flags);
	if (del)
		mail_index_set_deleted(trans);
	else
		mail_index_set_undeleted(trans);
	if (mail_index_transaction_commit(&trans) < 0) {
		mailbox_set_index_error(box);
		return -1;
	}

	if (del) {
		/* sync the mailbox. this finishes the index deletion and it
		   can succeed only for a single session. we do it here, so the
		   rest of the deletion code doesn't have to worry about race
		   conditions. */
		box->delete_sync_check = TRUE;
		ret = mailbox_sync(box, MAILBOX_SYNC_FLAG_FULL_READ);
		box->delete_sync_check = FALSE;
		if (ret < 0)
			return -1;
	}

	box->marked_deleted = del;
	return 0;
}

static void mailbox_close_reset_path(struct mailbox *box)
{
	i_zero(&box->_perm);
	box->_path = NULL;
	box->_index_path = NULL;
}

static int mailbox_delete_real(struct mailbox *box)
{
	bool list_locked;
	int ret;

	if (*box->name == '\0') {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       "Storage root can't be deleted");
		return -1;
	}

	struct event_reason *reason = event_reason_begin("mailbox:delete");

	box->deleting = TRUE;
	if (mailbox_open(box) < 0) {
		if (mailbox_get_last_mail_error(box) != MAIL_ERROR_NOTFOUND &&
		    !box->mailbox_deleted) {
			event_reason_end(&reason);
			return -1;
		}
		/* might be a \noselect mailbox, so continue deletion */
	}

	if (mailbox_list_lock(box->list) < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		list_locked = FALSE;
		ret = -1;
	} else {
		list_locked = TRUE;
		ret = box->v.delete_box(box);
	}
	if (ret < 0 && box->marked_deleted) {
		/* deletion failed. revert the mark so it can maybe be
		   tried again later. */
		if (mailbox_mark_index_deleted(box, FALSE) < 0)
			ret = -1;
	}
	if (list_locked)
		mailbox_list_unlock(box->list);

	box->deleting = FALSE;
	mailbox_close(box);

	/* if mailbox is reopened, its path may be different with
	   mailbox_list_layout=index */
	mailbox_close_reset_path(box);
	event_reason_end(&reason);
	return ret;
}

int mailbox_delete(struct mailbox *box)
{
	int ret;
	T_BEGIN {
		ret = mailbox_delete_real(box);
	} T_END;
	return ret;
}

int mailbox_delete_empty(struct mailbox *box)
{
	int ret;

	/* FIXME: should be a parameter to delete(), but since it changes API
	   don't do it for now */
	box->deleting_must_be_empty = TRUE;
	ret = mailbox_delete(box);
	box->deleting_must_be_empty = FALSE;
	return ret;
}

static bool
mail_storages_rename_compatible(struct mail_storage *storage1,
				struct mail_storage *storage2,
				const char **error_r)
{
	if (storage1 == storage2)
		return TRUE;

	if (strcmp(storage1->name, storage2->name) != 0) {
		*error_r = t_strdup_printf("storage %s != %s",
					   storage1->name, storage2->name);
		return FALSE;
	}
	if ((storage1->class_flags & MAIL_STORAGE_CLASS_FLAG_UNIQUE_ROOT) != 0) {
		/* e.g. mdbox where all mails are in storage/ directory and
		   they can't be easily moved from there. */
		*error_r = t_strdup_printf("storage %s uses unique root",
					   storage1->name);
		return FALSE;
	}
	return TRUE;
}

static bool nullequals(const void *p1, const void *p2)
{
	return (p1 == NULL && p2 == NULL) || (p1 != NULL && p2 != NULL);
}

static bool
mailbox_lists_rename_compatible(struct mailbox_list *list1,
				struct mailbox_list *list2,
				const char **error_r)
{
	if (!nullequals(list1->mail_set->mail_alt_path,
			list2->mail_set->mail_alt_path)) {
		*error_r = t_strdup_printf(
			"Namespace %s has mail_alt_path, %s doesn't",
			list1->ns->set->name, list2->ns->set->name);
		return FALSE;
	}
	if (!nullequals(list1->mail_set->mail_index_path,
			list2->mail_set->mail_index_path)) {
		*error_r = t_strdup_printf(
			"Namespace %s has mail_index_path, %s doesn't",
			list1->ns->set->name, list2->ns->set->name);
		return FALSE;
	}
	if (!nullequals(list1->mail_set->mail_cache_path,
			list2->mail_set->mail_cache_path)) {
		*error_r = t_strdup_printf(
			"Namespace %s has mail_cache_path, %s doesn't",
			list1->ns->set->name, list2->ns->set->name);
		return FALSE;
	}
	if (!nullequals(list1->mail_set->mail_control_path,
			list2->mail_set->mail_control_path)) {
		*error_r = t_strdup_printf(
			"Namespace %s has mail_control_path, %s doesn't",
			list1->ns->set->name, list2->ns->set->name);
		return FALSE;
	}
	return TRUE;
}

static
int mailbox_rename_check_children(struct mailbox *src, struct mailbox *dest)
{
	int ret = 0;
	size_t src_prefix_len = strlen(src->vname)+1; /* include separator */
	size_t dest_prefix_len = strlen(dest->vname)+1;
	/* this can return folders with * in their name, that are not
	   actually our children */
	char ns_sep = mail_namespace_get_sep(src->list->ns);
	const char *pattern = t_strdup_printf("%s%c*", src->vname, ns_sep);

	struct mailbox_list_iterate_context *iter = mailbox_list_iter_init(src->list, pattern,
				      MAILBOX_LIST_ITER_RETURN_NO_FLAGS);

	const struct mailbox_info *child;
	while((child = mailbox_list_iter_next(iter)) != NULL) {
		if (strncmp(child->vname, src->vname, src_prefix_len-1) != 0 ||
		    child->vname[src_prefix_len-1] != ns_sep)
			continue; /* not our child */
		/* if total length of new name exceeds the limit, fail */
		if (strlen(child->vname + src_prefix_len)+dest_prefix_len > MAILBOX_LIST_NAME_MAX_LENGTH) {
			mail_storage_set_error(src->storage, MAIL_ERROR_PARAMS,
				"Mailbox or child name too long");
			ret = -1;
			break;
		}
	}

	/* something went bad */
	if (mailbox_list_iter_deinit(&iter) < 0) {
		mail_storage_copy_list_error(src->storage, src->list);
		ret = -1;
	}
	return ret;
}

static int mailbox_rename_real(struct mailbox *src, struct mailbox *dest)
{
	const char *error = NULL;

	/* Check only name validity, \Noselect don't necessarily exist. */
	if (mailbox_verify_name(src) < 0)
		return -1;
	if (*src->name == '\0') {
		mail_storage_set_error(src->storage, MAIL_ERROR_PARAMS,
				       "Can't rename mailbox root");
		return -1;
	}
	if (mailbox_verify_create_name(dest) < 0) {
		mail_storage_copy_error(src->storage, dest->storage);
		return -1;
	}
	if (mailbox_rename_check_children(src, dest) != 0) {
		return -1;
	}

	if (!mail_storages_rename_compatible(src->storage,
					     dest->storage, &error) ||
	    !mailbox_lists_rename_compatible(src->list,
					     dest->list, &error)) {
		e_debug(src->event,
			"Can't rename '%s' to '%s': %s",
			src->vname, dest->vname, error);
		mail_storage_set_error(src->storage, MAIL_ERROR_NOTPOSSIBLE,
			"Can't rename mailboxes across specified storages.");
		return -1;
	}
	if (src->list != dest->list &&
	    (src->list->ns->type != MAIL_NAMESPACE_TYPE_PRIVATE ||
	     dest->list->ns->type != MAIL_NAMESPACE_TYPE_PRIVATE)) {
		mail_storage_set_error(src->storage, MAIL_ERROR_NOTPOSSIBLE,
			"Renaming not supported across non-private namespaces.");
		return -1;
	}
	if (src->list == dest->list && strcmp(src->name, dest->name) == 0) {
		mail_storage_set_error(src->storage, MAIL_ERROR_EXISTS,
				       "Can't rename mailbox to itself.");
		return -1;
	}

	/* It would be safer to lock both source and destination, but that
	   could lead to deadlocks. So at least for now lets just lock only the
	   destination list. */
	if (mailbox_list_lock(dest->list) < 0) {
		mail_storage_copy_list_error(src->storage, dest->list);
		return -1;
	}
	int ret = src->v.rename_box(src, dest);
	mailbox_list_unlock(dest->list);
	if (ret < 0)
		return -1;
	src->list->guid_cache_invalidated = TRUE;
	dest->list->guid_cache_invalidated = TRUE;
	return 0;
}

int mailbox_rename(struct mailbox *src, struct mailbox *dest)
{
	 int ret;
	 T_BEGIN {
		 struct event_reason *reason =
			 event_reason_begin("mailbox:rename");
		 ret = mailbox_rename_real(src, dest);
		 event_reason_end(&reason);
	 } T_END;
	 return ret;
}

int mailbox_set_subscribed(struct mailbox *box, bool set)
{
	int ret;

	if (mailbox_verify_name(box) < 0)
		return -1;

	struct event_reason *reason =
		event_reason_begin(set ? "mailbox:subscribe" :
				   "mailbox:unsubscribe");
	if (mailbox_list_iter_subscriptions_refresh(box->list) < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		ret = -1;
	} else if (mailbox_is_subscribed(box) == set)
		ret = 0;
	else T_BEGIN {
		ret = box->v.set_subscribed(box, set);
	} T_END;
	event_reason_end(&reason);
	return ret;
}

bool mailbox_is_subscribed(struct mailbox *box)
{
	struct mailbox_node *node;

	i_assert(box->list->subscriptions != NULL);

	node = mailbox_tree_lookup(box->list->subscriptions, box->vname);
	return node != NULL && (node->flags & MAILBOX_SUBSCRIBED) != 0;
}

struct mail_storage *mailbox_get_storage(const struct mailbox *box)
{
	return box->storage;
}

struct mail_namespace *
mailbox_get_namespace(const struct mailbox *box)
{
	return box->list->ns;
}

const struct mailbox_settings *mailbox_get_settings(struct mailbox *box)
{
	return box->set;
}

const char *mailbox_get_name(const struct mailbox *box)
{
	return box->name;
}

const char *mailbox_get_vname(const struct mailbox *box)
{
	return box->vname;
}

bool mailbox_is_readonly(struct mailbox *box)
{
	i_assert(box->opened);

	return box->v.is_readonly(box);
}

bool mailbox_backends_equal(const struct mailbox *box1,
			    const struct mailbox *box2)
{
	struct mail_namespace *ns1 = box1->list->ns, *ns2 = box2->list->ns;

	if (strcmp(box1->name, box2->name) != 0)
		return FALSE;

	while (ns1->alias_for != NULL)
		ns1 = ns1->alias_for;
	while (ns2->alias_for != NULL)
		ns2 = ns2->alias_for;
	return ns1 == ns2;
}

static void
mailbox_get_status_set_defaults(struct mailbox *box,
				struct mailbox_status *status_r)
{
	i_zero(status_r);
	if ((box->storage->class_flags & MAIL_STORAGE_CLASS_FLAG_HAVE_MAIL_GUIDS) != 0)
		status_r->have_guids = TRUE;
	if ((box->storage->class_flags & MAIL_STORAGE_CLASS_FLAG_HAVE_MAIL_SAVE_GUIDS) != 0)
		status_r->have_save_guids = TRUE;
	if ((box->storage->class_flags & MAIL_STORAGE_CLASS_FLAG_HAVE_MAIL_GUID128) != 0)
		status_r->have_only_guid128 = TRUE;
}

int mailbox_get_status(struct mailbox *box,
		       enum mailbox_status_items items,
		       struct mailbox_status *status_r)
{
	mailbox_get_status_set_defaults(box, status_r);
	if (mailbox_verify_existing_name(box) < 0)
		return -1;

	int ret;
	T_BEGIN {
		ret = box->v.get_status(box, items, status_r);
	} T_END;
	if (ret < 0)
		return -1;
	i_assert(status_r->have_guids || !status_r->have_save_guids);
	return 0;
}

void mailbox_get_open_status(struct mailbox *box,
			     enum mailbox_status_items items,
			     struct mailbox_status *status_r)
{
	i_assert(box->opened);
	i_assert((items & MAILBOX_STATUS_FAILING_ITEMS) == 0);

	mailbox_get_status_set_defaults(box, status_r);
	T_BEGIN {
		if (box->v.get_status(box, items, status_r) < 0)
			i_unreached();
	} T_END;
}

int mailbox_get_metadata(struct mailbox *box, enum mailbox_metadata_items items,
			 struct mailbox_metadata *metadata_r)
{
	i_zero(metadata_r);
	if (mailbox_verify_existing_name(box) < 0)
		return -1;

	/* NOTE: metadata_r->cache_fields is currently returned from
	   data stack, so can't use a data stack frame here. */
	if (box->v.get_metadata(box, items, metadata_r) < 0)
		return -1;

	i_assert((items & MAILBOX_METADATA_GUID) == 0 ||
		 !guid_128_is_empty(metadata_r->guid));
	return 0;
}

enum mail_flags mailbox_get_private_flags_mask(struct mailbox *box)
{
	if (box->v.get_private_flags_mask != NULL)
		return box->v.get_private_flags_mask(box);
	else if (box->list->mail_set->mail_index_private_path[0] != '\0')
		return MAIL_SEEN; /* FIXME */
	else
		return 0;
}

struct mailbox_sync_context *
mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct mailbox_sync_context *ctx;

	if (box->transaction_count != 0) {
		i_panic("Trying to sync mailbox %s with open transactions",
			box->name);
	}
	if (!box->opened) {
		if (mailbox_open(box) < 0) {
			ctx = i_new(struct mailbox_sync_context, 1);
			ctx->box = box;
			ctx->flags = flags;
			ctx->open_failed = TRUE;
			return ctx;
		}
	}
	T_BEGIN {
		ctx = box->v.sync_init(box, flags);
	} T_END;
	return ctx;
}

bool mailbox_sync_next(struct mailbox_sync_context *ctx,
		       struct mailbox_sync_rec *sync_rec_r)
{
	if (ctx->open_failed)
		return FALSE;

	bool ret;
	T_BEGIN {
		ret = ctx->box->v.sync_next(ctx, sync_rec_r);
	} T_END;
	return ret;
}

int mailbox_sync_deinit(struct mailbox_sync_context **_ctx,
			struct mailbox_sync_status *status_r)
{
	struct mailbox_sync_context *ctx = *_ctx;
	struct mailbox *box = ctx->box;
	const char *errormsg;
	enum mail_error error;
	int ret;

	*_ctx = NULL;

	i_zero(status_r);

	if (!ctx->open_failed) {
		T_BEGIN {
			ret = box->v.sync_deinit(ctx, status_r);
		} T_END;
	} else {
		i_free(ctx);
		ret = -1;
	}
	if (ret < 0 && box->inbox_user &&
	    !box->storage->user->inbox_open_error_logged) {
		errormsg = mailbox_get_last_internal_error(box, &error);
		if (error == MAIL_ERROR_NOTPOSSIBLE) {
			box->storage->user->inbox_open_error_logged = TRUE;
			e_error(box->event, "Syncing INBOX failed: %s", errormsg);
		}
	}
	if (ret == 0)
		box->synced = TRUE;
	return ret;
}

int mailbox_sync(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct mailbox_sync_context *ctx;
	struct mailbox_sync_status status;

	if (array_count(&box->search_results) == 0) {
		/* we don't care about mailbox's current state, so we might
		   as well fix inconsistency state */
		flags |= MAILBOX_SYNC_FLAG_FIX_INCONSISTENT;
	}

	ctx = mailbox_sync_init(box, flags);
	return mailbox_sync_deinit(&ctx, &status);
}

#undef mailbox_notify_changes
void mailbox_notify_changes(struct mailbox *box,
			    mailbox_notify_callback_t *callback, void *context)
{
	i_assert(box->opened);

	box->notify_callback = callback;
	box->notify_context = context;

	T_BEGIN {
		box->v.notify_changes(box);
	} T_END;
}

void mailbox_notify_changes_stop(struct mailbox *box)
{
	i_assert(box->opened);

	box->notify_callback = NULL;
	box->notify_context = NULL;

	T_BEGIN {
		box->v.notify_changes(box);
	} T_END;
}

struct mail_search_context *
mailbox_search_init(struct mailbox_transaction_context *t,
		    struct mail_search_args *args,
		    const enum mail_sort_type *sort_program,
		    enum mail_fetch_field wanted_fields,
		    struct mailbox_header_lookup_ctx *wanted_headers)
{
	i_assert(wanted_headers == NULL || wanted_headers->box == t->box);

	mail_search_args_ref(args);
	if (!args->simplified)
		mail_search_args_simplify(args);

	struct mail_search_context *ctx;
	T_BEGIN {
		ctx = t->box->v.search_init(t, args, sort_program,
					    wanted_fields, wanted_headers);
	} T_END;
	return ctx;
}

int mailbox_search_deinit(struct mail_search_context **_ctx)
{
	struct mail_search_context *ctx = *_ctx;
	struct mail_search_args *args = ctx->args;
	int ret;

	*_ctx = NULL;
	mailbox_search_results_initial_done(ctx);
	T_BEGIN {
		ret = ctx->transaction->box->v.search_deinit(ctx);
	} T_END;
	mail_search_args_unref(&args);
	return ret;
}

void mailbox_search_reset_progress_start(struct mail_search_context *ctx)
{
	i_zero(&ctx->search_start_time);
	i_zero(&ctx->last_notify);
}

void
mailbox_search_set_progress_hidden(struct mail_search_context *ctx, bool hidden)
{
	ctx->progress_hidden = hidden;
}

void mailbox_search_notify(struct mailbox *box, struct mail_search_context *ctx)
{
	if (ctx->search_start_time.tv_sec == 0) {
		ctx->search_start_time = ioloop_timeval;
		return;
	}

	if (ctx->last_notify.tv_sec == 0)
		ctx->last_notify = ctx->search_start_time;

	if (box->storage->callbacks.notify_progress == NULL ||
	    ctx->progress_hidden)
	    	return;

	if (++ctx->search_notify_passes % 1024 == 0)
		io_loop_time_refresh();

	if (ioloop_time - ctx->last_notify.tv_sec < MAIL_STORAGE_NOTIFY_INTERVAL_SECS)
	    	return;

	struct mail_storage_progress_details dtl = {
		.total = ctx->progress_max,
		.processed = ctx->progress_cur,
		.start_time = ctx->search_start_time,
		.now = ioloop_timeval,
	};

	box->storage->callbacks.notify_progress(box, &dtl,
						box->storage->callback_context);

	ctx->last_notify = ioloop_timeval;
}

bool mailbox_search_next(struct mail_search_context *ctx, struct mail **mail_r)
{
	bool tryagain;

	while (!mailbox_search_next_nonblock(ctx, mail_r, &tryagain)) {
		if (!tryagain)
			return FALSE;
	}
	return TRUE;
}

bool mailbox_search_next_nonblock(struct mail_search_context *ctx,
				  struct mail **mail_r, bool *tryagain_r)
{
	struct mailbox *box = ctx->transaction->box;
	bool ret;

	*mail_r = NULL;
	*tryagain_r = FALSE;

	T_BEGIN {
		mailbox_search_notify(box, ctx);
		ret = box->v.search_next_nonblock(ctx, mail_r, tryagain_r);
	} T_END;
	if (!ret)
		return FALSE;
	else {
		mailbox_search_results_add(ctx, (*mail_r)->uid);
		return TRUE;
	}
}

bool mailbox_search_seen_lost_data(struct mail_search_context *ctx)
{
	return ctx->seen_lost_data;
}

void mailbox_search_mail_detach(struct mail_search_context *ctx,
				struct mail *mail)
{
	struct mail_private *pmail =
		container_of(mail, struct mail_private, mail);
	unsigned int idx;

	if (!array_lsearch_ptr_idx(&ctx->mails, mail, &idx))
		i_unreached();
	pmail->search_mail = FALSE;
	array_delete(&ctx->mails, idx, 1);
}

int mailbox_search_result_build(struct mailbox_transaction_context *t,
				struct mail_search_args *args,
				enum mailbox_search_result_flags flags,
				struct mail_search_result **result_r)
{
	struct mail_search_context *ctx;
	struct mail *mail;
	int ret;

	ctx = mailbox_search_init(t, args, NULL, 0, NULL);
	*result_r = mailbox_search_result_save(ctx, flags);
	while (mailbox_search_next(ctx, &mail)) ;

	ret = mailbox_search_deinit(&ctx);
	if (ret < 0)
		mailbox_search_result_free(result_r);
	return ret;
}

struct mailbox_transaction_context *
mailbox_transaction_begin(struct mailbox *box,
			  enum mailbox_transaction_flags flags,
			  const char *reason)
{
	struct mailbox_transaction_context *trans;

	i_assert(box->opened);

	box->transaction_count++;
	T_BEGIN {
		trans = box->v.transaction_begin(box, flags, reason);
	} T_END;
	i_assert(trans->reason != NULL);
	return trans;
}

int mailbox_transaction_commit(struct mailbox_transaction_context **t)
{
	struct mail_transaction_commit_changes changes;
	int ret;

	/* Store changes temporarily so that plugins overriding
	   transaction_commit() can look at them. */
	ret = mailbox_transaction_commit_get_changes(t, &changes);
	pool_unref(&changes.pool);
	return ret;
}

int mailbox_transaction_commit_get_changes(
	struct mailbox_transaction_context **_t,
	struct mail_transaction_commit_changes *changes_r)
{
	struct mailbox_transaction_context *t = *_t;
	struct mailbox *box = t->box;
	unsigned int save_count = t->save_count;
	struct event_reason *reason = NULL;
	int ret;

	changes_r->pool = NULL;

	*_t = NULL;

	if (t->itrans->attribute_updates != NULL &&
	    t->itrans->attribute_updates->used > 0) {
		/* attribute changes are also done directly via lib-index
		   by ACL and Sieve */
		reason = event_reason_begin("mailbox:attributes_changed");
	}
	T_BEGIN {
		ret = box->v.transaction_commit(t, changes_r);
	} T_END;
	/* either all the saved messages get UIDs or none, because a) we
	   failed, b) MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS not set,
	   c) backend doesn't support it (e.g. virtual plugin) */
	i_assert(ret < 0 ||
		 seq_range_count(&changes_r->saved_uids) == save_count ||
		 array_count(&changes_r->saved_uids) == 0);
	/* decrease the transaction count only after transaction_commit().
	   that way if it creates and destroys transactions internally, we
	   don't see transaction_count=0 until the parent transaction is fully
	   finished */
	box->transaction_count--;
	event_reason_end(&reason);
	if (ret == 0 && box->mailbox_not_original) {
		/* The mailbox name changed while opening it. This is
		   intentional when virtual mailbox is opened for saving mails,
		   which causes the backend mailbox to be opened instead. In
		   this situation the UIDVALIDITY / UIDs are for the physical
		   mailbox, not the virtual mailbox. Use this flag to prevent
		   IMAP APPEND from returning any UIDs in the tagged reply,
		   since they would be wrong. */
		changes_r->no_read_perm = TRUE;
	}
	if (ret < 0 && changes_r->pool != NULL)
		pool_unref(&changes_r->pool);
	return ret;
}

void mailbox_transaction_rollback(struct mailbox_transaction_context **_t)
{
	struct mailbox_transaction_context *t = *_t;
	struct mailbox *box = t->box;

	*_t = NULL;
	T_BEGIN {
		box->v.transaction_rollback(t);
	} T_END;
	box->transaction_count--;
}

unsigned int mailbox_transaction_get_count(const struct mailbox *box)
{
	return box->transaction_count;
}

void mailbox_transaction_set_max_modseq(struct mailbox_transaction_context *t,
					uint64_t max_modseq,
					ARRAY_TYPE(seq_range) *seqs)
{
	mail_index_transaction_set_max_modseq(t->itrans, max_modseq, seqs);
}

struct mailbox *
mailbox_transaction_get_mailbox(const struct mailbox_transaction_context *t)
{
	return t->box;
}

static void mailbox_save_dest_mail_close(struct mail_save_context *ctx)
{
	struct mail_private *mail = (struct mail_private *)ctx->dest_mail;

	T_BEGIN {
		mail->v.close(&mail->mail);
	} T_END;
}

struct mail_save_context *
mailbox_save_alloc(struct mailbox_transaction_context *t)
{
	struct mail_save_context *ctx;
	T_BEGIN {
		ctx = t->box->v.save_alloc(t);
	} T_END;
	i_assert(!ctx->unfinished);
	ctx->unfinished = TRUE;
	ctx->data.received_date = (time_t)-1;
	ctx->data.save_date = (time_t)-1;

	/* Always have a dest_mail available. A lot of plugins make use
	   of this. */
	if (ctx->dest_mail == NULL)
		ctx->dest_mail = mail_alloc(t, 0, NULL);
	else {
		/* make sure the mail isn't used before mail_set_seq_saving() */
		mailbox_save_dest_mail_close(ctx);
	}

	return ctx;
}

void mailbox_save_context_deinit(struct mail_save_context *ctx)
{
	i_assert(ctx->dest_mail != NULL);

	mail_free(&ctx->dest_mail);
}

void mailbox_save_set_flags(struct mail_save_context *ctx,
			    enum mail_flags flags,
			    struct mail_keywords *keywords)
{
	struct mailbox *box = ctx->transaction->box;

	if (ctx->data.keywords != NULL)
		mailbox_keywords_unref(&ctx->data.keywords);

	ctx->data.flags = flags & ENUM_NEGATE(mailbox_get_private_flags_mask(box));
	ctx->data.pvt_flags = flags & mailbox_get_private_flags_mask(box);
	ctx->data.keywords = keywords;
	if (keywords != NULL)
		mailbox_keywords_ref(keywords);
}

void mailbox_save_copy_flags(struct mail_save_context *ctx, struct mail *mail)
{
	const char *const *keywords_list;
	struct mail_keywords *keywords;

	keywords_list = mail_get_keywords(mail);
	keywords = str_array_length(keywords_list) == 0 ? NULL :
		mailbox_keywords_create_valid(ctx->transaction->box,
					      keywords_list);
	mailbox_save_set_flags(ctx, mail_get_flags(mail), keywords);
	if (keywords != NULL)
		mailbox_keywords_unref(&keywords);
}

void mailbox_save_set_min_modseq(struct mail_save_context *ctx,
				 uint64_t min_modseq)
{
	ctx->data.min_modseq = min_modseq;
}

void mailbox_save_set_received_date(struct mail_save_context *ctx,
				    time_t received_date, int timezone_offset)
{
	ctx->data.received_date = received_date;
	ctx->data.received_tz_offset = timezone_offset;
}

void mailbox_save_set_save_date(struct mail_save_context *ctx,
				time_t save_date)
{
	ctx->data.save_date = save_date;
}

void mailbox_save_set_from_envelope(struct mail_save_context *ctx,
				    const char *envelope)
{
	i_free(ctx->data.from_envelope);
	ctx->data.from_envelope = i_strdup(envelope);
}

void mailbox_save_set_uid(struct mail_save_context *ctx, uint32_t uid)
{
	ctx->data.uid = uid;
}

void mailbox_save_set_guid(struct mail_save_context *ctx, const char *guid)
{
	i_assert(guid == NULL || *guid != '\0');

	i_free(ctx->data.guid);
	ctx->data.guid = i_strdup(guid);
}

void mailbox_save_set_pop3_uidl(struct mail_save_context *ctx, const char *uidl)
{
	i_assert(*uidl != '\0');
	i_assert(strchr(uidl, '\n') == NULL);

	i_free(ctx->data.pop3_uidl);
	ctx->data.pop3_uidl = i_strdup(uidl);
}

void mailbox_save_set_pop3_order(struct mail_save_context *ctx,
				 unsigned int order)
{
	i_assert(order > 0);

	ctx->data.pop3_order = order;
}

struct mail *mailbox_save_get_dest_mail(struct mail_save_context *ctx)
{
	return ctx->dest_mail;
}

int mailbox_save_begin(struct mail_save_context **ctx, struct istream *input)
{
	struct mailbox *box = (*ctx)->transaction->box;
	int ret;

	if (mail_index_is_deleted(box->index)) {
		mailbox_set_deleted(box);
		mailbox_save_cancel(ctx);
		return -1;
	}

	/* make sure parts get parsed early on */
	if (box->storage->set->parsed_mail_attachment_detection_add_flags)
		mail_add_temp_wanted_fields((*ctx)->dest_mail,
					    MAIL_FETCH_MESSAGE_PARTS, NULL);

	if (!(*ctx)->copying_or_moving) {
		/* We're actually saving the mail. We're not being called by
		   mail_storage_copy() because backend didn't support fast
		   copying. */
		i_assert(!(*ctx)->copying_via_save);
		(*ctx)->saving = TRUE;
	} else {
		i_assert((*ctx)->copying_via_save);
	}
	if (box->v.save_begin == NULL) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Saving messages not supported");
		ret = -1;
	} else T_BEGIN {
		ret = box->v.save_begin(*ctx, input);
	} T_END;

	if (ret < 0) {
		mailbox_save_cancel(ctx);
		return -1;
	}
	return 0;
}

int mailbox_save_begin_replace(struct mail_save_context **ctx,
			       struct istream *input,
			       struct mail *replaced)
{
	(*ctx)->expunged_mail = replaced;
	return mailbox_save_begin(ctx, input);
}

int mailbox_save_continue(struct mail_save_context *ctx)
{
	int ret;

	T_BEGIN {
		ret = ctx->transaction->box->v.save_continue(ctx);
	} T_END;
	return ret;
}

static void
mailbox_save_add_pvt_flags(struct mailbox_transaction_context *t,
			   enum mail_flags pvt_flags)
{
	struct mail_save_private_changes *save;

	if (!array_is_created(&t->pvt_saves))
		i_array_init(&t->pvt_saves, 8);
	save = array_append_space(&t->pvt_saves);
	save->mailnum = t->save_count;
	save->flags = pvt_flags;
}

static void
mailbox_save_context_reset(struct mail_save_context *ctx, bool success)
{
	i_assert(!ctx->unfinished);
	if (!ctx->copying_or_moving) {
		/* we're finishing a save (not copy/move). Note that we could
		   have come here also from mailbox_save_cancel(), in which
		   case ctx->saving may be FALSE. */
		i_assert(!ctx->copying_via_save);
		i_assert(ctx->saving || !success);
		ctx->saving = FALSE;
	} else {
		i_assert(ctx->copying_via_save || !success);
		/* We came from mailbox_copy(). saving==TRUE is possible here
		   if we also came from mailbox_save_using_mail(). Don't set
		   saving=FALSE yet in that case, because copy() is still
		   running. */
	}
}

int mailbox_save_finish(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;
	struct mailbox_transaction_context *t = ctx->transaction;
	/* we need to keep a copy of this because save_finish implementations
	   will likely zero the data structure during cleanup */
	enum mail_flags pvt_flags = ctx->data.pvt_flags;
	bool copying_via_save = ctx->copying_via_save;
	int ret;

	/* Do one final continue. The caller may not have done it if the
	   input stream's offset already matched the number of bytes that
	   were wanted to be saved. But due to nested istreams some of the
	   underlying ones may not have seen the EOF yet, and haven't flushed
	   out the pending data. */
	if (mailbox_save_continue(ctx) < 0) {
		mailbox_save_cancel(_ctx);
		return -1;
	}
	*_ctx = NULL;

	ctx->finishing = TRUE;
	T_BEGIN {
		ret = t->box->v.save_finish(ctx);
	} T_END;
	ctx->finishing = FALSE;

	if (ret == 0 && !copying_via_save) {
		if (pvt_flags != 0)
			mailbox_save_add_pvt_flags(t, pvt_flags);
		t->save_count++;
		if (ctx->expunged_mail != NULL)
			mail_expunge(ctx->expunged_mail);
	}

	mailbox_save_context_reset(ctx, TRUE);
	return ret;
}

void mailbox_save_cancel(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;

	*_ctx = NULL;
	T_BEGIN {
		ctx->transaction->box->v.save_cancel(ctx);
	} T_END;

	/* the dest_mail is no longer valid. if we're still saving
	   more mails, the mail sequence may get reused. make sure
	   the mail gets reset in between */
	mailbox_save_dest_mail_close(ctx);

	mailbox_save_context_reset(ctx, FALSE);
}

struct mailbox_transaction_context *
mailbox_save_get_transaction(struct mail_save_context *ctx)
{
	return ctx->transaction;
}

static int mailbox_copy_int(struct mail_save_context **_ctx, struct mail *mail)
{
	struct mail_save_context *ctx = *_ctx;
	struct mailbox_transaction_context *t = ctx->transaction;
	enum mail_flags pvt_flags = ctx->data.pvt_flags;
	struct mail *backend_mail;
	int ret;

	*_ctx = NULL;

	if (mail_index_is_deleted(t->box->index)) {
		mailbox_set_deleted(t->box);
		mailbox_save_cancel(&ctx);
		return -1;
	}

	/* bypass virtual storage, so hard linking can be used whenever
	   possible */
	if (mail_get_backend_mail(mail, &backend_mail) < 0) {
		mailbox_save_cancel(&ctx);
		return -1;
	}

	i_assert(!ctx->copying_or_moving);
	i_assert(ctx->copy_src_mail == NULL);
	ctx->copying_or_moving = TRUE;
	ctx->copy_src_mail = mail;
	ctx->finishing = TRUE;
	T_BEGIN {
		ret = t->box->v.copy(ctx, backend_mail);
	} T_END;
	ctx->finishing = FALSE;
	if (ret == 0) {
		if (pvt_flags != 0)
			mailbox_save_add_pvt_flags(t, pvt_flags);
		t->save_count++;
	}
	i_assert(!ctx->unfinished);

	ctx->copy_src_mail = NULL;
	ctx->copying_via_save = FALSE;
	ctx->copying_or_moving = FALSE;
	ctx->saving = FALSE; /* if we came from mailbox_save_using_mail() */
	return ret;
}

int mailbox_copy(struct mail_save_context **_ctx, struct mail *mail)
{
	struct mail_save_context *ctx = *_ctx;

	i_assert(!ctx->saving);
	i_assert(!ctx->moving);

	int ret;
	T_BEGIN {
		ret = mailbox_copy_int(_ctx, mail);
	} T_END;

	return ret;
}

int mailbox_move(struct mail_save_context **_ctx, struct mail *mail)
{
	struct mail_save_context *ctx = *_ctx;
	int ret;

	i_assert(!ctx->saving);
	i_assert(!ctx->moving);

	ctx->moving = TRUE;
	ctx->expunged_mail = mail;
	T_BEGIN {
		if ((ret = mailbox_copy_int(_ctx, mail)) == 0)
			mail_expunge(mail);
	} T_END;
	ctx->moving = FALSE;
	return ret;
}

int mailbox_save_using_mail(struct mail_save_context **_ctx, struct mail *mail)
{
	struct mail_save_context *ctx = *_ctx;

	i_assert(!ctx->saving);
	i_assert(!ctx->moving);

	ctx->saving = TRUE;
	return mailbox_copy_int(_ctx, mail);
}

bool mailbox_is_inconsistent(struct mailbox *box)
{
	return box->mailbox_deleted || box->v.is_inconsistent(box);
}

void mailbox_set_deleted(struct mailbox *box)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
			       "Mailbox was deleted under us");
	box->mailbox_deleted = TRUE;
}

static int get_path_to(struct mailbox *box, enum mailbox_list_path_type type,
		       const char **internal_path, const char **path_r)
{
	int ret;

	if (internal_path != NULL && *internal_path != NULL) {
		if ((*internal_path)[0] == '\0') {
			*path_r = NULL;
			return 0;
		}
		*path_r = *internal_path;
		return 1;
	}
	ret = mailbox_list_get_path(box->list, box->name, type, path_r);
	if (ret < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		return -1;
	}
	if (internal_path != NULL && *internal_path == NULL)
		*internal_path = ret == 0 ? "" : p_strdup(box->pool, *path_r);
	return ret;
}

int mailbox_get_path_to(struct mailbox *box, enum mailbox_list_path_type type,
			const char **path_r)
{
	if (type == MAILBOX_LIST_PATH_TYPE_MAILBOX)
		return get_path_to(box, type, &box->_path, path_r);
	if (type == MAILBOX_LIST_PATH_TYPE_INDEX)
		return get_path_to(box, type, &box->_index_path, path_r);
	return get_path_to(box, type, NULL, path_r);
}

const char *mailbox_get_path(struct mailbox *box)
{
	i_assert(box->_path != NULL);
	i_assert(box->_path[0] != '\0');
	return box->_path;
}

const char *mailbox_get_index_path(struct mailbox *box)
{
	i_assert(box->_index_path != NULL);
	i_assert(box->_index_path[0] != '\0');
	return box->_index_path;
}

static void mailbox_get_permissions_if_not_set(struct mailbox *box)
{
	if (box->_perm.file_create_mode != 0)
		return;

	if (box->input != NULL) {
		box->_perm.file_uid = geteuid();
		box->_perm.file_create_mode = 0600;
		box->_perm.dir_create_mode = 0700;
		box->_perm.file_create_gid = (gid_t)-1;
		box->_perm.file_create_gid_origin = "defaults";
		return;
	}

	struct mailbox_permissions perm;
	mailbox_list_get_permissions(box->list, box->name, &perm);
	mailbox_permissions_copy(&box->_perm, &perm, box->pool);
}

const struct mailbox_permissions *mailbox_get_permissions(struct mailbox *box)
{
	mailbox_get_permissions_if_not_set(box);

	if (!box->_perm.mail_index_permissions_set && box->index != NULL) {
		box->_perm.mail_index_permissions_set = TRUE;
		mail_index_set_permissions(box->index,
					   box->_perm.file_create_mode,
					   box->_perm.file_create_gid,
					   box->_perm.file_create_gid_origin);
	}
	return &box->_perm;
}

void mailbox_refresh_permissions(struct mailbox *box)
{
	i_zero(&box->_perm);
	(void)mailbox_get_permissions(box);
}

int mailbox_create_fd(struct mailbox *box, const char *path, int flags,
		      int *fd_r)
{
	const struct mailbox_permissions *perm = mailbox_get_permissions(box);
	mode_t old_mask;
	int fd;

	i_assert((flags & O_CREAT) != 0);

	*fd_r = -1;

	old_mask = umask(0);
	fd = open(path, flags, perm->file_create_mode);
	umask(old_mask);

	if (fd != -1) {
		/* ok */
	} else if (errno == EEXIST) {
		/* O_EXCL used, caller will handle this error */
		return 0;
	} else if (errno == ENOENT) {
		mailbox_set_deleted(box);
		return -1;
	} else if (errno == ENOTDIR) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			"Mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (mail_storage_set_error_from_errno(box->storage)) {
		return -1;
	} else {
		mailbox_set_critical(box, "open(%s, O_CREAT) failed: %m", path);
		return -1;
	}

	if (perm->file_create_gid != (gid_t)-1) {
		if (fchown(fd, (uid_t)-1, perm->file_create_gid) == 0) {
			/* ok */
		} else if (errno == EPERM) {
			mailbox_set_critical(box, "%s",
				eperm_error_get_chgrp("fchown", path,
					perm->file_create_gid,
					perm->file_create_gid_origin));
		} else {
			mailbox_set_critical(box,
				"fchown(%s) failed: %m", path);
		}
	}
	*fd_r = fd;
	return 1;
}

int mailbox_mkdir(struct mailbox *box, const char *path,
		  enum mailbox_list_path_type type)
{
	const struct mailbox_permissions *perm = mailbox_get_permissions(box);
	const char *root_dir;

	if (!perm->gid_origin_is_mailbox_path) {
		/* mailbox root directory doesn't exist, create it */
		root_dir = mailbox_list_get_root_forced(box->list, type);
		if (mailbox_list_mkdir_root(box->list, root_dir, type) < 0) {
			mail_storage_copy_list_error(box->storage, box->list);
			return -1;
		}
	}

	if (mkdir_parents_chgrp(path, perm->dir_create_mode,
				perm->file_create_gid,
				perm->file_create_gid_origin) == 0)
		return 1;
	else if (errno == EEXIST)
		return 0;
	else if (errno == ENOTDIR) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			"Mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (mail_storage_set_error_from_errno(box->storage)) {
		return -1;
	} else {
		mailbox_set_critical(box, "mkdir_parents(%s) failed: %m", path);
		return -1;
	}
}

int mailbox_create_missing_dir(struct mailbox *box,
			       enum mailbox_list_path_type type)
{
	const char *mail_dir, *dir;
	struct stat st;
	int ret;

	if ((ret = mailbox_get_path_to(box, type, &dir)) <= 0)
		return ret;
	if (mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_MAILBOX,
				&mail_dir) < 0)
		return -1;
	if (null_strcmp(dir, mail_dir) != 0) {
		/* Mailbox directory is different - create a missing dir */
	} else if ((box->list->props & MAILBOX_LIST_PROP_AUTOCREATE_DIRS) != 0) {
		/* This layout (e.g. imapc) wants to autocreate missing mailbox
		   directories as well. */
	} else {
		/* If the mailbox directory doesn't exist, the mailbox
		   shouldn't exist at all. So just assume that it's already
		   created and if there's a race condition just fail later. */
		return 0;
	}

	/* we call this function even when the directory exists, so first do a
	   quick check to see if we need to mkdir anything */
	if (stat(dir, &st) == 0)
		return 0;

	if ((box->storage->class_flags & MAIL_STORAGE_CLASS_FLAG_NO_ROOT) == 0 &&
	    null_strcmp(dir, mail_dir) != 0 && mail_dir != NULL &&
	    stat(mail_dir, &st) < 0 && (errno == ENOENT || errno == ENOTDIR)) {
		/* Race condition - mail root directory doesn't exist
		   anymore either. We shouldn't create this directory
		   anymore. */
		mailbox_set_deleted(box);
		return -1;
	}

	return mailbox_mkdir(box, dir, type);
}

unsigned int mail_storage_get_lock_timeout(struct mail_storage *storage,
					   unsigned int secs)
{
	return storage->set->mail_max_lock_timeout == 0 ? secs :
		I_MIN(secs, storage->set->mail_max_lock_timeout);
}

enum mail_index_open_flags
mail_storage_settings_to_index_flags(const struct mail_storage_settings *set)
{
	enum mail_index_open_flags index_flags = 0;

#ifndef MMAP_CONFLICTS_WRITE
	if (set->mmap_disable)
#endif
		index_flags |= MAIL_INDEX_OPEN_FLAG_MMAP_DISABLE;
	if (set->dotlock_use_excl)
		index_flags |= MAIL_INDEX_OPEN_FLAG_DOTLOCK_USE_EXCL;
	if (set->mail_nfs_index)
		index_flags |= MAIL_INDEX_OPEN_FLAG_NFS_FLUSH;
	return index_flags;
}

static void mailbox_settings_filters_add(struct event *event,
					 struct mailbox_list *list,
					 const char *vname)
{
	if (array_is_empty(&list->ns->set->mailboxes))
		return;

	const char *vname_without_prefix =
		mailbox_get_name_without_prefix(list->ns, vname);
	unsigned int i, count;
	const struct mailbox_settings *const *mailboxes =
		array_get(&list->ns->set->parsed_mailboxes, &count);

	for (i = 0; i < count; i++) {
		if (!wildcard_match(vname_without_prefix, mailboxes[i]->name))
			continue;

		const char *filter_name =
			array_idx_elem(&list->ns->set->mailboxes, i);
		settings_event_add_list_filter_name(event,
						    "mailbox", filter_name);
	}
}

struct event *
mail_storage_mailbox_create_event(struct event *parent,
				  struct mailbox_list *list, const char *vname)
{
	struct event *event = event_create(parent);
	event_add_category(event, &event_category_mailbox);

	mailbox_settings_filters_add(event, list, vname);
	event_add_str(event, "mailbox", vname);
	event_add_str(event, SETTINGS_EVENT_NAMESPACE_NAME, list->ns->set->name);
	settings_event_add_list_filter_name(event,
		SETTINGS_EVENT_NAMESPACE_NAME, list->ns->set->name);

	event_drop_parent_log_prefixes(event, 1);
	event_set_append_log_prefix(event, t_strdup_printf(
		"Mailbox %s: ", mailbox_name_sanitize(vname)));
	return event;
}

int mail_parse_human_timestamp(const char *str, time_t *timestamp_r,
			       bool *utc_r)
{
	struct tm tm;
	unsigned int secs;
	const char *error;
	int tz;

	if (i_isdigit(str[0]) && i_isdigit(str[1]) &&
	    i_isdigit(str[2]) && i_isdigit(str[3]) && str[4] == '-' &&
	    i_isdigit(str[5]) && i_isdigit(str[6]) && str[7] == '-' &&
	    i_isdigit(str[8]) && i_isdigit(str[9]) && str[10] == '\0') {
		/* yyyy-mm-dd */
		i_zero(&tm);
		tm.tm_year = (str[0]-'0') * 1000 + (str[1]-'0') * 100 +
			(str[2]-'0') * 10 + (str[3]-'0') - 1900;
		tm.tm_mon = (str[5]-'0') * 10 + (str[6]-'0') - 1;
		tm.tm_mday = (str[8]-'0') * 10 + (str[9]-'0');
		*timestamp_r = utc_mktime(&tm);
		*utc_r = TRUE;
		return 0;
	} else if (imap_parse_date(str, timestamp_r)) {
		/* imap datetime */
		*utc_r = FALSE;
		return 0;
	} else if (imap_parse_datetime(str, timestamp_r, &tz)) {
		/* imap datetime */
		*utc_r = TRUE;
		return 0;
	} else if (str_to_time(str, timestamp_r) == 0) {
		/* unix timestamp */
		*utc_r = TRUE;
		return 0;
	} else if (str_parse_get_interval(str, &secs, &error) == 0) {
		*timestamp_r = ioloop_time - secs;
		*utc_r = TRUE;
		return 0;
	} else {
		return -1;
	}
}

void mail_set_mail_cache_corrupted(struct mail *mail, const char *fmt, ...)
{
	struct mail_cache_view *cache_view =
		mail->transaction->cache_view;

	i_assert(cache_view != NULL);

	va_list va;
	va_start(va, fmt);

	T_BEGIN {
		mail_cache_set_seq_corrupted_reason(cache_view, mail->seq,
						    t_strdup_vprintf(fmt, va));
	} T_END;

	/* update also the storage's internal error */
	mailbox_set_index_error(mail->box);

	va_end(va);
}

static int
mail_storage_dotlock_create(const char *lock_path,
			    const struct file_create_settings *lock_set,
			    const struct mail_storage_settings *mail_set,
			    struct file_lock **lock_r, const char **error_r)
{
	const struct dotlock_settings dotlock_set = {
		.timeout = lock_set->lock_timeout_secs,
		.stale_timeout = I_MAX(60*5, lock_set->lock_timeout_secs),
		.lock_suffix = "",

		.use_excl_lock = mail_set->dotlock_use_excl,
		.nfs_flush = mail_set->mail_nfs_storage,
		.use_io_notify = TRUE,
	};
	struct dotlock *dotlock;
	int ret = file_dotlock_create(&dotlock_set, lock_path, 0, &dotlock);
	if (ret <= 0) {
		*error_r = t_strdup_printf("file_dotlock_create(%s) failed: %m",
					   lock_path);
		return ret;
	}
	*lock_r = file_lock_from_dotlock(&dotlock);
	return 1;
}

int mail_storage_lock_create(const char *lock_path,
			     const struct file_create_settings *lock_set,
			     const struct mail_storage_settings *mail_set,
			     struct file_lock **lock_r, const char **error_r)
{
	struct file_create_settings lock_set_new = *lock_set;
	bool created;

	if (lock_set->lock_settings.lock_method == FILE_LOCK_METHOD_DOTLOCK)
		return mail_storage_dotlock_create(lock_path, lock_set, mail_set, lock_r, error_r);

	lock_set_new.lock_settings.close_on_free = TRUE;
	lock_set_new.lock_settings.unlink_on_free = TRUE;
	if (file_create_locked(lock_path, &lock_set_new, lock_r,
			       &created, error_r) == -1) {
		*error_r = t_strdup_printf("file_create_locked(%s) failed: %s",
					   lock_path, *error_r);
		return errno == EAGAIN ? 0 : -1;
	}
	return 1;
}

int mailbox_lock_file_create(struct mailbox *box, const char *lock_fname,
			     unsigned int lock_secs, struct file_lock **lock_r,
			     const char **error_r)
{
	const struct mailbox_permissions *perm;
	struct file_create_settings set;
	const char *lock_path;

	perm = mailbox_get_permissions(box);
	i_zero(&set);
	set.lock_timeout_secs =
		mail_storage_get_lock_timeout(box->storage, lock_secs);
	set.lock_settings.lock_method = box->storage->set->parsed_lock_method;
	set.mode = perm->file_create_mode;
	set.gid = perm->file_create_gid;
	set.gid_origin = perm->file_create_gid_origin;

	if (box->list->mail_set->mail_volatile_path[0] == '\0')
		lock_path = t_strdup_printf("%s/%s", box->index->dir, lock_fname);
	else {
		unsigned char box_name_sha1[SHA1_RESULTLEN];
		string_t *str = t_str_new(128);

		/* Keep this simple: Use the lock_fname with a SHA1 of the
		   mailbox name as the suffix. The mailbox name itself could
		   be too large as a filename and creating the full directory
		   structure would be pretty troublesome. It would also make
		   it more difficult to perform the automated deletion of empty
		   lock directories. */
		str_printfa(str, "%s/%s.", box->list->mail_set->mail_volatile_path,
			    lock_fname);
		sha1_get_digest(box->name, strlen(box->name), box_name_sha1);
		binary_to_hex_append(str, box_name_sha1, sizeof(box_name_sha1));
		lock_path = str_c(str);
		set.mkdir_mode = 0700;
	}

	return mail_storage_lock_create(lock_path, &set,
					box->storage->set, lock_r, error_r);
}

void mailbox_sync_notify(struct mailbox *box, uint32_t uid,
			 enum mailbox_sync_type sync_type)
{
	if (box->v.sync_notify != NULL) T_BEGIN {
		box->v.sync_notify(box, uid, sync_type);
	} T_END;

	/* Send an event for expunged mail. */
	if (sync_type == MAILBOX_SYNC_TYPE_EXPUNGE) {
		e_debug(event_create_passthrough(box->event)->
			set_name("mail_expunged")->
			add_int("uid", uid)->event(),
			"UID %u: Mail expunged", uid);
	}
}

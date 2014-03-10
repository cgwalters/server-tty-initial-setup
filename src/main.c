/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <libgsystem.h>
#include <glib-unix.h>
#include <unistd.h>

#include <glib/gi18n.h>

static GHashTable *
parse_os_release (const char *contents,
                  const char *split)
{
  char **lines = g_strsplit (contents, split, -1);
  char **iter;
  GHashTable *ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (iter = lines; *iter; iter++)
    {
      char *line = *iter;
      char *eq;
      const char *quotedval;
      char *val;

      if (g_str_has_prefix (line, "#"))
        continue;
      
      eq = strchr (line, '=');
      if (!eq)
        continue;
      
      *eq = '\0';
      quotedval = eq + 1;
      val = g_shell_unquote (quotedval, NULL);
      if (!val)
        continue;
      
      g_hash_table_insert (ret, line, val);
    }

  return ret;
}

static gboolean
do_initial_setup (GCancellable     *cancellable,
                  GError          **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *etc_os_release =
    g_file_new_for_path ("/etc/os-release");
  gs_free char *contents = NULL;
  gs_unref_hashtable GHashTable *osrelease_values = NULL;
  const char *pretty_name;
  const char *passwd_root_argv[] = { "passwd", "root", NULL };
  gsize len;
  guint i;
  const guint max_passwd_attempts = 5;

  if (!g_file_load_contents (etc_os_release, cancellable,
                             &contents, &len, NULL, error))
    {
      g_prefix_error (error, "Reading /etc/os-release: ");
      goto out;
    }

  osrelease_values = parse_os_release (contents, "\n");
  pretty_name = g_hash_table_lookup (osrelease_values, "PRETTY_NAME");
  if (!pretty_name)
    pretty_name = g_hash_table_lookup (osrelease_values, "ID");
  if (!pretty_name)
    pretty_name = " (/etc/os-release not found)";

  g_print (_("Welcome to %s"), pretty_name);
  g_print ("\n");

  g_print (_("No user accounts found, and root password is locked"));
  g_print ("\n");

  for (i = 0; i < max_passwd_attempts; i++)
    {
      if (!g_spawn_sync (NULL, passwd_root_argv, NULL,
                         G_SPAWN_CHILD_INHERITS_STDIN,
                         NULL, NULL, NULL, NULL,
                         &estatus, error))
        goto out;

      if (g_spawn_check_exit_status (estatus, NULL))
        break;
    }
  if (i == max_passwd_attempts)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to run \"passwd root\" after %u attempts",
                   max_passwd_attempts);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

int
main (int    argc,
      char **argv)
{
  GError *local_error = NULL;
  GError **error = NULL;
  GCancellable *cancellable = NULL;
  gs_unref_variant GVariant *user_list = NULL;
  gs_unref_variant GVariant *root_account_reply = NULL;
  gs_unref_object GDBusProxy *accountsservice = NULL;
  gs_unref_object GDBusProxy *root_account_properties = NULL;
  gs_unref_variant GVariant *root_account_locked_reply = NULL;
  gs_unref_variant GVariant *root_account_locked_property = NULL;
  gs_unref_variant GVariant *root_account_locked_value = NULL;
  const char *root_account_path = NULL;
  gboolean have_user_accounts;
  gboolean root_is_locked;

  bindtextdomain (PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (PACKAGE, "UTF-8");
  textdomain (PACKAGE);
  
  accountsservice = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, NULL,
                                                   "org.freedesktop.Accounts",
                                                   "/org/freedesktop/Accounts",
                                                   "org.freedesktop.Accounts",
                                                   cancellable,
                                                   error);
  if (!accountsservice)
    goto out;

  user_list = g_dbus_proxy_call_sync (accountsservice, "ListCachedUsers",
                                      NULL, 0, -1, cancellable, error);
  if (!reply)
    goto out;
  
  have_user_accounts = g_variant_n_children (user_list) > 0;
  
  root_account_reply = g_dbus_proxy_call_sync (accountsservice,
                                               "FindUserById",
                                               g_variant_new ("(t)", (guint64)0),
                                               0, -1,
                                               cancellable, error);
  if (!root_account_reply)
    goto out;
  if (!g_variant_is_of_type (root_account_reply, G_VARIANT_TYPE ("(o)")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected return type from FindUserById");
      goto out;
    }
  g_variant_get (root_account_reply, "(&o)", &root_account_path);

  root_account_properties =
    g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, NULL,
                                   g_dbus_proxy_get_name_owner (accountsservice),
                                   root_account_path,
                                   "org.freedesktop.DBus.Properties",
                                   cancellable,
                                   error);
  if (!root_account_properties)
    goto out;
  
  root_account_locked_reply =
    g_dbus_proxy_call_sync (root_account_properties,
                            "Get", g_variant_new ("(ss)", "org.freedesktop.Accounts.User", "Locked"),
                            0, -1, cancellable, error);
  if (!root_account_locked_reply)
    goto out;
  if (!g_variant_is_of_type (root_account_locked_reply, G_VARIANT_TYPE ("(v)")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected return type from property Get");
      goto out;
    }
  g_variant_get (root_account_locked_reply, "(v)", &root_account_locked_value);
  root_account_locked_property = g_variant_get_variant (root_account_locked_value);
  if (!g_variant_is_of_type (root_account_locked_property, G_VARIANT_TYPE ("b")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unexpected non-boolean value for Locked");
      goto out;
    }
  
  root_is_locked = g_variant_get_boolean (root_account_locked_property);

  if (!(have_user_accounts && root_is_locked))
    {
      if (!do_initial_setup (cancellable, error))
        goto out;
    }

 out:
  if (local_error != NULL)
    {
      g_printerr ("error: %s\n", local_error->message);
      g_error_free (local_error);
      return 1;
    }
  return 0;
}

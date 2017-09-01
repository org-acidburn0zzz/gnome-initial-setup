/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include "gnome-initial-setup.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <gdm/gdm-client.h>

#include "cc-common-language.h"
#include "gis-assistant.h"
#include "gis-page-util.h"

#define GIS_TYPE_DRIVER_MODE (gis_driver_mode_get_type ())

/* Statically include this for now. Maybe later
 * we'll generate this from glib-mkenums. */
GType
gis_driver_mode_get_type (void) {
  static GType enum_type_id = 0;
  if (G_UNLIKELY (!enum_type_id))
    {
      static const GEnumValue values[] = {
        { GIS_DRIVER_MODE_NEW_USER, "GIS_DRIVER_MODE_NEW_USER", "new_user" },
        { GIS_DRIVER_MODE_EXISTING_USER, "GIS_DRIVER_MODE_EXISTING_USER", "existing_user" },
        { 0, NULL, NULL }
      };
      enum_type_id = g_enum_register_static("GisDriverMode", values);
    }
  return enum_type_id;
}

enum {
  REBUILD_PAGES,
  LOCALE_CHANGED,
  LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL];

enum {
  PROP_0,
  PROP_DEMO_MODE,
  PROP_LIVE_SESSION,
  PROP_LIVE_DVD,
  PROP_MODE,
  PROP_USERNAME,
  PROP_PASSWORDLESS,
  PROP_SMALL_SCREEN,
  PROP_LAST,
};

static GParamSpec *obj_props[PROP_LAST];

struct _GisDriverPrivate {
  GtkWindow *main_window;
  GisAssistant *assistant;

  GdmClient *client;
  GdmGreeter *greeter;
  GdmUserVerifier *user_verifier;

  ActUser *user_account;
  gchar *user_password;

  gchar *lang_id;
  gchar *username;
  gboolean passwordless;

  gboolean is_live_session;
  gboolean is_live_dvd;
  gboolean is_in_demo_mode;
  gboolean show_demo_mode;

  GisDriverMode mode;
  UmAccountMode account_mode;
  gboolean small_screen;

  locale_t locale;

  GKeyFile *vendor_conf_file;

  gchar *product_name;
};
typedef struct _GisDriverPrivate GisDriverPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GisDriver, gis_driver, GTK_TYPE_APPLICATION)

/* Should be kept in sync with eos-installer */
static gboolean
check_for_live_boot (gchar **uuid)
{
  const gchar *force = NULL;
  GError *error = NULL;
  g_autofree gchar *cmdline = NULL;
  gboolean live_boot = FALSE;
  g_autoptr(GRegex) reg = NULL;
  g_autoptr(GMatchInfo) info = NULL;

  force = g_getenv ("EI_FORCE_LIVE_BOOT_UUID");
  if (force != NULL && *force != '\0')
    {
      if (uuid != NULL)
        *uuid = g_strdup (force);
      return TRUE;
    }

  if (!g_file_get_contents ("/proc/cmdline", &cmdline, NULL, &error))
    {
      g_error_free (error);
      return FALSE;
    }

  live_boot = g_regex_match_simple ("\\bendless\\.live_boot\\b", cmdline, 0, 0);

  if (uuid != NULL)
    {
      reg = g_regex_new ("\\bendless\\.image\\.device=UUID=([^\\s]*)", 0, 0, NULL);
      g_regex_match (reg, cmdline, 0, &info);
      if (g_match_info_matches (info))
        *uuid = g_match_info_fetch (info, 1);
    }

  return live_boot;
}

static void
check_live_session (GisDriver   *driver,
                    const gchar *image_version)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (check_for_live_boot (NULL))
    {
      priv->is_live_session = TRUE;
      priv->is_live_dvd = image_version != NULL &&
        g_str_has_prefix (image_version, "eosdvd-");
    }
  else
    {
      priv->is_live_session = FALSE;
      priv->is_live_dvd = FALSE;
    }
}

#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

static char *
get_image_version (void)
{
  g_autoptr(GError) error_sysroot = NULL;
  g_autoptr(GError) error_root = NULL;
  char *image_version =
    gis_page_util_get_image_version (EOS_IMAGE_VERSION_PATH, &error_sysroot);

  if (image_version == NULL)
    image_version =
      gis_page_util_get_image_version (EOS_IMAGE_VERSION_ALT_PATH, &error_root);

  if (image_version == NULL)
    {
      g_warning ("%s", error_sysroot->message);
      g_warning ("%s", error_root->message);
    }

  return image_version;
}

static gboolean
image_supports_demo_mode (const gchar *image_version)
{
  return image_version != NULL && (
      g_str_has_prefix (image_version, "eosnonfree-") ||
      g_str_has_prefix (image_version, "eosoem-"));
}

static gchar *
get_product_from_image_version (const gchar *image_version)
{
  gchar *hyphen_index = NULL;

  if (image_version == NULL)
    return NULL;

  hyphen_index = index (image_version, '-');
  if (hyphen_index == NULL)
    return NULL;

  return g_strndup (image_version, hyphen_index - image_version);
}

const gchar *
gis_driver_get_product_name (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->product_name;
}

static void
gis_driver_dispose (GObject *object)
{
  GisDriver *driver = GIS_DRIVER (object);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  g_clear_object (&priv->user_verifier);
  g_clear_object (&priv->greeter);
  g_clear_object (&priv->client);

  G_OBJECT_CLASS (gis_driver_parent_class)->dispose (object);
}

static void
gis_driver_finalize (GObject *object)
{
  GisDriver *driver = GIS_DRIVER (object);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  g_free (priv->product_name);
  g_free (priv->lang_id);
  g_free (priv->username);
  g_free (priv->user_password);

  g_clear_object (&priv->user_account);
  g_clear_pointer (&priv->vendor_conf_file, g_key_file_free);

  if (priv->locale != (locale_t) 0)
    {
      uselocale (LC_GLOBAL_LOCALE);
      freelocale (priv->locale);
    }

  G_OBJECT_CLASS (gis_driver_parent_class)->finalize (object);
}

static void
assistant_page_changed (GtkScrolledWindow *sw)
{
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (sw), 0);
}

static void
prepare_main_window (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  GtkWidget *child, *sw;

  child = g_object_ref (gtk_bin_get_child (GTK_BIN (priv->main_window)));
  gtk_container_remove (GTK_CONTAINER (priv->main_window), child);
  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_show (sw);
  gtk_container_add (GTK_CONTAINER (priv->main_window), sw);
  gtk_container_add (GTK_CONTAINER (sw), child);
  g_object_unref (child);

  g_signal_connect_swapped (priv->assistant,
                            "page-changed",
                            G_CALLBACK (assistant_page_changed),
                            sw);

  gtk_window_set_titlebar (priv->main_window,
                           gis_assistant_get_titlebar (priv->assistant));
}

static void
rebuild_pages (GisDriver *driver)
{
  g_signal_emit (G_OBJECT (driver), signals[REBUILD_PAGES], 0);
}

GisAssistant *
gis_driver_get_assistant (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->assistant;
}

static void
gis_driver_real_locale_changed (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  GtkTextDirection direction;

  direction = gtk_get_locale_direction ();
  gtk_widget_set_default_direction (direction);

  rebuild_pages (driver);
  gis_assistant_locale_changed (priv->assistant);
}

static void
gis_driver_locale_changed (GisDriver *driver)
{
  g_signal_emit (G_OBJECT (driver), signals[LOCALE_CHANGED], 0);
}

void
gis_driver_set_user_language (GisDriver *driver, const gchar *lang_id, gboolean update_locale)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  g_free (priv->lang_id);
  priv->lang_id = g_strdup (lang_id);

  cc_common_language_set_current_language (lang_id);

  if (update_locale)
    {
      locale_t locale = newlocale (LC_MESSAGES_MASK, lang_id, (locale_t) 0);
      if (locale == (locale_t) 0)
        {
          g_warning ("Failed to create locale %s: %s", lang_id, g_strerror (errno));
          return;
        }

      uselocale (locale);

      if (priv->locale != (locale_t) 0 && priv->locale != LC_GLOBAL_LOCALE)
        freelocale (priv->locale);
      priv->locale = locale;

      gis_driver_locale_changed (driver);
    }

  /* Now, if we already have a user configured, make sure to
   * propagate that change to the user */
  if (priv->user_account)
    act_user_set_language (priv->user_account, lang_id);
}

const gchar *
gis_driver_get_user_language (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->lang_id;
}

void
gis_driver_set_username (GisDriver *driver, const gchar *username)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  g_free (priv->username);
  priv->username = g_strdup (username);
  g_object_notify (G_OBJECT (driver), "username");
}

const gchar *
gis_driver_get_username (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->username;
}

void
gis_driver_set_passwordless (GisDriver *self,
                             gboolean   passwordless)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (self);
  gboolean should_notify = (priv->passwordless != passwordless);
  priv->passwordless = passwordless;
  if (should_notify)
    g_object_notify (G_OBJECT (self), "passwordless");
}

gboolean
gis_driver_get_passwordless (GisDriver *self)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (self);
  return priv->passwordless;
}

void
gis_driver_set_user_permissions (GisDriver   *driver,
                                 ActUser     *user,
                                 const gchar *password)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  g_set_object (&priv->user_account, user);
  priv->user_password = g_strdup (password);
}

void
gis_driver_get_user_permissions (GisDriver    *driver,
                                 ActUser     **user,
                                 const gchar **password)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  *user = priv->user_account;
  *password = priv->user_password;
}

void
gis_driver_set_account_mode (GisDriver     *driver,
                             UmAccountMode  mode)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  priv->account_mode = mode;
}

UmAccountMode
gis_driver_get_account_mode (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->account_mode;
}

gboolean
gis_driver_get_gdm_objects (GisDriver        *driver,
                            GdmGreeter      **greeter,
                            GdmUserVerifier **user_verifier)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (priv->greeter == NULL || priv->user_verifier == NULL)
    return FALSE;

  *greeter = priv->greeter;
  *user_verifier = priv->user_verifier;

  return TRUE;
}

void
gis_driver_add_page (GisDriver *driver,
                     GisPage   *page)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  gis_assistant_add_page (priv->assistant, page);
}

void
gis_driver_show_window (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  gtk_window_present (priv->main_window);
}

void
gis_driver_hide_window (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  gtk_widget_hide (GTK_WIDGET (priv->main_window));
}

static void
load_vendor_conf_file (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) vendor_conf_file = g_key_file_new ();

  if(!g_key_file_load_from_file (vendor_conf_file, VENDOR_CONF_FILE,
                                 G_KEY_FILE_NONE, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Could not read file %s: %s", VENDOR_CONF_FILE, error->message);
      return;
    }

  priv->vendor_conf_file = g_steal_pointer (&vendor_conf_file);
}

static void
report_conf_error_if_needed (const gchar *group,
                             const gchar *key,
                             const GError *error)
{
  if (!g_error_matches (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
      !g_error_matches (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND))
    g_warning ("Error getting the value for key '%s' of group [%s] in "
               "%s: %s", group, key, VENDOR_CONF_FILE, error->message);
}

gboolean
gis_driver_conf_get_boolean (GisDriver *driver,
                             const gchar *group,
                             const gchar *key,
                             gboolean default_value)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (priv->vendor_conf_file) {
    g_autoptr(GError) error = NULL;
    gboolean new_value = g_key_file_get_boolean (priv->vendor_conf_file, group,
                                                 key, &error);
    if (error == NULL)
      return new_value;

    report_conf_error_if_needed (group, key, error);
  }

  return default_value;
}

GStrv
gis_driver_conf_get_string_list (GisDriver *driver,
                                 const gchar *group,
                                 const gchar *key,
                                 gsize *out_length)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (priv->vendor_conf_file) {
    g_autoptr(GError) error = NULL;
    GStrv new_value = g_key_file_get_string_list (priv->vendor_conf_file, group,
                                                  key, out_length, &error);
    if (error == NULL)
      return new_value;

    report_conf_error_if_needed (group, key, error);
  }

  return NULL;
}

gchar *
gis_driver_conf_get_string (GisDriver *driver,
                            const gchar *group,
                            const gchar *key)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (priv->vendor_conf_file) {
    g_autoptr(GError) error = NULL;
    gchar *new_value = g_key_file_get_string (priv->vendor_conf_file, group,
                                              key, &error);
    if (error == NULL)
      return new_value;

    report_conf_error_if_needed (group, key, error);
  }

  return NULL;
}

GisDriverMode
gis_driver_get_mode (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->mode;
}

gboolean
gis_driver_is_in_demo_mode (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->is_in_demo_mode;
}

#define DEMO_ACCOUNT_USERNAME "demo-guest"
#define DEMO_ACCOUNT_FULLNAME "Demo"
#define DEMO_ACCOUNT_AVATAR "/usr/share/pixmaps/faces/sunflower.jpg"

static void
handle_demo_mode_error (GError *error)
{
  GtkWidget *dialog;

  g_warning ("Failed to enter demo mode: %s", error->message);
  dialog = gtk_message_dialog_new (NULL,
                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE,
                                   _("Failed to enter demo mode: %s"),
                                   error->message);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
  g_error_free (error);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ActUser, g_object_unref)

static gboolean
create_demo_user (GisDriver *driver, GError **error)
{
  g_autoptr(ActUser) user;
  const gchar *language;

  user = act_user_manager_create_user (act_user_manager_get_default (),
                                       DEMO_ACCOUNT_USERNAME,
                                       DEMO_ACCOUNT_FULLNAME,
                                       ACT_USER_ACCOUNT_TYPE_STANDARD,
                                       error);
  if (!user)
    return FALSE;

  act_user_set_password_mode (user, ACT_USER_PASSWORD_MODE_NONE);
  act_user_set_automatic_login (user, TRUE);
  act_user_set_icon_file (user, DEMO_ACCOUNT_AVATAR);

  language = gis_driver_get_user_language (driver);

  if (language)
    act_user_set_language (user, language);

  gis_driver_set_user_permissions (driver, user, NULL);
  gis_update_login_keyring_password ("");

  return TRUE;
}

static gboolean
setup_demo_config (GisDriver *driver, GError **error)
{
  gchar *stamp_file = g_build_filename (g_get_user_config_dir (),
                                        "eos-demo-mode",
                                        NULL);

  if (!g_file_set_contents (stamp_file, "1", sizeof(char), error))
    {
      g_free (stamp_file);
      return FALSE;
    }

  g_free (stamp_file);
  return TRUE;
}

void
gis_driver_enter_demo_mode (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  if (priv->is_in_demo_mode)
    return;

  GError *error = NULL;
  if (!gis_pkexec (LIBEXECDIR "/eos-demo-mode", DEMO_ACCOUNT_USERNAME, NULL, &error))
    {
      handle_demo_mode_error (error);
      return;
    }

  if (!create_demo_user (driver, &error))
    {
      handle_demo_mode_error (error);
      return;
    }

  if (!setup_demo_config (driver, &error))
    {
      handle_demo_mode_error (error);
      return;
    }

  priv->is_in_demo_mode = TRUE;

  /* Set up the demo user account, destroying it if necessary */
  gis_driver_set_username (driver, DEMO_ACCOUNT_USERNAME);

  rebuild_pages (driver);

  /* Notify anyone interested that we are in demo mode now */
  g_object_notify_by_pspec (G_OBJECT (driver), obj_props[PROP_DEMO_MODE]);
}

gboolean
gis_driver_get_supports_demo_mode (GisDriver *driver)
{
  return !gis_driver_is_live_session (driver);
}

gboolean
gis_driver_get_show_demo_mode (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  return priv->show_demo_mode && !priv->is_in_demo_mode;
}

gboolean
gis_driver_is_live_session (GisDriver *driver)
{
    GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
    return priv->is_live_session;
}

gboolean
gis_driver_is_small_screen (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  return priv->small_screen;
}

static gboolean
monitor_is_small (GdkMonitor *monitor)
{
  GdkRectangle geom;

  if (g_getenv ("GIS_SMALL_SCREEN"))
    return TRUE;

  gdk_monitor_get_geometry (monitor, &geom);
  return geom.height < 800;
}

static void
gis_driver_get_property (GObject      *object,
                         guint         prop_id,
                         GValue       *value,
                         GParamSpec   *pspec)
{
  GisDriver *driver = GIS_DRIVER (object);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  switch (prop_id)
    {
    case PROP_DEMO_MODE:
      g_value_set_boolean (value, priv->is_in_demo_mode);
      break;
    case PROP_LIVE_SESSION:
      g_value_set_boolean (value, priv->is_live_session);
      break;
    case PROP_LIVE_DVD:
      g_value_set_boolean (value, priv->is_live_dvd);
      break;
    case PROP_MODE:
      g_value_set_enum (value, priv->mode);
      break;
    case PROP_USERNAME:
      g_value_set_string (value, priv->username);
      break;
    case PROP_PASSWORDLESS:
      g_value_set_boolean (value, priv->passwordless);
      break;
    case PROP_SMALL_SCREEN:
      g_value_set_boolean (value, priv->small_screen);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gis_driver_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  GisDriver *driver = GIS_DRIVER (object);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  switch (prop_id)
    {
    case PROP_MODE:
      priv->mode = g_value_get_enum (value);
      break;
    case PROP_USERNAME:
      g_free (priv->username);
      priv->username = g_value_dup_string (value);
      break;
    case PROP_PASSWORDLESS:
      priv->passwordless = g_value_get_boolean (value);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gis_driver_activate (GApplication *app)
{
  GisDriver *driver = GIS_DRIVER (app);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);

  G_APPLICATION_CLASS (gis_driver_parent_class)->activate (app);

  gtk_window_present (GTK_WINDOW (priv->main_window));
}

static gboolean
maximize (gpointer data)
{
  GtkWindow *window = data;

  gtk_window_maximize (window);
  gtk_window_present (window);

  return G_SOURCE_REMOVE;
}

static gboolean
unmaximize (gpointer data)
{
  GtkWindow *window = data;

  gtk_window_unmaximize (window);
  gtk_window_present (window);

  return G_SOURCE_REMOVE;
}

static void
set_small_screen_based_on_primary_monitor (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  GdkDisplay *default_display = gdk_display_get_default ();
  GdkMonitor *primary_monitor = gdk_display_get_primary_monitor (default_display);

  priv->small_screen = monitor_is_small (primary_monitor);
}

/* Recompute priv->small_screen based on the monitor where the window is
 * located, if the window is actually realized. If not, recompute it based on
 * the primary monitor of the default display. */
static void
recompute_small_screen (GisDriver *driver) {
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  GdkWindow *window;
  GdkDisplay *default_display = gdk_display_get_default ();
  GdkMonitor *active_monitor;
  gboolean old_value = priv->small_screen;

  if (!gtk_widget_get_realized (GTK_WIDGET (priv->main_window)))
    {
      set_small_screen_based_on_primary_monitor (driver);
    }
  else
    {
      window = gtk_widget_get_window (GTK_WIDGET (priv->main_window));
      active_monitor = gdk_display_get_monitor_at_window (default_display, window);
      priv->small_screen = monitor_is_small (active_monitor);
    }

  if (priv->small_screen != old_value)
    g_object_notify (G_OBJECT (driver), "small-screen");
}

static void
update_screen_size (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  GdkWindow *window;
  GdkGeometry size_hints;
  GtkWidget *sw;

  recompute_small_screen (driver);

  if (!gtk_widget_get_realized (GTK_WIDGET (priv->main_window)))
    return;

  sw = gtk_bin_get_child (GTK_BIN (priv->main_window));
  window = gtk_widget_get_window (GTK_WIDGET (priv->main_window));

  if (priv->small_screen)
    {
      if (window)
        gdk_window_set_functions (window,
                                  GDK_FUNC_ALL | GDK_FUNC_MINIMIZE | GDK_FUNC_CLOSE);

      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
                                      GTK_POLICY_AUTOMATIC,
                                      GTK_POLICY_AUTOMATIC);

      gtk_window_set_geometry_hints (priv->main_window, NULL, NULL, 0);
      gtk_window_set_resizable (priv->main_window, TRUE);
      gtk_window_set_position (priv->main_window, GTK_WIN_POS_NONE);

      g_idle_add (maximize, priv->main_window);
    }
  else
    {
      if (window)
        gdk_window_set_functions (window,
                                  GDK_FUNC_ALL | GDK_FUNC_MINIMIZE | GDK_FUNC_CLOSE |
                                  GDK_FUNC_RESIZE | GDK_FUNC_MOVE | GDK_FUNC_MAXIMIZE);

      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
                                      GTK_POLICY_NEVER,
                                      GTK_POLICY_NEVER);

      size_hints.min_width = size_hints.max_width = 1024;
      size_hints.min_height = size_hints.max_height = 768;
      size_hints.win_gravity = GDK_GRAVITY_CENTER;

      gtk_window_set_geometry_hints (priv->main_window,
                                     NULL,
                                     &size_hints,
                                     GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE | GDK_HINT_WIN_GRAVITY);
      gtk_window_set_resizable (priv->main_window, FALSE);
      gtk_window_set_position (priv->main_window, GTK_WIN_POS_CENTER_ALWAYS);

      g_idle_add (unmaximize, priv->main_window);
    }
}

static void
screen_size_changed (GdkScreen *screen, GisDriver *driver)
{
  update_screen_size (driver);
}

static void
window_realize_cb (GtkWidget *widget, gpointer user_data)
{
  update_screen_size (GIS_DRIVER (user_data));
}

static void
connect_to_gdm (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  g_autoptr(GError) error = NULL;

  priv->client = gdm_client_new ();

  priv->greeter = gdm_client_get_greeter_sync (priv->client, NULL, &error);
  if (error == NULL)
    priv->user_verifier = gdm_client_get_user_verifier_sync (priv->client, NULL, &error);

  if (error != NULL) {
    g_warning ("Failed to open connection to GDM: %s", error->message);
    g_clear_object (&priv->user_verifier);
    g_clear_object (&priv->greeter);
    g_clear_object (&priv->client);
  }
}

static void
gis_driver_startup (GApplication *app)
{
  GisDriver *driver = GIS_DRIVER (app);
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  char *image_version = NULL;

  G_APPLICATION_CLASS (gis_driver_parent_class)->startup (app);

  if (priv->mode == GIS_DRIVER_MODE_NEW_USER)
    connect_to_gdm (driver);

  priv->main_window = g_object_new (GTK_TYPE_APPLICATION_WINDOW,
                                    "application", app,
                                    "type", GTK_WINDOW_TOPLEVEL,
                                    "icon-name", "preferences-system",
                                    "deletable", FALSE,
                                    NULL);

  gtk_application_inhibit (GTK_APPLICATION (app), priv->main_window,
                           GTK_APPLICATION_INHIBIT_IDLE,
                           "Should not be idle on first boot.");

  g_signal_connect (priv->main_window,
                    "realize",
                    G_CALLBACK (window_realize_cb),
                    (gpointer)app);

  priv->assistant = g_object_new (GIS_TYPE_ASSISTANT, NULL);
  gtk_container_add (GTK_CONTAINER (priv->main_window), GTK_WIDGET (priv->assistant));

  gtk_widget_show (GTK_WIDGET (priv->assistant));

  image_version = get_image_version ();
  priv->product_name = get_product_from_image_version (image_version);

  check_live_session (driver, image_version);
  g_object_notify_by_pspec (G_OBJECT (driver), obj_props[PROP_LIVE_SESSION]);
  g_object_notify_by_pspec (G_OBJECT (driver), obj_props[PROP_LIVE_DVD]);

  priv->show_demo_mode = !priv->is_live_session && image_supports_demo_mode (image_version);

  gis_driver_set_user_language (driver, setlocale (LC_MESSAGES, NULL), FALSE);

  prepare_main_window (driver);
  rebuild_pages (driver);

  g_clear_pointer (&image_version, g_free);
}

static void
gis_driver_init (GisDriver *driver)
{
  GdkScreen *screen;

  screen = gdk_screen_get_default ();

  set_small_screen_based_on_primary_monitor (driver);

  load_vendor_conf_file (driver);

  g_signal_connect (screen, "size-changed",
                    G_CALLBACK (screen_size_changed), driver);
}

static void
gis_driver_class_init (GisDriverClass *klass)
{
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gis_driver_get_property;
  gobject_class->set_property = gis_driver_set_property;
  gobject_class->dispose = gis_driver_dispose;
  gobject_class->finalize = gis_driver_finalize;
  application_class->startup = gis_driver_startup;
  application_class->activate = gis_driver_activate;
  klass->locale_changed = gis_driver_real_locale_changed;

  signals[REBUILD_PAGES] =
    g_signal_new ("rebuild-pages",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GisDriverClass, rebuild_pages),
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[LOCALE_CHANGED] =
    g_signal_new ("locale-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GisDriverClass, locale_changed),
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  obj_props[PROP_DEMO_MODE] =
    g_param_spec_boolean ("demo-mode", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_LIVE_SESSION] =
    g_param_spec_boolean ("live-session", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_LIVE_DVD] =
    g_param_spec_boolean ("live-dvd", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_MODE] =
    g_param_spec_enum ("mode", "", "",
                       GIS_TYPE_DRIVER_MODE,
                       GIS_DRIVER_MODE_EXISTING_USER,
                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_USERNAME] =
    g_param_spec_string ("username", "", "",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_PASSWORDLESS] =
    g_param_spec_boolean ("passwordless", "", "User account is passwordless",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_props[PROP_SMALL_SCREEN] =
    g_param_spec_boolean ("small-screen", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}

void
gis_driver_save_data (GisDriver *driver)
{
  GisDriverPrivate *priv = gis_driver_get_instance_private (driver);
  gis_assistant_save_data (priv->assistant);
}

GisDriver *
gis_driver_new (GisDriverMode mode)
{
  return g_object_new (GIS_TYPE_DRIVER,
                       "application-id", "org.gnome.InitialSetup",
                       "mode", mode,
                       NULL);
}

/* gis-live-chooser-page.c
 *
 * Copyright (C) 2016 Endless Mobile, Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Written by:
 *     Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 */

#include "gis-live-chooser-page.h"
#include "live-chooser-resources.h"

#include <act/act-user-manager.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <polkit/polkit.h>

#include <locale.h>

#include "pages/language/cc-language-chooser.h"


#define LIVE_ACCOUNT_USERNAME "live"
#define LIVE_ACCOUNT_FULLNAME "Endless OS"

struct _GisLiveChooserPagePrivate
{
  GtkWidget *try_label;
  GtkWidget *try_label_dvd;
  GtkWidget *try_icon;
  GtkWidget *try_icon_dvd;
  GtkWidget *reformat_label;
  GtkWidget *try_button;
  GtkWidget *reformat_button;

  ActUserManager   *act_client;
};

typedef struct _GisLiveChooserPagePrivate GisLiveChooserPagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GisLiveChooserPage, gis_live_chooser_page, GIS_TYPE_PAGE);

static void
gis_live_chooser_page_save_data (GisPage *page)
{
  GisLiveChooserPage *self = GIS_LIVE_CHOOSER_PAGE (page);
  GisLiveChooserPagePrivate *priv = gis_live_chooser_page_get_instance_private (self);
  ActUser *user;
  g_autoptr(GError) error = NULL;
  const gchar *language;

  error = NULL;
  user = act_user_manager_create_user (priv->act_client,
                                       LIVE_ACCOUNT_USERNAME,
                                       LIVE_ACCOUNT_FULLNAME,
                                       ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR,
                                       &error);
  if (error)
    {
      g_warning ("Failed to create live user: %s", error->message);
      return;
    }

  act_user_set_password_mode (user, ACT_USER_PASSWORD_MODE_NONE);
  act_user_set_automatic_login (user, FALSE);
  act_user_set_icon_file (user, AVATAR_IMAGE_DEFAULT);

  language = gis_driver_get_user_language (page->driver);

  if (language)
    act_user_set_language (user, language);

  gis_driver_set_user_permissions (page->driver, user, NULL);

  gis_update_login_keyring_password ("");

  g_object_unref (user);
}

static void
load_css_overrides (GisLiveChooserPage *page)
{
  GtkCssProvider *provider;
  GError *error;

  error = NULL;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (provider, "/org/gnome/initial-setup/live-chooser.css");

  if (error != NULL)
    {
      g_warning ("Unable to load CSS overrides for the live-chooser page: %s", error->message);
      g_clear_error (&error);
    }
  else
    {
      gtk_style_context_add_provider_for_screen (gtk_widget_get_screen (GTK_WIDGET (page)),
                                                 GTK_STYLE_PROVIDER (provider),
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

  g_object_unref (provider);
}

static void
try_button_clicked (GisLiveChooserPage *page)
{
  GisAssistant *assistant = gis_driver_get_assistant (GIS_PAGE (page)->driver);

  gis_assistant_next_page (assistant);
}

static void
on_reformatter_exited (GisLiveChooserPage *page,
                       const GError       *error)
{
  GisDriver *driver = GIS_PAGE (page)->driver;

  gis_driver_show_window (driver);

  if (gis_driver_is_reformatter (driver))
    gis_assistant_previous_page (gis_driver_get_assistant (driver));

  if (error != NULL)
    {
      GtkWindow *toplevel = GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (page)));
      GtkWidget *message_dialog;

      g_critical ("Error running the reformatter: %s", error->message);

      message_dialog = gtk_message_dialog_new (toplevel,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_CLOSE,
                                               _("Error running the reformatter: %s"), error->message);

      gtk_dialog_run (GTK_DIALOG (message_dialog));
      gtk_widget_destroy (message_dialog);
    }
}

static void
reformatter_exited_cb (GPid     pid,
                       gint     status,
                       gpointer user_data)
{
  GisLiveChooserPage *page = user_data;
  g_autoptr(GError) error = NULL;

  g_spawn_check_exit_status (status, &error);
  on_reformatter_exited (page, error);
}

static void
gis_live_chooser_page_launch_reformatter (GisLiveChooserPage *page)
{
  g_autoptr(GError) error = NULL;
  GPid pid;

  gchar* command[] = { "gnome-image-installer", NULL };

  g_spawn_async ("/usr/lib/eos-installer",
                 (gchar**) command,
                 NULL,
                 G_SPAWN_DO_NOT_REAP_CHILD,
                 NULL,
                 NULL,
                 &pid,
                 &error);

  if (error)
    {
      on_reformatter_exited (page, error);
    }
  else
    {
      gis_driver_hide_window (GIS_PAGE (page)->driver);
      g_child_watch_add (pid, reformatter_exited_cb, page);
    }
}

static void
gis_live_chooser_page_finalize (GObject *object)
{
  GisLiveChooserPage *page = (GisLiveChooserPage *)object;
  GisLiveChooserPagePrivate *priv = gis_live_chooser_page_get_instance_private (page);

  G_OBJECT_CLASS (gis_live_chooser_page_parent_class)->finalize (object);
}

static void
gis_live_chooser_page_constructed (GObject *object)
{
  GisLiveChooserPage *page = GIS_LIVE_CHOOSER_PAGE (object);
  GisLiveChooserPagePrivate *priv = gis_live_chooser_page_get_instance_private (page);
  GisDriver *driver = GIS_PAGE (page)->driver;

  g_type_ensure (CC_TYPE_LANGUAGE_CHOOSER);

  G_OBJECT_CLASS (gis_live_chooser_page_parent_class)->constructed (object);

  gis_page_set_hide_forward_button (GIS_PAGE (page), TRUE);

  priv->act_client = act_user_manager_get_default ();

  g_signal_connect_swapped (priv->try_button,
                            "clicked",
                            G_CALLBACK (try_button_clicked),
                            page);

  g_signal_connect_swapped (priv->reformat_button,
                            "clicked",
                            G_CALLBACK (gis_live_chooser_page_launch_reformatter),
                            page);

  g_object_bind_property (driver, "live-dvd", priv->try_label, "visible", G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
  g_object_bind_property (driver, "live-dvd", priv->try_icon, "visible", G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
  g_object_bind_property (driver, "live-dvd", priv->try_label_dvd, "visible", G_BINDING_SYNC_CREATE);
  g_object_bind_property (driver, "live-dvd", priv->try_icon_dvd, "visible", G_BINDING_SYNC_CREATE);

  load_css_overrides (page);

  gtk_widget_show (GTK_WIDGET (page));
}

static void
gis_live_chooser_page_locale_changed (GisPage *page)
{
  GisLiveChooserPagePrivate *priv = gis_live_chooser_page_get_instance_private (GIS_LIVE_CHOOSER_PAGE (page));

  gtk_label_set_label (GTK_LABEL (priv->try_label), _("Try Endless OS by running it from the USB Stick."));
  gtk_label_set_label (GTK_LABEL (priv->try_label_dvd), _("Try Endless OS by running it from the DVD."));
  gtk_label_set_label (GTK_LABEL (priv->reformat_label), _("Reformat this computer with Endless OS."));
  gtk_button_set_label (GTK_BUTTON (priv->try_button), _("Try It"));
  gtk_button_set_label (GTK_BUTTON (priv->reformat_button), _("Reformat"));

  gis_page_set_title (page, _("Try or Reformat"));
}

static void
gis_live_chooser_page_shown (GisPage *page)
{
  GisLiveChooserPage *self = GIS_LIVE_CHOOSER_PAGE (page);

  if (gis_driver_is_reformatter (page->driver))
    gis_live_chooser_page_launch_reformatter (self);
}

static void
gis_live_chooser_page_class_init (GisLiveChooserPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GisPageClass *page_class = GIS_PAGE_CLASS (klass);

  page_class->page_id = "live-chooser";
  page_class->locale_changed = gis_live_chooser_page_locale_changed;
  page_class->save_data = gis_live_chooser_page_save_data;
  page_class->shown = gis_live_chooser_page_shown;

  gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (klass), "/org/gnome/initial-setup/gis-live-chooser-page.ui");

  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, try_label);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, try_label_dvd);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, try_icon);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, try_icon_dvd);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, reformat_label);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, try_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), GisLiveChooserPage, reformat_button);

  object_class->finalize = gis_live_chooser_page_finalize;
  object_class->constructed = gis_live_chooser_page_constructed;
}

static void
gis_live_chooser_page_init (GisLiveChooserPage *page)
{
  g_resources_register (live_chooser_get_resource ());

  gtk_widget_init_template (GTK_WIDGET (page));
}

void
gis_prepare_live_chooser_page (GisDriver *driver)
{
  /* Only include this page when running on live media or as a standalone
   * reformatter.
   */
  if (!gis_driver_is_live_session (driver) &&
      !gis_driver_is_reformatter (driver))
    return;

  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_LIVE_CHOOSER_PAGE,
                                     "driver", driver,
                                     NULL));
}

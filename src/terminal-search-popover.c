/*
 * Copyright © 2015 Christian Persch
 * Copyright © 2005 Paolo Maggi
 * Copyright © 2010 Red Hat (Red Hat author: Behdad Esfahbod)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef WITH_PCRE2
#include "terminal-pcre2.h"
#endif

#include "terminal-search-popover.h"
#include "terminal-intl.h"
#include "terminal-window.h"
#include "terminal-app.h"
#include "terminal-libgsystem.h"

typedef struct _TerminalSearchPopoverPrivate TerminalSearchPopoverPrivate;

struct _TerminalSearchPopover
{
  GtkPopover parent_instance;
};

struct _TerminalSearchPopoverClass
{
  GtkPopoverClass parent_class;

  /* Signals */
  void (* search) (TerminalSearchPopover *popover,
                   gboolean backward);
};

struct _TerminalSearchPopoverPrivate
{
  GtkWidget *search_entry;
  GtkWidget *search_prev_button;
  GtkWidget *search_next_button;
  GtkWidget *reveal_button;
  GtkWidget *close_button;
  GtkWidget *revealer;
  GtkWidget *match_case_checkbutton;
  GtkWidget *entire_word_checkbutton;
  GtkWidget *regex_checkbutton;
  GtkWidget *wrap_around_checkbutton;

  gboolean search_text_changed;

  /* Cached regex */
  gboolean regex_caseless;
  gboolean regex_multiline;
  char *regex_pattern;
#ifdef WITH_PCRE2
  VteRegex *regex;
#else
  GRegex *regex;
#endif
};

enum {
  PROP_0,
  PROP_REGEX,
  PROP_WRAP_AROUND,
  LAST_PROP
};

enum {
  SEARCH,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];
static GParamSpec *pspecs[LAST_PROP];
static GtkListStore *history_store;

G_DEFINE_TYPE_WITH_PRIVATE (TerminalSearchPopover, terminal_search_popover, GTK_TYPE_POPOVER)

#define PRIV(obj) ((TerminalSearchPopoverPrivate *) terminal_search_popover_get_instance_private ((TerminalSearchPopover *)(obj)))

/* history */

#define HISTORY_MIN_ITEM_LEN (3)
#define HISTORY_LENGTH (10)

static gboolean
history_enabled (void)
{
  gboolean enabled;

  /* not quite an exact setting for this, but close enough… */
  g_object_get (gtk_settings_get_default (), "gtk-recent-files-enabled", &enabled, NULL);
  if (!enabled)
    return FALSE;

  if (history_store == NULL) {
    history_store = gtk_list_store_new (1, G_TYPE_STRING);
    g_object_set_data_full (G_OBJECT (terminal_app_get ()), "search-history-store",
                            history_store, (GDestroyNotify) g_object_unref);
  }

  return TRUE;
}

static gboolean
history_remove_item (const char  *text)
{
  GtkTreeModel *model = GTK_TREE_MODEL (history_store);
  GtkTreeIter iter;

  if (!gtk_tree_model_get_iter_first (model, &iter))
    return FALSE;

  do {
    gs_free gchar *item_text;

    gtk_tree_model_get (model, &iter, 0, &item_text, -1);

    if (item_text != NULL && strcmp (item_text, text) == 0) {
      gtk_list_store_remove (history_store, &iter);
      return TRUE;
    }
  } while (gtk_tree_model_iter_next (model, &iter));

  return FALSE;
}

static void
history_clamp (int max)
{
  GtkTreePath *path;
  GtkTreeIter iter;

  /* -1 because TreePath counts from 0 */
  path = gtk_tree_path_new_from_indices (max - 1, -1);

  if (gtk_tree_model_get_iter (GTK_TREE_MODEL (history_store), &iter, path))
    while (1)
      if (!gtk_list_store_remove (history_store, &iter))
	break;

  gtk_tree_path_free (path);
}

static void
history_insert_item (const char *text)
{
  GtkTreeIter iter;

  if (!history_enabled () || text == NULL)
    return;

  if (g_utf8_strlen (text, -1) <= HISTORY_MIN_ITEM_LEN)
    return;

  /* remove the text from the store if it was already
   * present. If it wasn't, clamp to max history - 1
   * before inserting the new row, otherwise appending
   * would not work */
  if (!history_remove_item (text))
    history_clamp (HISTORY_LENGTH - 1);

  gtk_list_store_insert_with_values (history_store, &iter, 0,
                                     0, text,
                                     -1);
}

/* helper functions */

static void
update_sensitivity (TerminalSearchPopover *popover)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);
  gboolean can_search;

  can_search = priv->regex != NULL;

  gtk_widget_set_sensitive (priv->search_prev_button, can_search);
  gtk_widget_set_sensitive (priv->search_next_button, can_search);
}

static void
perform_search (TerminalSearchPopover *popover,
                gboolean backward)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);

  if (priv->regex == NULL)
    return;

  /* Add to search history */
  if (priv->search_text_changed) {
    const char *search_text;

    search_text = gtk_entry_get_text (GTK_ENTRY (priv->search_entry));
    history_insert_item (search_text);

    priv->search_text_changed = FALSE;
  }

  g_signal_emit (popover, signals[SEARCH], 0, backward);
}

#if GTK_CHECK_VERSION (3, 16, 0)
static void
previous_match_cb (GtkWidget *widget,
                  TerminalSearchPopover *popover)
{
  perform_search (popover, TRUE);
}

static void
next_match_cb (GtkWidget *widget,
               TerminalSearchPopover *popover)
{
  perform_search (popover, FALSE);
}
#endif /* GTK+ 3.16 */

static void
close_clicked_cb (GtkWidget *widget,
                  GtkWidget *popover)
{
  gtk_widget_hide (popover);
}

static void
search_button_clicked_cb (GtkWidget *button,
                          TerminalSearchPopover *popover)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);

  perform_search (popover, button == priv->search_prev_button);
}

static void
update_regex (TerminalSearchPopover *popover)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);
  const char *search_text;
  gboolean caseless, multiline = FALSE;
  gs_free char *pattern;
  gs_free_error GError *error = NULL;

  search_text = gtk_entry_get_text (GTK_ENTRY (priv->search_entry));

  caseless = !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->match_case_checkbutton));

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->regex_checkbutton))) {
    pattern = g_strdup (search_text);
    multiline = TRUE;
  } else {
    pattern = g_regex_escape_string (search_text, -1);
  }

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (priv->entire_word_checkbutton))) {
    char *new_pattern;
    new_pattern = g_strdup_printf ("\\b%s\\b", pattern);
    g_free (pattern);
    pattern = new_pattern;
  }

  if (priv->regex_caseless == caseless &&
      priv->regex_multiline == multiline &&
      g_strcmp0 (priv->regex_pattern, pattern) == 0)
    return;

  if (priv->regex) {
#ifdef WITH_PCRE2
    vte_regex_unref (priv->regex);
#else
    g_regex_unref (priv->regex);
#endif
  }

  g_clear_pointer (&priv->regex_pattern, g_free);

  /* FIXME: if comping the regex fails, show the error message somewhere */
  if (search_text[0] != '\0') {
#ifdef WITH_PCRE2
    guint32 compile_flags;

    compile_flags = PCRE2_UTF | PCRE2_NO_UTF_CHECK;
    if (caseless)
      compile_flags |= PCRE2_CASELESS;
    if (multiline)
      compile_flags |= PCRE2_MULTILINE;

    priv->regex = vte_regex_new (pattern, -1, compile_flags, &error);
    if (priv->regex != NULL &&
        (!vte_regex_jit (priv->regex, PCRE2_JIT_COMPLETE, NULL) ||
         !vte_regex_jit (priv->regex, PCRE2_JIT_PARTIAL_SOFT, NULL))) {
    }
#else
    GRegexCompileFlags compile_flags;

    compile_flags = G_REGEX_OPTIMIZE;
    if (caseless)
      compile_flags |= G_REGEX_CASELESS;
    if (multiline)
      compile_flags |= G_REGEX_MULTILINE;

    priv->regex = g_regex_new (pattern, compile_flags, 0, &error);
#endif
    if (priv->regex != NULL)
      gs_transfer_out_value (&priv->regex_pattern, &pattern);
  } else {
    priv->regex = NULL;
  }

  update_sensitivity (popover);

  g_object_notify_by_pspec (G_OBJECT (popover), pspecs[PROP_REGEX]);
}

static void
search_text_changed_cb (GtkToggleButton *button,
                        TerminalSearchPopover *popover)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);

  update_regex (popover);
  priv->search_text_changed = TRUE;
}

static void
search_parameters_changed_cb (GtkToggleButton *button,
                              TerminalSearchPopover *popover)
{
  update_regex (popover);
}

static void
wrap_around_toggled_cb (GtkToggleButton *button,
                        TerminalSearchPopover *popover)
{
  g_object_notify_by_pspec (G_OBJECT (popover), pspecs[PROP_WRAP_AROUND]);
}

/* public functions */

/* Class implementation */

static void
terminal_search_popover_init (TerminalSearchPopover *popover)
{
  TerminalSearchPopoverPrivate *priv = PRIV (popover);
  GtkWidget *widget = GTK_WIDGET (popover);

  priv->regex_pattern = 0;
  priv->regex_caseless = priv->regex_multiline = FALSE;

  gtk_widget_init_template (widget);

  /* Make the search entry reasonably wide */
  gtk_widget_set_size_request (priv->search_entry, 300, -1);

  /* Add entry completion with history */
#if 0
  g_object_set (G_OBJECT (priv->search_entry),
		"model", history_store,
		"entry-text-column", 0,
		NULL);
#endif

  if (history_enabled ()) {
    gs_unref_object GtkEntryCompletion *completion;

    completion = gtk_entry_completion_new ();
    gtk_entry_completion_set_model (completion, GTK_TREE_MODEL (history_store));
    gtk_entry_completion_set_text_column (completion, 0);
    gtk_entry_completion_set_minimum_key_length (completion, HISTORY_MIN_ITEM_LEN);
    gtk_entry_completion_set_popup_completion (completion, FALSE);
    gtk_entry_completion_set_inline_completion (completion, TRUE);
    gtk_entry_set_completion (GTK_ENTRY (priv->search_entry), completion);
  }

#if GTK_CHECK_VERSION (3, 17, 2)
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_popover_set_default_widget (GTK_POPOVER (popover), priv->search_next_button);
  G_GNUC_END_IGNORE_DEPRECATIONS
#endif

#if GTK_CHECK_VERSION (3, 16, 0)
  g_signal_connect (priv->search_entry, "previous-match", G_CALLBACK (previous_match_cb), popover);
  g_signal_connect (priv->search_entry, "next-match", G_CALLBACK (next_match_cb), popover);
#endif

  g_signal_connect (priv->search_prev_button, "clicked", G_CALLBACK (search_button_clicked_cb), popover);
  g_signal_connect (priv->search_next_button, "clicked", G_CALLBACK (search_button_clicked_cb), popover);

  g_signal_connect (priv->close_button, "clicked", G_CALLBACK (close_clicked_cb), popover);

  g_object_bind_property (priv->reveal_button, "active",
                          priv->revealer, "reveal-child",
                          G_BINDING_DEFAULT);

  update_sensitivity (popover);

  g_signal_connect (priv->search_entry, "search-changed", G_CALLBACK (search_text_changed_cb), popover);
  g_signal_connect (priv->match_case_checkbutton, "toggled", G_CALLBACK (search_parameters_changed_cb), popover);
  g_signal_connect (priv->entire_word_checkbutton, "toggled", G_CALLBACK (search_parameters_changed_cb), popover);
  g_signal_connect (priv->regex_checkbutton, "toggled", G_CALLBACK (search_parameters_changed_cb), popover);

  g_signal_connect (priv->wrap_around_checkbutton, "toggled", G_CALLBACK (wrap_around_toggled_cb), popover);
}

static void
terminal_search_popover_finalize (GObject *object)
{
  TerminalSearchPopover *popover = TERMINAL_SEARCH_POPOVER (object);
  TerminalSearchPopoverPrivate *priv = PRIV (popover);

  if (priv->regex) {
#ifdef WITH_PCRE2
    vte_regex_unref (priv->regex);
#else
    g_regex_unref (priv->regex);
#endif
  }

  g_free (priv->regex_pattern);

  G_OBJECT_CLASS (terminal_search_popover_parent_class)->finalize (object);
}

static void
terminal_search_popover_get_property (GObject *object,
                                      guint prop_id,
                                      GValue *value,
                                      GParamSpec *pspec)
{
  TerminalSearchPopover *popover = TERMINAL_SEARCH_POPOVER (object);

  switch (prop_id) {
  case PROP_REGEX:
    g_value_set_boxed (value, terminal_search_popover_get_regex (popover));
    break;
  case PROP_WRAP_AROUND:
    g_value_set_boolean (value, terminal_search_popover_get_wrap_around (popover));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
terminal_search_popover_set_property (GObject *object,
                                      guint prop_id,
                                      const GValue *value,
                                      GParamSpec *pspec)
{
  switch (prop_id) {
  case PROP_REGEX:
  case PROP_WRAP_AROUND:
    /* not writable */
    break;
  default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
terminal_search_popover_class_init (TerminalSearchPopoverClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->finalize = terminal_search_popover_finalize;
  gobject_class->get_property = terminal_search_popover_get_property;
  gobject_class->set_property = terminal_search_popover_set_property;

  signals[SEARCH] =
    g_signal_new (I_("search"),
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalSearchPopoverClass, search),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_BOOLEAN);

  pspecs[PROP_REGEX] =
    g_param_spec_boxed ("regex", NULL, NULL,
#ifdef WITH_PCRE2
                        VTE_TYPE_REGEX,
#else
                        G_TYPE_REGEX,
#endif
                        G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);

  pspecs[PROP_WRAP_AROUND] =
    g_param_spec_boolean ("wrap-around", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (pspecs), pspecs);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/terminal/ui/search-popover.ui");
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, search_prev_button);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, search_next_button);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, reveal_button);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, close_button);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, revealer);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, match_case_checkbutton);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, entire_word_checkbutton);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, regex_checkbutton);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalSearchPopover, wrap_around_checkbutton);
}

/* public API */

/**
 * terminal_search_popover_new:
 *
 * Returns: a new #TerminalSearchPopover
 */
TerminalSearchPopover *
terminal_search_popover_new (GtkWidget *relative_to_widget)
{
  return g_object_new (TERMINAL_TYPE_SEARCH_POPOVER,
                       "relative-to", relative_to_widget,
                       NULL);
}

/**
 * terminal_search_popover_get_regex:
 * @popover: a #TerminalSearchPopover
 *
 * Returns: (transfer none): the search regex, or %NULL
 */
#ifdef WITH_PCRE2
VteRegex *
#else
GRegex *
#endif
terminal_search_popover_get_regex (TerminalSearchPopover *popover)
{
  g_return_val_if_fail (TERMINAL_IS_SEARCH_POPOVER (popover), NULL);

  return PRIV (popover)->regex;
}

/**
 * terminal_search_popover_get_wrap_around:
 * @popover: a #TerminalSearchPopover
 *
 * Returns: (transfer none): whether search should wrap around
 */
gboolean
terminal_search_popover_get_wrap_around (TerminalSearchPopover *popover)
{
  g_return_val_if_fail (TERMINAL_IS_SEARCH_POPOVER (popover), FALSE);

  return gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (PRIV (popover)->wrap_around_checkbutton));
}

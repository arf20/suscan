/*

  Copyright (C) 2017 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#include <string.h>

#define SU_LOG_DOMAIN "estimator-ui"

#include "gui.h"
#include "estimatorui.h"

SUPRIVATE SUBOOL
suscan_gui_estimatorui_load_all_widgets(suscan_gui_estimatorui_t *ui)
{
  SU_TRYCATCH(
      ui->root =
          GTK_GRID(gtk_builder_get_object(
              ui->builder,
              "grRoot")),
          return SU_FALSE);

  SU_TRYCATCH(
      ui->enableToggleButton =
          GTK_TOGGLE_BUTTON(gtk_builder_get_object(
              ui->builder,
              "tbEnable")),
          return SU_FALSE);

  SU_TRYCATCH(
      ui->valueEntry =
          GTK_ENTRY(gtk_builder_get_object(
              ui->builder,
              "eValue")),
          return SU_FALSE);

  return SU_TRUE;
}

GtkWidget *
suscan_gui_estimatorui_get_root(const suscan_gui_estimatorui_t *ui)
{
  return GTK_WIDGET(ui->root);
}

suscan_gui_estimatorui_t *
suscan_gui_estimatorui_new(struct suscan_gui_estimatorui_params *params)
{
  suscan_gui_estimatorui_t *new = NULL;

  SU_TRYCATCH(new = calloc(1, sizeof(suscan_gui_estimatorui_t)), goto fail);

  SU_TRYCATCH(new->field = strdup(params->field), goto fail);

  new->estimator_id = params->estimator_id;
  new->inspector = params->inspector;
  new->index = -1;

  SU_TRYCATCH(
      new->builder = gtk_builder_new_from_file(
          PKGDATADIR "/gui/estimator.glade"),
      goto fail);

  SU_TRYCATCH(suscan_gui_estimatorui_load_all_widgets(new), goto fail);

  gtk_builder_connect_signals(new->builder, new);

  gtk_button_set_label(
      GTK_BUTTON(new->enableToggleButton),
      params->desc);

  return new;

fail:
  if (new != NULL)
    suscan_gui_estimatorui_destroy(new);

  return NULL;
}

void
suscan_gui_estimatorui_set_value(suscan_gui_estimatorui_t *ui, SUFLOAT value)
{
  ui->value = value;

  suscan_gui_modemctl_helper_write_float(ui->valueEntry, value);
}

void
suscan_gui_estimatorui_destroy(suscan_gui_estimatorui_t *ui)
{
  if (ui->field != NULL)
    free(ui->field);

  if (ui->builder != NULL)
    g_object_unref(G_OBJECT(ui->builder));

  free(ui);
}

/******************************* GUI Callbacks *******************************/
void
suscan_gui_estimatorui_on_toggle_enable(GtkWidget *widget, gpointer data)
{
  suscan_gui_estimatorui_t *ui = (suscan_gui_estimatorui_t *) data;

  SU_TRYCATCH(ui->index != -1, return);

  SU_TRYCATCH(
      suscan_analyzer_inspector_estimator_cmd_async(
          ui->inspector->_parent.gui->analyzer,
          ui->inspector->inshnd,
          ui->index,
          gtk_toggle_button_get_active(ui->enableToggleButton),
          rand()),
      return);
}

void
suscan_gui_estimatorui_on_apply(GtkWidget *widget, gpointer data)
{
  suscan_gui_estimatorui_t *ui = (suscan_gui_estimatorui_t *) data;
  SUFLOAT value = 0;

  SU_TRYCATCH(ui->index != -1, return);

  SU_TRYCATCH(
      suscan_config_set_float(
          ui->inspector->config,
          ui->field,
          ui->value),
      return);

  SU_TRYCATCH(suscan_gui_inspector_refresh_on_config(ui->inspector), return);

  SU_TRYCATCH(suscan_gui_inspector_commit_config(ui->inspector), return);
}


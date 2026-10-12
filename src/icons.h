#pragma once

#include <gtk/gtk.h>

typedef enum { ICON_PLAY, ICON_PAUSE, ICON_STOP, ICON_PREV, ICON_NEXT, ICON_VOLUME, ICON_NOTE } IconKind;

/* A drawn icon of size x size (logical pixels). */
GtkWidget *icon_new(IconKind kind, int size);
void icon_set(GtkWidget *icon, IconKind kind);

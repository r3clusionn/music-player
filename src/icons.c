/* Transport and volume icons drawn with cairo in the theme's text color:
 * this system has no icon theme installed, and drawn icons stay sharp at 2x. */
#include "icons.h"

static gboolean draw_icon(GtkWidget *w, cairo_t *cr, gpointer data)
{
	IconKind kind = GPOINTER_TO_INT(data);
	int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
	double s = MIN(width, height);
	GdkRGBA c;
	gtk_style_context_get_color(gtk_widget_get_style_context(w), gtk_widget_get_state_flags(w), &c);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_translate(cr, (width - s) / 2, (height - s) / 2);
	cairo_scale(cr, s / 16.0, s / 16.0); /* draw on a 16x16 grid */
	switch (kind) {
	case ICON_PLAY:
		cairo_move_to(cr, 4, 2.5);
		cairo_line_to(cr, 13.5, 8);
		cairo_line_to(cr, 4, 13.5);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case ICON_PAUSE:
		cairo_rectangle(cr, 3.5, 2.5, 3.2, 11);
		cairo_rectangle(cr, 9.3, 2.5, 3.2, 11);
		cairo_fill(cr);
		break;
	case ICON_STOP:
		cairo_rectangle(cr, 3, 3, 10, 10);
		cairo_fill(cr);
		break;
	case ICON_PREV:
		cairo_rectangle(cr, 2.5, 3, 2, 10);
		cairo_move_to(cr, 13.5, 3);
		cairo_line_to(cr, 5, 8);
		cairo_line_to(cr, 13.5, 13);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case ICON_NEXT:
		cairo_rectangle(cr, 11.5, 3, 2, 10);
		cairo_move_to(cr, 2.5, 3);
		cairo_line_to(cr, 11, 8);
		cairo_line_to(cr, 2.5, 13);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case ICON_VOLUME:
		cairo_move_to(cr, 1.5, 6);
		cairo_line_to(cr, 4.5, 6);
		cairo_line_to(cr, 8.5, 2.5);
		cairo_line_to(cr, 8.5, 13.5);
		cairo_line_to(cr, 4.5, 10);
		cairo_line_to(cr, 1.5, 10);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_set_line_width(cr, 1.3);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_arc(cr, 8.5, 8, 3, -0.9, 0.9);
		cairo_stroke(cr);
		cairo_arc(cr, 8.5, 8, 5.5, -0.9, 0.9);
		cairo_stroke(cr);
		break;
	case ICON_NOTE:
		cairo_set_line_width(cr, 0.6);
		cairo_arc(cr, 5, 12, 2.2, 0, 2 * G_PI);
		cairo_fill(cr);
		cairo_arc(cr, 12, 10.5, 2.2, 0, 2 * G_PI);
		cairo_fill(cr);
		cairo_rectangle(cr, 6.4, 3, 0.9, 9);
		cairo_rectangle(cr, 13.4, 1.5, 0.9, 9);
		cairo_fill(cr);
		cairo_move_to(cr, 6.4, 3);
		cairo_line_to(cr, 14.3, 1.5);
		cairo_line_to(cr, 14.3, 3.3);
		cairo_line_to(cr, 6.4, 4.8);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	}
	return TRUE;
}

GtkWidget *icon_new(IconKind kind, int size)
{
	GtkWidget *w = gtk_drawing_area_new();
	gtk_widget_set_size_request(w, size, size);
	g_signal_connect(w, "draw", G_CALLBACK(draw_icon), GINT_TO_POINTER(kind));
	return w;
}

void icon_set(GtkWidget *icon, IconKind kind)
{
	g_signal_handlers_disconnect_matched(icon, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, draw_icon, NULL);
	g_signal_connect(icon, "draw", G_CALLBACK(draw_icon), GINT_TO_POINTER(kind));
	gtk_widget_queue_draw(icon);
}

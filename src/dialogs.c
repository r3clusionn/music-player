/* Preferences, the column editor, the equalizer and About. */
#include "app.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

/* ---- preferences ---------------------------------------------------------- */

static GtkListStore *folder_store;

static void fill_folders(void)
{
	gtk_list_store_clear(folder_store);
	char **folders = library_folders(app->lib);
	for (char **f = folders; *f; f++)
		gtk_list_store_insert_with_values(folder_store, NULL, -1, 0, *f, -1);
	g_strfreev(folders);
}

static void add_folder(GtkButton *b, gpointer data)
{
	(void)b;
	GtkWidget *parent = data;
	GtkFileChooserNative *fc = gtk_file_chooser_native_new("Add a music folder", GTK_WINDOW(parent),
							       GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Add", "_Cancel");
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
		library_add_folder(app->lib, dir);
		g_free(dir);
		fill_folders();
		library_scan(app->lib, app_scan_progress, NULL);
	}
	g_object_unref(fc);
}

static void remove_folder(GtkButton *b, gpointer data)
{
	(void)b;
	GtkTreeView *tv = data;
	GtkTreeIter it;
	GtkTreeModel *m;
	if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), &m, &it))
		return;
	char *dir;
	gtk_tree_model_get(m, &it, 0, &dir, -1);
	library_remove_folder(app->lib, dir);
	g_free(dir);
	fill_folders();
	library_scan(app->lib, app_scan_progress, NULL);
}

static void rescan(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	library_scan(app->lib, app_scan_progress, NULL);
}

static void rg_mode_changed(GtkComboBox *c, gpointer data)
{
	(void)data;
	app->rg_mode = gtk_combo_box_get_active(c);
	app_apply_audio_settings();
}

static void rg_preamp_changed(GtkSpinButton *s, gpointer data)
{
	(void)data;
	app->rg_preamp = gtk_spin_button_get_value(s);
	app_apply_audio_settings();
}

static void rg_clip_toggled(GtkToggleButton *t, gpointer data)
{
	(void)data;
	app->rg_clip = gtk_toggle_button_get_active(t);
	app_apply_audio_settings();
}

static void dark_toggled(GtkToggleButton *t, gpointer data)
{
	(void)data;
	app->dark = gtk_toggle_button_get_active(t);
	app_apply_theme();
	app_save_config();
}

/* PipeWire resamples everything to one rate unless told which rates it may
 * switch to; this drop-in lets it follow the music. */
static char *rates_conf_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "pipewire", "pipewire.conf.d",
				"60-music-player-rates.conf", NULL);
}

static void bitperfect_toggled(GtkToggleButton *t, gpointer data)
{
	GtkLabel *note = data;
	app->bitperfect = gtk_toggle_button_get_active(t);
	char *path = rates_conf_path();
	if (app->bitperfect) {
		char *dir = g_path_get_dirname(path);
		g_mkdir_with_parents(dir, 0755);
		g_free(dir);
		static const char conf[] =
			"# Written by music-player (Preferences > Follow each file's sample rate).\n"
			"context.properties = {\n"
			"    default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 ]\n"
			"}\n";
		g_file_set_contents(path, conf, -1, NULL);
	} else {
		g_unlink(path);
	}
	g_free(path);
	gtk_label_set_text(note, "Takes effect the next time audio starts (F2 off and on).");
	app_save_config();
}

void dialog_preferences(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Preferences", GTK_WINDOW(app->window),
						   GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_default_size(GTK_WINDOW(d), 620, 520);
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_box_pack_start(GTK_BOX(area), grid, TRUE, TRUE, 0);
	int row = 0;

	GtkWidget *h = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(h), "<b>Library folders</b>");
	gtk_widget_set_halign(h, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), h, 0, row++, 2, 1);

	folder_store = gtk_list_store_new(1, G_TYPE_STRING);
	GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(folder_store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes(
						      "", gtk_cell_renderer_text_new(), "text", 0, NULL));
	fill_folders();
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_size_request(sw, -1, 120);
	gtk_widget_set_hexpand(sw, TRUE);
	gtk_container_add(GTK_CONTAINER(sw), tv);
	gtk_grid_attach(GTK_GRID(grid), sw, 0, row, 1, 1);
	GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *b = gtk_button_new_with_mnemonic("_Add…");
	g_signal_connect(b, "clicked", G_CALLBACK(add_folder), d);
	gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	b = gtk_button_new_with_mnemonic("_Remove");
	g_signal_connect(b, "clicked", G_CALLBACK(remove_folder), tv);
	gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	b = gtk_button_new_with_mnemonic("Re_scan");
	g_signal_connect(b, "clicked", G_CALLBACK(rescan), NULL);
	gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), bb, 1, row++, 1, 1);

	h = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(h), "<b>Playback</b>");
	gtk_widget_set_halign(h, GTK_ALIGN_START);
	gtk_widget_set_margin_top(h, 8);
	gtk_grid_attach(GTK_GRID(grid), h, 0, row++, 2, 1);

	GtkWidget *rgbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(rgbox), gtk_label_new("ReplayGain"), FALSE, FALSE, 0);
	GtkWidget *rg = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rg), "Off");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rg), "Track gain");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rg), "Album gain");
	gtk_combo_box_set_active(GTK_COMBO_BOX(rg), app->rg_mode);
	g_signal_connect(rg, "changed", G_CALLBACK(rg_mode_changed), NULL);
	gtk_box_pack_start(GTK_BOX(rgbox), rg, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(rgbox), gtk_label_new("Preamp (dB)"), FALSE, FALSE, 8);
	GtkWidget *pre = gtk_spin_button_new_with_range(-15, 15, 0.5);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(pre), app->rg_preamp);
	g_signal_connect(pre, "value-changed", G_CALLBACK(rg_preamp_changed), NULL);
	gtk_box_pack_start(GTK_BOX(rgbox), pre, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), rgbox, 0, row++, 2, 1);

	GtkWidget *clip = gtk_check_button_new_with_mnemonic("_Prevent clipping (lower the gain when the peak would clip)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clip), app->rg_clip);
	g_signal_connect(clip, "toggled", G_CALLBACK(rg_clip_toggled), NULL);
	gtk_grid_attach(GTK_GRID(grid), clip, 0, row++, 2, 1);

	GtkWidget *bp = gtk_check_button_new_with_mnemonic(
		"_Follow each file's sample rate (no resampling in PipeWire)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bp), app->bitperfect);
	GtkWidget *note = gtk_label_new("For bit-perfect output also keep the volume at 100% and turn "
					"ReplayGain and the equalizer off.");
	gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
	g_signal_connect(bp, "toggled", G_CALLBACK(bitperfect_toggled), note);
	gtk_grid_attach(GTK_GRID(grid), bp, 0, row++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 2, 1);

	h = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(h), "<b>Appearance</b>");
	gtk_widget_set_halign(h, GTK_ALIGN_START);
	gtk_widget_set_margin_top(h, 8);
	gtk_grid_attach(GTK_GRID(grid), h, 0, row++, 2, 1);
	GtkWidget *dark = gtk_check_button_new_with_mnemonic("_Dark theme");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark), app->dark);
	g_signal_connect(dark, "toggled", G_CALLBACK(dark_toggled), NULL);
	gtk_grid_attach(GTK_GRID(grid), dark, 0, row++, 2, 1);
	GtkWidget *cols = gtk_button_new_with_mnemonic("Edit playlist _columns…");
	gtk_widget_set_halign(cols, GTK_ALIGN_START);
	g_signal_connect_swapped(cols, "clicked", G_CALLBACK(dialog_columns), NULL);
	gtk_grid_attach(GTK_GRID(grid), cols, 0, row++, 2, 1);

	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show_all(d);
}

/* ---- columns -------------------------------------------------------------- */

enum { CC_VISIBLE, CC_NAME, CC_FORMAT, CC_ALIGN, CC_WIDTH, CC_N };

static GtkListStore *col_store;

static void col_store_fill(void)
{
	gtk_list_store_clear(col_store);
	for (guint i = 0; i < app->columns->len; i++) {
		Column *c = g_ptr_array_index(app->columns, i);
		const char *align = c->align > 0.75 ? "Right" : c->align > 0.25 ? "Center" : "Left";
		gtk_list_store_insert_with_values(col_store, NULL, -1, CC_VISIBLE, c->visible, CC_NAME, c->name,
						  CC_FORMAT, c->format_src, CC_ALIGN, align, CC_WIDTH, c->width, -1);
	}
}

static void col_store_apply(void)
{
	g_ptr_array_set_size(app->columns, 0);
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(col_store), &it);
	while (ok) {
		gboolean vis;
		char *name, *fmt, *align;
		int width;
		gtk_tree_model_get(GTK_TREE_MODEL(col_store), &it, CC_VISIBLE, &vis, CC_NAME, &name, CC_FORMAT, &fmt,
				   CC_ALIGN, &align, CC_WIDTH, &width, -1);
		float a = !strcmp(align, "Right") ? 1.0f : !strcmp(align, "Center") ? 0.5f : 0.0f;
		g_ptr_array_add(app->columns, column_new(name, fmt, width, a, vis));
		g_free(name);
		g_free(fmt);
		g_free(align);
		ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(col_store), &it);
	}
	columns_save();
	columns_rebuild();
	app_save_config();
}

static void col_toggled(GtkCellRendererToggle *r, char *path, gpointer data)
{
	(void)data;
	GtkTreeIter it;
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(col_store), &it, path);
	gtk_list_store_set(col_store, &it, CC_VISIBLE, !gtk_cell_renderer_toggle_get_active(r), -1);
	col_store_apply();
}

static void col_edited(GtkCellRendererText *r, char *path, char *text, gpointer data)
{
	(void)r;
	GtkTreeIter it;
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(col_store), &it, path);
	gtk_list_store_set(col_store, &it, GPOINTER_TO_INT(data), text, -1);
	col_store_apply();
}

static void col_add(GtkButton *b, gpointer data)
{
	(void)b;
	GtkTreeView *tv = data;
	GtkTreeIter it;
	gtk_list_store_insert_with_values(col_store, &it, -1, CC_VISIBLE, TRUE, CC_NAME, "New column", CC_FORMAT,
					  "%title%", CC_ALIGN, "Left", CC_WIDTH, 150, -1);
	col_store_apply();
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(col_store), &it);
	gtk_tree_view_set_cursor(tv, p, gtk_tree_view_get_column(tv, 1), TRUE);
	gtk_tree_path_free(p);
}

static void col_remove(GtkButton *b, gpointer data)
{
	(void)b;
	GtkTreeIter it;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(data), NULL, &it)) {
		gtk_list_store_remove(col_store, &it);
		col_store_apply();
	}
}

static void col_move(GtkTreeView *tv, int dir)
{
	GtkTreeIter it, other;
	if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), NULL, &it))
		return;
	other = it;
	gboolean ok = dir < 0 ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(col_store), &other)
			      : gtk_tree_model_iter_next(GTK_TREE_MODEL(col_store), &other);
	if (ok) {
		gtk_list_store_swap(col_store, &it, &other);
		col_store_apply();
	}
}

static void col_up(GtkButton *b, gpointer data) { (void)b; col_move(data, -1); }
static void col_down(GtkButton *b, gpointer data) { (void)b; col_move(data, 1); }

static void col_reset(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	g_key_file_remove_group(app->cfg, "columns", NULL);
	g_ptr_array_unref(app->columns);
	columns_load();
	columns_save();
	columns_rebuild();
	col_store_fill();
}

void dialog_columns(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Playlist columns", GTK_WINDOW(app->window),
						   GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_default_size(GTK_WINDOW(d), 860, 460);
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(area), 12);

	col_store = gtk_list_store_new(CC_N, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	col_store_fill();
	GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(col_store));
	GtkCellRenderer *r = gtk_cell_renderer_toggle_new();
	g_signal_connect(r, "toggled", G_CALLBACK(col_toggled), NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes("Show", r, "active",
											      CC_VISIBLE, NULL));
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "editable", TRUE, NULL);
	g_signal_connect(r, "edited", G_CALLBACK(col_edited), GINT_TO_POINTER(CC_NAME));
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes("Name", r, "text",
											      CC_NAME, NULL));
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "editable", TRUE, "family", "monospace", NULL);
	g_signal_connect(r, "edited", G_CALLBACK(col_edited), GINT_TO_POINTER(CC_FORMAT));
	GtkTreeViewColumn *fc = gtk_tree_view_column_new_with_attributes("Format", r, "text", CC_FORMAT, NULL);
	gtk_tree_view_column_set_expand(fc, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), fc);
	GtkListStore *aligns = gtk_list_store_new(1, G_TYPE_STRING);
	gtk_list_store_insert_with_values(aligns, NULL, -1, 0, "Left", -1);
	gtk_list_store_insert_with_values(aligns, NULL, -1, 0, "Center", -1);
	gtk_list_store_insert_with_values(aligns, NULL, -1, 0, "Right", -1);
	r = gtk_cell_renderer_combo_new();
	g_object_set(r, "model", aligns, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
	g_signal_connect(r, "edited", G_CALLBACK(col_edited), GINT_TO_POINTER(CC_ALIGN));
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes("Align", r, "text",
											      CC_ALIGN, NULL));
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(sw), tv);
	gtk_box_pack_start(GTK_BOX(area), sw, TRUE, TRUE, 0);

	GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(bb, 8);
	struct {
		const char *label;
		GCallback cb;
	} buttons[] = {
		{"_Add", G_CALLBACK(col_add)}, {"_Remove", G_CALLBACK(col_remove)},
		{"Move _up", G_CALLBACK(col_up)}, {"Move _down", G_CALLBACK(col_down)},
		{"Re_set to defaults", G_CALLBACK(col_reset)},
	};
	for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
		GtkWidget *b = gtk_button_new_with_mnemonic(buttons[i].label);
		g_signal_connect(b, "clicked", buttons[i].cb, tv);
		gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(area), bb, FALSE, FALSE, 0);
	GtkWidget *help = gtk_label_new("Fields: %title% %artist% %album artist% %album% %tracknumber% %discnumber% "
					"%date% %year% %genre% %length% %codec% %bitrate% %samplerate% %channels% "
					"%bitspersample% %filename% %filename_ext% %directoryname% %path% "
					"%replaygain_track_gain% %replaygain_album_gain% %list_index% %isplaying%.  "
					"Functions: $if(c,a,b) $if2(a,b) $num(n,len) $upper(s) $lower(s) $left(s,n). "
					"[ ... ] hides its text when its fields are empty; 'quote' literal characters.");
	gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
	gtk_label_set_xalign(GTK_LABEL(help), 0);
	gtk_label_set_selectable(GTK_LABEL(help), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(help), "dim-label");
	gtk_widget_set_margin_top(help, 8);
	gtk_box_pack_start(GTK_BOX(area), help, FALSE, FALSE, 0);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show_all(d);
}

/* ---- equalizer ------------------------------------------------------------ */

static GtkWidget *eq_scales[10];

static void eq_changed(GtkRange *r, gpointer data)
{
	app->eq[GPOINTER_TO_INT(data)] = round(gtk_range_get_value(r) * 2) / 2;
	app_apply_audio_settings();
}

static void eq_toggled(GtkToggleButton *t, gpointer data)
{
	(void)data;
	app->eq_on = gtk_toggle_button_get_active(t);
	app_apply_audio_settings();
}

static void eq_reset(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	for (int i = 0; i < 10; i++)
		gtk_range_set_value(GTK_RANGE(eq_scales[i]), 0);
}

void dialog_equalizer(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Equalizer", GTK_WINDOW(app->window),
						   GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(area), 12);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *on = gtk_check_button_new_with_mnemonic("_Enabled");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(on), app->eq_on);
	g_signal_connect(on, "toggled", G_CALLBACK(eq_toggled), NULL);
	gtk_box_pack_start(GTK_BOX(top), on, FALSE, FALSE, 0);
	GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset");
	g_signal_connect(reset, "clicked", G_CALLBACK(eq_reset), NULL);
	gtk_box_pack_end(GTK_BOX(top), reset, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(area), top, FALSE, FALSE, 0);

	GtkWidget *bands = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_margin_top(bands, 12);
	for (int i = 0; i < 10; i++) {
		GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -12, 12, 0.5);
		gtk_range_set_inverted(GTK_RANGE(s), TRUE);
		gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_BOTTOM);
		gtk_scale_add_mark(GTK_SCALE(s), 0, GTK_POS_LEFT, NULL);
		gtk_widget_set_size_request(s, 44, 240);
		gtk_range_set_value(GTK_RANGE(s), app->eq[i]);
		g_signal_connect(s, "value-changed", G_CALLBACK(eq_changed), GINT_TO_POINTER(i));
		eq_scales[i] = s;
		char label[16];
		int f = player_eq_bands[i];
		if (f >= 1000)
			g_snprintf(label, sizeof label, "%dk", f / 1000);
		else
			g_snprintf(label, sizeof label, "%d", f);
		gtk_box_pack_start(GTK_BOX(col), s, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(col), gtk_label_new(label), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(bands), col, TRUE, TRUE, 0);
	}
	gtk_box_pack_start(GTK_BOX(area), bands, TRUE, TRUE, 0);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show_all(d);
}

void dialog_about(void)
{
	gtk_show_about_dialog(GTK_WINDOW(app->window), "program-name", "music-player", "version", "1.0",
			      "comments", "A foobar2000-style music player: GTK3, libmpv, ffmpeg and SQLite.",
			      "license-type", GTK_LICENSE_MIT_X11, "website",
			      "https://github.com/r3clusionn/music-player", NULL);
}

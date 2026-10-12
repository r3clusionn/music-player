/* Playlists: tabs of column lists. The list model is a thin GtkTreeModel
 * over the playlist's track array, so 100,000 rows cost one pointer each and
 * cells are formatted only when drawn (fixed-height mode). */
#include "app.h"

#include <math.h>
#include <string.h>

/* ---- the model ---------------------------------------------------------- */

#define PL_TYPE_MODEL (pl_model_get_type())
G_DECLARE_FINAL_TYPE(PlModel, pl_model, PL, MODEL, GObject)

struct _PlModel {
	GObject parent;
	Playlist *pl;
	int stamp;
};

static void pl_model_iface_init(GtkTreeModelIface *iface);
G_DEFINE_TYPE_WITH_CODE(PlModel, pl_model, G_TYPE_OBJECT,
			G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, pl_model_iface_init))

static void pl_model_init(PlModel *m)
{
	m->stamp = g_random_int();
}

static void pl_model_class_init(PlModelClass *k)
{
	(void)k;
}

static GtkTreeModelFlags m_flags(GtkTreeModel *m)
{
	(void)m;
	return GTK_TREE_MODEL_LIST_ONLY;
}

static int m_n_columns(GtkTreeModel *m)
{
	(void)m;
	return 1;
}

static GType m_column_type(GtkTreeModel *m, int i)
{
	(void)m;
	(void)i;
	return G_TYPE_POINTER;
}

static gboolean set_iter(PlModel *m, GtkTreeIter *it, int i)
{
	if (i < 0 || i >= (int)m->pl->tracks->len) {
		it->stamp = 0;
		return FALSE;
	}
	it->stamp = m->stamp;
	it->user_data = GINT_TO_POINTER(i);
	return TRUE;
}

static gboolean m_get_iter(GtkTreeModel *tm, GtkTreeIter *it, GtkTreePath *path)
{
	if (gtk_tree_path_get_depth(path) != 1)
		return FALSE;
	return set_iter(PL_MODEL(tm), it, gtk_tree_path_get_indices(path)[0]);
}

static GtkTreePath *m_get_path(GtkTreeModel *tm, GtkTreeIter *it)
{
	(void)tm;
	return gtk_tree_path_new_from_indices(GPOINTER_TO_INT(it->user_data), -1);
}

static void m_get_value(GtkTreeModel *tm, GtkTreeIter *it, int col, GValue *v)
{
	(void)col;
	PlModel *m = PL_MODEL(tm);
	g_value_init(v, G_TYPE_POINTER);
	int i = GPOINTER_TO_INT(it->user_data);
	if (i < (int)m->pl->tracks->len)
		g_value_set_pointer(v, g_ptr_array_index(m->pl->tracks, i));
}

static gboolean m_iter_next(GtkTreeModel *tm, GtkTreeIter *it)
{
	return set_iter(PL_MODEL(tm), it, GPOINTER_TO_INT(it->user_data) + 1);
}

static gboolean m_iter_previous(GtkTreeModel *tm, GtkTreeIter *it)
{
	return set_iter(PL_MODEL(tm), it, GPOINTER_TO_INT(it->user_data) - 1);
}

static gboolean m_iter_children(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *parent)
{
	return parent ? FALSE : set_iter(PL_MODEL(tm), it, 0);
}

static gboolean m_iter_has_child(GtkTreeModel *tm, GtkTreeIter *it)
{
	(void)tm;
	(void)it;
	return FALSE;
}

static int m_iter_n_children(GtkTreeModel *tm, GtkTreeIter *it)
{
	return it ? 0 : (int)PL_MODEL(tm)->pl->tracks->len;
}

static gboolean m_iter_nth_child(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *parent, int n)
{
	return parent ? FALSE : set_iter(PL_MODEL(tm), it, n);
}

static gboolean m_iter_parent(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *child)
{
	(void)tm;
	(void)it;
	(void)child;
	return FALSE;
}

static void pl_model_iface_init(GtkTreeModelIface *i)
{
	i->get_flags = m_flags;
	i->get_n_columns = m_n_columns;
	i->get_column_type = m_column_type;
	i->get_iter = m_get_iter;
	i->get_path = m_get_path;
	i->get_value = m_get_value;
	i->iter_next = m_iter_next;
	i->iter_previous = m_iter_previous;
	i->iter_children = m_iter_children;
	i->iter_has_child = m_iter_has_child;
	i->iter_n_children = m_iter_n_children;
	i->iter_nth_child = m_iter_nth_child;
	i->iter_parent = m_iter_parent;
}

static void model_row_inserted(Playlist *pl, int i)
{
	GtkTreeIter it;
	set_iter(PL_MODEL(pl->model), &it, i);
	GtkTreePath *path = gtk_tree_path_new_from_indices(i, -1);
	gtk_tree_model_row_inserted(pl->model, path, &it);
	gtk_tree_path_free(path);
}

void playlist_row_changed(Playlist *pl, int i)
{
	GtkTreeIter it;
	if (!pl || !set_iter(PL_MODEL(pl->model), &it, i))
		return;
	GtkTreePath *path = gtk_tree_path_new_from_indices(i, -1);
	gtk_tree_model_row_changed(pl->model, path, &it);
	gtk_tree_path_free(path);
}

/* ---- columns ------------------------------------------------------------ */

Column *column_new(const char *name, const char *format, int width, float align, gboolean visible)
{
	Column *c = g_new0(Column, 1);
	c->name = g_strdup(name);
	c->format_src = g_strdup(format);
	c->format = format_compile(format);
	c->width = width;
	c->align = align;
	c->visible = visible;
	return c;
}

void column_free(Column *c)
{
	g_free(c->name);
	g_free(c->format_src);
	format_free(c->format);
	g_free(c);
}

static const struct {
	const char *name, *format;
	int width;
	float align;
	gboolean visible;
} default_columns[] = {
	{"Playing", "$if(%isplaying%,%isplaying%,$if(%queue_index%,'('%queue_index%')',))", 34, 0.5, TRUE},
	{"Artist/album", "%album artist% - %album%", 300, 0, TRUE},
	{"Track no", "%tracknumber%", 80, 1, TRUE},
	{"Title / track artist", "%title%[ // %track artist%]", 360, 0, TRUE},
	{"Duration", "%length%", 86, 1, TRUE},
	{"Date", "%date%", 60, 0, FALSE},
	{"Genre", "%genre%", 120, 0, FALSE},
	{"Codec", "%codec%", 70, 0, FALSE},
	{"Bitrate", "[%bitrate% kbps]", 80, 1, FALSE},
	{"Sample rate", "[%samplerate% Hz]", 90, 1, FALSE},
	{"File name", "%filename_ext%", 260, 0, FALSE},
	{"Track gain", "%replaygain_track_gain%", 90, 1, FALSE},
	{"Album gain", "%replaygain_album_gain%", 90, 1, FALSE},
};

void columns_load(void)
{
	app->columns = g_ptr_array_new_with_free_func((GDestroyNotify)column_free);
	gsize n = 0;
	char **names = g_key_file_get_string_list(app->cfg, "columns", "names", &n, NULL);
	char **formats = g_key_file_get_string_list(app->cfg, "columns", "formats", NULL, NULL);
	gsize nw = 0, na = 0, nv = 0;
	int *widths = g_key_file_get_integer_list(app->cfg, "columns", "widths", &nw, NULL);
	double *aligns = g_key_file_get_double_list(app->cfg, "columns", "aligns", &na, NULL);
	gboolean *vis = g_key_file_get_boolean_list(app->cfg, "columns", "visible", &nv, NULL);
	if (names && formats && n && g_strv_length(formats) == n && nw == n && na == n && nv == n) {
		for (gsize i = 0; i < n; i++)
			g_ptr_array_add(app->columns, column_new(names[i], formats[i], widths[i], aligns[i], vis[i]));
	} else {
		for (size_t i = 0; i < G_N_ELEMENTS(default_columns); i++)
			g_ptr_array_add(app->columns,
					column_new(default_columns[i].name, default_columns[i].format,
						   default_columns[i].width, default_columns[i].align,
						   default_columns[i].visible));
	}
	g_strfreev(names);
	g_strfreev(formats);
	g_free(widths);
	g_free(aligns);
	g_free(vis);
}

void columns_save(void)
{
	guint n = app->columns->len;
	const char **names = g_new0(const char *, n + 1), **formats = g_new0(const char *, n + 1);
	int *widths = g_new(int, n);
	double *aligns = g_new(double, n);
	gboolean *vis = g_new(gboolean, n);
	for (guint i = 0; i < n; i++) {
		Column *c = g_ptr_array_index(app->columns, i);
		names[i] = c->name;
		formats[i] = c->format_src;
		widths[i] = c->width;
		aligns[i] = c->align;
		vis[i] = c->visible;
	}
	g_key_file_set_string_list(app->cfg, "columns", "names", names, n);
	g_key_file_set_string_list(app->cfg, "columns", "formats", formats, n);
	g_key_file_set_integer_list(app->cfg, "columns", "widths", widths, n);
	g_key_file_set_double_list(app->cfg, "columns", "aligns", aligns, n);
	g_key_file_set_boolean_list(app->cfg, "columns", "visible", vis, n);
	g_free(names);
	g_free(formats);
	g_free(widths);
	g_free(aligns);
	g_free(vis);
}

static void cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *tm, GtkTreeIter *it,
		      gpointer data)
{
	(void)col;
	Column *c = data;
	PlModel *m = PL_MODEL(tm);
	int i = GPOINTER_TO_INT(it->user_data);
	if (i >= (int)m->pl->tracks->len)
		return;
	Track *t = g_ptr_array_index(m->pl->tracks, i);
	FormatContext ctx = {.list_index = i + 1};
	gboolean playing = m->pl == app->playing_pl && i == app->playing_index &&
			   player_current(app->player) == t;
	if (playing)
		ctx.playing = player_state(app->player) == PLAYER_PAUSED ? 2 : 1;
	ctx.queued = app_queue_position(m->pl, t);
	g_object_set(cell, "text", format_eval_static(c->format, t, &ctx),
		     "weight", playing ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
}

static void on_column_width(GObject *obj, GParamSpec *ps, gpointer data)
{
	(void)ps;
	Column *c = data;
	int w = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
	if (w > 8 && w != c->width) {
		c->width = w;
		columns_save();
	}
}

static void header_clicked(GtkTreeViewColumn *col, gpointer data)
{
	Column *c = data;
	Playlist *pl = g_object_get_data(G_OBJECT(col), "playlist");
	int idx = -1;
	for (guint i = 0; i < app->columns->len; i++)
		if (g_ptr_array_index(app->columns, i) == c)
			idx = (int)i;
	gboolean desc = pl->sort_column == idx ? !pl->sort_desc : FALSE;
	pl->sort_column = idx;
	pl->sort_desc = desc;
	playlist_sort_by_format(pl, c->format_src, desc);
}

static void header_menu_toggle(GtkCheckMenuItem *item, gpointer data)
{
	Column *c = data;
	c->visible = gtk_check_menu_item_get_active(item);
	columns_save();
	columns_rebuild();
}

static gboolean header_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	(void)w;
	(void)data;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	GtkWidget *menu = gtk_menu_new();
	for (guint i = 0; i < app->columns->len; i++) {
		Column *c = g_ptr_array_index(app->columns, i);
		GtkWidget *item = gtk_check_menu_item_new_with_label(c->name);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), c->visible);
		g_signal_connect(item, "toggled", G_CALLBACK(header_menu_toggle), c);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	GtkWidget *edit = gtk_menu_item_new_with_label("Edit columns…");
	g_signal_connect_swapped(edit, "activate", G_CALLBACK(dialog_columns), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit);
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

static void build_columns(Playlist *pl)
{
	GtkTreeView *tv = GTK_TREE_VIEW(pl->view);
	GList *old = gtk_tree_view_get_columns(tv);
	for (GList *l = old; l; l = l->next)
		gtk_tree_view_remove_column(tv, l->data);
	g_list_free(old);
	for (guint i = 0; i < app->columns->len; i++) {
		Column *c = g_ptr_array_index(app->columns, i);
		if (!c->visible)
			continue;
		GtkCellRenderer *r = gtk_cell_renderer_text_new();
		g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", c->align, "ypad", 1, NULL);
		GtkTreeViewColumn *col = gtk_tree_view_column_new();
		/* A narrow column (the playing marker) gets no header text rather than a cut-off one. */
		gtk_tree_view_column_set_title(col, c->width < 50 ? "" : c->name);
		gtk_tree_view_column_pack_start(col, r, TRUE);
		gtk_tree_view_column_set_cell_data_func(col, r, cell_data, c, NULL);
		gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(col, c->width);
		gtk_tree_view_column_set_resizable(col, TRUE);
		gtk_tree_view_column_set_clickable(col, TRUE);
		gtk_tree_view_column_set_alignment(col, c->align);
		g_object_set_data(G_OBJECT(col), "playlist", pl);
		g_signal_connect(col, "clicked", G_CALLBACK(header_clicked), c);
		g_signal_connect(col, "notify::width", G_CALLBACK(on_column_width), c);
		gtk_tree_view_append_column(tv, col);
		GtkWidget *button = gtk_tree_view_column_get_button(col);
		g_signal_connect(button, "button-press-event", G_CALLBACK(header_button), NULL);
	}
	/* A last empty column takes the leftover width. */
	GtkTreeViewColumn *filler = gtk_tree_view_column_new();
	gtk_tree_view_column_set_sizing(filler, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_expand(filler, TRUE);
	gtk_tree_view_append_column(tv, filler);
}

void columns_rebuild(void)
{
	for (guint i = 0; i < app->playlists->len; i++)
		build_columns(g_ptr_array_index(app->playlists, i));
}

void playlist_redraw_all(void)
{
	for (guint i = 0; i < app->playlists->len; i++) {
		Playlist *pl = g_ptr_array_index(app->playlists, i);
		if (pl->view)
			gtk_widget_queue_draw(pl->view);
	}
}

/* ---- playlist operations ------------------------------------------------ */

static gboolean save_now(gpointer data)
{
	Playlist *pl = data;
	pl->save_id = 0;
	library_playlist_save(app->lib, pl->id, pl->tracks);
	return G_SOURCE_REMOVE;
}

void playlist_save_soon(Playlist *pl)
{
	if (!pl->save_id)
		pl->save_id = g_timeout_add_seconds(2, save_now, pl);
}

static void invalidate_shuffle(Playlist *pl)
{
	if (pl->shuffle) {
		g_array_unref(pl->shuffle);
		pl->shuffle = NULL;
	}
}

int playlist_index_of(Playlist *pl, Track *t, int hint)
{
	if (!pl || !t)
		return -1;
	if (hint >= 0 && hint < (int)pl->tracks->len && g_ptr_array_index(pl->tracks, hint) == t)
		return hint;
	for (guint i = 0; i < pl->tracks->len; i++)
		if (g_ptr_array_index(pl->tracks, i) == t)
			return (int)i;
	return -1;
}

static void fix_playing_index(Playlist *pl)
{
	if (app->playing_pl == pl)
		app->playing_index = playlist_index_of(pl, player_current(app->player), app->playing_index);
	if (app->pending_pl == pl)
		app->pending_index = -1;
}

void playlist_add(Playlist *pl, GPtrArray *tracks, int at)
{
	if (!tracks || !tracks->len)
		return;
	if (at < 0 || at > (int)pl->tracks->len)
		at = (int)pl->tracks->len;
	for (guint i = 0; i < tracks->len; i++) {
		g_ptr_array_insert(pl->tracks, at + (int)i, track_ref(g_ptr_array_index(tracks, i)));
		model_row_inserted(pl, at + (int)i);
	}
	invalidate_shuffle(pl);
	fix_playing_index(pl);
	playlist_save_soon(pl);
	if (pl == app->playing_pl)
		app_requeue();
	app_update_status();
}

/* Replace the view's model to show big changes (removals, sorts) in one go. */
void playlist_changed(Playlist *pl, gboolean keep_selection)
{
	GtkTreeView *tv = GTK_TREE_VIEW(pl->view);
	GHashTable *sel = NULL;
	if (keep_selection) {
		GPtrArray *s = playlist_selected(pl);
		sel = g_hash_table_new(NULL, NULL);
		for (guint i = 0; i < s->len; i++)
			g_hash_table_add(sel, g_ptr_array_index(s, i));
		g_ptr_array_unref(s);
	}
	GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
	double scroll = gtk_adjustment_get_value(adj);
	PL_MODEL(pl->model)->stamp++;
	gtk_tree_view_set_model(tv, NULL);
	gtk_tree_view_set_model(tv, pl->model);
	if (sel) {
		GtkTreeSelection *ts = gtk_tree_view_get_selection(tv);
		for (guint i = 0; i < pl->tracks->len; i++)
			if (g_hash_table_contains(sel, g_ptr_array_index(pl->tracks, i))) {
				GtkTreePath *p = gtk_tree_path_new_from_indices((int)i, -1);
				gtk_tree_selection_select_path(ts, p);
				gtk_tree_path_free(p);
			}
		g_hash_table_unref(sel);
	}
	gtk_adjustment_set_value(adj, scroll);
	invalidate_shuffle(pl);
	fix_playing_index(pl);
	playlist_save_soon(pl);
	if (pl == app->playing_pl)
		app_requeue();
	app_update_status();
}

void playlist_clear(Playlist *pl)
{
	g_ptr_array_set_size(pl->tracks, 0);
	playlist_changed(pl, FALSE);
}

GPtrArray *playlist_selected(Playlist *pl)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(pl->view));
	GList *rows = gtk_tree_selection_get_selected_rows(ts, NULL);
	for (GList *l = rows; l; l = l->next) {
		int i = gtk_tree_path_get_indices(l->data)[0];
		if (i < (int)pl->tracks->len)
			g_ptr_array_add(out, track_ref(g_ptr_array_index(pl->tracks, i)));
	}
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return out;
}

static GArray *selected_indices(Playlist *pl)
{
	GArray *a = g_array_new(FALSE, FALSE, sizeof(int));
	GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(pl->view));
	GList *rows = gtk_tree_selection_get_selected_rows(ts, NULL);
	for (GList *l = rows; l; l = l->next) {
		int i = gtk_tree_path_get_indices(l->data)[0];
		g_array_append_val(a, i);
	}
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return a; /* ascending: GTK returns rows in order */
}

int playlist_cursor(Playlist *pl)
{
	GtkTreePath *p = NULL;
	gtk_tree_view_get_cursor(GTK_TREE_VIEW(pl->view), &p, NULL);
	int i = p ? gtk_tree_path_get_indices(p)[0] : -1;
	if (p)
		gtk_tree_path_free(p);
	return i;
}

void playlist_remove_selected(Playlist *pl)
{
	GArray *idx = selected_indices(pl);
	if (!idx->len) {
		g_array_unref(idx);
		return;
	}
	int first = g_array_index(idx, int, 0);
	for (int k = (int)idx->len - 1; k >= 0; k--)
		g_ptr_array_remove_index(pl->tracks, g_array_index(idx, int, k));
	g_array_unref(idx);
	playlist_changed(pl, FALSE);
	if (pl->tracks->len)
		playlist_scroll_to(pl, MIN(first, (int)pl->tracks->len - 1), TRUE);
}

void playlist_scroll_to(Playlist *pl, int index, gboolean select)
{
	if (index < 0 || index >= (int)pl->tracks->len)
		return;
	GtkTreePath *p = gtk_tree_path_new_from_indices(index, -1);
	if (select)
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(pl->view), p, NULL, FALSE);
	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(pl->view), p, NULL, TRUE, 0.4, 0);
	gtk_tree_path_free(p);
}

typedef struct {
	Track *t;
	char *key;
	int pos;
} SortItem;

static gboolean sort_desc_flag;

static int cmp_sort(const void *a, const void *b)
{
	const SortItem *x = a, *y = b;
	int r = strcmp(x->key, y->key); /* collation keys compare bytewise */
	if (sort_desc_flag)
		r = -r;
	return r ? r : x->pos - y->pos; /* stable */
}

void playlist_sort_by_format(Playlist *pl, const char *format, gboolean desc)
{
	Format *f = format_compile(format);
	guint n = pl->tracks->len;
	SortItem *items = g_new(SortItem, n);
	GString *buf = g_string_new(NULL);
	for (guint i = 0; i < n; i++) {
		Track *t = g_ptr_array_index(pl->tracks, i);
		g_string_truncate(buf, 0);
		format_eval(f, t, NULL, buf);
		/* Filename collation sorts digit runs as numbers ("2" before "10"). */
		items[i].t = t;
		items[i].key = g_utf8_collate_key_for_filename(buf->str, -1);
		items[i].pos = (int)i;
	}
	g_string_free(buf, TRUE);
	format_free(f);
	sort_desc_flag = desc;
	qsort(items, n, sizeof *items, cmp_sort);
	for (guint i = 0; i < n; i++) {
		pl->tracks->pdata[i] = items[i].t;
		g_free(items[i].key);
	}
	g_free(items);
	playlist_changed(pl, TRUE);
}

double playlist_total_length(Playlist *pl, int *count)
{
	double total = 0;
	for (guint i = 0; i < pl->tracks->len; i++)
		total += ((Track *)g_ptr_array_index(pl->tracks, i))->duration;
	if (count)
		*count = (int)pl->tracks->len;
	return total;
}

void playlist_export(Playlist *pl, const char *path)
{
	GString *s = g_string_new("#EXTM3U\n");
	for (guint i = 0; i < pl->tracks->len; i++) {
		Track *t = g_ptr_array_index(pl->tracks, i);
		if (t->cue)
			continue; /* a cue range has no file of its own */
		g_string_append_printf(s, "#EXTINF:%d,%s - %s\n%s\n", (int)lround(t->duration),
				       track_album_artist(t), track_display_title(t), t->path);
	}
	g_file_set_contents(path, s->str, (gssize)s->len, NULL);
	g_string_free(s, TRUE);
}

/* ---- the view: input, menus, drag and drop -------------------------------- */

enum { DND_ROWS, DND_TRACK_IDS, DND_URIS };
static const GtkTargetEntry dnd_targets[] = {
	{"music-player/rows", GTK_TARGET_SAME_WIDGET, DND_ROWS},
	{"music-player/track-ids", GTK_TARGET_SAME_APP, DND_TRACK_IDS},
	{"text/uri-list", 0, DND_URIS},
};

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
	(void)tv;
	(void)col;
	app_play_track(data, gtk_tree_path_get_indices(path)[0]);
}

static void menu_play(GtkMenuItem *i, gpointer data)
{
	(void)i;
	Playlist *pl = data;
	int c = playlist_cursor(pl);
	if (c >= 0)
		app_play_track(pl, c);
}

static void menu_remove(GtkMenuItem *i, gpointer data)
{
	(void)i;
	playlist_remove_selected(data);
}

static void menu_crop(GtkMenuItem *i, gpointer data)
{
	(void)i;
	Playlist *pl = data;
	GPtrArray *keep = playlist_selected(pl);
	g_ptr_array_set_size(pl->tracks, 0);
	for (guint k = 0; k < keep->len; k++)
		g_ptr_array_add(pl->tracks, track_ref(g_ptr_array_index(keep, k)));
	g_ptr_array_unref(keep);
	playlist_changed(pl, FALSE);
}

static void menu_queue(GtkMenuItem *i, gpointer data)
{
	(void)i;
	Playlist *pl = data;
	GPtrArray *s = playlist_selected(pl);
	for (guint k = 0; k < s->len; k++)
		app_enqueue(pl, g_ptr_array_index(s, k));
	g_ptr_array_unref(s);
}

static void menu_info(GtkMenuItem *i, gpointer data)
{
	(void)i;
	Playlist *pl = data;
	int c = playlist_cursor(pl);
	if (c >= 0)
		app_show_track_info(g_ptr_array_index(pl->tracks, c));
}

static void menu_open_folder(GtkMenuItem *i, gpointer data)
{
	(void)i;
	Playlist *pl = data;
	int c = playlist_cursor(pl);
	if (c < 0)
		return;
	char *dir = g_path_get_dirname(((Track *)g_ptr_array_index(pl->tracks, c))->path);
	char *uri = g_filename_to_uri(dir, NULL, NULL);
	if (uri)
		gtk_show_uri_on_window(GTK_WINDOW(app->window), uri, GDK_CURRENT_TIME, NULL);
	g_free(uri);
	g_free(dir);
}

static void add_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data, gboolean sensitive)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	g_signal_connect(item, "activate", cb, data);
	gtk_widget_set_sensitive(item, sensitive);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	Playlist *pl = data;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	GtkTreePath *path;
	if (gtk_tree_view_get_path_at_pos(tv, (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL)) {
		GtkTreeSelection *ts = gtk_tree_view_get_selection(tv);
		if (!gtk_tree_selection_path_is_selected(ts, path))
			gtk_tree_view_set_cursor(tv, path, NULL, FALSE);
		gtk_tree_path_free(path);
	}
	gboolean any = gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(tv)) > 0;
	GtkWidget *menu = gtk_menu_new();
	add_item(menu, "_Play", G_CALLBACK(menu_play), pl, any);
	add_item(menu, "Add to playback _queue", G_CALLBACK(menu_queue), pl, any);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	add_item(menu, "_Remove", G_CALLBACK(menu_remove), pl, any);
	add_item(menu, "_Crop to selection", G_CALLBACK(menu_crop), pl, any);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	add_item(menu, "Open containing _folder", G_CALLBACK(menu_open_folder), pl, any);
	add_item(menu, "_Properties", G_CALLBACK(menu_info), pl, any);
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	(void)w;
	Playlist *pl = data;
	guint state = ev->state & gtk_accelerator_get_default_mod_mask();
	if (ev->keyval == GDK_KEY_Delete && !state) {
		playlist_remove_selected(pl);
		return TRUE;
	}
	if (ev->keyval == GDK_KEY_q && !state) {
		menu_queue(NULL, pl);
		return TRUE;
	}
	return FALSE;
}

static void on_selection_changed(GtkTreeSelection *ts, gpointer data)
{
	(void)ts;
	Playlist *pl = data;
	int c = playlist_cursor(pl);
	if (c >= 0 && c < (int)pl->tracks->len)
		app_show_track_info(g_ptr_array_index(pl->tracks, c));
}

static void drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sd, guint info, guint time,
			  gpointer data)
{
	(void)w;
	(void)ctx;
	(void)time;
	Playlist *pl = data;
	if (info != DND_ROWS)
		return;
	GArray *idx = selected_indices(pl);
	gtk_selection_data_set(sd, gdk_atom_intern_static_string("music-player/rows"), 8,
			       (const guchar *)idx->data, (int)(idx->len * sizeof(int)));
	g_array_unref(idx);
}

static int drop_index(GtkTreeView *tv, Playlist *pl, int x, int y)
{
	GtkTreePath *path;
	GtkTreeViewDropPosition pos;
	int at = (int)pl->tracks->len;
	if (gtk_tree_view_get_dest_row_at_pos(tv, x, y, &path, &pos)) {
		at = gtk_tree_path_get_indices(path)[0];
		if (pos == GTK_TREE_VIEW_DROP_AFTER || pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
			at++;
		gtk_tree_path_free(path);
	}
	return at;
}

static void drag_data_received(GtkWidget *w, GdkDragContext *ctx, int x, int y, GtkSelectionData *sd,
			       guint info, guint time, gpointer data)
{
	Playlist *pl = data;
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	int at = drop_index(tv, pl, x, y);
	const guchar *raw = gtk_selection_data_get_data(sd);
	int len = gtk_selection_data_get_length(sd);
	if (info == DND_ROWS && raw && len > 0) {
		/* Move the selected rows to the drop point. */
		int n = len / (int)sizeof(int);
		const int *rows = (const int *)raw;
		GPtrArray *moved = g_ptr_array_new();
		for (int k = n - 1; k >= 0; k--) {
			if (rows[k] < at)
				at--;
			g_ptr_array_insert(moved, 0, g_ptr_array_steal_index(pl->tracks, rows[k]));
		}
		for (guint k = 0; k < moved->len; k++)
			g_ptr_array_insert(pl->tracks, at + (int)k, g_ptr_array_index(moved, k));
		g_ptr_array_unref(moved);
		playlist_changed(pl, TRUE);
	} else if (info == DND_TRACK_IDS && raw && len > 0) {
		char *text = g_strndup((const char *)raw, len);
		char **ids = g_strsplit(text, ",", -1);
		GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
		for (char **id = ids; *id; id++) {
			Track *t = library_track(app->lib, g_ascii_strtoll(*id, NULL, 10));
			if (t)
				g_ptr_array_add(tracks, t);
		}
		playlist_add(pl, tracks, at);
		g_ptr_array_unref(tracks);
		g_strfreev(ids);
		g_free(text);
	} else if (info == DND_URIS) {
		char **uris = gtk_selection_data_get_uris(sd);
		GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
		for (char **u = uris; u && *u; u++) {
			char *path = g_filename_from_uri(*u, NULL, NULL);
			if (!path)
				continue;
			GPtrArray *t = g_file_test(path, G_FILE_TEST_IS_DIR)
				? library_tracks_for_folder(app->lib, path)
				: library_tracks_for_file(app->lib, path);
			for (guint k = 0; k < t->len; k++)
				g_ptr_array_add(tracks, track_ref(g_ptr_array_index(t, k)));
			g_ptr_array_unref(t);
			g_free(path);
		}
		g_strfreev(uris);
		playlist_add(pl, tracks, at);
		g_ptr_array_unref(tracks);
	}
	gtk_drag_finish(ctx, TRUE, FALSE, time);
	g_signal_stop_emission_by_name(w, "drag-data-received");
}

static void on_tab_label_button(GtkWidget *w, GdkEventButton *ev, gpointer data);

static GtkWidget *tab_label(Playlist *pl)
{
	GtkWidget *box = gtk_event_box_new();
	GtkWidget *label = gtk_label_new(pl->name);
	gtk_container_add(GTK_CONTAINER(box), label);
	g_object_set_data(G_OBJECT(box), "label", label);
	g_signal_connect(box, "button-press-event", G_CALLBACK(on_tab_label_button), pl);
	gtk_widget_show_all(box);
	return box;
}

static Playlist *create(gint64 id, const char *name, GPtrArray *tracks)
{
	Playlist *pl = g_new0(Playlist, 1);
	pl->id = id;
	pl->name = g_strdup(name);
	pl->tracks = tracks ? tracks : g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	pl->sort_column = -1;
	PlModel *m = g_object_new(PL_TYPE_MODEL, NULL);
	m->pl = pl;
	pl->model = GTK_TREE_MODEL(m);

	pl->view = gtk_tree_view_new_with_model(pl->model);
	GtkTreeView *tv = GTK_TREE_VIEW(pl->view);
	gtk_tree_view_set_fixed_height_mode(tv, TRUE);
	gtk_tree_view_set_enable_search(tv, FALSE);
	gtk_tree_view_set_rubber_banding(tv, TRUE);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_MULTIPLE);
	build_columns(pl);
	g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), pl);
	g_signal_connect(tv, "button-press-event", G_CALLBACK(on_button), pl);
	g_signal_connect(tv, "key-press-event", G_CALLBACK(on_key), pl);
	g_signal_connect(gtk_tree_view_get_selection(tv), "changed", G_CALLBACK(on_selection_changed), pl);
	gtk_tree_view_enable_model_drag_source(tv, GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_MOVE);
	gtk_tree_view_enable_model_drag_dest(tv, dnd_targets, G_N_ELEMENTS(dnd_targets),
					     GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(tv, "drag-data-get", G_CALLBACK(drag_data_get), pl);
	g_signal_connect(tv, "drag-data-received", G_CALLBACK(drag_data_received), pl);

	pl->scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(pl->scroller), pl->view);
	gtk_widget_show_all(pl->scroller);
	g_object_set_data(G_OBJECT(pl->scroller), "playlist", pl);
	gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), pl->scroller, tab_label(pl));
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app->notebook), pl->scroller, TRUE);
	g_ptr_array_add(app->playlists, pl);
	return pl;
}

Playlist *playlist_new(const char *name, gboolean select)
{
	Playlist *pl = create(library_playlist_create(app->lib, name, (int)app->playlists->len), name, NULL);
	if (select)
		playlist_select(pl);
	return pl;
}

Playlist *playlist_active(void)
{
	int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
	if (page < 0)
		return NULL;
	GtkWidget *w = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), page);
	return g_object_get_data(G_OBJECT(w), "playlist");
}

void playlist_select(Playlist *pl)
{
	int page = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), pl->scroller);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), page);
}

Playlist *playlist_find_or_create(const char *name)
{
	for (guint i = 0; i < app->playlists->len; i++) {
		Playlist *pl = g_ptr_array_index(app->playlists, i);
		if (!strcmp(pl->name, name))
			return pl;
	}
	return playlist_new(name, FALSE);
}

void playlist_close(Playlist *pl)
{
	if (pl->save_id)
		g_source_remove(pl->save_id);
	library_playlist_delete(app->lib, pl->id);
	if (app->playing_pl == pl) {
		app->playing_pl = NULL;
		app->playing_index = -1;
	}
	if (app->pending_pl == pl)
		app->pending_pl = NULL;
	/* Queue entries from this playlist go too. */
	for (GList *l = app->queue->head; l;) {
		GList *next = l->next;
		QueueItem *q = l->data;
		if (q->pl == pl) {
			track_unref(q->track);
			g_free(q);
			g_queue_delete_link(app->queue, l);
		}
		l = next;
	}
	g_ptr_array_remove(app->playlists, pl);
	gtk_widget_destroy(pl->scroller);
	g_object_unref(pl->model);
	g_ptr_array_unref(pl->tracks);
	invalidate_shuffle(pl);
	g_free(pl->name);
	g_free(pl);
	if (!app->playlists->len)
		playlist_new("Default", TRUE);
	app_requeue();
}

static void rename_response(GtkDialog *d, int response, gpointer data)
{
	Playlist *pl = data;
	if (response == GTK_RESPONSE_OK) {
		GtkEntry *e = g_object_get_data(G_OBJECT(d), "entry");
		const char *name = gtk_entry_get_text(e);
		if (*name) {
			g_free(pl->name);
			pl->name = g_strdup(name);
			library_playlist_rename(app->lib, pl->id, name);
			GtkWidget *box = gtk_notebook_get_tab_label(GTK_NOTEBOOK(app->notebook), pl->scroller);
			gtk_label_set_text(g_object_get_data(G_OBJECT(box), "label"), name);
		}
	}
	gtk_widget_destroy(GTK_WIDGET(d));
}

void playlist_rename_dialog(Playlist *pl)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Rename playlist", GTK_WINDOW(app->window),
						   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
						   "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_OK, NULL);
	GtkWidget *e = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(e), pl->name);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(area), 12);
	gtk_container_add(GTK_CONTAINER(area), e);
	g_object_set_data(G_OBJECT(d), "entry", e);
	g_signal_connect(d, "response", G_CALLBACK(rename_response), pl);
	gtk_widget_show_all(d);
}

static void tab_menu_rename(GtkMenuItem *i, gpointer data)
{
	(void)i;
	playlist_rename_dialog(data);
}

static void tab_menu_close(GtkMenuItem *i, gpointer data)
{
	(void)i;
	playlist_close(data);
}

static void tab_menu_new(GtkMenuItem *i, gpointer data)
{
	(void)i;
	(void)data;
	char name[32];
	g_snprintf(name, sizeof name, "New playlist (%u)", app->playlists->len + 1);
	playlist_new(name, TRUE);
}

static void on_tab_label_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	(void)w;
	Playlist *pl = data;
	if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
		playlist_rename_dialog(pl);
	} else if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
		playlist_close(pl);
	} else if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
		GtkWidget *menu = gtk_menu_new();
		add_item(menu, "_New playlist", G_CALLBACK(tab_menu_new), NULL, TRUE);
		add_item(menu, "_Rename…", G_CALLBACK(tab_menu_rename), pl, TRUE);
		add_item(menu, "_Close", G_CALLBACK(tab_menu_close), pl, TRUE);
		gtk_widget_show_all(menu);
		gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	}
}

static void on_reordered(GtkNotebook *nb, GtkWidget *child, guint num, gpointer data)
{
	(void)child;
	(void)num;
	(void)data;
	int n = gtk_notebook_get_n_pages(nb);
	gint64 *ids = g_new(gint64, n);
	g_ptr_array_set_size(app->playlists, 0);
	for (int i = 0; i < n; i++) {
		Playlist *pl = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(nb, i)), "playlist");
		g_ptr_array_add(app->playlists, pl);
		ids[i] = pl->id;
	}
	library_playlist_set_order(app->lib, ids, n);
	g_free(ids);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer data)
{
	(void)nb;
	(void)num;
	(void)data;
	Playlist *pl = g_object_get_data(G_OBJECT(page), "playlist");
	if (pl) {
		g_key_file_set_int64(app->cfg, "state", "active_playlist", pl->id);
		app_update_status();
	}
}

void playlists_init(void)
{
	app->playlists = g_ptr_array_new();
	g_signal_connect(app->notebook, "page-reordered", G_CALLBACK(on_reordered), NULL);
	GPtrArray *infos = library_playlists(app->lib);
	for (guint i = 0; i < infos->len; i++) {
		PlaylistInfo *p = g_ptr_array_index(infos, i);
		create(p->id, p->name, library_playlist_tracks(app->lib, p->id));
	}
	g_ptr_array_unref(infos);
	if (!app->playlists->len)
		playlist_new("Default", FALSE);
	gint64 active = g_key_file_get_int64(app->cfg, "state", "active_playlist", NULL);
	for (guint i = 0; i < app->playlists->len; i++) {
		Playlist *pl = g_ptr_array_index(app->playlists, i);
		if (pl->id == active)
			playlist_select(pl);
	}
	g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
}

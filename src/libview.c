/* The library tree: All music > artist > album > track (or by genre, year or
 * folder), filled lazily as nodes open, filtered by the search box. */
#include "app.h"

#include <string.h>

enum { C_LABEL, C_DEPTH, C_K1, C_K2, C_K3, C_ID, C_LOADED, C_N };

typedef struct {
	const char *name;
	int levels;                 /* grouping levels above the tracks */
	const char *expr[3];        /* SQL for each level's key */
	const char *label[3];       /* SQL for each level's label (NULL: the key) */
	const char *order[3];       /* SQL ORDER BY per level */
} ViewMode;

#define ARTIST "coalesce(nullif(albumartist,''),nullif(artist,''),'(no artist)')"
#define TARTIST "coalesce(nullif(artist,''),nullif(albumartist,''),'(no artist)')"
#define ALBUM "coalesce(nullif(album,''),'(no album)')"
#define ALBUM_LABEL ALBUM " || coalesce(' (' || substr(min(date),1,4) || ')','')"
#define GENRE "coalesce(nullif(genre,''),'(no genre)')"
#define YEAR "coalesce(nullif(substr(date,1,4),''),'(no date)')"

static const ViewMode modes[] = {
	{"Album artist / album", 2, {ARTIST, ALBUM}, {NULL, ALBUM_LABEL},
	 {"k COLLATE NOCASE", "min(date), k COLLATE NOCASE"}},
	{"Artist / album", 2, {TARTIST, ALBUM}, {NULL, ALBUM_LABEL},
	 {"k COLLATE NOCASE", "min(date), k COLLATE NOCASE"}},
	{"Genre / album artist / album", 3, {GENRE, ARTIST, ALBUM}, {NULL, NULL, ALBUM_LABEL},
	 {"k COLLATE NOCASE", "k COLLATE NOCASE", "min(date), k COLLATE NOCASE"}},
	{"Year / album", 2, {YEAR, ARTIST " || ' - ' || " ALBUM}, {NULL, NULL},
	 {"k", "k COLLATE NOCASE"}},
	{"Folder", 0, {NULL}, {NULL}, {NULL}},
};
#define MODE_FOLDER 4

static GtkTreeStore *store;
static guint search_id;

static int mode(void)
{
	int m = gtk_combo_box_get_active(GTK_COMBO_BOX(app->lib_mode));
	return m < 0 ? 0 : m;
}

static const char *filter_sql(void)
{
	return library_filtered(app->lib) ? " AND id IN (SELECT id FROM filter)" : "";
}

/* WHERE clause for a node at depth (1-based levels), with its keys bound from 1. */
static GString *where_for(const ViewMode *vm, int depth)
{
	GString *w = g_string_new("in_library");
	g_string_append(w, filter_sql());
	for (int l = 0; l < depth && l < vm->levels; l++)
		g_string_append_printf(w, " AND %s = ?", vm->expr[l]);
	return w;
}

static void add_placeholder(GtkTreeIter *parent)
{
	GtkTreeIter it;
	gtk_tree_store_append(store, &it, parent);
	gtk_tree_store_set(store, &it, C_LABEL, "", C_DEPTH, -1, -1);
}

static void bind_keys(sqlite3_stmt *s, char *const *keys, int n)
{
	for (int i = 0; i < n; i++)
		sqlite3_bind_text(s, i + 1, keys[i], -1, SQLITE_TRANSIENT);
}

/* Children of a node at `depth` (0 = All music) with the given keys. */
static void load_children(GtkTreeIter *parent, int depth, char *const *keys)
{
	const ViewMode *vm = &modes[mode()];
	sqlite3 *db = library_db(app->lib);
	sqlite3_stmt *s;
	GString *where = where_for(vm, depth);
	if (depth < vm->levels) {
		const char *lab = vm->label[depth] ? vm->label[depth] : vm->expr[depth];
		char *sql = g_strdup_printf("SELECT %s AS k, %s, count(*) FROM tracks WHERE %s GROUP BY k ORDER BY %s",
					    vm->expr[depth], lab, where->str, vm->order[depth]);
		sqlite3_prepare_v2(db, sql, -1, &s, NULL);
		g_free(sql);
		bind_keys(s, keys, depth);
		while (sqlite3_step(s) == SQLITE_ROW) {
			GtkTreeIter it;
			const char *k = (const char *)sqlite3_column_text(s, 0);
			gtk_tree_store_append(store, &it, parent);
			gtk_tree_store_set(store, &it, C_LABEL, (const char *)sqlite3_column_text(s, 1),
					   C_DEPTH, depth + 1, C_LOADED, FALSE, -1);
			for (int l = 0; l < depth; l++)
				gtk_tree_store_set(store, &it, C_K1 + l, keys[l], -1);
			gtk_tree_store_set(store, &it, C_K1 + depth, k, -1);
			add_placeholder(&it);
		}
	} else {
		char *sql = g_strdup_printf("SELECT id, tracknumber, coalesce(nullif(title,''), path), discnumber"
					    " FROM tracks WHERE %s ORDER BY discnumber, tracknumber, path", where->str);
		sqlite3_prepare_v2(db, sql, -1, &s, NULL);
		g_free(sql);
		bind_keys(s, keys, depth);
		while (sqlite3_step(s) == SQLITE_ROW) {
			GtkTreeIter it;
			char label[600];
			int n = sqlite3_column_int(s, 1);
			const char *title = (const char *)sqlite3_column_text(s, 2);
			if (!sqlite3_column_bytes(s, 2) || strchr(title, '/') == title) {
				const char *b = strrchr(title, '/');
				title = b ? b + 1 : title;
			}
			if (n > 0)
				g_snprintf(label, sizeof label, "%02d. %s", n, title);
			else
				g_strlcpy(label, title, sizeof label);
			gtk_tree_store_append(store, &it, parent);
			gtk_tree_store_set(store, &it, C_LABEL, label, C_DEPTH, depth + 1,
					   C_ID, sqlite3_column_int64(s, 0), C_LOADED, TRUE, -1);
		}
	}
	sqlite3_finalize(s);
	g_string_free(where, TRUE);
}

/* ---- folder mode: directories from the paths ------------------------------ */

static void load_folder(GtkTreeIter *parent, const char *dir)
{
	sqlite3 *db = library_db(app->lib);
	sqlite3_stmt *s;
	char *sql = g_strdup_printf("SELECT id, path, tracknumber, coalesce(nullif(title,''),'') FROM tracks"
				    " WHERE in_library%s AND path >= ? AND path < ? ORDER BY path, cue",
				    filter_sql());
	sqlite3_prepare_v2(db, sql, -1, &s, NULL);
	g_free(sql);
	char *lo = g_strconcat(dir, "/", NULL), *hi = g_strconcat(dir, "0", NULL); /* '0' follows '/' */
	sqlite3_bind_text(s, 1, lo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(s, 2, hi, -1, SQLITE_TRANSIENT);
	size_t plen = strlen(lo);
	char *last_sub = NULL;
	while (sqlite3_step(s) == SQLITE_ROW) {
		const char *path = (const char *)sqlite3_column_text(s, 1);
		const char *rest = path + plen;
		const char *slash = strchr(rest, '/');
		GtkTreeIter it;
		if (slash) {
			char *sub = g_strndup(rest, slash - rest);
			if (!last_sub || strcmp(sub, last_sub)) {
				char *full = g_build_filename(dir, sub, NULL);
				gtk_tree_store_append(store, &it, parent);
				gtk_tree_store_set(store, &it, C_LABEL, sub, C_DEPTH, 1, C_K1, full, C_LOADED, FALSE, -1);
				add_placeholder(&it);
				g_free(full);
				g_free(last_sub);
				last_sub = sub;
			} else {
				g_free(sub);
			}
		} else {
			const char *title = (const char *)sqlite3_column_text(s, 3);
			int n = sqlite3_column_int(s, 2);
			char label[600];
			if (*title && n > 0)
				g_snprintf(label, sizeof label, "%02d. %s", n, title);
			else
				g_strlcpy(label, *title ? title : rest, sizeof label);
			gtk_tree_store_append(store, &it, parent);
			gtk_tree_store_set(store, &it, C_LABEL, label, C_DEPTH, 2, C_ID, sqlite3_column_int64(s, 0),
					   C_LOADED, TRUE, -1);
		}
	}
	g_free(last_sub);
	g_free(lo);
	g_free(hi);
	sqlite3_finalize(s);
}

static int count_tracks(void)
{
	sqlite3_stmt *s;
	char *sql = g_strdup_printf("SELECT count(*) FROM tracks WHERE in_library%s", filter_sql());
	sqlite3_prepare_v2(library_db(app->lib), sql, -1, &s, NULL);
	g_free(sql);
	int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
	sqlite3_finalize(s);
	return n;
}

void libview_refresh(void)
{
	GtkTreeView *tv = GTK_TREE_VIEW(app->lib_tree);
	gtk_tree_view_set_model(tv, NULL);
	gtk_tree_store_clear(store);
	GtkTreeIter root;
	char label[64];
	int n = count_tracks();
	g_snprintf(label, sizeof label, library_filtered(app->lib) ? "Search results (%d)" : "All music (%d)", n);
	gtk_tree_store_append(store, &root, NULL);
	gtk_tree_store_set(store, &root, C_LABEL, label, C_DEPTH, 0, C_LOADED, TRUE, -1);
	if (mode() == MODE_FOLDER) {
		char **folders = library_folders(app->lib);
		for (char **f = folders; *f; f++) {
			GtkTreeIter it;
			gtk_tree_store_append(store, &it, &root);
			gtk_tree_store_set(store, &it, C_LABEL, *f, C_DEPTH, 1, C_K1, *f, C_LOADED, FALSE, -1);
			add_placeholder(&it);
		}
		g_strfreev(folders);
	} else {
		load_children(&root, 0, NULL);
	}
	gtk_tree_view_set_model(tv, GTK_TREE_MODEL(store));
	GtkTreePath *p = gtk_tree_path_new_first();
	gtk_tree_view_expand_row(tv, p, FALSE);
	/* Few results: open everything. */
	if (library_filtered(app->lib) && n <= 200)
		gtk_tree_view_expand_all(tv);
	gtk_tree_path_free(p);
}

static void on_test_expand(GtkTreeView *tv, GtkTreeIter *it, GtkTreePath *path, gpointer data)
{
	(void)tv;
	(void)path;
	(void)data;
	gboolean loaded;
	int depth;
	char *keys[3] = {NULL, NULL, NULL};
	gtk_tree_model_get(GTK_TREE_MODEL(store), it, C_LOADED, &loaded, C_DEPTH, &depth, C_K1, &keys[0],
			   C_K2, &keys[1], C_K3, &keys[2], -1);
	if (!loaded) {
		GtkTreeIter child;
		while (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, it))
			gtk_tree_store_remove(store, &child);
		if (mode() == MODE_FOLDER)
			load_folder(it, keys[0]);
		else
			load_children(it, depth, keys);
		gtk_tree_store_set(store, it, C_LOADED, TRUE, -1);
	}
	for (int i = 0; i < 3; i++)
		g_free(keys[i]);
}

/* ---- tracks under the selected nodes -------------------------------------- */

static void tracks_of_node(GtkTreeIter *it, GPtrArray *out)
{
	int depth;
	gint64 id;
	char *keys[3] = {NULL, NULL, NULL};
	gtk_tree_model_get(GTK_TREE_MODEL(store), it, C_DEPTH, &depth, C_ID, &id, C_K1, &keys[0], C_K2,
			   &keys[1], C_K3, &keys[2], -1);
	const ViewMode *vm = &modes[mode()];
	sqlite3 *db = library_db(app->lib);
	sqlite3_stmt *s = NULL;
	if (id > 0) {
		Track *t = library_track(app->lib, id);
		if (t)
			g_ptr_array_add(out, t);
	} else if (mode() == MODE_FOLDER && depth >= 1) {
		char *sql = g_strdup_printf("SELECT id FROM tracks WHERE in_library%s AND path >= ? AND path < ?"
					    " ORDER BY path, cue", filter_sql());
		sqlite3_prepare_v2(db, sql, -1, &s, NULL);
		g_free(sql);
		char *lo = g_strconcat(keys[0], "/", NULL), *hi = g_strconcat(keys[0], "0", NULL);
		sqlite3_bind_text(s, 1, lo, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 2, hi, -1, SQLITE_TRANSIENT);
		g_free(lo);
		g_free(hi);
	} else {
		GString *where = where_for(vm, MIN(depth, vm->levels));
		GString *order = g_string_new(NULL);
		for (int l = 0; l < vm->levels; l++)
			g_string_append_printf(order, "%s COLLATE NOCASE, ", vm->expr[l]);
		char *sql = g_strdup_printf("SELECT id FROM tracks WHERE %s ORDER BY %s date, album, discnumber,"
					    " tracknumber, path, cue", where->str, order->str);
		sqlite3_prepare_v2(db, sql, -1, &s, NULL);
		g_free(sql);
		bind_keys(s, keys, MIN(depth, vm->levels));
		g_string_free(where, TRUE);
		g_string_free(order, TRUE);
	}
	if (s) {
		while (sqlite3_step(s) == SQLITE_ROW) {
			Track *t = library_track(app->lib, sqlite3_column_int64(s, 0));
			if (t)
				g_ptr_array_add(out, t);
		}
		sqlite3_finalize(s);
	}
	for (int i = 0; i < 3; i++)
		g_free(keys[i]);
}

static GPtrArray *selected_tracks(void)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->lib_tree));
	GList *rows = gtk_tree_selection_get_selected_rows(ts, NULL);
	for (GList *l = rows; l; l = l->next) {
		/* Skip nodes inside another selected node. */
		gboolean nested = FALSE;
		for (GList *m = rows; m && !nested; m = m->next)
			nested = m != l && gtk_tree_path_is_descendant(l->data, m->data);
		if (nested)
			continue;
		GtkTreeIter it;
		if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, l->data))
			tracks_of_node(&it, out);
	}
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return out;
}

static void send_to(const char *playlist, gboolean replace, gboolean play)
{
	GPtrArray *tracks = selected_tracks();
	if (!tracks->len) {
		g_ptr_array_unref(tracks);
		return;
	}
	Playlist *pl = playlist ? playlist_find_or_create(playlist) : playlist_active();
	int start = replace ? 0 : (int)pl->tracks->len;
	if (replace)
		playlist_clear(pl);
	playlist_add(pl, tracks, -1);
	playlist_select(pl);
	if (play)
		app_play_track(pl, start);
	g_ptr_array_unref(tracks);
}

static void m_play(void) { send_to("Library selection", TRUE, TRUE); }
static void m_send_current(void) { send_to(NULL, TRUE, FALSE); }
static void m_add_current(void) { send_to(NULL, FALSE, FALSE); }

static void m_send_new(void)
{
	GPtrArray *tracks = selected_tracks();
	if (tracks->len) {
		Track *t = g_ptr_array_index(tracks, 0);
		const char *name = t->album && *t->album ? t->album : "New playlist";
		Playlist *pl = playlist_new(name, TRUE);
		playlist_add(pl, tracks, -1);
	}
	g_ptr_array_unref(tracks);
}

static void m_queue(void)
{
	GPtrArray *tracks = selected_tracks();
	for (guint i = 0; i < tracks->len; i++)
		app_enqueue(NULL, g_ptr_array_index(tracks, i));
	g_ptr_array_unref(tracks);
}

static void on_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
	(void)tv;
	(void)path;
	(void)col;
	(void)data;
	m_play();
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	(void)data;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	GtkTreePath *path;
	if (gtk_tree_view_get_path_at_pos(tv, (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL)) {
		if (!gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(tv), path))
			gtk_tree_view_set_cursor(tv, path, NULL, FALSE);
		gtk_tree_path_free(path);
	}
	GtkWidget *menu = gtk_menu_new();
	struct {
		const char *label;
		void (*fn)(void);
	} items[] = {
		{"_Play", m_play},
		{"_Send to current playlist", m_send_current},
		{"_Add to current playlist", m_add_current},
		{"Send to _new playlist", m_send_new},
		{"Add to playback _queue", m_queue},
	};
	for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
		GtkWidget *item = gtk_menu_item_new_with_mnemonic(items[i].label);
		g_signal_connect_swapped(item, "activate", G_CALLBACK(items[i].fn), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

static const GtkTargetEntry drag_targets[] = {
	{"music-player/track-ids", GTK_TARGET_SAME_APP, 1},
};

static void drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sd, guint info, guint time,
			  gpointer data)
{
	(void)w;
	(void)ctx;
	(void)info;
	(void)time;
	(void)data;
	GPtrArray *tracks = selected_tracks();
	GString *s = g_string_new(NULL);
	for (guint i = 0; i < tracks->len; i++)
		g_string_append_printf(s, "%s%" G_GINT64_FORMAT, i ? "," : "",
				       ((Track *)g_ptr_array_index(tracks, i))->id);
	gtk_selection_data_set(sd, gdk_atom_intern_static_string("music-player/track-ids"), 8,
			       (const guchar *)s->str, (int)s->len);
	g_string_free(s, TRUE);
	g_ptr_array_unref(tracks);
}

/* ---- search --------------------------------------------------------------- */

static gboolean run_search(gpointer data)
{
	(void)data;
	search_id = 0;
	library_search(app->lib, gtk_entry_get_text(GTK_ENTRY(app->lib_search)));
	libview_refresh();
	return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *e, gpointer data)
{
	(void)e;
	(void)data;
	if (search_id)
		g_source_remove(search_id);
	search_id = g_timeout_add(120, run_search, NULL);
}

static void on_search_activate(GtkEntry *e, gpointer data)
{
	(void)e;
	(void)data;
	/* Enter plays everything found. */
	GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->lib_tree));
	GtkTreePath *p = gtk_tree_path_new_first();
	gtk_tree_selection_unselect_all(ts);
	gtk_tree_selection_select_path(ts, p);
	gtk_tree_path_free(p);
	m_play();
}

static void on_mode_changed(GtkComboBox *c, gpointer data)
{
	(void)data;
	g_key_file_set_integer(app->cfg, "library", "view", gtk_combo_box_get_active(c));
	libview_refresh();
}

void libview_focus_search(void)
{
	gtk_widget_grab_focus(app->lib_search);
}

GtkWidget *libview_new(void)
{
	store = gtk_tree_store_new(C_N, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				   G_TYPE_INT64, G_TYPE_BOOLEAN);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

	app->lib_search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(app->lib_search), "Search library");
	g_signal_connect(app->lib_search, "search-changed", G_CALLBACK(on_search_changed), NULL);
	g_signal_connect(app->lib_search, "activate", G_CALLBACK(on_search_activate), NULL);
	gtk_box_pack_start(GTK_BOX(box), app->lib_search, FALSE, FALSE, 0);

	app->lib_mode = gtk_combo_box_text_new();
	for (size_t i = 0; i < G_N_ELEMENTS(modes); i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->lib_mode), modes[i].name);
	int m = g_key_file_get_integer(app->cfg, "library", "view", NULL);
	gtk_combo_box_set_active(GTK_COMBO_BOX(app->lib_mode), m >= 0 && m < (int)G_N_ELEMENTS(modes) ? m : 0);
	g_signal_connect(app->lib_mode, "changed", G_CALLBACK(on_mode_changed), NULL);
	gtk_box_pack_start(GTK_BOX(box), app->lib_mode, FALSE, FALSE, 0);

	app->lib_tree = gtk_tree_view_new();
	GtkTreeView *tv = GTK_TREE_VIEW(app->lib_tree);
	gtk_tree_view_set_headers_visible(tv, FALSE);
	gtk_tree_view_set_enable_search(tv, TRUE);
	gtk_tree_view_set_search_column(tv, C_LABEL);
	GtkCellRenderer *r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 0, NULL);
	GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("", r, "text", C_LABEL, NULL);
	gtk_tree_view_append_column(tv, col);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_MULTIPLE);
	g_signal_connect(tv, "test-expand-row", G_CALLBACK(on_test_expand), NULL);
	g_signal_connect(tv, "row-activated", G_CALLBACK(on_activated), NULL);
	g_signal_connect(tv, "button-press-event", G_CALLBACK(on_button), NULL);
	gtk_tree_view_enable_model_drag_source(tv, GDK_BUTTON1_MASK, drag_targets, 1, GDK_ACTION_COPY);
	g_signal_connect(tv, "drag-data-get", G_CALLBACK(drag_data_get), NULL);

	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), app->lib_tree);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	return box;
}

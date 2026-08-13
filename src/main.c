#include <adwaita.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>

typedef struct {
    char *name;
    char *stream_id;
    char *icon;
    char *category_id;
} Channel;

typedef struct { char *name, *id; } Category;
typedef struct { GPtrArray *channels, *categories; } LoadResult;
typedef struct { char *id, *name, *server, *user; } Profile;

typedef struct {
    AdwApplication *app;
    AdwApplicationWindow *window;
    AdwViewStack *stack;
    AdwEntryRow *server;
    AdwEntryRow *playlist_name;
    AdwComboRow *profile_picker;
    AdwEntryRow *username;
    AdwPasswordEntryRow *password;
    GtkButton *connect_button;
    GtkSpinner *spinner;
    AdwStatusPage *empty_page;
    GtkSearchEntry *search;
    GtkDropDown *category_picker;
    GtkListView *channel_list;
    GtkStringList *channel_model;
    GtkStringFilter *channel_filter;
    GtkPicture *video;
    GtkLabel *now_playing;
    GtkButton *play_button;
    GtkButton *mute_button;
    GtkButton *fullscreen_button;
    GtkRevealer *player_controls;
    GtkWidget *video_surface;
    guint controls_timeout;
    AdwToolbarView *player_toolbar;
    GtkWidget *sidebar;
    GtkRevealer *toast_revealer;
    GtkLabel *toast_label;
    GPtrArray *channels;
    GPtrArray *categories;
    GPtrArray *profiles;
    char *profile_id;
    GSettings *interface_settings;
    guint theme_mode;
    GstElement *pipeline;
    gboolean playing;
    gboolean muted;
    gboolean fullscreen;
    char *base_url;
    char *user;
    char *pass;
} App;

typedef struct {
    char *base_url;
    char *user;
    char *pass;
} LoginRequest;

static void channel_free(gpointer data) {
    Channel *channel = data;
    g_free(channel->name);
    g_free(channel->stream_id);
    g_free(channel->icon);
    g_free(channel->category_id);
    g_free(channel);
}

static void category_free(gpointer data) {
    Category *category = data;
    g_free(category->name); g_free(category->id); g_free(category);
}

static void profile_free(gpointer data) {
    Profile *profile = data;
    g_free(profile->id); g_free(profile->name); g_free(profile->server);
    g_free(profile->user); g_free(profile);
}

static void load_result_free(gpointer data) {
    LoadResult *result = data;
    if (result->channels) g_ptr_array_unref(result->channels);
    if (result->categories) g_ptr_array_unref(result->categories);
    g_free(result);
}

static void login_request_free(gpointer data) {
    LoginRequest *request = data;
    g_free(request->base_url);
    g_free(request->user);
    g_free(request->pass);
    g_free(request);
}

static char *normalize_server(const char *server) {
    char *result = g_strdup(server);
    g_strstrip(result);
    while (result[0] && result[strlen(result) - 1] == '/')
        result[strlen(result) - 1] = '\0';
    return result;
}

static void show_message(App *app, const char *message) {
    gtk_label_set_text(app->toast_label, message);
    gtk_revealer_set_reveal_child(app->toast_revealer, TRUE);
}

static gboolean hide_message(gpointer data) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(data), FALSE);
    return G_SOURCE_REMOVE;
}

static void flash_message(App *app, const char *message) {
    show_message(app, message);
    g_timeout_add_seconds(4, hide_message, app->toast_revealer);
}

static JsonParser *fetch_api(LoginRequest *request, const char *action, GError **error) {
    g_autofree char *cache_key_source = g_strdup_printf("%s|%s|%s", request->base_url, request->user, action);
    g_autofree char *cache_key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cache_key_source, -1);
    g_autofree char *cache_dir = g_build_filename(g_get_user_cache_dir(), "xtream-player", NULL);
    g_autofree char *cache_path = g_strdup_printf("%s/%s.json", cache_dir, cache_key);
    g_autofree char *cached_data = NULL;
    gsize cached_length = 0;
    GStatBuf cache_stat;
    gboolean have_cache = g_file_get_contents(cache_path, &cached_data, &cached_length, NULL);
    gboolean cache_fresh = have_cache && g_stat(cache_path, &cache_stat) == 0 &&
        time(NULL) - cache_stat.st_mtime < 6 * 60 * 60;
    if (cache_fresh) {
        JsonParser *cached = json_parser_new();
        if (json_parser_load_from_data(cached, cached_data, (gssize)cached_length, NULL)) {
            return cached;
        }
        g_object_unref(cached);
    }
    g_autoptr(SoupSession) session = soup_session_new();
    g_object_set(session, "timeout", 20, NULL);
    g_autofree char *escaped_user = g_uri_escape_string(request->user, NULL, TRUE);
    g_autofree char *escaped_pass = g_uri_escape_string(request->pass, NULL, TRUE);
    g_autofree char *url = g_strdup_printf(
        "%s/player_api.php?username=%s&password=%s&action=%s",
        request->base_url, escaped_user, escaped_pass, action);
    g_autoptr(SoupMessage) message = soup_message_new("GET", url);
    if (!message) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The server address is invalid.");
        return NULL;
    }

    soup_message_headers_append(soup_message_get_request_headers(message), "User-Agent", "XtreamPlayer/0.1");
    g_autoptr(GBytes) bytes = soup_session_send_and_read(session, message, NULL, error);
    if (!bytes) {
        if (have_cache) {
            g_clear_error(error);
            JsonParser *cached = json_parser_new();
            if (json_parser_load_from_data(cached, cached_data, (gssize)cached_length, error)) {
                return cached;
            }
            g_object_unref(cached);
        }
        return NULL;
    }
    guint status = soup_message_get_status(message);
    if (status < 200 || status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Server returned HTTP %u.", status);
        return NULL;
    }

    gsize length;
    const char *body = g_bytes_get_data(bytes, &length);
    g_mkdir_with_parents(cache_dir, 0700);
    g_file_set_contents(cache_path, body, (gssize)length, NULL);
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, (gssize)length, error)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Login failed or the provider returned an unexpected response.");
        g_object_unref(parser);
        return NULL;
    }
    return parser;
}

static LoadResult *fetch_channels(LoginRequest *request, GError **error) {
    g_autoptr(JsonParser) channel_parser = fetch_api(request, "get_live_streams", error);
    if (!channel_parser) return NULL;
    g_autoptr(JsonParser) category_parser = fetch_api(request, "get_live_categories", error);
    if (!category_parser) return NULL;

    LoadResult *result = g_new0(LoadResult, 1);
    result->channels = g_ptr_array_new_with_free_func(channel_free);
    result->categories = g_ptr_array_new_with_free_func(category_free);
    JsonArray *array = json_node_get_array(json_parser_get_root(channel_parser));
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonObject *item = json_array_get_object_element(array, i);
        if (!item || !json_object_has_member(item, "stream_id")) continue;
        Channel *channel = g_new0(Channel, 1);
        channel->name = g_strdup(json_object_get_string_member_with_default(item, "name", "Untitled channel"));
        channel->stream_id = g_strdup_printf("%" G_GINT64_FORMAT,
                                             json_object_get_int_member(item, "stream_id"));
        channel->icon = g_strdup(json_object_get_string_member_with_default(item, "stream_icon", ""));
        channel->category_id = g_strdup(json_object_get_string_member_with_default(item, "category_id", ""));
        g_ptr_array_add(result->channels, channel);
    }
    array = json_node_get_array(json_parser_get_root(category_parser));
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonObject *item = json_array_get_object_element(array, i);
        if (!item) continue;
        Category *category = g_new0(Category, 1);
        category->name = g_strdup(json_object_get_string_member_with_default(item, "category_name", "Other"));
        category->id = g_strdup(json_object_get_string_member_with_default(item, "category_id", ""));
        g_ptr_array_add(result->categories, category);
    }
    return result;
}

static void connect_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source;
    (void)cancel;
    GError *error = NULL;
    LoadResult *channels = fetch_channels(task_data, &error);
    if (channels) g_task_return_pointer(task, channels, load_result_free);
    else g_task_return_error(task, error);
}

static void channel_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; (void)data;
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_set_margin_top(GTK_WIDGET(box), 10);
    gtk_widget_set_margin_bottom(GTK_WIDGET(box), 10);
    gtk_widget_set_margin_start(GTK_WIDGET(box), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(box), 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("video-display-symbolic");
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(box, icon);
    gtk_box_append(box, label);
    gtk_box_append(box, gtk_image_new_from_icon_name("media-playback-start-symbolic"));
    g_object_set_data(G_OBJECT(box), "title-label", label);
    gtk_list_item_set_child(item, GTK_WIDGET(box));
}

static void channel_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; (void)data;
    GtkStringObject *object = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    GtkWidget *box = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title-label")),
                       gtk_string_object_get_string(object));
}

static void rebuild_channel_model(App *app) {
    guint selected = gtk_drop_down_get_selected(app->category_picker);
    const char *category_id = NULL;
    if (selected > 0 && app->categories && selected - 1 < app->categories->len)
        category_id = ((Category *)g_ptr_array_index(app->categories, selected - 1))->id;
    gtk_string_list_splice(app->channel_model, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(app->channel_model)), NULL);
    if (!app->channels) return;
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    g_autoptr(GPtrArray) visible = g_ptr_array_new();
    for (guint i = 0; i < app->channels->len; i++) {
        Channel *channel = g_ptr_array_index(app->channels, i);
        if (category_id && g_strcmp0(category_id, channel->category_id) != 0) continue;
        g_ptr_array_add(names, channel->name);
        g_ptr_array_add(visible, channel);
    }
    g_ptr_array_add(names, NULL);
    gtk_string_list_splice(app->channel_model, 0, 0, (const char * const *)names->pdata);
    for (guint i = 0; i < visible->len; i++) {
        g_autoptr(GObject) object = g_list_model_get_item(G_LIST_MODEL(app->channel_model), i);
        g_object_set_data(object, "channel", g_ptr_array_index(visible, i));
    }
}

static void category_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)object; (void)pspec;
    rebuild_channel_model(data);
}

static char *profiles_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xtream-player", "playlists.ini", NULL);
}

static char *lookup_password(const char *profile_id) {
    const char *argv[] = { "secret-tool", "lookup", "application", "xtream-player",
                           "profile", profile_id, NULL };
    g_autoptr(GSubprocess) process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    char *output = NULL;
    if (!process || !g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL, NULL))
        return NULL;
    if (output) g_strchomp(output);
    return output;
}

static void save_profile(App *app) {
    g_autofree char *identity = g_strdup_printf("%s|%s", app->base_url, app->user);
    g_autofree char *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
    const char *name = gtk_editable_get_text(GTK_EDITABLE(app->playlist_name));
    if (!name[0]) name = app->base_url;
    g_free(app->profile_id);
    app->profile_id = g_strdup(id);

    const char *argv[] = { "secret-tool", "store", "--label=Xtream Player",
                           "application", "xtream-player", "profile", id, NULL };
    g_autoptr(GSubprocess) process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (process) g_subprocess_communicate_utf8(process, app->pass, NULL, NULL, NULL, NULL);

    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autofree char *path = profiles_path();
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(file, id, "name", name);
    g_key_file_set_string(file, id, "server", app->base_url);
    g_key_file_set_string(file, id, "username", app->user);
    g_autofree char *directory = g_path_get_dirname(path);
    g_mkdir_with_parents(directory, 0700);
    g_key_file_save_to_file(file, path, NULL);
}

static void profile_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)object; (void)pspec;
    App *app = data;
    guint selected = adw_combo_row_get_selected(app->profile_picker);
    if (selected == 0 || !app->profiles || selected - 1 >= app->profiles->len) {
        g_clear_pointer(&app->profile_id, g_free);
        gtk_editable_set_text(GTK_EDITABLE(app->playlist_name), "");
        gtk_editable_set_text(GTK_EDITABLE(app->server), "");
        gtk_editable_set_text(GTK_EDITABLE(app->username), "");
        gtk_editable_set_text(GTK_EDITABLE(app->password), "");
        if (app->connect_button) gtk_button_set_label(app->connect_button, "Add and Connect");
        return;
    }
    Profile *profile = g_ptr_array_index(app->profiles, selected - 1);
    g_free(app->profile_id); app->profile_id = g_strdup(profile->id);
    gtk_editable_set_text(GTK_EDITABLE(app->playlist_name), profile->name);
    gtk_editable_set_text(GTK_EDITABLE(app->server), profile->server);
    gtk_editable_set_text(GTK_EDITABLE(app->username), profile->user);
    g_autofree char *password = lookup_password(profile->id);
    gtk_editable_set_text(GTK_EDITABLE(app->password), password ? password : "");
    if (app->connect_button) gtk_button_set_label(app->connect_button, "Connect");
}

static void load_profiles(App *app) {
    app->profiles = g_ptr_array_new_with_free_func(profile_free);
    GtkStringList *names = gtk_string_list_new(NULL);
    gtk_string_list_append(names, "New playlist…");
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autofree char *path = profiles_path();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        gsize count = 0;
        g_auto(GStrv) groups = g_key_file_get_groups(file, &count);
        for (gsize i = 0; i < count; i++) {
            Profile *profile = g_new0(Profile, 1);
            profile->id = g_strdup(groups[i]);
            profile->name = g_key_file_get_string(file, groups[i], "name", NULL);
            profile->server = g_key_file_get_string(file, groups[i], "server", NULL);
            profile->user = g_key_file_get_string(file, groups[i], "username", NULL);
            if (!profile->name || !profile->server || !profile->user) { profile_free(profile); continue; }
            g_ptr_array_add(app->profiles, profile);
            gtk_string_list_append(names, profile->name);
        }
    }
    adw_combo_row_set_model(app->profile_picker, G_LIST_MODEL(names));
    g_object_unref(names);
    if (app->profiles->len) adw_combo_row_set_selected(app->profile_picker, 1);
}

static char *settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xtream-player", "settings.ini", NULL);
}

static void apply_theme(App *app) {
    AdwStyleManager *manager = adw_style_manager_get_default();
    if (app->theme_mode == 1) {
        adw_style_manager_set_color_scheme(manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
    } else if (app->theme_mode == 2) {
        adw_style_manager_set_color_scheme(manager, ADW_COLOR_SCHEME_FORCE_DARK);
    } else {
        g_autofree char *scheme = g_settings_get_string(app->interface_settings, "color-scheme");
        adw_style_manager_set_color_scheme(manager,
            g_strcmp0(scheme, "prefer-dark") == 0
                ? ADW_COLOR_SCHEME_PREFER_DARK : ADW_COLOR_SCHEME_PREFER_LIGHT);
    }
}

static void system_theme_changed(GSettings *settings, const char *key, gpointer data) {
    (void)settings; (void)key;
    App *app = data;
    if (app->theme_mode == 0) apply_theme(app);
}

static void theme_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    App *app = data;
    app->theme_mode = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
    apply_theme(app);
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_integer(file, "appearance", "theme", (gint)app->theme_mode);
    g_autofree char *path = settings_path();
    g_autofree char *directory = g_path_get_dirname(path);
    g_mkdir_with_parents(directory, 0700);
    g_key_file_save_to_file(file, path, NULL);
}

static void show_preferences(GtkButton *button, gpointer data) {
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    App *app = data;
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_preferences_dialog_set_search_enabled(dialog, FALSE);
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Appearance");
    adw_preferences_page_set_icon_name(page, "applications-graphics-symbolic");
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Theme");
    AdwComboRow *theme = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme), "Color scheme");
    const char *choices[] = { "Follow system", "Light", "Dark", NULL };
    GtkStringList *model = gtk_string_list_new(choices);
    adw_combo_row_set_model(theme, G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(theme, app->theme_mode);
    g_signal_connect(theme, "notify::selected", G_CALLBACK(theme_changed), app);
    adw_preferences_group_add(group, GTK_WIDGET(theme));
    adw_preferences_page_add(page, group);
    adw_preferences_dialog_add(dialog, page);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(app->window));
}

static GtkWidget *make_menu_button(App *app) {
    GtkMenuButton *menu = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(menu, "open-menu-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(menu), "Main menu");
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_widget_set_margin_top(GTK_WIDGET(box), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(box), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(box), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(box), 6);
    GtkButton *preferences = GTK_BUTTON(gtk_button_new_with_label("Preferences…"));
    gtk_widget_add_css_class(GTK_WIDGET(preferences), "flat");
    g_signal_connect(preferences, "clicked", G_CALLBACK(show_preferences), app);
    gtk_box_append(box, GTK_WIDGET(preferences));
    gtk_popover_set_child(popover, GTK_WIDGET(box));
    gtk_menu_button_set_popover(menu, GTK_WIDGET(popover));
    return GTK_WIDGET(menu);
}

static void connect_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    App *app = data;
    GError *error = NULL;
    LoadResult *loaded = g_task_propagate_pointer(G_TASK(result), &error);
    gtk_spinner_stop(app->spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), TRUE);
    if (!loaded) {
        flash_message(app, error->message);
        g_error_free(error);
        return;
    }
    if (app->channels) g_ptr_array_unref(app->channels);
    if (app->categories) g_ptr_array_unref(app->categories);
    app->channels = g_steal_pointer(&loaded->channels);
    app->categories = g_steal_pointer(&loaded->categories);
    load_result_free(loaded);
    GtkStringList *category_names = gtk_string_list_new(NULL);
    gtk_string_list_append(category_names, "All channels");
    for (guint i = 0; i < app->categories->len; i++)
        gtk_string_list_append(category_names, ((Category *)g_ptr_array_index(app->categories, i))->name);
    gtk_drop_down_set_model(app->category_picker, G_LIST_MODEL(category_names));
    g_object_unref(category_names);
    gtk_drop_down_set_selected(app->category_picker, 0);
    rebuild_channel_model(app);
    save_profile(app);
    adw_view_stack_set_visible_child_name(app->stack, "player");
    flash_message(app, app->channels->len ? "Connected — choose a channel" : "Connected, but no live channels were found");
}

static void connect_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    const char *server = gtk_editable_get_text(GTK_EDITABLE(app->server));
    const char *user = gtk_editable_get_text(GTK_EDITABLE(app->username));
    const char *pass = gtk_editable_get_text(GTK_EDITABLE(app->password));
    if (!server[0] || !user[0] || !pass[0]) {
        flash_message(app, "Enter the server, username, and password.");
        return;
    }
    g_free(app->base_url); g_free(app->user); g_free(app->pass);
    app->base_url = normalize_server(server);
    app->user = g_strdup(user);
    app->pass = g_strdup(pass);

    LoginRequest *request = g_new0(LoginRequest, 1);
    request->base_url = g_strdup(app->base_url);
    request->user = g_strdup(app->user);
    request->pass = g_strdup(app->pass);
    GTask *task = g_task_new(NULL, NULL, connect_done, app);
    g_task_set_task_data(task, request, login_request_free);
    g_task_run_in_thread(task, connect_worker);
    g_object_unref(task);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), FALSE);
    gtk_spinner_start(app->spinner);
}

static void play_channel(App *app, Channel *channel) {
    g_autofree char *user = g_uri_escape_string(app->user, NULL, TRUE);
    g_autofree char *pass = g_uri_escape_string(app->pass, NULL, TRUE);
    g_autofree char *url = g_strdup_printf("%s/live/%s/%s/%s.ts", app->base_url, user, pass, channel->stream_id);
    if (!app->pipeline) {
        app->pipeline = gst_element_factory_make("playbin", "player");
        GstElement *sink = gst_element_factory_make("gtk4paintablesink", "video-sink");
        if (!app->pipeline || !sink) {
            flash_message(app, "The GStreamer GTK 4 video plugin is missing.");
            if (sink) gst_object_unref(sink);
            return;
        }
        g_object_set(app->pipeline, "video-sink", sink, NULL);
        GdkPaintable *paintable = NULL;
        g_object_get(sink, "paintable", &paintable, NULL);
        gtk_picture_set_paintable(app->video, paintable);
        g_clear_object(&paintable);
        gst_object_unref(sink);
    }
    /* playbin must leave PLAYING before its URI can be replaced reliably. */
    gst_element_set_state(app->pipeline, GST_STATE_NULL);
    gst_element_get_state(app->pipeline, NULL, NULL, GST_SECOND);
    g_object_set(app->pipeline, "uri", url, NULL);
    gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    app->playing = TRUE;
    gtk_label_set_text(app->now_playing, channel->name);
    gtk_button_set_icon_name(app->play_button, "media-playback-pause-symbolic");
}

static void channel_activated(GtkListView *view, guint position, gpointer data) {
    GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(view));
    g_autoptr(GObject) item = g_list_model_get_item(model, position);
    Channel *channel = g_object_get_data(item, "channel");
    if (channel) play_channel(data, channel);
}

static void search_changed(GtkSearchEntry *entry, gpointer data) {
    gtk_string_filter_set_search(GTK_STRING_FILTER(data),
                                 gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void play_clicked(GtkButton *button, gpointer data) {
    App *app = data;
    if (!app->pipeline) return;
    app->playing = !app->playing;
    gst_element_set_state(app->pipeline, app->playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    gtk_button_set_icon_name(button, app->playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}

static void mute_clicked(GtkButton *button, gpointer data) {
    App *app = data;
    if (!app->pipeline) return;
    app->muted = !app->muted;
    g_object_set(app->pipeline, "mute", app->muted, NULL);
    gtk_button_set_icon_name(button, app->muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
}

static gboolean hide_player_controls(gpointer data) {
    App *app = data;
    app->controls_timeout = 0;
    if (app->fullscreen) {
        gtk_revealer_set_reveal_child(app->player_controls, FALSE);
        gtk_widget_set_cursor_from_name(app->video_surface, "none");
    }
    return G_SOURCE_REMOVE;
}

static void reveal_player_controls(App *app) {
    gtk_revealer_set_reveal_child(app->player_controls, TRUE);
    gtk_widget_set_cursor_from_name(app->video_surface, NULL);
    if (app->controls_timeout) g_source_remove(app->controls_timeout);
    app->controls_timeout = app->fullscreen
        ? g_timeout_add(2800, hide_player_controls, app) : 0;
}

static void pointer_moved(GtkEventControllerMotion *controller, double x, double y, gpointer data) {
    (void)controller; (void)x; (void)y;
    reveal_player_controls(data);
}

static void toggle_fullscreen(App *app) {
    app->fullscreen = !app->fullscreen;
    gtk_widget_set_visible(app->sidebar, !app->fullscreen);
    adw_toolbar_view_set_reveal_top_bars(app->player_toolbar, !app->fullscreen);
    if (app->fullscreen) {
        gtk_window_fullscreen(GTK_WINDOW(app->window));
        gtk_widget_grab_focus(GTK_WIDGET(app->video));
        gtk_button_set_icon_name(app->fullscreen_button, "view-restore-symbolic");
        reveal_player_controls(app);
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(app->window));
        gtk_button_set_icon_name(app->fullscreen_button, "view-fullscreen-symbolic");
        if (app->controls_timeout) {
            g_source_remove(app->controls_timeout);
            app->controls_timeout = 0;
        }
        reveal_player_controls(app);
    }
}

static void fullscreen_clicked(GtkButton *button, gpointer data) {
    (void)button;
    toggle_fullscreen(data);
}

static void video_pressed(GtkGestureClick *gesture, int press_count,
                          double x, double y, gpointer data) {
    (void)gesture; (void)x; (void)y;
    if (press_count == 2) toggle_fullscreen(data);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode; (void)state;
    App *app = data;
    if (keyval == GDK_KEY_F11) { toggle_fullscreen(app); return TRUE; }
    if (keyval == GDK_KEY_Escape && app->fullscreen) { toggle_fullscreen(app); return TRUE; }
    return FALSE;
}

static void show_login(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->pipeline) gst_element_set_state(app->pipeline, GST_STATE_NULL);
    adw_view_stack_set_visible_child_name(app->stack, "login");
}

static GtkWidget *build_login(App *app) {
    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_title_widget(header, adw_window_title_new("Xtream Player", "Native IPTV"));
    adw_header_bar_pack_end(header, make_menu_button(app));
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
    AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());
    adw_clamp_set_maximum_size(clamp, 520);
    gtk_widget_set_margin_top(GTK_WIDGET(clamp), 48);
    gtk_widget_set_margin_bottom(GTK_WIDGET(clamp), 48);
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24));
    gtk_widget_set_margin_start(GTK_WIDGET(box), 24);
    gtk_widget_set_margin_end(GTK_WIDGET(box), 24);
    AdwStatusPage *intro = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(intro, "video-display-symbolic");
    adw_status_page_set_title(intro, "Connect to IPTV");
    adw_status_page_set_description(intro,
        "Choose a saved playlist or add the connection details from your provider.");
    gtk_box_append(box, GTK_WIDGET(intro));
    AdwPreferencesGroup *playlist_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(playlist_group, "Playlist");
    adw_preferences_group_set_description(playlist_group,
        "Choose a saved provider connection, or select New playlist to add one.");
    app->profile_picker = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->profile_picker), "Saved playlist");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(app->profile_picker),
                                "Switch between your IPTV providers");
    g_signal_connect(app->profile_picker, "notify::selected", G_CALLBACK(profile_changed), app);
    app->playlist_name = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->playlist_name), "Display name");
    adw_preferences_group_add(playlist_group, GTK_WIDGET(app->profile_picker));
    adw_preferences_group_add(playlist_group, GTK_WIDGET(app->playlist_name));
    gtk_box_append(box, GTK_WIDGET(playlist_group));

    AdwPreferencesGroup *details_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(details_group, "Provider details");
    adw_preferences_group_set_description(details_group,
        "Use the Xtream Codes credentials supplied by your IPTV provider.");
    app->server = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->server), "Server address");
    app->username = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->username), "Username");
    app->password = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->password), "Password");
    adw_preferences_group_add(details_group, GTK_WIDGET(app->server));
    adw_preferences_group_add(details_group, GTK_WIDGET(app->username));
    adw_preferences_group_add(details_group, GTK_WIDGET(app->password));
    gtk_box_append(box, GTK_WIDGET(details_group));
    GtkBox *actions = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_set_halign(GTK_WIDGET(actions), GTK_ALIGN_END);
    app->spinner = GTK_SPINNER(gtk_spinner_new());
    app->connect_button = GTK_BUTTON(gtk_button_new_with_label("Add and Connect"));
    gtk_widget_add_css_class(GTK_WIDGET(app->connect_button), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(app->connect_button), "pill");
    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(connect_clicked), app);
    gtk_box_append(actions, GTK_WIDGET(app->spinner));
    gtk_box_append(actions, GTK_WIDGET(app->connect_button));
    gtk_box_append(box, GTK_WIDGET(actions));
    adw_clamp_set_child(clamp, GTK_WIDGET(box));
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(clamp));
    return GTK_WIDGET(toolbar);
}

static GtkWidget *build_player(App *app) {
    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    app->player_toolbar = toolbar;
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_title_widget(header, adw_window_title_new("Live TV", "Xtream Player"));
    GtkButton *account = GTK_BUTTON(gtk_button_new_from_icon_name("system-log-out-symbolic"));
    GtkButton *fullscreen = GTK_BUTTON(gtk_button_new_from_icon_name("view-fullscreen-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(fullscreen), "Fullscreen (F11)");
    g_signal_connect(fullscreen, "clicked", G_CALLBACK(fullscreen_clicked), app);
    gtk_widget_set_tooltip_text(GTK_WIDGET(account), "Change provider");
    g_signal_connect(account, "clicked", G_CALLBACK(show_login), app);
    adw_header_bar_pack_end(header, GTK_WIDGET(account));
    adw_header_bar_pack_end(header, GTK_WIDGET(fullscreen));
    adw_header_bar_pack_end(header, make_menu_button(app));
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
    GtkPaned *paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_position(paned, 340);
    gtk_paned_set_resize_start_child(paned, FALSE);
    GtkBox *sidebar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    app->sidebar = GTK_WIDGET(sidebar);
    gtk_widget_set_size_request(GTK_WIDGET(sidebar), 260, -1);
    gtk_widget_set_margin_top(GTK_WIDGET(sidebar), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(sidebar), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(sidebar), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(sidebar), 12);
    app->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(app->search, "Search channels");
    app->category_picker = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->category_picker), "Channel category");
    g_signal_connect(app->category_picker, "notify::selected", G_CALLBACK(category_changed), app);
    GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app->channel_model = gtk_string_list_new(NULL);
    GtkExpression *expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    app->channel_filter = gtk_string_filter_new(expression);
    gtk_string_filter_set_ignore_case(app->channel_filter, TRUE);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(app->channel_model)), GTK_FILTER(g_object_ref(app->channel_filter)));
    gtk_filter_list_model_set_incremental(filtered, TRUE);
    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(filtered));
    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_single_selection_set_can_unselect(selection, TRUE);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(channel_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(channel_item_bind), NULL);
    app->channel_list = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory));
    gtk_list_view_set_single_click_activate(app->channel_list, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(app->channel_list), "boxed-list");
    g_signal_connect(app->search, "search-changed", G_CALLBACK(search_changed), app->channel_filter);
    g_signal_connect(app->channel_list, "activate", G_CALLBACK(channel_activated), app);
    gtk_scrolled_window_set_child(scroll, GTK_WIDGET(app->channel_list));
    gtk_box_append(sidebar, GTK_WIDGET(app->search));
    gtk_box_append(sidebar, GTK_WIDGET(app->category_picker));
    gtk_box_append(sidebar, GTK_WIDGET(scroll));
    gtk_widget_set_vexpand(GTK_WIDGET(scroll), TRUE);
    gtk_paned_set_start_child(paned, GTK_WIDGET(sidebar));
    GtkOverlay *content = GTK_OVERLAY(gtk_overlay_new());
    app->video_surface = GTK_WIDGET(content);
    gtk_widget_add_css_class(GTK_WIDGET(content), "player-surface");
    app->video = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(app->video, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(GTK_WIDGET(app->video), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->video), TRUE);
    GtkGesture *video_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(video_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(video_click, "pressed", G_CALLBACK(video_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->video), GTK_EVENT_CONTROLLER(video_click));
    GtkBox *controls = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_add_css_class(GTK_WIDGET(controls), "player-controls");
    gtk_widget_set_margin_top(GTK_WIDGET(controls), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(controls), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(controls), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(controls), 16);
    app->play_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
    app->mute_button = GTK_BUTTON(gtk_button_new_from_icon_name("audio-volume-high-symbolic"));
    app->fullscreen_button = GTK_BUTTON(gtk_button_new_from_icon_name("view-fullscreen-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->fullscreen_button), "Toggle fullscreen (F11)");
    app->now_playing = GTK_LABEL(gtk_label_new("Choose a channel"));
    gtk_widget_set_hexpand(GTK_WIDGET(app->now_playing), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(app->now_playing), GTK_ALIGN_START);
    gtk_widget_add_css_class(GTK_WIDGET(app->now_playing), "title-4");
    g_signal_connect(app->play_button, "clicked", G_CALLBACK(play_clicked), app);
    g_signal_connect(app->mute_button, "clicked", G_CALLBACK(mute_clicked), app);
    g_signal_connect(app->fullscreen_button, "clicked", G_CALLBACK(fullscreen_clicked), app);
    gtk_box_append(controls, GTK_WIDGET(app->play_button));
    gtk_box_append(controls, GTK_WIDGET(app->now_playing));
    gtk_box_append(controls, GTK_WIDGET(app->mute_button));
    gtk_box_append(controls, GTK_WIDGET(app->fullscreen_button));
    app->player_controls = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(app->player_controls, GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_reveal_child(app->player_controls, TRUE);
    gtk_revealer_set_child(app->player_controls, GTK_WIDGET(controls));
    gtk_widget_set_halign(GTK_WIDGET(app->player_controls), GTK_ALIGN_FILL);
    gtk_widget_set_valign(GTK_WIDGET(app->player_controls), GTK_ALIGN_END);
    gtk_overlay_set_child(content, GTK_WIDGET(app->video));
    gtk_overlay_add_overlay(content, GTK_WIDGET(app->player_controls));
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(pointer_moved), app);
    gtk_widget_add_controller(GTK_WIDGET(content), motion);
    gtk_paned_set_end_child(paned, GTK_WIDGET(content));
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(paned));
    return GTK_WIDGET(toolbar);
}

static void app_free(gpointer data) {
    App *app = data;
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
    }
    if (app->controls_timeout) g_source_remove(app->controls_timeout);
    g_clear_object(&app->interface_settings);
    if (app->channels) g_ptr_array_unref(app->channels);
    if (app->categories) g_ptr_array_unref(app->categories);
    if (app->profiles) g_ptr_array_unref(app->profiles);
    g_clear_object(&app->channel_model);
    g_clear_object(&app->channel_filter);
    g_free(app->profile_id); g_free(app->base_url); g_free(app->user); g_free(app->pass); g_free(app);
}

static void activate(GApplication *application, gpointer user_data) {
    (void)user_data;
    App *app = g_new0(App, 1);
    app->app = ADW_APPLICATION(application);
    app->interface_settings = g_settings_new("org.gnome.desktop.interface");
    g_autoptr(GKeyFile) settings = g_key_file_new();
    g_autofree char *saved_settings = settings_path();
    if (g_key_file_load_from_file(settings, saved_settings, G_KEY_FILE_NONE, NULL)) {
        gint saved_theme = g_key_file_get_integer(settings, "appearance", "theme", NULL);
        if (saved_theme >= 0 && saved_theme <= 2) app->theme_mode = (guint)saved_theme;
    }
    apply_theme(app);
    g_signal_connect(app->interface_settings, "changed::color-scheme",
                     G_CALLBACK(system_theme_changed), app);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".player-surface { background-color: #000000; }"
        ".player-controls {"
        "  background: rgba(0, 0, 0, 0.78); color: white;"
        "  border-radius: 14px; margin: 16px; padding: 4px;"
        "}"
        ".player-controls button { color: white; }"
        ".player-controls label { color: white; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    app->window = ADW_APPLICATION_WINDOW(
        adw_application_window_new(GTK_APPLICATION(app->app)));
    gtk_window_set_title(GTK_WINDOW(app->window), "Xtream Player");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1180, 720);
    app->stack = ADW_VIEW_STACK(adw_view_stack_new());
    adw_view_stack_add_named(app->stack, build_login(app), "login");
    adw_view_stack_add_named(app->stack, build_player(app), "player");
    GtkOverlay *overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_overlay_set_child(overlay, GTK_WIDGET(app->stack));
    app->toast_revealer = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(app->toast_revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    app->toast_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(app->toast_label), "toast");
    gtk_widget_set_halign(GTK_WIDGET(app->toast_revealer), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(app->toast_revealer), GTK_ALIGN_START);
    gtk_widget_set_margin_top(GTK_WIDGET(app->toast_revealer), 12);
    gtk_revealer_set_child(app->toast_revealer, GTK_WIDGET(app->toast_label));
    gtk_overlay_add_overlay(overlay, GTK_WIDGET(app->toast_revealer));
    adw_application_window_set_content(app->window, GTK_WIDGET(overlay));
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), keys);
    g_object_set_data_full(G_OBJECT(app->window), "app-state", app, app_free);
    load_profiles(app);
    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_autoptr(AdwApplication) application = adw_application_new(
        "io.github.mackilanu.XtreamPlayer", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(application), argc, argv);
}

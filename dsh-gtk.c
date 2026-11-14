/* dsh-gtk: a small native window for the dsh web UI (GTK3 + WebKitGTK),
 * with a StatusNotifierItem tray icon so it stays resident in the menu bar
 * (modeled on magpie's `tray` mode).
 *
 * Project name / final program name: dsh-gtk
 *
 * - Loads an already-running `dsh web` in WebKitGTK, keeping its own cookie
 *   jar so the launch token only has to be supplied once per cookie lifetime.
 * - If the server answers 401, the token printed by `dsh web` is recovered
 *   from the user journal and the page is reloaded with it.
 * - Closing the window hides it to the tray (process stays alive).
 * - Tray left-click shows/focuses the window; right-click menu: Show / Quit.
 * - On startup it waits for org.kde.StatusNotifierWatcher (waybar) before
 *   registering the indicator, mirroring magpie's ExecStartPre logic.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o dsh-gtk dsh-gtk.c \
 *       $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 ayatana-appindicator3-0.1)
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <libayatana-appindicator/app-indicator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_URL "http://127.0.0.1:3080/"
#define APP_DIR "dsh-gtk"
/* Fallback tray icon name (theme icon resolved by appindicator). */
#define TRAY_ICON_NAME "utilities-terminal"

static WebKitWebView *view;
static GtkWidget *window;
static AppIndicator *indicator;
static char *base_url;
static char *tray_icon;
static gboolean token_retry_done = FALSE;
static gboolean quit_requested = FALSE;

/* --- token recovery --- */
static char *read_token_from_journal(void) {
    FILE *p = popen(
        "journalctl --user -u dsh-web -o cat --no-pager 2>/dev/null"
        " | sed -n 's/.*\\(token=[A-Za-z0-9_-][A-Za-z0-9_-]*\\).*/\\1/p' | tail -1",
        "r");
    if (!p) return NULL;

    char buf[512] = {0};
    char *tok = NULL;
    if (fgets(buf, sizeof buf, p)) {
        char *nl = strpbrk(buf, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(buf, "token=", 6) == 0) tok = g_strdup(buf + 6);
    }
    pclose(p);
    return tok;
}

static void load_with_token(void) {
    const char *tok = g_getenv("DSH_WEB_TOKEN");
    char *owned = NULL;
    if (!tok || !*tok) {
        owned = read_token_from_journal();
        tok = owned;
    }
    if (!tok || !*tok) {
        g_printerr("dsh-gtk: 401 from dsh web and no launch token found\n");
        g_free(owned);
        return;
    }
    char *url = g_strdup_printf("%s%ctoken=%s", base_url,
                                strchr(base_url, '?') ? '&' : '?', tok);
    webkit_web_view_load_uri(view, url);
    g_free(url);
    g_free(owned);
}

static void on_resource_finished(WebKitWebResource *resource, gpointer unused) {
    (void)unused;
    WebKitURIResponse *resp = webkit_web_resource_get_response(resource);
    if (!resp) return;
    guint code = webkit_uri_response_get_status_code(resp);
    if (code != 401) return;
    if (token_retry_done) return;
    token_retry_done = TRUE;
    load_with_token();
}

static void on_resource_load_started(WebKitWebView *v, WebKitWebResource *resource,
                                     WebKitURIRequest *request, gpointer unused) {
    (void)v; (void)request; (void)unused;
    g_signal_connect(resource, "finished", G_CALLBACK(on_resource_finished), NULL);
}

/* --- window lifecycle: hide to tray instead of quitting --- */
static void show_and_focus(void) {
    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer unused) {
    (void)w; (void)ev; (void)unused;
    if (quit_requested) return FALSE;          /* real quit */
    gtk_widget_hide(window);                   /* hide to tray */
    return TRUE;                               /* stop default destroy */
}

static void do_quit(void) {
    quit_requested = TRUE;
    gtk_main_quit();
}

/* --- tray menu --- */
static void on_tray_show(GtkMenuItem *item, gpointer unused) {
    (void)item; (void)unused;
    show_and_focus();
}

static void on_tray_quit(GtkMenuItem *item, gpointer unused) {
    (void)item; (void)unused;
    do_quit();
}

static void on_indicator_activate(AppIndicator *ind, gpointer unused) {
    (void)ind; (void)unused;
    show_and_focus();
}

/* --- keyboard --- */
static gboolean reload_accel(GtkWidget *w, GdkEventKey *ev, gpointer unused) {
    (void)w; (void)unused;
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R)) {
        webkit_web_view_reload(view);
        return TRUE;
    }
    return FALSE;
}

static gboolean fullscreen_accel(GtkWidget *w, GdkEventKey *ev, gpointer unused) {
    (void)unused;
    if (ev->keyval == GDK_KEY_F11) {
        GtkWindow *win = GTK_WINDOW(gtk_widget_get_toplevel(w));
        if (gdk_window_get_state(gtk_widget_get_window(GTK_WIDGET(win))) & GDK_WINDOW_STATE_FULLSCREEN)
            gtk_window_unfullscreen(win);
        else
            gtk_window_fullscreen(win);
        return TRUE;
    }
    return FALSE;
}

/* --- wait for StatusNotifierWatcher (waybar) like magpie does --- */
static gboolean wait_for_watcher(void) {
    gboolean ok = FALSE;
    GVariant *ret = NULL;
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        if (err) g_clear_error(&err);
        return FALSE;
    }
    ret = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 100, NULL, &err);
    if (ret) {
        g_variant_get(ret, "(b)", &ok);
        g_variant_unref(ret);
    }
    if (err) g_clear_error(&err);
    return ok;
}

static gboolean watcher_poll(gpointer unused) {
    (void)unused;
    if (wait_for_watcher()) {
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;   /* keep polling until waybar is up */
}

/* --- tray setup --- */
static void setup_tray(void) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *mi_show = gtk_menu_item_new_with_label("显示窗口");
    g_signal_connect(mi_show, "activate", G_CALLBACK(on_tray_show), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_show);

    GtkWidget *mi_quit = gtk_menu_item_new_with_label("退出");
    g_signal_connect(mi_quit, "activate", G_CALLBACK(on_tray_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_quit);

    gtk_widget_show_all(menu);

    indicator = app_indicator_new("dsh-gtk", tray_icon,
                                  APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    g_signal_connect(indicator, "activate", G_CALLBACK(on_indicator_activate), NULL);

    /* register once the watcher is present; poll until then (magpie pattern) */
    g_timeout_add(1000, watcher_poll, NULL);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    base_url = argc > 1 ? g_strdup(argv[1]) : g_strdup(DEFAULT_URL);

    /* Tray icon: search $XDG_DATA_HOME/dsh-gtk/ then each $XDG_DATA_DIRS/dsh-gtk/
     * for dsh-tray.png; fall back to a theme icon name. This lets the AUR package
     * install the icon to /usr/share/dsh-gtk/ and still be found at runtime. */
    tray_icon = g_build_filename(g_get_user_data_dir(), APP_DIR, "dsh-tray.png", NULL);
    if (!g_file_test(tray_icon, G_FILE_TEST_EXISTS)) {
        g_free(tray_icon);
        tray_icon = NULL;
        const gchar * const *dirs = g_get_system_data_dirs();
        for (const gchar * const *d = dirs; d && *d; d++) {
            char *cand = g_build_filename(*d, APP_DIR, "dsh-tray.png", NULL);
            if (g_file_test(cand, G_FILE_TEST_EXISTS)) { tray_icon = cand; break; }
            g_free(cand);
        }
    }
    if (!tray_icon)
        tray_icon = g_strdup(TRAY_ICON_NAME);
    const char *data_home = g_get_user_data_dir();
    const char *cache_home = g_get_user_cache_dir();
    char *data_dir = g_build_filename(data_home, APP_DIR, "webkit", NULL);
    char *cache_dir = g_build_filename(cache_home, APP_DIR, NULL);

    WebKitWebsiteDataManager *dm = webkit_website_data_manager_new(
        "base-data-directory", data_dir,
        "base-cache-directory", cache_dir,
        NULL);
    WebKitWebContext *ctx = webkit_web_context_new_with_website_data_manager(dm);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "dsh");
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 860);
    gtk_window_set_icon_name(GTK_WINDOW(window), "utilities-terminal");

    view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(ctx));
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

    g_signal_connect(view, "resource-load-started",
                     G_CALLBACK(on_resource_load_started), NULL);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(reload_accel), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(fullscreen_accel), NULL);

    setup_tray();

    webkit_web_view_load_uri(view, base_url);
    gtk_widget_show_all(window);
    gtk_main();

    g_free(cache_dir);
    g_free(data_dir);
    g_free(base_url);
    g_free(tray_icon);
    return 0;
}

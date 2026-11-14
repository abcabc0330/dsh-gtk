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
 * - Single instance: a second launch talks to the running process over the
 *   session bus instead of opening a second window.
 * - `dsh-gtk --toggle` asks the running instance to hide/show itself, so a
 *   compositor keybinding (sway, i3, Hyprland, KDE, GNOME) can drive the
 *   window even though global shortcuts never reach the application.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o dsh-gtk dsh-gtk.c \
 *       $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 ayatana-appindicator3-0.1)
 *
 * Usage:
 *   dsh-gtk [url]        open the window (or raise the running one)
 *   dsh-gtk --toggle     hide the window if shown, show it if hidden;
 *                        starts it when nothing is running
 *   dsh-gtk --version
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <libayatana-appindicator/app-indicator.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSH_GTK_VERSION
#define DSH_GTK_VERSION "0.1.3"
#endif

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

/* F8 toggles window visibility: hide to tray if shown, show+focus if hidden.
 * Registered as a window-level accel so WebKit cannot swallow the key. */
static void on_toggle_accel(gpointer unused) {
    (void)unused;
    if (gtk_widget_get_visible(window))
        gtk_widget_hide(window);
    else
        show_and_focus();
}


/* --- single instance + external toggle bridge (session bus) ---
 *
 * On Wayland a compositor handles global shortcuts itself, so a key bound to
 * this application never reaches its window. The portable answer is to let a
 * keybinding run a *second* `dsh-gtk` with `--toggle`: it connects to the bus,
 * finds the well-known name below, forwards one method call and exits.
 *
 * The same well-known name doubles as the single-instance guard, so a plain
 * second launch raises the existing window instead of opening another one.
 */
#define BUS_NAME   "org.dsh.Gtk"
#define OBJ_PATH   "/org/dsh/Gtk"
#define IFACE_NAME "org.dsh.Gtk"

static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='" IFACE_NAME "'>"
    "    <method name='Toggle'/>"
    "    <method name='Activate'/>"
    "  </interface>"
    "</node>";

/* Hide if shown, show+focus if hidden: what both F8 and `--toggle` do. */
static void toggle_visibility(void) {
    if (gtk_widget_get_visible(window))
        gtk_widget_hide(window);
    else
        show_and_focus();
}

/* Exported methods, invoked by a second process. */
static void on_dbus_method_call(GDBusConnection *conn, const gchar *sender,
                                const gchar *path, const gchar *iface,
                                const gchar *method, GVariant *params,
                                GDBusMethodInvocation *inv, gpointer unused) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)params; (void)unused;
    if (g_strcmp0(method, "Toggle") == 0)
        toggle_visibility();
    else if (g_strcmp0(method, "Activate") == 0)
        show_and_focus();
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable dbus_vtable = {
    on_dbus_method_call, NULL, NULL, {0}
};

/* Session bus, or NULL when D-Bus is unavailable (the app then just runs,
 * losing only the single-instance and --toggle behaviour). */
static GDBusConnection *get_session_bus(void) {
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus && err) {
        g_printerr("dsh-gtk: no session bus (%s)\n", err->message);
        g_clear_error(&err);
    }
    return bus;
}

/* Claim BUS_NAME without queueing behind an existing owner.
 * Returns 1 (we own it), 3 (another instance owns it) or 0 on error. */
static int request_bus_name(GDBusConnection *bus) {
    GError *err = NULL;
    GVariant *ret;
    guint32 reply = 0;

    ret = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", BUS_NAME, 4u /* DBUS_NAME_FLAG_DO_NOT_QUEUE */),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (ret) {
        g_variant_get(ret, "(u)", &reply);
        g_variant_unref(ret);
    }
    if (err) g_clear_error(&err);
    return (int)reply;
}

/* Publish the interface on OBJ_PATH so other processes can call us. */
static void export_dbus_interface(GDBusConnection *bus) {
    GDBusNodeInfo *info = NULL;
    GError *err = NULL;

    info = g_dbus_node_info_new_for_xml(introspection_xml, &err);
    if (!info) {
        g_printerr("dsh-gtk: bad introspection XML: %s\n", err->message);
        g_clear_error(&err);
        return;
    }
    g_dbus_connection_register_object(bus, OBJ_PATH, info->interfaces[0],
                                      &dbus_vtable, NULL, NULL, &err);
    if (err) {
        g_printerr("dsh-gtk: cannot export %s: %s\n", IFACE_NAME, err->message);
        g_clear_error(&err);
    }
    g_dbus_node_info_unref(info);
}

/* Ask the running instance to run `method`; TRUE when it answered. */
static gboolean ask_running_instance(const char *method) {
    GDBusConnection *bus = get_session_bus();
    GError *err = NULL;
    GVariant *ret;
    gboolean ok;

    if (!bus) return FALSE;
    ret = g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE_NAME,
                                      method, NULL, NULL,
                                      G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    ok = ret != NULL;
    if (ret) g_variant_unref(ret);
    if (err) g_clear_error(&err);
    return ok;
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

static void usage(FILE *out) {
    fprintf(out,
        "Usage: dsh-gtk [OPTION]... [URL]\n"
        "\n"
        "  --toggle     hide the running window if shown, show it if hidden;\n"
        "               starts dsh-gtk when nothing is running\n"
        "  --version    print the version and exit\n"
        "  -h, --help   show this help and exit\n"
        "\n"
        "A plain launch raises the already running instance instead of\n"
        "opening a second window. Bind --toggle to a key in the compositor:\n"
        "\n"
        "  sway:     bindsym --no-repeat F8 exec dsh-gtk --toggle\n"
        "  Hyprland: bind = , F8, exec, dsh-gtk --toggle\n");
}

int main(int argc, char **argv) {
    gboolean want_toggle = FALSE;
    GDBusConnection *bus;
    int owner;

    /* GTK consumes its own options; ours stay in argv. */
    gtk_init(&argc, &argv);

    base_url = g_strdup(DEFAULT_URL);
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--toggle") == 0) {
            want_toggle = TRUE;
        } else if (g_strcmp0(argv[i], "--version") == 0) {
            printf("dsh-gtk %s\n", DSH_GTK_VERSION);
            g_free(base_url);
            return 0;
        } else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0) {
            usage(stdout);
            g_free(base_url);
            return 0;
        } else if (argv[i][0] != '-') {
            g_free(base_url);
            base_url = g_strdup(argv[i]);
        }
    }

    /* Single instance: whoever owns BUS_NAME is the running window. A second
     * process forwards Toggle/Activate there and exits before touching GTK. */
    bus = get_session_bus();
    if (bus) {
        owner = request_bus_name(bus);
        if (owner == 3) {                       /* already running */
            gboolean ok = ask_running_instance(want_toggle ? "Toggle" : "Activate");
            g_free(base_url);
            return ok ? 0 : 1;
        }
        if (owner == 1)                         /* we are the one instance */
            export_dbus_interface(bus);
    }

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
    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_accel_group_connect(ag, GDK_KEY_F8, (GdkModifierType)0, 0,
                            g_cclosure_new(G_CALLBACK(on_toggle_accel), NULL, NULL));
    gtk_window_add_accel_group(GTK_WINDOW(window), ag);
    g_object_unref(ag);
    setup_tray();

    webkit_web_view_load_uri(view, base_url);
    gtk_widget_show_all(window);

    gtk_main();

    if (bus) g_object_unref(bus);
    g_free(cache_dir);
    g_free(data_dir);
    g_free(base_url);
    g_free(tray_icon);
    return 0;
}

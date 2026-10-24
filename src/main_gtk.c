/*
 * usbimager/main_gtk.c
 *
 * Copyright (C) 2023 bzt (bztsrc@gitlab)
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * @brief Native GTK User interface for Linux
 *
 */

/* wether to start a separate thread for reader / writer */
/*#define USE_THREADS*/

#include <errno.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include "lang.h"
#include "stream.h"
#include "disks.h"
#include "misc/wm_icon.h"       /* window manager icon */

char **lang = NULL;
extern char *dict[NUMLANGS][NUMTEXTS + 1];

static char *bkpdir = NULL;
static GtkWidget *mainwin = NULL, *source, *sourceButton, *writeButton, *target, *refreshButton, *pbar, *status;
#if !defined(USE_WRONLY) || !USE_WRONLY
GtkWidget *readButton, *verify, *compr, *blksize;
#endif
static int blksizesel = 0;
#ifdef USE_THREADS
GThread *thrd = NULL;
#endif
char *main_errorMessage;

/* modern look, relies on the theme's named colors so it works with dark themes too */
static const char css[] =
    ".usbimager-card { background-color: @theme_base_color; border: 1px solid alpha(@theme_fg_color, 0.12);"
    "  border-radius: 12px; padding: 14px; }"
    ".usbimager-icon { color: alpha(@theme_fg_color, 0.6); }"
    ".usbimager-action { padding: 8px 18px; font-weight: bold; border-radius: 8px; }"
    ".usbimager-action image { margin-right: 6px; }"
    ".usbimager-card entry, .usbimager-card combobox button { min-height: 34px; }"
    "progressbar trough, progressbar progress { min-height: 8px; border-radius: 4px; }"
    ".usbimager-status { font-size: smaller; }";

/**
 * Enable or disable the input widgets while an operation is in progress
 */
static void setSensitive(int enabled)
{
#if !defined(USE_WRONLY) || !USE_WRONLY
    int targetId;
#endif
    gtk_widget_set_sensitive(source, enabled);
    gtk_widget_set_sensitive(sourceButton, enabled);
    gtk_widget_set_sensitive(target, enabled);
    gtk_widget_set_sensitive(refreshButton, enabled);
    gtk_widget_set_sensitive(writeButton, enabled);
#if !defined(USE_WRONLY) || !USE_WRONLY
    targetId = gtk_combo_box_get_active(GTK_COMBO_BOX(target));
    gtk_widget_set_sensitive(readButton, enabled &&
        (targetId < 0 || targetId >= DISKS_MAX || disks_targets[targetId] < 1024));
    gtk_widget_set_sensitive(verify, enabled);
    gtk_widget_set_sensitive(compr, enabled);
    gtk_widget_set_sensitive(blksize, enabled);
#endif
}

void main_addToCombobox(char *option)
{
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(target), NULL, option);
}

void main_getErrorMessage(void)
{
    main_errorMessage = errno ? strerror(errno) : NULL;
}

static int onDone(void *data)
{
    if(mainwin) {
        setSensitive(TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 0);
        gtk_label_set_label(GTK_LABEL(status), (char*)data);
    }
    main_errorMessage = NULL;
    return 0;
}

void main_onDone(void *data)
{
    if(mainwin) {
#ifdef USE_THREADS
        g_idle_add(onDone, data);
#else
        onDone(data);
#endif
    }
}

static int onProgress(void *data)
{
    char textstat[128];
    int pos = 0;
    if(mainwin) {
        if(data)
            pos = stream_status((stream_t*)data, textstat, 0);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), (gdouble)pos/100.0);
        gtk_label_set_label(GTK_LABEL(status), !data ? lang[L_WAITING] : textstat);
    }
    return 0;
}

void main_onProgress(void *data)
{
    if(mainwin) {
#ifdef USE_THREADS
        g_idle_add(onProgress, data);
#else
        onProgress(data);
        while(gtk_events_pending()) gtk_main_iteration();
#endif
    }
}

static int onThreadError(void *data)
{
    GtkWidget *mbox = gtk_message_dialog_new(GTK_WINDOW(mainwin), 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", (char*)data);
    gtk_window_set_title(GTK_WINDOW(mbox), main_errorMessage && *main_errorMessage ? main_errorMessage : lang[L_ERROR]);
    gtk_window_set_default_size(GTK_WINDOW(mbox), 200, 64);
    gtk_dialog_run(GTK_DIALOG(mbox));
    gtk_widget_destroy(mbox);
    return 0;
}

void main_onThreadError(void *data)
{
#ifdef USE_THREADS
    g_idle_add(onThreadError, data);
#else
    onThreadError(data);
#endif
}


/**
 * Function that reads from input and writes to disk
 */
static void *writerRoutine(void *data)
{
    int dst, needVerify, numberOfBytesRead;
    int numberOfBytesWritten, numberOfBytesVerify, needWrite, targetId = gtk_combo_box_get_active(GTK_COMBO_BOX(target));
    static char lpStatus[128];
    static stream_t ctx;
    (void)data;
#if !defined(USE_WRONLY) || !USE_WRONLY
    needVerify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(verify));
#else
    needVerify = 1;
#endif

    ctx.fileSize = 0;
    dst = stream_open(&ctx, (char*)gtk_entry_get_text(GTK_ENTRY(source)), targetId >= 0 && targetId < DISKS_MAX && disks_targets[targetId] >= 1024);
    if(!dst) {
        dst = (int)((long int)disks_open(targetId, ctx.fileSize));
        if(dst > 0) {
            while(mainwin) {
                if((numberOfBytesRead = stream_read(&ctx)) >= 0) {
                    if(numberOfBytesRead == 0) {
                        if(!ctx.fileSize) ctx.fileSize = ctx.readSize;
                        break;
                    } else {
                        errno = 0; needWrite = 1; numberOfBytesVerify = 0;
                        if(!force) {
                            numberOfBytesVerify = read(dst, ctx.verifyBuf, numberOfBytesRead);
                            if(numberOfBytesVerify == numberOfBytesRead &&
                                !memcmp(ctx.buffer, ctx.verifyBuf, numberOfBytesRead)) {
                                if(verbose > 1) printf("  numberOfBytesVerify %d matches disk, skipping write\n", numberOfBytesRead);
                                needWrite = 0;
                                main_onProgress(&ctx);
                            } else
                                lseek(dst, -((off_t)numberOfBytesVerify), SEEK_CUR);
                        }
                        if(needWrite) {
                            numberOfBytesWritten = (int)write(dst, ctx.buffer, numberOfBytesRead);
                            if(verbose > 1) printf("write(%d) numberOfBytesWritten %d errno=%d\n",
                                numberOfBytesRead, numberOfBytesWritten, errno);
                            if(numberOfBytesWritten == numberOfBytesRead) {
                                if(needVerify) {
                                    lseek(dst, -((off_t)numberOfBytesWritten), SEEK_CUR);
                                    numberOfBytesVerify = read(dst, ctx.verifyBuf, numberOfBytesWritten);
                                    if(verbose > 1) printf("  numberOfBytesVerify %d\n", numberOfBytesVerify);
                                    if(numberOfBytesVerify != numberOfBytesWritten ||
                                        memcmp(ctx.buffer, ctx.verifyBuf, numberOfBytesWritten)) {
                                            onThreadError(lang[L_VRFYERR]);
                                        break;
                                    }
                                }
                                main_onProgress(&ctx);
                            } else {
                                if(errno) main_errorMessage = strerror(errno);
                                main_onThreadError(lang[L_WRTRGERR]);
                                break;
                            }
                        }
                        stream_hash(&ctx, numberOfBytesVerify);
                    }
                } else {
                    main_onThreadError(lang[L_RDSRCERR]);
                    break;
                }
            }
            disks_close((void*)((long int)dst));
        } else {
            main_onThreadError(lang[dst == -1 ? L_TRGERR : (dst == -2 ? L_UMOUNTERR : (dst == -4 ? L_COMMERR : L_OPENTRGERR))]);
        }
        stream_close(&ctx);
    } else {
        if(errno) main_errorMessage = strerror(errno);
        main_onThreadError(lang[dst == 2 ? L_ENCZIPERR : (dst == 3 ? L_CMPZIPERR : (dst == 4 ? L_CMPERR : L_SRCERR))]);
    }
    stream_status(&ctx, lpStatus, 1);
    main_onDone(&lpStatus);
    if(verbose) printf("Worker thread finished.\r\n");
    return NULL;
}

static void onWriteButtonClicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    setSensitive(FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 0);
    gtk_label_set_label(GTK_LABEL(status), "");
    main_errorMessage = NULL;

    if(verbose) printf("Starting worker thread for writing.\r\n");
#ifdef USE_THREADS
    thrd = g_thread_new("writer", writerRoutine, NULL);
#else
    writerRoutine(NULL);
#endif
}

#if !defined(USE_WRONLY) || !USE_WRONLY

/**
 * Function that reads from disk and writes to output file
 */
static void *readerRoutine(void *data)
{
    int src, size, needCompress = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(compr)), numberOfBytesRead;
    static char lpStatus[128];
    static stream_t ctx;
    char *env, fn[PATH_MAX];
    struct stat st;
    struct tm *lt;
    time_t now = time(NULL);
    int i, targetId = gtk_combo_box_get_active(GTK_COMBO_BOX(target));
    (void)data;

    if(targetId >= 0 && targetId < DISKS_MAX && disks_targets[targetId] >= 1024) return NULL;

    ctx.fileSize = 0;
    src = (int)((long int)disks_open(targetId, 0));
    if(src > 0) {
        fn[0] = 0;
        if(bkpdir && !stat(bkpdir, &st)) {
            strncpy(fn, bkpdir, sizeof(fn)-1);
        } else {
            if((env = getenv("HOME")))
                strncpy(fn, env, sizeof(fn)-1);
            else if((env = getenv("LOGNAME")))
                snprintf(fn, sizeof(fn)-1, "/home/%s", env);
            if(!fn[0]) strcpy(fn, ".");
            i = strlen(fn);
            strncpy(fn + i, "/Desktop", sizeof(fn)-1-i);
            if(stat(fn, &st)) {
                strncpy(fn + i, "/Downloads", sizeof(fn)-1-i);
                if(stat(fn, &st)) strcpy(fn, ".");
            }
        }
        i = strlen(fn);
        lt = localtime(&now);
        snprintf(fn + i, sizeof(fn)-1-i, "/usbimager-%04d%02d%02dT%02d%02d.dd%s",
            lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday, lt->tm_hour, lt->tm_min,
            needCompress ? ".zst" : "");
        gtk_entry_set_text(GTK_ENTRY(source), fn);

        if(!stream_create(&ctx, fn, needCompress, disks_capacity[targetId])) {
            while(mainwin && ctx.readSize < ctx.fileSize) {
                errno = 0;
                size = ctx.fileSize - ctx.readSize < (uint64_t)buffer_size ? (int)(ctx.fileSize - ctx.readSize) : buffer_size;
                numberOfBytesRead = (int)read(src, ctx.buffer, size);
                if(verbose > 1) printf("read(%d) numberOfBytesRead %d errno=%d\n", size, numberOfBytesRead, errno);
                if(numberOfBytesRead == size) {
                    if(stream_write(&ctx, ctx.buffer, size)) {
                        main_onProgress(&ctx);
                    } else {
                        if(errno) main_errorMessage = strerror(errno);
                        main_onThreadError(lang[L_WRIMGERR]);
                        break;
                    }
                } else {
                    if(errno) main_errorMessage = strerror(errno);
                    main_onThreadError(lang[L_RDSRCERR]);
                    break;
                }
            }
            stream_close(&ctx);
            if(errno == ENOSPC) remove(fn);
        } else {
            if(errno) main_errorMessage = strerror(errno);
            main_onThreadError(lang[L_OPENIMGERR]);
        }
        disks_close((void*)((long int)src));
    } else {
        main_onThreadError(lang[src == -1 ? L_TRGERR : (src == -2 ? L_UMOUNTERR : (src == -4 ? L_COMMERR : L_OPENTRGERR))]);
    }
    stream_status(&ctx, lpStatus, 1);
    main_onDone(&lpStatus);
    if(verbose) printf("Worker thread finished.\r\n");
    return NULL;
}

static void onReadButtonClicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;

    setSensitive(FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 0);
    gtk_label_set_label(GTK_LABEL(status), "");
    main_errorMessage = NULL;
    if(verbose) printf("Starting worker thread for reading.\r\n");
#ifdef USE_THREADS
    thrd = g_thread_new("reader", readerRoutine, NULL);
#else
    readerRoutine(NULL);
#endif
}

static void refreshBlkSize(GtkWidget *w, gpointer data)
{
    int current = gtk_combo_box_get_active(GTK_COMBO_BOX(blksize));
    (void)w;
    (void)data;
    buffer_size = (1UL << current) * 1024UL * 1024UL;
}
#endif

static void doRefreshTarget(void)
{
    int current = gtk_combo_box_get_active(GTK_COMBO_BOX(target));
    gtk_list_store_clear(GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(target))));
    disks_refreshlist();
    /* we should resize the popup now, but neither of these work... */
    gtk_widget_compute_expand(target, GTK_ORIENTATION_VERTICAL);
    gtk_widget_queue_resize(target);
    gtk_widget_queue_allocate(target);
    gtk_widget_queue_draw(target);
    gtk_widget_show_all(GTK_WIDGET(mainwin));
    while(gtk_events_pending()) gtk_main_iteration();
    if(current < 0 || current >= gtk_tree_model_iter_n_children(GTK_TREE_MODEL(gtk_combo_box_get_model(GTK_COMBO_BOX(target))), NULL))
        current = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(target), current);
    if(current >= 0 && current < DISKS_MAX && disks_targets[current] >= 1024) {
        gtk_button_set_label(GTK_BUTTON(writeButton), lang[L_SEND]);
#if !defined(USE_WRONLY) || !USE_WRONLY
        gtk_widget_set_sensitive(readButton, FALSE);
#endif
    } else {
        gtk_button_set_label(GTK_BUTTON(writeButton), lang[L_WRITE]);
#if !defined(USE_WRONLY) || !USE_WRONLY
        gtk_widget_set_sensitive(readButton, TRUE);
#endif
    }
}

static void refreshTarget(GtkWidget *w, gpointer data)
{
    GdkEvent *evt = gtk_get_current_event();
    int pressed = !evt || evt->type == GDK_BUTTON_PRESS;
    (void)w;
    (void)data;
    /* there's no reliable "popup is about to open" signal on GtkComboBox, only grab-notify, which is sent
     * on open and on close too. Only refresh when it was triggered by a mouse button press. */
    if(evt) gdk_event_free(evt);
    if(pressed) doRefreshTarget();
}

static void onRefreshClicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    doRefreshTarget();
}

static void onSelectClicked(GtkButton *btn, gpointer data)
{
    char *fn;
    const char *exts[] = { "*.[iI][sS][oO]", "*.[iI][mM][gG]", "*.[bB][iI][nN]", "*.[rR][aA][wW]", "*.[dD][dD]",
        "*.[gG][zZ]", "*.[bB][zZ]2", "*.[xX][zZ]", "*.[zZ][sS][tT]", "*.[zZ][iI][pP]", "*.[zZ][zZ][zZ]",
        "*.[tT][aA][rR]", "*.[cC][pP][iI][oO]", "*.[pP][aA][xX]", NULL };
    int i;
    GtkFileFilter *filter;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(lang[L_OPEN], GTK_WINDOW(mainwin), GTK_FILE_CHOOSER_ACTION_OPEN,
        lang[L_CANCEL], GTK_RESPONSE_CANCEL, lang[L_OPEN], GTK_RESPONSE_ACCEPT, NULL);
    (void)btn;
    (void)data;
    /* raw images, compressed images and archives that stream.c can decode */
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "*.iso *.img *.bin *.raw *.dd *.gz *.bz2 *.xz *.zst *.zip *.zzz *.tar *.cpio *.pax");
    for(i = 0; exts[i]; i++)
        gtk_file_filter_add_pattern(filter, exts[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, lang[L_ALLFILES]);
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if(fn) {
            gtk_entry_set_text(GTK_ENTRY(source), fn);
            g_free(fn);
        }
    }
    gtk_widget_destroy(chooser);
}

static void onDragDataReceived(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel, guint info,
    guint time, gpointer data)
{
    char **uris, *fn;
    (void)w; (void)x; (void)y; (void)info; (void)data;
    if(gtk_widget_get_sensitive(source) && (uris = gtk_selection_data_get_uris(sel))) {
        if(uris[0] && (fn = g_filename_from_uri(uris[0], NULL, NULL))) {
            gtk_entry_set_text(GTK_ENTRY(source), fn);
            g_free(fn);
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static void onAboutClicked(GtkButton *btn, gpointer data)
{
    const char *authors[] = { "bzt", NULL };
    (void)btn;
    (void)data;
    gtk_show_about_dialog(GTK_WINDOW(mainwin),
        "program-name", "USBImager",
        "version", USBIMAGER_VERSION
#ifdef USBIMAGER_BUILD
        " (build " USBIMAGER_BUILD ")"
#endif
        ,
        "comments", "Writes compressed disk images to USB drives and creates backups",
        "website", "https://gitlab.com/bztsrc/usbimager",
        "copyright", "Copyright (C) 2020 bzt",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo", gtk_window_get_icon(GTK_WINDOW(mainwin)),
        NULL);
}

static void freePixels(guchar *pixels, gpointer data)
{
    (void)data;
    g_free(pixels);
}

/**
 * Create a pixbuf from the embedded ARGB window manager icon
 */
static GdkPixbuf *getIcon(void)
{
    int i, w = wm_icon[0], h = wm_icon[1];
    guchar *pixels = g_malloc(w * h * 4);
    for(i = 0; i < w * h; i++) {
        pixels[i*4 + 0] = (wm_icon[2 + i] >> 16) & 0xFF;
        pixels[i*4 + 1] = (wm_icon[2 + i] >> 8) & 0xFF;
        pixels[i*4 + 2] = wm_icon[2 + i] & 0xFF;
        pixels[i*4 + 3] = (wm_icon[2 + i] >> 24) & 0xFF;
    }
    return gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
        freePixels, NULL);
}

/**
 * Helpers to build the modern layout
 */
static GtkWidget *newCard(void)
{
    GtkWidget *card = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(card), 10);
    gtk_grid_set_column_spacing(GTK_GRID(card), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "usbimager-card");
    return card;
}

static GtkWidget *newIcon(const char *name)
{
    GtkWidget *icon = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_style_context_add_class(gtk_widget_get_style_context(icon), "usbimager-icon");
    return icon;
}

static GtkWidget *newIconButton(const char *name, const char *tooltip, GCallback cb)
{
    GtkWidget *btn = gtk_button_new_from_icon_name(name, GTK_ICON_SIZE_BUTTON);
    if(tooltip) gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

static GtkWidget *newActionButton(const char *icon, const char *label, const char *style, GCallback cb)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(btn), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "usbimager-action");
    if(style) gtk_style_context_add_class(gtk_widget_get_style_context(btn), style);
    gtk_widget_set_hexpand(btn, TRUE);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

static void onClosing(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
#ifdef USE_THREADS
    if(thrd) { g_thread_unref(thrd); thrd = NULL; }
#endif
    gtk_widget_destroy(mainwin);
    gtk_main_quit();
    mainwin = NULL;
}

int main(int argc, char **argv)
{
    int i, j;
    char *lc = getenv("LANG"), btntext[256];
    GtkWidget *vbox, *hbox, *header, *card, *linked;
    GtkCssProvider *provider;
    GdkPixbuf *icon;
    char help[] = "USBImager " USBIMAGER_VERSION
#if USE_WRONLY
        "_wo"
#endif
#if USE_UDISKS2
        "_udisks2"
#endif
#ifdef USBIMAGER_BUILD
        " (build " USBIMAGER_BUILD ")"
#endif
        " - MIT license, Copyright (C) 2020 bzt\r\n\r\n"
        "./usbimager [-v|-vv|-a|-f|-s[baud]|-S[baud]|-1|-2|-3|-4|-5|-6|-7|-8|-9|-L(xx)|-m(x)] <backup path>\r\n\r\n"
        "https://gitlab.com/bztsrc/usbimager\r\n\r\n";

    for(j = 1; j < argc && argv[j]; j++) {
        if(argv[j][0] == '-') {
            if(!strcmp(argv[j], "--version")) {
                printf(USBIMAGER_VERSION "\n");
                exit(0);
            }
            if(!strcmp(argv[j], "--help")) {
                printf("%s", help);
                exit(0);
            }
            for(i = 1; argv[j][i]; i++)
                switch(argv[j][i]) {
                    case 'f': force++; break;
                    case 'v':
                        verbose++;
                        if(verbose == 1) printf("%s", help);
                    break;
                    case 's':
                        disks_serial = 1;
                        if(argv[j][i+1] >= '0' && argv[j][i+1] <= '9') {
                            stream_baud(atoi(argv[j] + i + 1));
                            while(argv[j][i+1] >= '0' && argv[j][i+1] <= '9') i++;
                        }
                        break;
                    case 'S':
                        disks_serial = 2;
                        if(argv[j][i+1] >= '0' && argv[j][i+1] <= '9') {
                            stream_baud(atoi(argv[j] + i + 1));
                            while(argv[j][i+1] >= '0' && argv[j][i+1] <= '9') i++;
                        }
                        break;
                    case 'a': disks_all = 1; break;
                    case '1': blksizesel = 1; buffer_size = 2*1024*1024; break;
                    case '2': blksizesel = 2; buffer_size = 4*1024*1024; break;
                    case '3': blksizesel = 3; buffer_size = 8*1024*1024; break;
                    case '4': blksizesel = 4; buffer_size = 16*1024*1024; break;
                    case '5': blksizesel = 5; buffer_size = 32*1024*1024; break;
                    case '6': blksizesel = 6; buffer_size = 64*1024*1024; break;
                    case '7': blksizesel = 7; buffer_size = 128*1024*1024; break;
                    case '8': blksizesel = 8; buffer_size = 256*1024*1024; break;
                    case '9': blksizesel = 9; buffer_size = 512*1024*1024; break;
                    case 'L': lc = &argv[j][++i]; ++i; break;
                    case 'm': disks_maxsize = atoi(&argv[j][++i]); continue;
                }
        } else
            bkpdir = argv[j];
    }

#if MACOSX
    if(!lc) lc = disks_getlang();
#endif
    if(!lc) lc = "en";
    for(i = 0; i < NUMLANGS; i++) {
        if(!memcmp(lc, dict[i][0], strlen(dict[i][0]))) {
            lang = &dict[i][1];
            break;
        }
    }
    if(!lang) lang = &dict[0][1];

    if(verbose) {
        printf("LANG '%s', dict '%s', serial %d, buffer_size %d MiB, force %d\r\n",
            lc, lang[-1], disks_serial, buffer_size/1024/1024, force);
        printf("disks_maxsize %d GiB\r\n", disks_maxsize);
        if(disks_serial) printf("Serial %d,8,n,1\r\n", baud);
#if !defined(USE_WRONLY) || !USE_WRONLY
        if(bkpdir) printf("bkpdir '%s'\r\n", bkpdir);
#endif
    }

    gtk_init(&argc, &argv);

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    mainwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(mainwin), "USBImager " USBIMAGER_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(mainwin), 520, -1);
    icon = getIcon();
    gtk_window_set_icon(GTK_WINDOW(mainwin), icon);
    g_object_unref(icon);
    g_signal_connect(mainwin, "destroy", G_CALLBACK(onClosing), NULL);

    /* accept image files dropped anywhere on the window */
    gtk_drag_dest_set(mainwin, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(mainwin);
    g_signal_connect(mainwin, "drag-data-received", G_CALLBACK(onDragDataReceived), NULL);

    header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "USBImager");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), USBIMAGER_VERSION);
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), newIconButton("help-about-symbolic", NULL, G_CALLBACK(onAboutClicked)));
    gtk_window_set_titlebar(GTK_WINDOW(mainwin), header);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);
    gtk_container_add(GTK_CONTAINER(mainwin), vbox);

    /* source image and target device */
    card = newCard();
    gtk_box_pack_start(GTK_BOX(vbox), card, FALSE, TRUE, 0);

    gtk_grid_attach(GTK_GRID(card), newIcon("media-optical-symbolic"), 0, 0, 1, 1);
    linked = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(linked), "linked");
    gtk_widget_set_hexpand(linked, TRUE);
    gtk_grid_attach(GTK_GRID(card), linked, 1, 0, 1, 1);
    source = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(source), "*.iso *.img *.gz *.bz2 *.xz *.zst *.zip ...");
    /* the window handles drops, the entry would insert the uri as text */
    gtk_drag_dest_unset(source);
    gtk_box_pack_start(GTK_BOX(linked), source, TRUE, TRUE, 0);
    sourceButton = newIconButton("document-open-symbolic", lang[L_OPEN], G_CALLBACK(onSelectClicked));
    gtk_box_pack_start(GTK_BOX(linked), sourceButton, FALSE, TRUE, 0);

    gtk_grid_attach(GTK_GRID(card), newIcon("drive-removable-media-symbolic"), 0, 1, 1, 1);
    linked = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(linked), "linked");
    gtk_widget_set_hexpand(linked, TRUE);
    gtk_grid_attach(GTK_GRID(card), linked, 1, 1, 1, 1);
    target = gtk_combo_box_text_new();
    /* GtkComboBox has no "popup-shown" signal, grab-notify is the only one sent when the popup opens */
    g_signal_connect(target, "grab-notify", G_CALLBACK(refreshTarget), NULL);
    gtk_box_pack_start(GTK_BOX(linked), target, TRUE, TRUE, 0);
    refreshButton = newIconButton("view-refresh-symbolic", NULL, G_CALLBACK(onRefreshClicked));
    gtk_box_pack_start(GTK_BOX(linked), refreshButton, FALSE, TRUE, 0);

#if !defined(USE_WRONLY) || !USE_WRONLY
    /* options */
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    verify = gtk_check_button_new_with_label(lang[L_VERIFY]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(verify), 1);
    gtk_box_pack_start(GTK_BOX(hbox), verify, FALSE, TRUE, 4);

    compr = gtk_check_button_new_with_label(lang[L_COMPRESS]);
    gtk_box_pack_start(GTK_BOX(hbox), compr, FALSE, TRUE, 0);

    blksize = gtk_combo_box_text_new();
    for(i = 0; i < 10; i++) {
        sprintf(btntext, "%d %s", (1<<i), lang[L_MIB]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(blksize), NULL, btntext);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(blksize), blksizesel);
    g_signal_connect(blksize, "changed", G_CALLBACK(refreshBlkSize), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), blksize, FALSE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), newIcon("drive-harddisk-symbolic"), FALSE, TRUE, 0);

    /* actions */
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    writeButton = newActionButton("go-down-symbolic", lang[L_WRITE], "destructive-action", G_CALLBACK(onWriteButtonClicked));
    gtk_box_pack_start(GTK_BOX(hbox), writeButton, TRUE, TRUE, 0);

    readButton = newActionButton("go-up-symbolic", lang[L_READ], NULL, G_CALLBACK(onReadButtonClicked));
    gtk_box_pack_start(GTK_BOX(hbox), readButton, TRUE, TRUE, 0);
#else
    writeButton = newActionButton("go-down-symbolic", lang[L_WRITE], "destructive-action", G_CALLBACK(onWriteButtonClicked));
    gtk_box_pack_start(GTK_BOX(vbox), writeButton, FALSE, TRUE, 0);
#endif

    /* progress */
    hbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    pbar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(hbox), pbar, FALSE, TRUE, 0);

    status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(status), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(status), "usbimager-status");
    gtk_box_pack_start(GTK_BOX(hbox), status, FALSE, FALSE, 0);

    doRefreshTarget();

    gtk_widget_show_all(GTK_WIDGET(mainwin));
    /* don't focus the entry on startup, so that its placeholder is visible */
    gtk_window_set_focus(GTK_WINDOW(mainwin), NULL);
    gtk_main();
#ifdef USE_THREADS
    if(thrd) { g_thread_unref(thrd); thrd = NULL; }
#endif
    return 0;
}

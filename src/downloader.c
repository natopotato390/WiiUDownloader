#include <curl/curl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <cdecrypt/cdecrypt.h>
#include <downloader.h>
#include <keygen.h>
#include <nfd.h>
#include <tmd.h>
#include <utils.h>

#include <curl/curl.h>
#include <gtk/gtk.h>

struct MemoryStruct {
    uint8_t *memory;
    size_t size;
};

struct CURLProgress {
    GtkWidget *progress_bar;
    GtkWidget *gameLabel;
    CURL *handle;
};

static GtkWidget *window;

static char currentFile[255] = "None";
static char currentTitle[1024] = "None";
static char *selected_dir = NULL;
static bool cancelled = false;
static bool paused = false;
static size_t titleSize = 0;
static size_t downloadedSize = 0;
static size_t previousDownloadedSize = 0;
static char totalSize[255];
static bool *queueCancelled;

static char *readable_fs(double size, char *buf) {
    int i = 0;
    const char *units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    while (size > 1024) {
        size /= 1024;
        i++;
    }
    sprintf(buf, "%.*f %s", i, size, units[i]);
    return buf;
}

static size_t write_function(void *data, size_t size, size_t nmemb, void *userp) {
    size_t written = fwrite(data, size, nmemb, userp);
    return cancelled ? 0 : written;
}

static void cancel_button_clicked(GtkWidget *widget, gpointer data) {
    cancelled = true;
    *queueCancelled = true;
}

static void pause_button_clicked(GtkWidget *widget, gpointer data) {
    struct CURLProgress *progress = (struct CURLProgress *) data;
    if (paused) {
        curl_easy_pause(progress->handle, CURLPAUSE_CONT);
        gtk_button_set_label(GTK_BUTTON(widget), "Pause");
    } else {
        curl_easy_pause(progress->handle, CURLPAUSE_ALL);
        gtk_button_set_label(GTK_BUTTON(widget), "Resume");
    }
    paused = !paused;
}

//LibCurl progress function
int progress_func(void *p,
                  curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal == 0)
        dltotal = 1;
    if (dlnow == 0)
        dlnow = 1;
    struct CURLProgress *progress = (struct CURLProgress *) p;

    char downloadString[1024];
    char speedString[255];
    char downNow[255];
    downloadedSize -= previousDownloadedSize;
    downloadedSize += dlnow;
    readable_fs(downloadedSize, downNow);
    double speed;
    curl_easy_getinfo(progress->handle, CURLINFO_SPEED_DOWNLOAD, &speed);
    readable_fs(speed, speedString);
    strcat(speedString, "/s");
    sprintf(downloadString, "Downloading %s (%s/%s) (%s)", currentFile, downNow, totalSize, speedString);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress->progress_bar), (double) downloadedSize / (double) titleSize);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress->progress_bar), downloadString);
    // force redraw
    while (gtk_events_pending())
        gtk_main_iteration();

    previousDownloadedSize = dlnow;
    return 0;
}

static size_t WriteDataToMemory(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *) userp;

    mem->memory = realloc(mem->memory, mem->size + realsize);
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;

    return realsize;
}

static void progressDialog(struct CURLProgress *progress) {
    gtk_init(NULL, NULL);
    GtkWidget *cancelButton = gtk_button_new();
    GtkWidget *pauseButton = gtk_button_new();
    progress->gameLabel = gtk_label_new(currentTitle);

    //Create window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Download Progress");
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 50);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);

    //Create progress bar
    progress->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress->progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress->progress_bar), "Downloading");

    gtk_button_set_label(GTK_BUTTON(cancelButton), "Cancel");
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(cancel_button_clicked), NULL);

    gtk_button_set_label(GTK_BUTTON(pauseButton), "Pause");
    g_signal_connect(pauseButton, "clicked", G_CALLBACK(pause_button_clicked), progress);

    gtk_label_set_text(GTK_LABEL(progress->gameLabel), currentTitle);

    //Create container for the window
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), main_box);
    gtk_box_pack_start(GTK_BOX(main_box), progress->gameLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), progress->progress_bar, FALSE, FALSE, 0);
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(main_box), button_box);
    gtk_box_pack_end(GTK_BOX(button_box), cancelButton, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_box), pauseButton, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
}

static int downloadFile(const char *download_url, const char *output_path, struct CURLProgress *progress) {
    previousDownloadedSize = 0;
    FILE *file = fopen(output_path, "wb");
    if (file == NULL)
        return 1;
    curl_easy_setopt(progress->handle, CURLOPT_FAILONERROR, 1L);

    curl_easy_setopt(progress->handle, CURLOPT_WRITEFUNCTION, write_function);
    curl_easy_setopt(progress->handle, CURLOPT_URL, download_url);
    curl_easy_setopt(progress->handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(progress->handle, CURLOPT_XFERINFOFUNCTION, progress_func);
    curl_easy_setopt(progress->handle, CURLOPT_PROGRESSDATA, progress);

    curl_easy_setopt(progress->handle, CURLOPT_WRITEDATA, file);

    curl_easy_setopt(progress->handle, CURLOPT_SSL_VERIFYPEER, FALSE);
    curl_easy_setopt(progress->handle, CURLOPT_SSL_VERIFYHOST, FALSE);
    curl_easy_setopt(progress->handle, CURLOPT_ACCEPTTIMEOUT_MS, 5);
    curl_easy_setopt(progress->handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(progress->handle, CURLOPT_TCP_NODELAY, 1);
    curl_easy_setopt(progress->handle, CURLOPT_CONNECTTIMEOUT, 5);
    curl_easy_setopt(progress->handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(progress->handle, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(progress->handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(progress->handle, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(progress->handle, CURLOPT_TCP_KEEPINTVL, 60L);

    curl_easy_perform(progress->handle);
    fclose(file);
    return 0;
}

// function to return the path of the selected folder
static char *show_folder_select_dialog() {
    NFD_Init();

    nfdchar_t *outPath = NULL;

    nfdresult_t result = NFD_PickFolder(&outPath, NULL);

    // Quit NFD
    NFD_Quit();

    return outPath;
}

static void prepend(char *s, const char *t) {
    size_t len = strlen(t);
    memmove(s + len, s, strlen(s) + 1);
    memcpy(s, t, len);
}

static char *dirname(char *path) {
    int len = strlen(path);
    int last = len - 1;
    char *parent = malloc(sizeof(char) * (len + 1));
    strcpy(parent, path);
    parent[len] = '\0';

    while (last >= 0) {
        if (parent[last] == '/') {
            parent[last] = '\0';
            break;
        }
        last--;
    }
    return parent;
}

void downloadTitle(const char *titleID, const char *name, bool decrypt, bool *cancelQueue) {
    // initialize some useful variables
    cancelled = false;
    queueCancelled = cancelQueue;
    strcpy(currentTitle, name);
    if (*queueCancelled) {
        return;
    }
    char *output_dir = malloc(1024);
    char folder_name[1024];
    getTitleNameFromTid(strtoull(titleID, NULL, 16), folder_name);
    strcpy(output_dir, folder_name);
    prepend(output_dir, "/");
    if (selected_dir == NULL)
        selected_dir = show_folder_select_dialog();
    if (selected_dir == NULL) {
        free(output_dir);
        return;
    }
    prepend(output_dir, selected_dir);
    if (output_dir[strlen(output_dir) - 1] == '/' || output_dir[strlen(output_dir) - 1] == '\\') {
        output_dir[strlen(output_dir) - 1] = '\0';
    }
    char base_url[69];
    snprintf(base_url, 69, "http://ccs.cdn.c.shop.nintendowifi.net/ccs/download/%s", titleID);
    char download_url[81];
    char output_path[strlen(output_dir) + 14];

// create the output directory if it doesn't exist
#ifdef _WIN32
    mkdir(output_dir);
#else
    mkdir(output_dir, 0700);
#endif

    // initialize curl
    curl_global_init(CURL_GLOBAL_ALL);

    // make an own handle for the tmd file, as we wanna download it to memory first
    CURL *tmd_handle = curl_easy_init();
    curl_easy_setopt(tmd_handle, CURLOPT_FAILONERROR, 1L);

    // Download the tmd and save it in memory, as we need some data from it
    curl_easy_setopt(tmd_handle, CURLOPT_WRITEFUNCTION, WriteDataToMemory);
    snprintf(download_url, 73, "%s/%s", base_url, "tmd");
    curl_easy_setopt(tmd_handle, CURLOPT_URL, download_url);

    struct MemoryStruct tmd_mem;
    tmd_mem.memory = malloc(0);
    tmd_mem.size = 0;
    curl_easy_setopt(tmd_handle, CURLOPT_WRITEDATA, (void *) &tmd_mem);
    curl_easy_perform(tmd_handle);
    curl_easy_cleanup(tmd_handle);
    // write out the tmd file
    snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, "title.cert");
    generateCert(output_path);
    snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, "title.tmd");
    FILE *tmd_file = fopen(output_path, "wb");
    if (!tmd_file) {
        free(output_dir);
        fprintf(stderr, "Error: The file \"%s\" couldn't be opened. Will exit now.\n", output_path);
        exit(EXIT_FAILURE);
    }
    fwrite(tmd_mem.memory, 1, tmd_mem.size, tmd_file);
    fclose(tmd_file);
    printf("Finished downloading \"%s\".\n", output_path);

    TMD *tmd_data = (TMD *) tmd_mem.memory;

    uint16_t title_version = tmd_data->title_version;
    snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, "title.tik");
    char titleKey[128];
    generateKey(titleID, titleKey);
    generateTicket(output_path, strtoull(titleID, NULL, 16), titleKey, title_version);

    uint16_t content_count = bswap_16(tmd_data->num_contents);

    titleSize = 0;
    downloadedSize = 0;
    previousDownloadedSize = 0;
    struct CURLProgress *progress = (struct CURLProgress *) malloc(sizeof(struct CURLProgress));
    progress->handle = curl_easy_init();
    progressDialog(progress);
    for (size_t i = 0; i < content_count; i++) {
        titleSize += bswap_64(tmd_data->contents[i].size);
    }
    readable_fs(titleSize, totalSize);
    printf("Total size: %s (%zu)\n", totalSize, titleSize);
    for (int i = 0; i < content_count; i++) {
        if (!cancelled) {
            int offset = 2820 + (48 * i);
            uint32_t id = bswap_32(tmd_data->contents[i].cid); // the id should usually be chronological, but we wanna be sure

            // add a curl handle for the content file (.app file)
            snprintf(output_path, sizeof(output_path), "%s/%08X.app", output_dir, id);
            snprintf(download_url, 78, "%s/%08X", base_url, id);
            sprintf(currentFile, "%08X.app", id);
            downloadFile(download_url, output_path, progress);

            if (tmd_data->contents[i].type & TMD_CONTENT_TYPE_HASHED) {
                // add a curl handle for the hash file (.h3 file)
                snprintf(output_path, sizeof(output_path), "%s/%08X.h3", output_dir, id);
                snprintf(download_url, 81, "%s/%08X.h3", base_url, id);
                sprintf(currentFile, "%08X.h3", id);
                downloadFile(download_url, output_path, progress);
            }
        }
    }
    free(tmd_mem.memory);

    printf("Downloading all files for TitleID %s done...\n", titleID);

    // cleanup curl stuff
    gtk_widget_destroy(GTK_WIDGET(window));
    curl_easy_cleanup(progress->handle);
    curl_global_cleanup();
    if (decrypt && !cancelled) {
        char *argv[2] = {"WiiUDownloader", dirname(output_path)};
        cdecrypt(2, argv);
    }
    free(output_dir);
    free(progress);
}
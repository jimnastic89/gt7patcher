/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Save images are mounted the way earthonion's Garlic SaveMgr does it: the
 * image keeps its sealed key at 0x800, /dev/pfsmgr unseals it, and
 * sceFsMountSaveData mounts the image with it. Mounting straight from
 * savedata_prospero fails, so every image is copied under /data first, and
 * a changed copy replaces the save only after it has been mounted again and
 * read back.
 */
#include "career_import.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "career.h"
#include "notify.h"

#define CAREER_DIR "/data/gt7p/career"
#define WORK_DIR CAREER_DIR "/work"
#define MNT_DIR CAREER_DIR "/mnt"
#define BACKUP_DIR CAREER_DIR "/backups"
#define ADDED_DIR CAREER_DIR "/online-saves"
#define LOG_PATH CAREER_DIR "/career.log"
#define OFFLINE_SAVE "sdimg_gt7offline"
#define OFFLINE_FILE "gt7save.bin"
#define ONLINE_FILE "memory.dat"
#define COPY_CHUNK (1u << 20)
#define PFSMGR_UNSEAL 0xc0845302

typedef struct
{
    uint8_t reserved;
    char *budgetid;
} MountOpt;
typedef struct
{
    uint8_t dummy;
} UmountOpt;
int sceFsInitMountSaveDataOpt(MountOpt *opt);
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount, uint8_t *key);
int sceFsInitUmountSaveDataOpt(UmountOpt *opt);
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle, int ignore);

static const char *const titles[] = {"PPSA01317", "PPSA01316"};
static int (*game_running)(void);
static char *cached_scan;

/* A growable reply. */
typedef struct
{
    char *s;
    size_t len, cap;
} text;

static void add_n(text *t, const char *s, size_t n)
{
    if (t->len + n + 1 > t->cap)
    {
        size_t cap = t->cap ? t->cap : 4096;
        while (cap < t->len + n + 1)
            cap *= 2;
        char *p = realloc(t->s, cap);
        if (!p)
            return;
        t->s = p;
        t->cap = cap;
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void add(text *t, const char *s)
{
    add_n(t, s, strlen(s));
}

static void addf(text *t, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        add_n(t, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static void add_str(text *t, const char *s)
{
    add(t, "\"");
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            char e[2] = {'\\', (char)c};
            add_n(t, e, 2);
        }
        else if (c < 0x20)
            addf(t, "\\u%04x", c);
        else
            add_n(t, (const char *)&c, 1);
    }
    add(t, "\"");
}

static int reply(char **out, int code, const char *msg)
{
    *out = strdup(msg);
    return code;
}

static void log_line(const char *fmt, ...)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%s ", when);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

static int failed(char **out, const char *msg)
{
    log_line("error: %s", msg);
    return reply(out, 409, msg);
}

/* Files */

static int is_file(const char *path, time_t *mtime, size_t *size)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode))
        return 0;
    if (mtime)
        *mtime = st.st_mtime;
    if (size)
        *size = (size_t)st.st_size;
    return 1;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    uint8_t *buf = malloc(COPY_CHUNK);
    int rc = buf ? 0 : -1;
    while (!rc)
    {
        ssize_t n = read(in, buf, COPY_CHUNK);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            rc = -1;
        if (n <= 0)
            break;
        rc = write_all(out, buf, (size_t)n);
    }
    free(buf);
    if (fsync(out))
        rc = -1;
    close(out);
    close(in);
    return rc;
}

static int same_file(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    uint8_t *x = malloc(COPY_CHUNK), *y = malloc(COPY_CHUNK);
    int same = fa >= 0 && fb >= 0 && x && y;
    while (same)
    {
        ssize_t n = read(fa, x, COPY_CHUNK);
        ssize_t m = n > 0 ? read(fb, y, (size_t)n) : read(fb, y, 1);
        if (n < 0 || m != (n > 0 ? n : 0))
            same = 0;
        else if (!n)
            break;
        else if (memcmp(x, y, (size_t)n))
            same = 0;
    }
    free(x);
    free(y);
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return same;
}

static int copy_checked(const char *src, const char *dst)
{
    return !copy_file(src, dst) && same_file(src, dst) ? 0 : -1;
}

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    *buf = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 0)
    {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size, got = 0;
    uint8_t *p = malloc(size + 1);
    while (p && got < size)
    {
        ssize_t n = read(fd, p + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (!p || got != size)
    {
        free(p);
        return -1;
    }
    *buf = p;
    *len = size;
    return 0;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, buf, len);
    if (fsync(fd))
        rc = -1;
    close(fd);
    return rc;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
    {
        unlink(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            remove_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

/* Save images */

typedef struct
{
    uint64_t authid;
    uint8_t caps[16];
} credentials;

/* The save-mount rights, held only for one request. */
static int raise_rights(credentials *saved)
{
    pid_t self = getpid();
    saved->authid = kernel_get_ucred_authid(self);
    if (!saved->authid || kernel_get_ucred_caps(self, saved->caps))
        return -1;
    uint8_t caps[16];
    memcpy(caps, saved->caps, sizeof(caps));
    caps[7] |= 0x40;
    if (kernel_set_ucred_authid(self, 0x4800000000000010ULL) || kernel_set_ucred_caps(self, caps))
    {
        kernel_set_ucred_authid(self, saved->authid);
        kernel_set_ucred_caps(self, saved->caps);
        return -1;
    }
    setuid(0);
    return 0;
}

static void drop_rights(credentials *saved)
{
    pid_t self = getpid();
    kernel_set_ucred_authid(self, saved->authid);
    kernel_set_ucred_caps(self, saved->caps);
}

static void unmount_save(void)
{
    UmountOpt opt;
    memset(&opt, 0, sizeof(opt));
    sceFsInitUmountSaveDataOpt(&opt);
    sceFsUmountSaveData(&opt, MNT_DIR, 0, 0);
    sync();
}

static int mount_save(const char *image)
{
    uint8_t *data = calloc(1, 0x100);
    if (!data)
        return -1;
    int fd = open(image, O_RDONLY);
    int ok = fd >= 0 && pread(fd, data, 0x60, 0x800) == 0x60;
    if (fd >= 0)
        close(fd);
    int pfs = ok ? open("/dev/pfsmgr", O_RDWR) : -1;
    ok = pfs >= 0 && ioctl(pfs, PFSMGR_UNSEAL, data) >= 0;
    if (pfs >= 0)
        close(pfs);
    uint8_t key[0x20];
    memcpy(key, data + 0x60, sizeof(key));
    free(data);
    if (!ok)
        return -1;
    mkdir(MNT_DIR, 0777);
    MountOpt opt;
    memset(&opt, 0, sizeof(opt));
    sceFsInitMountSaveDataOpt(&opt);
    opt.budgetid = "system";
    return sceFsMountSaveData(&opt, image, MNT_DIR, key) < 0 ? -1 : 0;
}

/* Read one file out of a save image through a mounted copy of it. */
static int read_from_save(const char *image, const char *inner, uint8_t **buf, size_t *len, const char **why)
{
    const char *copy = WORK_DIR "/read.img";
    char path[256];
    *buf = NULL;
    if (copy_file(image, copy))
    {
        *why = "the save could not be copied";
        return -1;
    }
    if (mount_save(copy))
    {
        unlink(copy);
        *why = "the console could not open this save (a save from another console cannot be opened)";
        return -1;
    }
    snprintf(path, sizeof(path), MNT_DIR "/%s", inner);
    int rc = read_file(path, buf, len);
    if (rc)
        *why = !strcmp(inner, ONLINE_FILE) ? "it holds no " ONLINE_FILE : "it holds no " OFFLINE_FILE;
    unmount_save();
    unlink(copy);
    return rc;
}

/* Names and requests */

static int hex_name(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 16)
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')))
            return 0;
    return 1;
}

static int file_name(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (!n || n > max || s[0] == '.')
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '.' ||
              *s == '_' || *s == '-'))
            return 0;
    return 1;
}

static int known_title(const char *s)
{
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        if (!strcmp(s, titles[i]))
            return 1;
    return 0;
}

/* The value of key in a form body or query string; only plain characters
 * (letters, digits, ".:_-") are accepted. */
static int field(const char *form, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    out[0] = 0;
    for (const char *p = form; p && *p; p = strchr(p, '&'), p = p ? p + 1 : NULL)
    {
        if (strncmp(p, key, klen) || p[klen] != '=')
            continue;
        size_t n = 0;
        for (const char *v = p + klen + 1; *v && *v != '&'; v++)
        {
            char c = *v;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == ':' ||
                  c == '_' || c == '-') ||
                n + 1 >= len)
                return -1;
            out[n++] = c;
        }
        out[n] = 0;
        return n ? 0 : -1;
    }
    return -1;
}

static void save_dir(const char *uid, const char *title, char *out, size_t len)
{
    snprintf(out, len, "/user/home/%s/savedata_prospero/%s", uid, title);
}

static void user_name(const char *uid, char *out, size_t len)
{
    char path[128];
    uint8_t *buf;
    size_t n;
    snprintf(path, sizeof(path), "/user/home/%s/username.dat", uid);
    out[0] = 0;
    if (!read_file(path, &buf, &n))
    {
        size_t k = 0;
        for (; k < n && k + 1 < len && k < 16 && buf[k]; k++)
            out[k] = (char)buf[k];
        out[k] = 0;
        free(buf);
    }
    if (!out[0])
        snprintf(out, len, "User %s", uid);
}

typedef struct
{
    char key[80];
    char label[96];
    char path[320];
} source;

static int find_source(const char *uid, const char *title, const char *key, source *s)
{
    char dir[160];
    save_dir(uid, title, dir, sizeof(dir));
    memset(s, 0, sizeof(*s));
    snprintf(s->key, sizeof(s->key), "%s", key);
    if (!strcmp(key, "live"))
    {
        snprintf(s->label, sizeof(s->label), "Online save on this console");
        snprintf(s->path, sizeof(s->path), "%s/sdimg_sce_sdmemory", dir);
    }
    else if (!strcmp(key, "bu"))
    {
        snprintf(s->label, sizeof(s->label), "System backup copy of the online save");
        snprintf(s->path, sizeof(s->path), "%s/sdimg_sce_bu_sce_sdmemory", dir);
    }
    else if (!strncmp(key, "add:", 4) && file_name(key + 4, 64))
    {
        snprintf(s->label, sizeof(s->label), "File you added: %s", key + 4);
        snprintf(s->path, sizeof(s->path), ADDED_DIR "/%s", key + 4);
    }
    else
        return -1;
    return 0;
}

static void format_time(time_t t, char *out, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M", &tm);
}

/* Scan */

static void add_backups(const char *uid, const char *title, text *t)
{
    char prefix[64], names[48][64];
    int count = 0;
    snprintf(prefix, sizeof(prefix), "%s-%s-", uid, title);
    size_t plen = strlen(prefix);
    DIR *d = opendir(BACKUP_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && count < 48)
        if (!strncmp(e->d_name, prefix, plen) && file_name(e->d_name, 63))
            snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
    if (d)
        closedir(d);
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[j], names[i]) > 0)
            {
                char tmp[64];
                memcpy(tmp, names[i], sizeof(tmp));
                memcpy(names[i], names[j], sizeof(tmp));
                memcpy(names[j], tmp, sizeof(tmp));
            }
    add(t, "\"backups\":[");
    for (int i = 0; i < count; i++)
    {
        char path[256], note[200] = "", when[24] = "";
        uint8_t *buf;
        size_t n;
        snprintf(path, sizeof(path), BACKUP_DIR "/%s/note.txt", names[i]);
        if (!read_file(path, &buf, &n))
        {
            size_t k = 0;
            for (; k < n && k + 1 < sizeof(note) && buf[k] != '\n'; k++)
                note[k] = (char)buf[k];
            note[k] = 0;
            free(buf);
        }
        const char *s = names[i] + plen; /* YYYYMMDD-HHMMSS */
        if (strlen(s) >= 15)
            snprintf(when, sizeof(when), "%.4s-%.2s-%.2s %.2s:%.2s", s, s + 4, s + 6, s + 9, s + 11);
        add(t, i ? ",{\"id\":" : "{\"id\":");
        add_str(t, names[i]);
        add(t, ",\"date\":");
        add_str(t, when);
        add(t, ",\"note\":");
        add_str(t, note);
        add(t, ",\"files\":[");
        static const char *const files[] = {OFFLINE_SAVE, "gt7save.before.bin", "gt7save.after.bin"};
        int first = 1;
        for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++)
        {
            size_t size;
            snprintf(path, sizeof(path), BACKUP_DIR "/%s/%s", names[i], files[f]);
            if (!is_file(path, NULL, &size))
                continue;
            add(t, first ? "{\"name\":" : ",{\"name\":");
            add_str(t, files[f]);
            addf(t, ",\"size\":%zu}", size);
            first = 0;
        }
        add(t, "]}");
    }
    add(t, "]");
}

static void add_source(const source *s, const gt7_records *offline, text *t, int *first)
{
    time_t mtime;
    if (!is_file(s->path, &mtime, NULL))
        return;
    char when[24];
    const char *why = "";
    format_time(mtime, when, sizeof(when));
    uint8_t *mem;
    size_t mem_len;
    gt7_records online = {0};
    gt7_merge_report rep;
    memset(&rep, 0, sizeof(rep));
    int career = 0;
    char err[160] = "";
    if (read_from_save(s->path, ONLINE_FILE, &mem, &mem_len, &why))
        snprintf(err, sizeof(err), "%s", why);
    else
    {
        gt7_parse_memory(mem, mem_len, offline, &online);
        career = gt7_find(&online, "GARAGE") != NULL;
        uint8_t *merged;
        size_t merged_len;
        if (career && offline && !gt7_merge(&online, offline, &merged, &merged_len, &rep, err, sizeof(err)))
            free(merged);
        free(mem);
    }
    const gt7_record *garage = gt7_find(&online, "GARAGE");
    add(t, *first ? "{\"key\":" : ",{\"key\":");
    *first = 0;
    add_str(t, s->key);
    add(t, ",\"label\":");
    add_str(t, s->label);
    add(t, ",\"date\":");
    add_str(t, when);
    addf(t, ",\"career\":%s,\"records\":%zu,\"bytes\":%zu,\"garage\":%u,\"salvaged\":%s", career ? "true" : "false",
         online.count, gt7_records_bytes(&online), garage ? garage->size : 0, online.salvaged ? "true" : "false");
    add(t, ",\"newer\":");
    add_str(t, rep.newer);
    add(t, ",\"error\":");
    add_str(t, err);
    add(t, "}");
    gt7_records_free(&online);
}

static void scan_user(const char *uid, const char *title, text *t, int *first)
{
    char dir[160], path[256], name[40], when[24] = "";
    const char *why = "";
    save_dir(uid, title, dir, sizeof(dir));
    struct stat st;
    if (stat(dir, &st) || !S_ISDIR(st.st_mode))
        return;
    user_name(uid, name, sizeof(name));
    add(t, *first ? "{\"uid\":" : ",{\"uid\":");
    *first = 0;
    add_str(t, uid);
    add(t, ",\"title\":");
    add_str(t, title);
    add(t, ",\"name\":");
    add_str(t, name);

    snprintf(path, sizeof(path), "%s/" OFFLINE_SAVE, dir);
    time_t mtime;
    int found = is_file(path, &mtime, NULL);
    uint8_t *off = NULL;
    size_t off_len = 0;
    gt7_records offline = {0};
    char err[160] = "";
    int parsed = 0;
    if (found)
    {
        format_time(mtime, when, sizeof(when));
        if (read_from_save(path, OFFLINE_FILE, &off, &off_len, &why))
            snprintf(err, sizeof(err), "%s", why);
        else
            parsed = !gt7_parse_offline(off, off_len, &offline, err, sizeof(err)) && offline.count;
    }
    const gt7_record *garage = parsed ? gt7_find(&offline, "GARAGE") : NULL;
    addf(t, ",\"offline\":{\"found\":%s,\"usable\":%s,\"bytes\":%zu,\"garage\":%u,\"date\":", found ? "true" : "false",
         parsed ? "true" : "false", gt7_records_bytes(&offline), garage ? garage->size : 0);
    add_str(t, when);
    add(t, ",\"error\":");
    add_str(t, err);
    add(t, "},\"sources\":[");

    int sfirst = 1;
    source s;
    if (!find_source(uid, title, "live", &s))
        add_source(&s, parsed ? &offline : NULL, t, &sfirst);
    if (!find_source(uid, title, "bu", &s))
        add_source(&s, parsed ? &offline : NULL, t, &sfirst);
    DIR *d = opendir(ADDED_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)))
    {
        char key[80];
        snprintf(key, sizeof(key), "add:%s", e->d_name);
        if (file_name(e->d_name, 64) && !find_source(uid, title, key, &s))
            add_source(&s, parsed ? &offline : NULL, t, &sfirst);
    }
    if (d)
        closedir(d);
    add(t, "],");
    add_backups(uid, title, t);
    add(t, "}");
    gt7_records_free(&offline);
    free(off);
}

void career_init(int (*running)(void))
{
    game_running = running;
}

static int prepare(void)
{
    mkdir(CAREER_DIR, 0777);
    mkdir(BACKUP_DIR, 0777);
    mkdir(ADDED_DIR, 0777);
    remove_tree(WORK_DIR);
    mkdir(WORK_DIR, 0777);
    mkdir(MNT_DIR, 0777);
    struct stat st;
    return stat(WORK_DIR, &st) ? -1 : 0;
}

int career_state(int rescan, char **out)
{
    int running = game_running() != 0;
    if (!running && (rescan || !cached_scan))
    {
        credentials saved;
        if (prepare() || raise_rights(&saved))
            return reply(out, 500, "Could not get the rights to open saves.");
        text t = {0};
        add(&t, "[");
        int first = 1;
        DIR *d = opendir("/user/home");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (hex_name(e->d_name))
                for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
                    scan_user(e->d_name, titles[i], &t, &first);
        if (d)
            closedir(d);
        add(&t, "]");
        drop_rights(&saved);
        free(cached_scan);
        cached_scan = t.s;
    }
    text t = {0};
    addf(&t, "{\"gameRunning\":%s,\"scanned\":%s,\"users\":", running ? "true" : "false",
         cached_scan ? "true" : "false");
    add(&t, cached_scan ? cached_scan : "[]");
    add(&t, "}");
    *out = t.s;
    return t.s ? 200 : 500;
}

static void forget_scan(void)
{
    free(cached_scan);
    cached_scan = NULL;
}

/* Backups: BACKUP_DIR/<uid>-<title>-<YYYYMMDD-HHMMSS>[-n], each with note.txt. */
static int new_backup(const char *uid, const char *title, const char *note, char *id, size_t idlen, char *dir,
                      size_t dirlen)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    for (int n = 1; n < 100; n++)
    {
        if (n == 1)
            snprintf(id, idlen, "%s-%s-%s", uid, title, stamp);
        else
            snprintf(id, idlen, "%s-%s-%s-%d", uid, title, stamp, n);
        snprintf(dir, dirlen, BACKUP_DIR "/%s", id);
        if (!mkdir(dir, 0777))
        {
            char path[256];
            snprintf(path, sizeof(path), "%s/note.txt", dir);
            write_file(path, (const uint8_t *)note, strlen(note));
            return 0;
        }
    }
    return -1;
}

static int parse_backup(const char *id, char *uid, size_t uidlen, char *title, size_t titlelen, char *dir,
                        size_t dirlen)
{
    const char *dash = strchr(id, '-');
    if (!file_name(id, 63) || strchr(id, '.') || !dash || (size_t)(dash - id) >= uidlen ||
        strlen(dash + 1) < 10 || titlelen < 10 || dash[10] != '-')
        return -1;
    memcpy(uid, id, (size_t)(dash - id));
    uid[dash - id] = 0;
    memcpy(title, dash + 1, 9);
    title[9] = 0;
    if (!hex_name(uid) || !known_title(title))
        return -1;
    snprintf(dir, dirlen, BACKUP_DIR "/%s", id);
    struct stat st;
    return !stat(dir, &st) && S_ISDIR(st.st_mode) ? 0 : -1;
}

/* Move */

static int do_move(const char *uid, const char *title, const source *src, int dry, char **out)
{
    char dir[160], target[256], name[40], id[96] = "", bdir[160], path[256], note[400], err[200] = "", why[160] = "";
    const char *reason = "";
    save_dir(uid, title, dir, sizeof(dir));
    snprintf(target, sizeof(target), "%s/" OFFLINE_SAVE, dir);
    if (!is_file(target, NULL, NULL))
        return failed(out, "This user has no Offline Mode save yet. Start GT7 once with Offline Mode on and get to "
                           "the world map, then close it and try again.");
    if (!is_file(src->path, NULL, NULL))
        return failed(out, "That online save is not there any more. Press Check again.");
    user_name(uid, name, sizeof(name));
    log_line("move: %s (%s, %s) from %s%s", uid, name, title, src->path, dry ? ", test run" : "");

    uint8_t *mem = NULL, *off = NULL, *merged = NULL, *check = NULL;
    size_t mem_len = 0, off_len = 0, merged_len = 0, check_len = 0;
    gt7_records online = {0}, offline = {0};
    gt7_merge_report rep;
    memset(&rep, 0, sizeof(rep));
    const char *work = WORK_DIR "/target.img";
    int mounted = 0, ok = 0;

    snprintf(note, sizeof(note), "%s\nSource: %s\n",
             dry ? "Test run: the saves were not changed" : "Before moving the online career into Offline Mode",
             src->path);
    if (new_backup(uid, title, note, id, sizeof(id), bdir, sizeof(bdir)))
    {
        snprintf(err, sizeof(err), "Cannot create a backup folder in " BACKUP_DIR ".");
        goto done;
    }
    snprintf(path, sizeof(path), "%s/" OFFLINE_SAVE, bdir);
    if (copy_checked(target, path))
    {
        snprintf(err, sizeof(err), "The Offline Mode save could not be backed up. Nothing was changed.");
        goto done;
    }
    if (read_from_save(src->path, ONLINE_FILE, &mem, &mem_len, &reason))
    {
        snprintf(err, sizeof(err), "The online save cannot be read: %s.", reason);
        goto done;
    }
    if (copy_file(target, work) || mount_save(work))
    {
        snprintf(err, sizeof(err), "The Offline Mode save cannot be opened. Nothing was changed.");
        goto done;
    }
    mounted = 1;
    if (read_file(MNT_DIR "/" OFFLINE_FILE, &off, &off_len))
    {
        snprintf(err, sizeof(err), "The Offline Mode save holds no " OFFLINE_FILE ". Play until the game saves once.");
        goto done;
    }
    snprintf(path, sizeof(path), "%s/gt7save.before.bin", bdir);
    write_file(path, off, off_len);
    if (gt7_parse_offline(off, off_len, &offline, why, sizeof(why)) || !offline.count)
    {
        snprintf(err, sizeof(err), "The Offline Mode save cannot be read (%s).", why[0] ? why : "it is empty");
        goto done;
    }
    gt7_parse_memory(mem, mem_len, &offline, &online);
    if (!gt7_find(&online, "GARAGE"))
    {
        snprintf(err, sizeof(err), "This online save holds no career.");
        goto done;
    }
    if (gt7_merge(&online, &offline, &merged, &merged_len, &rep, why, sizeof(why)))
    {
        snprintf(err, sizeof(err), "The career could not be moved: %s.", why);
        goto done;
    }
    if (write_file(MNT_DIR "/" OFFLINE_FILE, merged, merged_len))
    {
        snprintf(err, sizeof(err), "Writing the new " OFFLINE_FILE " failed. Nothing was changed.");
        goto done;
    }
    unmount_save();
    mounted = 0;
    if (mount_save(work))
    {
        snprintf(err, sizeof(err), "The new save could not be opened again. Nothing was changed.");
        goto done;
    }
    mounted = 1;
    if (read_file(MNT_DIR "/" OFFLINE_FILE, &check, &check_len) || check_len != merged_len ||
        memcmp(check, merged, merged_len))
    {
        snprintf(err, sizeof(err), "The new save did not read back the same. Nothing was changed.");
        goto done;
    }
    unmount_save();
    mounted = 0;
    snprintf(path, sizeof(path), "%s/gt7save.after.bin", bdir);
    write_file(path, merged, merged_len);
    if (!dry)
    {
        if (game_running())
        {
            snprintf(err, sizeof(err), "GT7 was started. Nothing was changed.");
            goto done;
        }
        if (copy_checked(work, target))
        {
            snprintf(path, sizeof(path), "%s/" OFFLINE_SAVE, bdir);
            snprintf(err, sizeof(err), "Replacing the Offline Mode save failed; %s",
                     !copy_checked(path, target) ? "the old save was put back." : "restore the backup below.");
            goto done;
        }
    }
    ok = 1;

done:
    if (mounted)
        unmount_save();
    unlink(work);
    int code;
    if (ok)
    {
        log_line("move: done, backup %s, %zu records, left out [%s], newer [%s], recovered %d", id, rep.carried,
                 rep.dropped, rep.newer, online.salvaged);
        text t = {0};
        addf(&t, "{\"dry\":%s,\"carried\":%zu,\"salvaged\":%s,\"bytes\":%zu,\"backup\":", dry ? "true" : "false",
             rep.carried, online.salvaged ? "true" : "false", merged_len);
        add_str(&t, id);
        add(&t, ",\"dropped\":");
        add_str(&t, rep.dropped);
        add(&t, ",\"newer\":");
        add_str(&t, rep.newer);
        add(&t, "}");
        *out = t.s;
        code = 200;
        if (!dry)
            notify("GT7Patcher: Career moved for %s. Start GT7 with Offline Mode on.", name);
    }
    else
        code = failed(out, err);
    gt7_records_free(&online);
    gt7_records_free(&offline);
    free(mem);
    free(off);
    free(merged);
    free(check);
    return code;
}

int career_move(const char *body, char **out)
{
    char uid[20], title[12], key[80], dry[4] = "";
    source src;
    if (field(body, "uid", uid, sizeof(uid)) || field(body, "title", title, sizeof(title)) ||
        field(body, "source", key, sizeof(key)) || !hex_name(uid) || !known_title(title) ||
        find_source(uid, title, key, &src))
        return reply(out, 400, "Unknown user or online save.");
    field(body, "dry", dry, sizeof(dry));
    if (game_running())
        return failed(out, "GT7 is running. Close it first.");
    credentials saved;
    if (prepare() || raise_rights(&saved))
        return reply(out, 500, "Could not get the rights to open saves.");
    int code = do_move(uid, title, &src, !strcmp(dry, "1"), out);
    drop_rights(&saved);
    forget_scan();
    return code;
}

/* Back up the save as it is now, then put the backup image in its place. */
static int restore_into(const char *id, const char *uid, const char *title, const char *image, const char *target,
                        char **out)
{
    char note[128], nid[96], ndir[160], path[256];
    if (!is_file(target, NULL, NULL))
        return failed(out, "This user has no Offline Mode save to restore into. Start GT7 once with Offline Mode on.");
    const char *stamp = id + strlen(uid) + strlen(title) + 2; /* YYYYMMDD-HHMMSS */
    if (strlen(stamp) >= 15)
        snprintf(note, sizeof(note), "Before restoring the backup from %.4s-%.2s-%.2s %.2s:%.2s\n", stamp, stamp + 4,
                 stamp + 6, stamp + 9, stamp + 11);
    else
        snprintf(note, sizeof(note), "Before restoring the backup %s\n", id);
    if (new_backup(uid, title, note, nid, sizeof(nid), ndir, sizeof(ndir)))
        return failed(out, "Cannot create a backup folder in " BACKUP_DIR ".");
    snprintf(path, sizeof(path), "%s/" OFFLINE_SAVE, ndir);
    if (copy_checked(target, path))
        return failed(out, "The current save could not be backed up. Nothing was changed.");
    if (copy_checked(image, target))
        return failed(out, !copy_checked(path, target) ? "Restoring failed; the save is as it was."
                                                      : "Restoring failed. Try again, or restore the newest backup.");
    log_line("restore: %s into %s; the save before it is in %s", id, target, nid);
    text t = {0};
    add(&t, "{\"backup\":");
    add_str(&t, nid);
    add(&t, "}");
    *out = t.s;
    return 200;
}

int career_restore(const char *body, char **out)
{
    char id[80], uid[20], title[12], dir[160], image[256], sdir[160], target[256];
    if (field(body, "id", id, sizeof(id)) || parse_backup(id, uid, sizeof(uid), title, sizeof(title), dir, sizeof(dir)))
        return reply(out, 400, "Unknown backup.");
    snprintf(image, sizeof(image), "%s/" OFFLINE_SAVE, dir);
    save_dir(uid, title, sdir, sizeof(sdir));
    snprintf(target, sizeof(target), "%s/" OFFLINE_SAVE, sdir);
    if (game_running())
        return failed(out, "GT7 is running. Close it first.");
    if (!is_file(image, NULL, NULL))
        return failed(out, "This backup holds no Offline Mode save.");
    credentials saved;
    if (prepare() || raise_rights(&saved))
        return reply(out, 500, "Could not get the rights to open saves.");
    int code = restore_into(id, uid, title, image, target, out);
    drop_rights(&saved);
    forget_scan();
    return code;
}

int career_delete(const char *body, char **out)
{
    char id[80], uid[20], title[12], dir[160];
    if (field(body, "id", id, sizeof(id)) || parse_backup(id, uid, sizeof(uid), title, sizeof(title), dir, sizeof(dir)))
        return reply(out, 400, "Unknown backup.");
    remove_tree(dir);
    log_line("delete: backup %s", id);
    forget_scan();
    return reply(out, 200, "{}");
}

int career_remove_added(const char *body, char **out)
{
    char name[72], path[160];
    if (field(body, "name", name, sizeof(name)) || !file_name(name, 64))
        return reply(out, 400, "Unknown file.");
    snprintf(path, sizeof(path), ADDED_DIR "/%s", name);
    if (unlink(path))
        return failed(out, "The file could not be removed.");
    log_line("removed the added online save %s", name);
    forget_scan();
    return reply(out, 200, "{}");
}

int career_download_path(const char *query, char *path, size_t len, char *name, size_t namelen)
{
    char id[80], uid[20], title[12], dir[160];
    if (field(query, "id", id, sizeof(id)) || field(query, "file", name, namelen) || !file_name(name, 64) ||
        parse_backup(id, uid, sizeof(uid), title, sizeof(title), dir, sizeof(dir)))
        return -1;
    snprintf(path, len, "%s/%s", dir, name);
    return is_file(path, NULL, NULL) ? 0 : -1;
}

int career_upload_path(const char *query, char *path, size_t len, char *part, size_t partlen)
{
    char name[72];
    if (field(query, "name", name, sizeof(name)) || !file_name(name, 64) || prepare())
        return -1;
    snprintf(path, len, ADDED_DIR "/%s", name);
    snprintf(part, partlen, WORK_DIR "/upload.part");
    return 0;
}

void career_upload_done(const char *path, size_t size)
{
    log_line("added the online save %s (%zu bytes)", path, size);
    forget_scan();
}

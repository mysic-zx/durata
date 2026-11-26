/* 读源实现：path（read 窗口）、mem（零拷贝）、custom（调用方 vtable）。
 *
 * 三种模式对外统一成 vtable + 窗口视图，解析器不感知底层是什么。
 * 堆分配只发生在 open 阶段：source 结构体 + path 模式的窗口缓冲，
 * mem 模式全程零分配（文档 §8.1 外部内存模式）。
 */
#include "internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 底层"任意位置读"封装（只给 path 模式用）。
 *
 * 用 fd + lseek 而不是 FILE*：Android Bionic 在 API 24 之前没有
 * fseeko/ftello，32 位 ABI 的 off_t 还是 32 位——大文件必须显式走
 * lseek64（API 9+，不受 _FILE_OFFSET_BITS 影响）。Windows 走
 * _lseeki64/_read，Apple/Linux 用 lseek（off_t 64 位）。             */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
typedef long long du_file_off;
#define DU_OPEN_RDONLY(p) _open((p), _O_RDONLY | _O_BINARY)
#define DU_SEEK(fd, off) _lseeki64((fd), (du_file_off)(off), SEEK_SET)
#define DU_READ(fd, dst, n) _read((fd), (dst), (unsigned int)(n))
#define DU_CLOSE(fd) _close((fd))
static int64_t du_fd_size(int fd) {
    du_file_off cur = DU_SEEK(fd, 0);
    du_file_off end = _lseeki64(fd, 0, SEEK_END);
    _lseeki64(fd, cur, SEEK_SET);
    return (int64_t)end;
}
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__ANDROID__)
#include <sys/stat.h>
typedef off64_t du_file_off;
#define DU_SEEK(fd, off) lseek64((fd), (off), SEEK_SET)
static int64_t du_fd_size(int fd) {
    struct stat64 st;
    if (fstat64(fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}
#else
typedef off_t du_file_off;
#define DU_SEEK(fd, off) lseek((fd), (off), SEEK_SET)
static int64_t du_fd_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}
#endif
#define DU_OPEN_RDONLY(p) open((p), O_RDONLY)
#define DU_READ(fd, dst, n) read((fd), (dst), (size_t)(n))
#define DU_CLOSE(fd) close((fd))
#endif

typedef struct du_file_handle {
    int fd;
} du_file_handle_t;

static int64_t du_file_read(void* user, uint64_t off, void* dst, int64_t n) {
    du_file_handle_t* h = (du_file_handle_t*)user;
    if (DU_SEEK(h->fd, (du_file_off)off) < 0) return -1;
    int64_t total = 0;
    while (total < n) {
        int64_t got = (int64_t)DU_READ(h->fd, (char*)dst + total, n - total);
        if (got <= 0) break;
        total += got;
    }
    return total;
}

static int64_t du_file_size(void* user) {
    return du_fd_size(((du_file_handle_t*)user)->fd);
}

static void du_file_close(void* user) {
    du_file_handle_t* h = (du_file_handle_t*)user;
    if (h->fd >= 0) DU_CLOSE(h->fd);
    free(h);
}

static const du_io_vtable DU_FILE_VT = {
    DURATA_ABI_VERSION, du_file_read, du_file_size, du_file_close,
};

/* ------------------------------------------------------------------ */
/* mem 模式                                                            */
/* ------------------------------------------------------------------ */

typedef struct du_mem_handle {
    const uint8_t* buf;
    size_t len;
} du_mem_handle_t;

static int64_t du_mem_read(void* user, uint64_t off, void* dst, int64_t n) {
    du_mem_handle_t* h = (du_mem_handle_t*)user;
    if (off > h->len) return 0;
    uint64_t avail = h->len - off;
    if ((uint64_t)n > avail) n = (int64_t)avail;
    memcpy(dst, h->buf + off, (size_t)n);
    return n;
}

static int64_t du_mem_size(void* user) {
    return (int64_t)((du_mem_handle_t*)user)->len;
}

static void du_mem_close(void* user) { free(user); }

static const du_io_vtable DU_MEM_VT = {
    DURATA_ABI_VERSION, du_mem_read, du_mem_size, du_mem_close,
};

/* ------------------------------------------------------------------ */
/* 构造 / 析构                                                          */
/* ------------------------------------------------------------------ */

static du_source_t* source_new(const du_io_vtable* vt, void* user) {
    du_source_t* s = (du_source_t*)calloc(1, sizeof(du_source_t));
    if (s == NULL) return NULL;
    s->vt = vt;
    s->user = user;
    s->size = vt->size ? vt->size(user) : -1;
    s->win_cap = DU_IO_WINDOW;
    return s;
}

du_status_t du_source_open_path(const char* path, du_source_t** out) {
    if (path == NULL || out == NULL) return DU_ERR_INVALID_ARG;
    du_file_handle_t* h = (du_file_handle_t*)calloc(1, sizeof(*h));
    if (h == NULL) return DU_ERR_NOMEM;
    h->fd = DU_OPEN_RDONLY(path);
    if (h->fd < 0) {
        free(h);
        return DU_ERR_IO;
    }
    du_source_t* s = source_new(&DU_FILE_VT, h);
    if (s == NULL) {
        DU_CLOSE(h->fd);
        free(h);
        return DU_ERR_NOMEM;
    }
    s->window = (uint8_t*)malloc(s->win_cap);
    if (s->window == NULL) {
        du_source_close(s);
        return DU_ERR_NOMEM;
    }
    *out = s;
    return DU_OK;
}

du_status_t du_source_open_mem(const uint8_t* buf, size_t len, du_source_t** out) {
    if (buf == NULL || out == NULL) return DU_ERR_INVALID_ARG;
    du_mem_handle_t* h = (du_mem_handle_t*)calloc(1, sizeof(*h));
    if (h == NULL) return DU_ERR_NOMEM;
    h->buf = buf;
    h->len = len;
    du_source_t* s = source_new(&DU_MEM_VT, h);
    if (s == NULL) {
        free(h);
        return DU_ERR_NOMEM;
    }
    *out = s;
    return DU_OK;
}

du_status_t du_source_open_custom(const du_io_vtable* vt, void* user, du_source_t** out) {
    if (vt == NULL || vt->read == NULL || out == NULL) return DU_ERR_INVALID_ARG;
    if (vt->abi_version != DURATA_ABI_VERSION) return DU_ERR_INVALID_ARG;
    du_source_t* s = source_new(vt, user);
    if (s == NULL) return DU_ERR_NOMEM;
    s->window = (uint8_t*)malloc(s->win_cap);
    if (s->window == NULL) {
        du_source_close(s);
        return DU_ERR_NOMEM;
    }
    *out = s;
    return DU_OK;
}

void du_source_close(du_source_t* s) {
    if (s == NULL) return;
    if (s->vt && s->vt->close) s->vt->close(s->user);
    free(s->window);
    free(s);
}

/* ------------------------------------------------------------------ */
/* 视图与读                                                             */
/* ------------------------------------------------------------------ */

int64_t du_io_size(du_source_t* s) { return s->size; }

const uint8_t* du_io_view(du_source_t* s, uint64_t off, uint64_t need, uint64_t* avail) {
    if (avail) *avail = 0;
    if (need == 0) need = 1;

    /* mem 模式：无窗口，调用方缓冲本身就是连续视图 */
    if (s->window == NULL) {
        du_mem_handle_t* h = (du_mem_handle_t*)s->user;
        if ((int64_t)off >= (int64_t)h->len) return NULL;
        if (avail) *avail = h->len - off;
        return h->buf + off;
    }

    /* 文件尾之外 */
    if (s->size >= 0 && (int64_t)off >= s->size) return NULL;

    /* 窗口命中 */
    if (s->window != NULL &&
        off >= s->win_off && off + need <= s->win_off + s->win_len) {
        if (avail) {
            uint64_t rest = (uint64_t)s->win_len - (off - s->win_off);
            *avail = rest;
        }
        return s->window + (off - s->win_off);
    }

    if (need > s->win_cap) return NULL;   /* 约定：视图请求不超过窗口容量 */

    /* 重装窗口：从 off 开始读满（顺序访问场景一窗口一读） */
    int64_t got = s->vt->read(s->user, off, s->window, (int64_t)s->win_cap);
    if (got < 0) return NULL;
    if (got == 0) return NULL;
    s->win_off = off;
    s->win_len = (uint32_t)got;

    if ((uint64_t)got < need) {
        if (avail) *avail = (uint64_t)got;
        return s->window;   /* 尾部不足 need，给多少算多少 */
    }
    if (avail) *avail = (uint64_t)s->win_len;
    return s->window;
}

int64_t du_io_read_at(du_source_t* s, uint64_t off, void* dst, int64_t n) {
    if (n <= 0) return 0;
    if (s->size >= 0) {
        int64_t left = s->size - (int64_t)off;
        if (left < 0) return 0;
        if (n > left) n = left;
    }
    int64_t total = 0;
    uint8_t* d = (uint8_t*)dst;
    while (total < n) {
        uint64_t avail = 0;
        const uint8_t* p = du_io_view(s, off + (uint64_t)total,
                                      1, &avail);
        if (p == NULL || avail == 0) break;
        int64_t chunk = (int64_t)avail;
        if (chunk > n - total) chunk = n - total;
        memcpy(d + total, p, (size_t)chunk);
        total += chunk;
        if (avail < (uint64_t)chunk) break;
    }
    return total;
}


#define _GNU_SOURCE
#include "stateram9.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFDIO_COPY_MODE_WP
#define UFFDIO_COPY_MODE_WP (1<<1)
#endif

enum rep_kind {
    REP_RECIPE = 0,
    REP_DELTA = 1,
    REP_SNAPSHOT = 2
};

typedef struct {
    unsigned char *data;
    uint32_t len;
    uint8_t kind;
} page_rep;

/*
 * Delta format:
 *   uint16_t run_count
 *   repeated:
 *      uint16_t offset
 *      uint16_t length
 *      <length bytes replacement data>
 *
 * PAGE_SIZE is expected <= 65535 for this prototype.
 */
static int encode_delta(const unsigned char *base,
                        const unsigned char *cur,
                        size_t n,
                        unsigned char **out,
                        uint32_t *out_len,
                        uint8_t *out_kind)
{
    if (n > 65535) {
        errno = ENOTSUP;
        return -1;
    }

    size_t cap = n + 2 + (n/2)*4;
    unsigned char *buf = malloc(cap);
    if (!buf) return -1;

    uint16_t runs = 0;
    size_t w = 2;
    size_t i = 0;

    while (i < n) {
        if (base[i] == cur[i]) {
            i++;
            continue;
        }

        size_t start = i;
        while (i < n && base[i] != cur[i] && (i-start) < 65535)
            i++;
        size_t len = i-start;

        if (w + 4 + len > cap) {
            size_t nc = cap*2 + len + 64;
            unsigned char *nb = realloc(buf, nc);
            if (!nb) { free(buf); return -1; }
            buf = nb; cap = nc;
        }

        uint16_t off16 = (uint16_t)start;
        uint16_t len16 = (uint16_t)len;
        memcpy(buf+w, &off16, 2); w += 2;
        memcpy(buf+w, &len16, 2); w += 2;
        memcpy(buf+w, cur+start, len); w += len;
        runs++;
    }

    if (runs == 0) {
        free(buf);
        *out = NULL;
        *out_len = 0;
        *out_kind = REP_RECIPE;
        return 0;
    }

    memcpy(buf, &runs, 2);

    /*
     * If sparse encoding is not cheaper than the raw page, store one
     * page-sized snapshot. This guarantees no pathological delta expansion.
     */
    if (w >= n) {
        unsigned char *snap = malloc(n);
        if (!snap) { free(buf); return -1; }
        memcpy(snap, cur, n);
        free(buf);
        *out = snap;
        *out_len = (uint32_t)n;
        *out_kind = REP_SNAPSHOT;
        return 0;
    }

    unsigned char *shrunk = realloc(buf, w);
    if (shrunk) buf = shrunk;
    *out = buf;
    *out_len = (uint32_t)w;
    *out_kind = REP_DELTA;
    return 0;
}

static int apply_rep(unsigned char *page, size_t n, const page_rep *rep) {
    if (!rep || rep->kind == REP_RECIPE || !rep->data)
        return 0;

    if (rep->kind == REP_SNAPSHOT) {
        if (rep->len != n) return -1;
        memcpy(page, rep->data, n);
        return 0;
    }

    if (rep->kind != REP_DELTA || rep->len < 2)
        return -1;

    uint16_t runs;
    memcpy(&runs, rep->data, 2);
    size_t p = 2;

    for (uint16_t r=0; r<runs; r++) {
        if (p+4 > rep->len) return -1;
        uint16_t off,len;
        memcpy(&off, rep->data+p, 2); p+=2;
        memcpy(&len, rep->data+p, 2); p+=2;
        if ((size_t)off + len > n || p+len > rep->len)
            return -1;
        memcpy(page+off, rep->data+p, len);
        p += len;
    }
    return 0;
}

struct stateram9_region {
    void *base;
    size_t len;
    size_t page_size;
    uint64_t pages;

    int uffd;
    pthread_t handler;
    atomic_int stop;

    stateram9_recipe_fn recipe;
    void *ctx;

    _Atomic unsigned char *dirty;
    page_rep *rep;

    atomic_ullong missing_faults;
    atomic_ullong write_faults;

    pthread_mutex_t meta_lock;
};

static int open_uffd(void) {
    return (int)syscall(SYS_userfaultfd,
                        O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
}

static int set_wp(stateram9_region *r, void *addr, size_t len, int enabled) {
    struct uffdio_writeprotect wp;
    memset(&wp,0,sizeof(wp));
    wp.range.start=(unsigned long)addr;
    wp.range.len=len;
    wp.mode=enabled ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
    return ioctl(r->uffd, UFFDIO_WRITEPROTECT, &wp);
}

static int materialize_page_image(stateram9_region *r,
                                  uint64_t idx,
                                  unsigned char *out)
{
    memset(out,0,r->page_size);
    if (r->recipe(idx,out,r->page_size,r->ctx)!=0)
        return -1;

    pthread_mutex_lock(&r->meta_lock);
    int rc = apply_rep(out,r->page_size,&r->rep[idx]);
    pthread_mutex_unlock(&r->meta_lock);
    return rc;
}

static void *handler_main(void *arg) {
    stateram9_region *r=arg;

    unsigned char *scratch=mmap(NULL,r->page_size,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (scratch==MAP_FAILED) {
        perror("stateram9 scratch");
        return NULL;
    }

    struct pollfd pfd={.fd=r->uffd,.events=POLLIN};

    while (!atomic_load(&r->stop)) {
        int pr=poll(&pfd,1,100);
        if (pr<0) {
            if (errno==EINTR) continue;
            perror("stateram9 poll");
            break;
        }
        if (pr==0 || !(pfd.revents&POLLIN)) continue;

        struct uffd_msg msg;
        ssize_t n=read(r->uffd,&msg,sizeof(msg));
        if (n<0) {
            if (errno==EAGAIN || errno==EINTR) continue;
            perror("stateram9 read");
            break;
        }
        if ((size_t)n!=sizeof(msg) || msg.event!=UFFD_EVENT_PAGEFAULT)
            continue;

        uintptr_t fault=(uintptr_t)msg.arg.pagefault.address;
        uintptr_t pa=fault & ~(uintptr_t)(r->page_size-1);
        if (pa<(uintptr_t)r->base || pa>=(uintptr_t)r->base+r->len)
            break;

        uint64_t idx=(pa-(uintptr_t)r->base)/r->page_size;
        uint64_t flags=msg.arg.pagefault.flags;

        if (flags & UFFD_PAGEFAULT_FLAG_WP) {
            /*
             * First write since last checkpoint/materialization.
             * Mark dirty before permitting the application write.
             */
            atomic_store(&r->dirty[idx],1);
            atomic_fetch_add(&r->write_faults,1);
            if (set_wp(r,(void *)pa,r->page_size,0)==-1) {
                perror("clear WP");
                break;
            }
            continue;
        }

        if (materialize_page_image(r,idx,scratch)!=0) {
            fprintf(stderr,"StateRAM-9: failed reconstruct page %" PRIu64 "\n",idx);
            memset(scratch,0,r->page_size);
        }

        int first_is_write=!!(flags & UFFD_PAGEFAULT_FLAG_WRITE);
        if (first_is_write)
            atomic_store(&r->dirty[idx],1);

        struct uffdio_copy cp;
        memset(&cp,0,sizeof(cp));
        cp.src=(unsigned long)scratch;
        cp.dst=(unsigned long)pa;
        cp.len=r->page_size;
        cp.mode=first_is_write ? 0 : UFFDIO_COPY_MODE_WP;

        if (ioctl(r->uffd,UFFDIO_COPY,&cp)==-1) {
            if (errno==EEXIST) continue;
            perror("UFFDIO_COPY");
            break;
        }

        atomic_fetch_add(&r->missing_faults,1);
        if (first_is_write)
            atomic_fetch_add(&r->write_faults,1);
    }

    munmap(scratch,r->page_size);
    return NULL;
}

stateram9_region *stateram9_create(
    size_t logical_bytes,
    stateram9_recipe_fn recipe,
    void *ctx)
{
    if (!logical_bytes || !recipe) { errno=EINVAL; return NULL; }

    stateram9_region *r=calloc(1,sizeof(*r));
    if (!r) return NULL;

    long ps=sysconf(_SC_PAGESIZE);
    if (ps<=0 || ps>65535) { free(r); errno=ENOTSUP; return NULL; }

    r->page_size=(size_t)ps;
    r->len=((logical_bytes+r->page_size-1)/r->page_size)*r->page_size;
    r->pages=r->len/r->page_size;
    r->recipe=recipe;
    r->ctx=ctx;

    r->dirty=calloc(r->pages,sizeof(*r->dirty));
    r->rep=calloc(r->pages,sizeof(*r->rep));
    if (!r->dirty || !r->rep) {
        free(r->dirty); free(r->rep); free(r); return NULL;
    }
    pthread_mutex_init(&r->meta_lock,NULL);

    r->base=mmap(NULL,r->len,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (r->base==MAP_FAILED) goto fail;

    r->uffd=open_uffd();
    if (r->uffd<0) goto fail_map;

    struct uffdio_api ua;
    memset(&ua,0,sizeof(ua));
    ua.api=UFFD_API;
    if (ioctl(r->uffd,UFFDIO_API,&ua)==-1) goto fail_fd;

    struct uffdio_register ur;
    memset(&ur,0,sizeof(ur));
    ur.range.start=(unsigned long)r->base;
    ur.range.len=r->len;
    ur.mode=UFFDIO_REGISTER_MODE_MISSING|UFFDIO_REGISTER_MODE_WP;
    if (ioctl(r->uffd,UFFDIO_REGISTER,&ur)==-1) goto fail_fd;

    atomic_store(&r->stop,0);
    if (pthread_create(&r->handler,NULL,handler_main,r)!=0)
        goto fail_reg;

    return r;

fail_reg: {
    struct uffdio_range rr={
        .start=(unsigned long)r->base,.len=r->len
    };
    ioctl(r->uffd,UFFDIO_UNREGISTER,&rr);
}
fail_fd:
    close(r->uffd);
fail_map:
    munmap(r->base,r->len);
fail:
    pthread_mutex_destroy(&r->meta_lock);
    free(r->dirty); free(r->rep); free(r);
    return NULL;
}

void *stateram9_data(stateram9_region *r) {
    return r ? r->base : NULL;
}

size_t stateram9_size(stateram9_region *r) {
    return r ? r->len : 0;
}

uint64_t stateram9_dirty_pages(stateram9_region *r) {
    if (!r) return 0;
    uint64_t x=0;
    for (uint64_t i=0;i<r->pages;i++)
        x += !!atomic_load(&r->dirty[i]);
    return x;
}

uint64_t stateram9_delta_pages(stateram9_region *r) {
    if (!r) return 0;
    uint64_t x=0;
    pthread_mutex_lock(&r->meta_lock);
    for (uint64_t i=0;i<r->pages;i++)
        x += r->rep[i].kind==REP_DELTA;
    pthread_mutex_unlock(&r->meta_lock);
    return x;
}

uint64_t stateram9_snapshot_pages(stateram9_region *r) {
    if (!r) return 0;
    uint64_t x=0;
    pthread_mutex_lock(&r->meta_lock);
    for (uint64_t i=0;i<r->pages;i++)
        x += r->rep[i].kind==REP_SNAPSHOT;
    pthread_mutex_unlock(&r->meta_lock);
    return x;
}

uint64_t stateram9_representation_bytes(stateram9_region *r) {
    if (!r) return 0;
    uint64_t x=0;
    pthread_mutex_lock(&r->meta_lock);
    for (uint64_t i=0;i<r->pages;i++)
        x += r->rep[i].len;
    pthread_mutex_unlock(&r->meta_lock);
    return x;
}

uint64_t stateram9_missing_faults(stateram9_region *r) {
    return r ? atomic_load(&r->missing_faults) : 0;
}

uint64_t stateram9_write_faults(stateram9_region *r) {
    return r ? atomic_load(&r->write_faults) : 0;
}

int stateram9_checkpoint_deltas(stateram9_region *r) {
    if (!r) { errno=EINVAL; return -1; }

    unsigned char *base=malloc(r->page_size);
    if (!base) return -1;

    /*
     * Cooperative rule: caller has quiesced writes to this region.
     * This avoids racing application mutations while bytes are diffed.
     */
    for (uint64_t i=0;i<r->pages;i++) {
        if (!atomic_load(&r->dirty[i]))
            continue;

        unsigned char *cur=(unsigned char *)r->base+i*r->page_size;

        if (r->recipe(i,base,r->page_size,r->ctx)!=0) {
            free(base); errno=EIO; return -1;
        }

        unsigned char *enc=NULL;
        uint32_t enc_len=0;
        uint8_t enc_kind=REP_RECIPE;

        if (encode_delta(base,cur,r->page_size,
                         &enc,&enc_len,&enc_kind)!=0) {
            free(base); return -1;
        }

        pthread_mutex_lock(&r->meta_lock);
        free(r->rep[i].data);
        r->rep[i].data=enc;
        r->rep[i].len=enc_len;
        r->rep[i].kind=enc_kind;
        pthread_mutex_unlock(&r->meta_lock);

        /*
         * Representation is durable in userspace metadata now.
         * Raw physical page can disappear.
         */
        if (madvise(cur,r->page_size,MADV_DONTNEED)!=0) {
            free(base); return -1;
        }
        atomic_store(&r->dirty[i],0);
    }

    free(base);
    return 0;
}

int stateram9_dematerialize_safe(stateram9_region *r) {
    if (!r) { errno=EINVAL; return -1; }

    uint64_t i=0;
    while (i<r->pages) {
        while (i<r->pages && atomic_load(&r->dirty[i])) i++;
        if (i>=r->pages) break;
        uint64_t s=i;
        while (i<r->pages && !atomic_load(&r->dirty[i])) i++;
        size_t len=(i-s)*r->page_size;
        void *a=(char *)r->base+s*r->page_size;
        if (madvise(a,len,MADV_DONTNEED)!=0) return -1;
    }
    return 0;
}

int stateram9_rebase_recipe(stateram9_region *r) {
    if (!r) { errno=EINVAL; return -1; }

    /*
     * Cooperative semantic promise:
     * recipe() now regenerates current logical contents for every page.
     */
    pthread_mutex_lock(&r->meta_lock);
    for (uint64_t i=0;i<r->pages;i++) {
        free(r->rep[i].data);
        r->rep[i].data=NULL;
        r->rep[i].len=0;
        r->rep[i].kind=REP_RECIPE;
        atomic_store(&r->dirty[i],0);
    }
    pthread_mutex_unlock(&r->meta_lock);

    /*
     * Protect present pages so future modifications are detected.
     * Missing pages will be installed WP on their first read.
     */
    if (set_wp(r,r->base,r->len,1)==-1)
        return -1;
    return 0;
}



static int byte_range_to_pages(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes,
    uint64_t *first_page,
    uint64_t *end_page)
{
    if (!r || !first_page || !end_page) { errno=EINVAL; return -1; }
    if (offset_bytes >= r->len || length_bytes == 0) {
        *first_page = *end_page = 0;
        return 0;
    }

    size_t end_bytes;
    if (length_bytes > r->len - offset_bytes)
        end_bytes = r->len;
    else
        end_bytes = offset_bytes + length_bytes;

    *first_page = offset_bytes / r->page_size;
    *end_page = (end_bytes + r->page_size - 1) / r->page_size;
    if (*end_page > r->pages) *end_page = r->pages;
    return 0;
}

int stateram9_materialize_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes)
{
    if (!r) { errno=EINVAL; return -1; }

    uint64_t first=0,end=0;
    if (byte_range_to_pages(r,offset_bytes,length_bytes,&first,&end)!=0)
        return -1;
    if (first==end) return 0;

    volatile unsigned char sink=0;
    unsigned char *p=(unsigned char *)r->base;
    for (uint64_t i=first;i<end;i++)
        sink ^= p[i*r->page_size];
    (void)sink;
    return 0;
}

int stateram9_checkpoint_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes)
{
    if (!r) { errno=EINVAL; return -1; }

    uint64_t first=0,end=0;
    if (byte_range_to_pages(r,offset_bytes,length_bytes,&first,&end)!=0)
        return -1;
    if (first==end) return 0;

    unsigned char *base=malloc(r->page_size);
    if (!base) return -1;

    for (uint64_t i=first;i<end;i++) {
        if (!atomic_load(&r->dirty[i]))
            continue;

        unsigned char *cur=(unsigned char *)r->base+i*r->page_size;

        if (r->recipe(i,base,r->page_size,r->ctx)!=0) {
            free(base); errno=EIO; return -1;
        }

        unsigned char *enc=NULL;
        uint32_t enc_len=0;
        uint8_t enc_kind=REP_RECIPE;

        if (encode_delta(base,cur,r->page_size,
                         &enc,&enc_len,&enc_kind)!=0) {
            free(base); return -1;
        }

        pthread_mutex_lock(&r->meta_lock);
        free(r->rep[i].data);
        r->rep[i].data=enc;
        r->rep[i].len=enc_len;
        r->rep[i].kind=enc_kind;
        pthread_mutex_unlock(&r->meta_lock);

        if (madvise(cur,r->page_size,MADV_DONTNEED)!=0) {
            free(base); return -1;
        }
        atomic_store(&r->dirty[i],0);
    }

    free(base);
    return 0;
}

int stateram9_dematerialize_safe_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes)
{
    if (!r) { errno=EINVAL; return -1; }

    uint64_t first=0,end=0;
    if (byte_range_to_pages(r,offset_bytes,length_bytes,&first,&end)!=0)
        return -1;
    if (first==end) return 0;

    uint64_t i=first;
    while (i<end) {
        while (i<end && atomic_load(&r->dirty[i])) i++;
        if (i>=end) break;
        uint64_t s=i;
        while (i<end && !atomic_load(&r->dirty[i])) i++;

        size_t len=(size_t)(i-s)*r->page_size;
        void *a=(char *)r->base+(size_t)s*r->page_size;
        if (madvise(a,len,MADV_DONTNEED)!=0) return -1;
    }
    return 0;
}


int stateram9_materialize_all(stateram9_region *r) {
    if (!r) { errno=EINVAL; return -1; }
    volatile unsigned char sink = 0;
    unsigned char *p = (unsigned char *)r->base;
    for (uint64_t i=0; i<r->pages; i++)
        sink ^= p[i*r->page_size];
    (void)sink;
    return 0;
}

uint64_t stateram9_resident_pages(stateram9_region *r) {
    if (!r) return 0;
    unsigned char *vec = calloc(r->pages, 1);
    if (!vec) return 0;
    if (mincore(r->base, r->len, vec) != 0) {
        free(vec);
        return 0;
    }
    uint64_t n=0;
    for (uint64_t i=0; i<r->pages; i++)
        if (vec[i] & 1) n++;
    free(vec);
    return n;
}

size_t stateram9_page_size(stateram9_region *r) {
    return r ? r->page_size : 0;
}

void stateram9_destroy(stateram9_region *r) {
    if (!r) return;
    atomic_store(&r->stop,1);
    pthread_join(r->handler,NULL);

    struct uffdio_range rr={
        .start=(unsigned long)r->base,.len=r->len
    };
    ioctl(r->uffd,UFFDIO_UNREGISTER,&rr);

    close(r->uffd);
    munmap(r->base,r->len);

    for (uint64_t i=0;i<r->pages;i++)
        free(r->rep[i].data);
    free(r->rep);
    free(r->dirty);
    pthread_mutex_destroy(&r->meta_lock);
    free(r);
}

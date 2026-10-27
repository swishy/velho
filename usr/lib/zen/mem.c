#include <errno.h>
#include <zen/sys/unix.h>
#include <zen/mem.h>
#include <zen/bitop.h>
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
# if __GLIBC_PREREQ(2, 32)
#  include <sys/single_threaded.h>
#  define memsinglethreaded() __libc_single_threaded
# endif
#endif
#ifndef memsinglethreaded
#define memsinglethreaded() 0
#endif

struct memglob g_mem ALIGNED(PAGESIZE);
THREADLOCAL struct memtls *t_memtls;

/* The same public libc proof used by the queue bit lock also applies to
 * published leaf words. Other targets always use the shared atomic path. */
static INLINE uintptr_t
memmapor(uintptr_t *map, uintptr_t bits)
{
    if (memsinglethreaded()) {
        uintptr_t old = __atomic_load_n(map, __ATOMIC_RELAXED);
        __atomic_store_n(map, old | bits, __ATOMIC_RELEASE);
        return old;
    }
    return __atomic_fetch_or(map, bits, __ATOMIC_ACQ_REL);
}

static INLINE uintptr_t
memmapand(uintptr_t *map, uintptr_t bits)
{
    if (memsinglethreaded()) {
        uintptr_t old = __atomic_load_n(map, __ATOMIC_RELAXED);
        __atomic_store_n(map, old & bits, __ATOMIC_RELEASE);
        return old;
    }
    return __atomic_fetch_and(map, bits, __ATOMIC_ACQ_REL);
}

static INLINE int
memtryfree(uintptr_t *map, uintptr_t *old, uintptr_t next)
{
    if (memsinglethreaded()) {
        __atomic_store_n(map, next, __ATOMIC_RELEASE);
        return 1;
    }
    return __atomic_compare_exchange_n(map, old, next, 1,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

void
meminit(struct memglob *mem)
{
    mtlkfmtx(&mem->mtx);
    if (!(__atomic_load_n(&mem->flg, __ATOMIC_RELAXED) & MEM_INIT_BIT)) {
        for (size_t qid = 1; qid < MEM_BLK_QUEUES; ++qid) {
            mem->blktab[qid].size = memblksize(qid);
        }
        for (size_t qid = 1; qid < MEM_RUN_QUEUES; ++qid) {
            mem->runtab[qid].size = memrunsize(qid);
        }
        __atomic_store_n(&mem->flg, MEM_INIT_BIT, __ATOMIC_RELEASE);
    }
    mtunlkfmtx(&mem->mtx);
}

/* Head accesses are atomic because lock waiters also read this word.
 * All helpers below require the queue lock, except lock/unlock themselves. */
static INLINE struct membuf *
memqhead(struct membufq *q)
{
    return (struct membuf *)(__atomic_load_n((m_atomic_t *)&q->head,
                                            __ATOMIC_RELAXED)
                            & ~(uintptr_t)MEM_LK_BIT);
}

static INLINE void
memqsethead(struct membufq *q, struct membuf *head)
{
    __atomic_store_n((m_atomic_t *)&q->head,
                     (m_atomic_t)((uintptr_t)head | MEM_LK_BIT),
                     __ATOMIC_RELAXED);
}

static INLINE int
memqretains(struct membufq *q)
{
    return !q->retired && q->size <= MEM_SLAB_SIZE / 2;
}

static INLINE uintptr_t
memwordmask(struct membuf *buf, size_t word)
{
    size_t count = min(buf->max - word * MEM_BUF_MAP_BITS, MEM_BUF_MAP_BITS);
    uintptr_t mask = UINTPTR_MAX >> (MEM_BUF_MAP_BITS - count);
    return word ? mask : mask & ~MEM_BUF_FLAGS;
}

static INLINE void
memqlock(struct membufq *q)
{
    /* Keep the same lock-bit state even when libc proves no peer exists.
     * Once threads are created, the normal atomic acquisition applies. */
    if (memsinglethreaded()) {
        uintptr_t head = __atomic_load_n((uintptr_t *)&q->head, __ATOMIC_RELAXED);
        if (!(head & MEM_LK_BIT)) {
            __atomic_store_n((uintptr_t *)&q->head, head | MEM_LK_BIT, __ATOMIC_RELAXED);
            return;
        }
    }
    mtlkbit((m_atomic_t *)&q->head, MEM_LK_BIT_OFS);
}

static INLINE void
memqunlock(struct membufq *q)
{
    mtunlkval((m_atomic_t *)&q->head, (m_atomic_t)memqhead(q));
}

static void
memqueuebuf(struct membufq *q, struct membuf *buf)
{
    buf->prev = q->tail;
    buf->next = NULL;
    if (q->tail) {
        q->tail->next = buf;
    } else {
        if (memqretains(q)) {
            memmapor(&buf->map[0], MEM_BUF_HEAD_BIT);
        }
        memqsethead(q, buf);
    }
    q->tail = buf;
}

static void
memdequebuf(struct membufq *q, struct membuf *buf)
{
    if (buf->prev) {
        buf->prev->next = buf->next;
    } else {
        memmapand(&buf->map[0], ~MEM_BUF_HEAD_BIT);
        if (buf->next && memqretains(q)) {
            memmapor(&buf->next->map[0], MEM_BUF_HEAD_BIT);
        }
        memqsethead(q, buf->next);
    }
    if (buf->next) {
        buf->next->prev = buf->prev;
    } else {
        q->tail = buf->prev;
    }
    buf->prev = buf->next = NULL;
}


/* Embed ownership in an aligned mapping. No header pool or page index. */
static struct membuf *
memnewbuf(size_t stride, long type)
{
    int8_t *raw = mapanon(-1, 2 * MEM_SLAB_SIZE, 0);
    if (raw == MAP_FAILED) {
        return NULL;
    }
    struct membuf *buf = memalignptr(raw, MEM_SLAB_SIZE);
    size_t prefix = (int8_t *)buf - raw;
    if (prefix) {
        unmapanon(raw, prefix);
    }
    size_t suffix = MEM_SLAB_SIZE - prefix;
    if (suffix) {
        unmapanon((int8_t *)buf + MEM_SLAB_SIZE, suffix);
    }
    size_t offset = type == MEM_BLK ? membufsize() : PAGESIZE;
    buf->adr = (int8_t *)buf + offset;
    buf->size = MEM_SLAB_SIZE;
    buf->max = (MEM_SLAB_SIZE - offset) / stride;
    buf->bits = 0;
    for (size_t word = 0, start = 0; start < buf->max;
         ++word, start += MEM_BUF_MAP_BITS) {
        size_t count = min(buf->max - start, MEM_BUF_MAP_BITS);
        buf->map[word] = (UINTPTR_MAX >> (MEM_BUF_MAP_BITS - count))
            & (word ? UINTPTR_MAX : ~MEM_BUF_FLAGS);
        buf->wordmap[word / MEM_BUF_MAP_BITS] |= (uintptr_t)1 << (word % MEM_BUF_MAP_BITS);
        buf->bits |= (uintptr_t)1 << (word / MEM_BUF_MAP_BITS);
    }
    return buf;
}

static void *
memgetsmall(unsigned long qid, long type, size_t align, size_t *retsize)
{
    struct membufq *q = type == MEM_BLK
        ? &t_memtls->blktab[qid] : &t_memtls->runtab[qid];
    size_t size = q->size;
    struct membuf *buf = memqhead(q);
    int8_t *ptr;
    /* Only this thread removes a retained head. Owner-only cached bits need
     * neither an allocation reference nor an atomic operation. */
    if (buf && memqretains(q)) {
        uintptr_t local = buf->local;
        if (local) {
            buf->local = local & (local - 1);
            ptr = buf->adr + m_ctz(local) * size;
            goto out;
        }
    }
    memqlock(q);
    buf = memqhead(q);
    if (!buf) {
        memqunlock(q);
        struct membufq *glob = type == MEM_BLK
            ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
        memqlock(glob);
        buf = memqhead(glob);
        if (buf) {
            memdequebuf(glob, buf);
            glob->nbuf--;
        }
        memqunlock(glob);
        if (!buf) {
            buf = memnewbuf(size, type);
            if (!buf) {
                return NULL;
            }
        }
        memqlock(q);
        buf->queue = q;
        q->nbuf++;
        memqueuebuf(q, buf);
        buf = memqhead(q);
    }
    unsigned long group = m_ctz(buf->bits);
    unsigned long word = group * MEM_BUF_MAP_BITS + m_ctz(buf->wordmap[group]);
    uintptr_t mask = __atomic_load_n(&buf->map[word], __ATOMIC_RELAXED);
    uintptr_t avail = mask & (word ? UINTPTR_MAX : ~MEM_BUF_FLAGS);
    uintptr_t bit = avail & -avail;
    uintptr_t take = bit;
    if (!word && memqretains(q) && (avail & (avail - 1))) {
        /* Reserve a batch for this owner, leaving one shared free bit so
         * the head remains available until the local batch is exhausted. */
        take = avail & (avail - 1);
        bit = take & -take;
        buf->local = take & ~bit;
    }
    unsigned long index = word * MEM_BUF_MAP_BITS + m_ctz(bit);
    ptr = buf->adr + index * size;
    mask = memmapand(&buf->map[word], ~take);
    if (word && mask == memwordmask(buf, word)) {
        ++buf->busy;
    }
    if (!((mask & ~take) & (word ? UINTPTR_MAX : ~MEM_BUF_FLAGS))) {
        buf->wordmap[group] &= ~((uintptr_t)1 << (word % MEM_BUF_MAP_BITS));
        if (!buf->wordmap[group]) {
            buf->bits &= ~((uintptr_t)1 << group);
        }
    }
    if (!buf->bits) {
        memdequebuf(q, buf);
    }
    memqunlock(q);
out:
    if (align > (type == MEM_BLK ? MEM_BLK_UNIT : PAGESIZE)) {
        int8_t *aligned = memalignptr(ptr, align);
        if (retsize) {
            *retsize = size - (aligned - ptr);
        }
        ptr = aligned;
    } else if (retsize) {
        *retsize = size;
    }
    return ptr;
}

/* Empty, detached slab: discard payload on ownership release, not reuse.
 * Its first page contains the header and remains resident. */
static void
mempurgeempty(struct membuf *buf)
{
    int saved_errno = errno;
    madvise((int8_t *)buf + PAGESIZE, MEM_SLAB_SIZE - PAGESIZE, MADV_DONTNEED);
    errno = saved_errno;
}

/* Owner lock covers the last free and every queue/lifetime transition.
 * Nested order remains local -> global; allocation drops global first. */
static void
memreleaseempty(struct membuf *buf)
{
    struct membufq *q = buf->queue;
    long type = q->size < PAGESIZE ? MEM_BLK : MEM_RUN;
    unsigned long qid = q->size / (type == MEM_BLK ? MEM_BLK_UNIT : PAGESIZE);
    long limit = q->retired ? 0
        : type == MEM_BLK ? memnblktlsbuf(qid) : memnruntlsbuf(qid);
    if (q->nbuf <= limit || (limit && memqhead(q) == buf)) {
        return;
    }
    memdequebuf(q, buf);
    q->nbuf--;
    struct membufq *glob = type == MEM_BLK
        ? &g_mem.blktab[qid] : &g_mem.runtab[qid];
    limit = type == MEM_BLK ? memnblkbuf(qid) : memnrunbuf(qid);
    memqlock(glob);
    if (glob->nbuf < limit) {
        mempurgeempty(buf);
        buf->queue = glob;
        glob->nbuf++;
        memqueuebuf(glob, buf);
        memqunlock(glob);
    } else {
        memqunlock(glob);
        unmapanon(buf, buf->size);
    }
}

/* The returned pointer's predecessor selects the header-containing slab,
 * including when explicit alignment puts the pointer on a slab boundary. */
static void *
memgetbig(size_t size, size_t align, size_t *retsize)
{
    if (align > SIZE_MAX - MEM_SLAB_SIZE - (PAGESIZE - 1)
        || size > SIZE_MAX - align - MEM_SLAB_SIZE - (PAGESIZE - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t mapsz = roundup2(size + align + MEM_SLAB_SIZE, PAGESIZE);
    int8_t *raw = mapanon(-1, mapsz, 0);
    if (raw == MAP_FAILED) {
        return NULL;
    }
    int8_t *ret = memalignptr(raw + MEM_SLAB_SIZE, align);
    struct membuf *buf = memptrbuf(ret);
    int8_t *end = memalignptr(ret + size, PAGESIZE);
    size_t prefix = (int8_t *)buf - raw;
    size_t suffix = raw + mapsz - end;
    if (prefix) {
        unmapanon(raw, prefix);
    }
    if (suffix) {
        unmapanon(end, suffix);
    }
    buf->adr = ret;
    buf->size = end - (int8_t *)buf;
    buf->queue = NULL;
    if (retsize) {
        *retsize = end - ret;
    }
    return ret;
}

void *
memget(size_t size, size_t align, size_t *retsize)
{
    size_t aln = max(align, MEM_ALIGN_MIN);
    size_t alnsz = memalnsize(size, aln);
    if (!size) {
        return NULL;
    }
    if (alnsz > SIZE_MAX - (PAGESIZE - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    unsigned long qid = memblkqid(alnsz);
    if (qid < MEM_BLK_QUEUES) {
        return memgetsmall(qid, MEM_BLK, aln, retsize);
    }
    qid = memrunqid(alnsz);
    if (qid < MEM_RUN_QUEUES) {
        return memgetsmall(qid, MEM_RUN, aln, retsize);
    }
    /* BIG alignment is provided by its prefix, not extra payload bytes. */
    return memgetbig(size, aln, retsize);
}

void
memput(void *ptr)
{
    /* A live slot keeps its header/owner alive until the publishing CAS.
     * After success the fast path must not touch either again. */
    struct membuf *buf = memptrbuf(ptr);
    struct membufq *q = buf->queue;
    if (!q) {
        unmapanon(buf, buf->size);
        return;
    }
    size_t index = ((uintptr_t)ptr - (uintptr_t)buf->adr) / q->size;
    size_t word = index / MEM_BUF_MAP_BITS;
    uintptr_t bit = (uintptr_t)1 << (index % MEM_BUF_MAP_BITS);
    uintptr_t valid = memwordmask(buf, word);
    uintptr_t mask = __atomic_load_n(&buf->map[word], __ATOMIC_RELAXED);
    /* Only the allocating thread touches local. A head is retired only by
     * that thread, after local is exhausted, so there is nothing to drain. */
    if (!word && (mask & MEM_BUF_HEAD_BIT)
        && (uintptr_t)q - (uintptr_t)t_memtls < sizeof(struct memtls)) {
        buf->local |= bit;
        return;
    }
    for (;;) {
        uintptr_t next = mask | bit;
        if (!(mask & valid)
            || ((next & valid) == valid
                && (word || !(mask & MEM_BUF_HEAD_BIT)))) {
            break;
        }
        if (memtryfree(&buf->map[word], &mask, next)) return;
    }
    memqlock(q);
    uintptr_t old = buf->bits;
    mask = memmapor(&buf->map[word], bit);
    size_t group = word / MEM_BUF_MAP_BITS;
    buf->wordmap[group] |= (uintptr_t)1 << (word % MEM_BUF_MAP_BITS);
    buf->bits |= (uintptr_t)1 << group;
    if (!old) {
        memqueuebuf(q, buf);
    }
    if (word && ((mask | bit) & valid) == valid) {
        --buf->busy;
    }
    if (!buf->busy) {
        uintptr_t first = __atomic_load_n(&buf->map[0], __ATOMIC_ACQUIRE);
        uintptr_t all = memwordmask(buf, 0);
        if ((first & ~MEM_BUF_FLAGS) == all) {
            memreleaseempty(buf);
        }
    }
    memqunlock(q);
}

/* The queue tables stay mapped for outstanding remote frees. Empty slabs
 * leave immediately; a live slab leaves when its final remote free arrives. */
void
memretire(struct memtls *tls)
{
    for (size_t type = MEM_BLK; type <= MEM_RUN; ++type) {
        struct membufq *table = type == MEM_BLK ? tls->blktab : tls->runtab;
        size_t count = type == MEM_BLK ? MEM_BLK_QUEUES : MEM_RUN_QUEUES;
        for (size_t i = 1; i < count; ++i) {
            struct membufq *q = &table[i];
            memqlock(q);
            q->retired = 1;
            struct membuf *buf = memqhead(q);
            if (buf) {
                memmapand(&buf->map[0], ~MEM_BUF_HEAD_BIT);
                if (buf->local) {
                    memmapor(&buf->map[0], buf->local);
                    buf->local = 0;
                    buf->wordmap[0] |= 1;
                    buf->bits |= 1;
                }
            }
            while (buf) {
                struct membuf *next = buf->next;
                if (!buf->busy
                    && (__atomic_load_n(&buf->map[0], __ATOMIC_ACQUIRE) & ~MEM_BUF_FLAGS)
                        == memwordmask(buf, 0)) {
                    memreleaseempty(buf);
                }
                buf = next;
            }
            memqunlock(q);
        }
    }
}

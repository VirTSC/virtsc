/*
 * is1gl - the guest half of the IntelliStar's accelerated GL.
 *
 * This is libGL.so.1 as far as renderd is concerned. It does not wrap the
 * vendor library, it replaces it: GL calls become records in a ring in AGP
 * aperture memory, a QEMU device replays them against the host's GL, and the
 * finished frame is written straight into the Thunderstorm's TSH_FRAME.
 *
 * Built and preloaded like the shims it replaces. See
 * notes/gl-acceleration-plan.md, notes/is1gl-phase0.md, notes/is1gl-phase1.md.
 *
 * Three rules hold this together, all of them measured rather than assumed:
 *
 *  1. No GL call makes a syscall. Records are stores to mapped memory and the
 *     doorbell is a store to a mapped MMIO page. The guest is libc_r - N:1
 *     threads - so a syscall inside a GL call is a scheduling yield point
 *     where the real r200 driver had none, and a blocking one would stall
 *     renderd's ingest and decode along with its renderer.
 *
 *  2. Every glGet is answered from state tracked here. The application asks
 *     510 questions a frame (notes/is1gl-phase0.md); as round trips at the
 *     8 us an MMIO read costs, that alone would be 4.1 ms of a 33 ms frame.
 *
 *  3. Only glReadPixels waits, and it waits by spinning briefly on a word in
 *     ordinary memory before falling back to nanosleep. Phase 1 measured the
 *     host answering in 7 us and the sleep quantum at 1000 us.
 *
 * And one rule learned the hard way:
 *
 *  4. Every entry point is indivisible, and belongs to the calling thread's
 *     context. renderd calls GL from two threads with a context each, and
 *     libc_r can switch threads at any instruction. See "threads and
 *     contexts" below.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/agpio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include "is1gl_ring.h"
#include "is1gl_ops.h"

#ifndef GL_TEXTURE_RECTANGLE_NV
#define GL_TEXTURE_RECTANGLE_NV        0x84F5
#endif
#ifndef GL_PACK_INVERT_MESA
#define GL_PACK_INVERT_MESA            0x8758
#endif
#ifndef GL_UNPACK_CLIENT_STORAGE_APPLE
#define GL_UNPACK_CLIENT_STORAGE_APPLE 0x85B2
#endif
#ifndef GL_YCBCR_MESA
#define GL_YCBCR_MESA                  0x8757
#endif
#ifndef GL_UNSIGNED_SHORT_8_8_MESA
#define GL_UNSIGNED_SHORT_8_8_MESA     0x85BA
#define GL_UNSIGNED_SHORT_8_8_REV_MESA 0x85BB
#endif

#define PAGE_SZ    4096
#define MAX_BLOCKS 64
/*
 * The ring is reserved at the top of the aperture, aligned to the
 * Thunderstorm's frame stride. Allocations for the application come off the
 * bottom and are NOT realigned, because the point is that they land exactly
 * where libagpnv would have put them.
 *
 * renderd derives its frame layout from where its block lands and then walks
 * the full set. Taking the ring from the bottom moved that block off
 * aperture offset 0, after which renderd read 1.6 MB past the end of its own
 * mmap and died in a memcmp - deterministically, seventeen frames in.
 * Aligning the offset was not enough, because the mmap'd *address* was still
 * unaligned. See notes/is1gl-phase3.md.
 */
#define AGP_ALIGN  (2 * 1024 * 1024)

static FILE *lg;
static int   verbose;

static FILE *is1gl_log(void)
{
	if (!lg) {
		const char *p = getenv("IS1GL_LOG");
		/*
		 * One file per process. This library is preloaded into every
		 * process in the application's tree, and istard restarts
		 * renderd whenever it dies - so a single path is opened "w" by
		 * several processes at once and they erase each other. The
		 * first crash report of this phase was lost exactly that way.
		 */
		if (p) {
			char path[1024];
			snprintf(path, sizeof path, "%s.%d", p, (int)getpid());
			lg = fopen(path, "w");
		}
		if (!lg) lg = stderr;
	}
	return lg;
}

static void say(const char *fmt, ...)
{
	va_list ap;
	if (!verbose) return;
	va_start(ap, fmt);
	fprintf(is1gl_log(), "is1gl: ");
	vfprintf(is1gl_log(), fmt, ap);
	va_end(ap);
	fflush(is1gl_log());
}

/* ------------------------------------------------------------------ AGP
 *
 * Lifted from src/agp/libagpnv.c, which this library absorbs. The ring and
 * renderd's frame buffers come from the same bump allocator, because they
 * come from the same aperture and the card is told about the frames by
 * aperture offset.
 */
struct block {
	void         *ptr;
	unsigned int  offset;      /* byte offset into the aperture */
	unsigned int  bytes;
	int           key;
};

static int           agp_fd = -1;
static unsigned int  aper_base, aper_size, next_offset;
/*
 * The ring comes off the TOP of the aperture, and everything renderd asks
 * for comes off the bottom exactly as it did from libagpnv.
 *
 * This is not neatness. renderd derives its frame layout from where its
 * block lands, and taking the ring first moved that block from aperture
 * offset 0 to 0x01001000 - after which renderd walked 1.6 MB off the end of
 * its own mmap and died in a memcmp, deterministically, seventeen frames in.
 * Aligning the offset was not enough; the only safe thing is for renderd's
 * allocation to be byte-identical to what it was without us.
 */
static unsigned int  ring_limit;
static struct block  blocks[MAX_BLOCKS];
static int           nblocks;

static int agp_open(void)
{
	agp_info info;

	if (agp_fd >= 0) return 0;
	agp_fd = open("/dev/agpgart", O_RDWR);
	if (agp_fd < 0) {
		say("open /dev/agpgart: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(agp_fd, AGPIOC_INFO, &info) < 0 ||
	    ioctl(agp_fd, AGPIOC_ACQUIRE, 0) < 0) {
		say("AGPIOC_INFO/ACQUIRE: %s\n", strerror(errno));
		close(agp_fd);
		agp_fd = -1;
		return -1;
	}
	aper_base = (unsigned int)info.aper_base;
	aper_size = (unsigned int)info.aper_size * 1024 * 1024;
	next_offset = 0;
	ring_limit = aper_size;
	say("aperture 0x%08x size %u MB\n", aper_base, (unsigned)info.aper_size);
	return 0;
}

/*
 * Reserve `size` bytes at the top of the aperture, aligned to the frame
 * stride, and return the offset. Called once, for the ring.
 */
static int agp_reserve_top(unsigned int size)
{
	unsigned int bytes = (size + AGP_ALIGN - 1) & ~(AGP_ALIGN - 1);

	if (agp_open() < 0) return -1;
	if (bytes + next_offset > ring_limit) return -1;
	ring_limit -= bytes;
	return (int)ring_limit;
}

static void *agp_alloc(unsigned int size, unsigned int *offset_out)
{
	agp_allocate alloc;
	agp_bind bind;
	struct block *b;
	unsigned int bytes;
	void *p;

	if (size == 0 || nblocks >= MAX_BLOCKS) return 0;
	if (agp_open() < 0) return 0;

	bytes = (size + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
	if (next_offset + bytes > ring_limit) {
		say("aperture full: want %u, have %u\n", bytes,
		    ring_limit - next_offset);
		return 0;
	}
	memset(&alloc, 0, sizeof alloc);
	alloc.type = 0;
	alloc.pg_count = bytes / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_ALLOCATE, &alloc) < 0) {
		say("AGPIOC_ALLOCATE %u pages: %s\n",
		    (unsigned)alloc.pg_count, strerror(errno));
		return 0;
	}
	bind.key = alloc.key;
	bind.pg_start = next_offset / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_BIND, &bind) < 0) {
		say("AGPIOC_BIND: %s\n", strerror(errno));
		ioctl(agp_fd, AGPIOC_DEALLOCATE, &alloc.key);
		return 0;
	}
	p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, agp_fd,
	         (off_t)next_offset);
	if (p == MAP_FAILED) {
		say("mmap at 0x%x: %s\n", next_offset, strerror(errno));
		return 0;
	}
	b = &blocks[nblocks++];
	b->ptr = p;
	b->offset = next_offset;
	b->bytes = bytes;
	b->key = alloc.key;
	if (offset_out) *offset_out = next_offset;
	next_offset += bytes;
	say("alloc %u -> %p, aperture offset 0x%08x (phys 0x%08x)\n",
	    size, p, b->offset, aper_base + b->offset);
	return p;
}

/*
 * The same allocation, bound at a caller-chosen aperture offset. Only the
 * ring uses it, and only at the top of the aperture, so it never collides
 * with the bump allocator working upward.
 */
static void *agp_alloc_at(unsigned int size, unsigned int offset)
{
	agp_allocate alloc;
	agp_bind bind;
	struct block *b;
	unsigned int bytes;
	void *p;

	if (size == 0 || nblocks >= MAX_BLOCKS) return 0;
	if (agp_open() < 0) return 0;

	bytes = (size + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
	memset(&alloc, 0, sizeof alloc);
	alloc.type = 0;
	alloc.pg_count = bytes / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_ALLOCATE, &alloc) < 0) {
		say("AGPIOC_ALLOCATE %u pages: %s\n",
		    (unsigned)alloc.pg_count, strerror(errno));
		return 0;
	}
	bind.key = alloc.key;
	bind.pg_start = offset / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_BIND, &bind) < 0) {
		say("AGPIOC_BIND at 0x%x: %s\n", offset, strerror(errno));
		ioctl(agp_fd, AGPIOC_DEALLOCATE, &alloc.key);
		return 0;
	}
	p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, agp_fd,
	         (off_t)offset);
	if (p == MAP_FAILED) {
		say("mmap at 0x%x: %s\n", offset, strerror(errno));
		return 0;
	}
	b = &blocks[nblocks++];
	b->ptr = p;
	b->offset = offset;
	b->bytes = bytes;
	b->key = alloc.key;
	say("ring alloc %u -> %p, aperture offset 0x%08x (phys 0x%08x)\n",
	    size, p, offset, aper_base + offset);
	return p;
}

/* Guest-physical address of a pointer inside an AGP block, or 0. */
static unsigned int agp_phys(const void *ptr)
{
	int i;

	for (i = 0; i < nblocks; i++) {
		const char *base = (const char *)blocks[i].ptr;
		if (base && (const char *)ptr >= base &&
		    (const char *)ptr < base + blocks[i].bytes)
			return aper_base + blocks[i].offset +
			       (unsigned int)((const char *)ptr - base);
	}
	return 0;
}

/* ------------------------------------------------- threads and contexts
 *
 * renderd drives GL from two threads, each with a GLX context of its own. A
 * loader thread creates the textures, glyph pages and display lists in one;
 * the renderer draws in the other, which shares the loader's textures and
 * lists. Each thread makes its context current once and keeps it, so the
 * two contexts' calls arrive interleaved with nothing to say whose is whose.
 *
 * The first version of this library assumed that libc_r only switches
 * threads where one blocks, and kept one set of state for everybody. libc_r
 * also preempts a thread whose timeslice has run out, at any instruction.
 * Landing between the loader's glBindTexture and its upload, that sent a
 * glyph or an image into whatever texture the renderer had bound: text came
 * out as solid blocks. Landing while the loader was copying an image into
 * the ring, it let the renderer ring the doorbell over a half-written
 * record, and the host uploaded the rest of it from stale ring contents:
 * noise. Neither is a GL error, so neither was ever reported.
 *
 * So now:
 *  - every entry point runs under gl_lock, which makes the call - its state
 *    updates and its whole record - indivisible;
 *  - the state answered locally belongs to the calling thread's context;
 *  - whenever a record comes from a different context than the one before
 *    it, a MAKE_CURRENT goes on the ring first, and the host replays each
 *    guest context in a host context of its own.
 */
#define NTARGETS 3
static const GLenum tex_targets[NTARGETS] = {
	GL_TEXTURE_2D, GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_1D
};

/* The attribute stacks. The application pushes GL_ENABLE_BIT|GL_COLOR_BUFFER_BIT
 * 257,027 times a run, which restores neither the texture binding nor the
 * current colour - so tracking the mask, rather than invalidating on every
 * pop, is what makes the cache worth having. glcache found this first. */
#define ATTRIB_DEPTH 32

/*
 * Everything the application can ask about, answered here. glcache proved
 * the two hot ones can be tracked exactly; this is the same idea with the
 * rest of what notes/is1gl-phase0.md found the app queries. All of it is
 * per context in GL, including display-list compile mode, which does not
 * read back wrong so much as redirect every call after it.
 */
struct glstate {
	GLuint  tex[NTARGETS];      /* binding per target - the app uses two */
	GLfloat color[4];
	GLint   viewport[4];
	GLint   pack_row_length, pack_alignment, pack_skip_rows, pack_skip_pixels;
	GLint   pack_invert;        /* GL_PACK_INVERT_MESA, ours to honour */
	GLint   unpack_row_length, unpack_alignment;
	GLint   unpack_skip_rows, unpack_skip_pixels;
	GLint   unpack_lsb_first, unpack_swap_bytes;
	GLenum  error;
	GLenum  matrix_mode;
	int     in_begin;
	int     in_list;

	struct {
		GLbitfield mask;
		GLfloat    color[4];
		GLuint     tex[NTARGETS];
	} attrib_stack[ATTRIB_DEPTH];
	int attrib_sp;

	struct {
		GLbitfield mask;
		GLint      pack_row_length, pack_alignment, pack_invert;
		GLint      unpack_row_length, unpack_alignment;
		GLint      unpack_skip_rows, unpack_skip_pixels;
	} client_stack[ATTRIB_DEPTH];
	int client_sp;
};

enum list_op_type {
	LIST_BIND_TEXTURE, LIST_COLOR, LIST_PUSH_ATTRIB,
	LIST_POP_ATTRIB, LIST_CALL_LIST
};

/* Replay only the list commands that change values answered by glGet. */
struct list_op {
	enum list_op_type type;
	GLuint name;
	GLenum target;
	GLbitfield mask;
	GLfloat color[4];
};

struct gl_list {
	GLuint name;
	struct list_op *ops;
	size_t count, capacity;
	struct gl_list *next;
};

struct is1gl_ctx {
	int id;
	Display *dpy;
	unsigned int w, h;      /* its drawable, repeated in each MAKE_CURRENT */
	int bound;              /* made current at least once, so w, h are real */
	/*
	 * Keep a loader from running until libc_r's full timeslice expires while
	 * the renderer is ready.  These are charged as records are appended and
	 * cleared when the loader voluntarily yields outside gl_lock.
	 */
	unsigned int burst_records;
	unsigned long burst_bytes;
	struct gl_list *compiling;
	GLenum list_mode;
	struct glstate st;
};

/* Names belong to the share group, which is every context there is. */
static GLuint next_texture = 1, next_list = 1;
static int    next_ctx_id = 1;
static struct gl_list *lists;

/* Which context each thread has current. renderd has five threads. */
#define MAX_THREADS 16
static struct {
	void              *thr;
	struct is1gl_ctx  *ctx;
	GLXDrawable        drawable;
} bindings[MAX_THREADS];

/* Valid only under gl_lock, and set by gl_enter(). */
static struct is1gl_ctx *cur;       /* the calling thread's context, or 0 */
static struct glstate   *S;         /* its state, or no_ctx's */
static struct glstate    no_ctx;    /* for calls made with nothing current */

static struct is1gl_ctx *emitted;   /* whose records the host is replaying */
static unsigned long n_switches, n_contended;

/*
 * renderd uses libc_r, so all five application threads are scheduled in one
 * kernel thread.  Its display-list/texture loader can otherwise consume a
 * complete libc_r timeslice while the renderer is ready, which turns a short
 * product update into a visible run of missed frames.
 *
 * The context which calls glReadPixels is the renderer.  Make every other
 * context yield after a bounded command burst.  The yield is deliberately
 * performed only after gl_lock is released: the renderer must be able to
 * enter GL when it wakes.
 *
 * Do not raise the renderer's libc_r priority here.  renderd has three other
 * non-GL threads which must service CORBA requests, and a renderer which is
 * almost always runnable can starve them until the process is declared dead.
 */
static struct is1gl_ctx *renderer_ctx;
static unsigned int      loader_burst_records = 128;
static unsigned long     loader_burst_bytes = 256 * 1024;
static unsigned long     n_loader_yields;

/*
 * Weak, so a process linked against plain libc rather than libc_r still
 * loads this library; it then has one thread, and every caller is it.
 */
#pragma weak pthread_self
#pragma weak pthread_yield

static void *thr_self(void)
{
	return pthread_self ? (void *)pthread_self() : (void *)1;
}

static void state_init(struct glstate *g)
{
	memset(g, 0, sizeof *g);
	g->pack_alignment = 4;
	g->unpack_alignment = 4;
	g->matrix_mode = GL_MODELVIEW;
	g->error = GL_NO_ERROR;
}

static int binding_slot(void *thr)
{
	int i;

	for (i = 0; i < MAX_THREADS; i++)
		if (bindings[i].thr == thr) return i;
	return -1;
}

static struct is1gl_ctx *thread_ctx(void *thr)
{
	int i = binding_slot(thr);

	return i >= 0 ? bindings[i].ctx : 0;
}

/*
 * gl_lock. libc_r runs every thread on one kernel thread, so the only way to
 * lose the processor mid-call is its timeslice signal, and a single
 * cmpxchg cannot be split by a signal. The lock prefix is not needed for
 * that; it is there so the lock stays correct on anything else.
 *
 * Waiting is by nanosleep rather than sched_yield: a yield hands the CPU to
 * the next runnable thread of the same or higher priority, which may never
 * be the holder, whereas a sleeping thread is not runnable at all. The
 * holder only ever holds it for one GL call, so this is rare.
 */
static void *volatile gl_owner;
static int gl_depth;

static void *cas_ptr(void *volatile *p, void *old, void *new)
{
	void *prev;

	__asm __volatile("lock; cmpxchgl %2, %1"
	    : "=a" (prev), "=m" (*p)
	    : "r" (new), "0" (old), "m" (*p)
	    : "memory", "cc");
	return prev;
}

static void gl_enter(void)
{
	void *me = thr_self();

	if (gl_owner == me) {           /* glXWaitGL -> glFinish, and so on */
		gl_depth++;
		return;
	}
	if (cas_ptr(&gl_owner, 0, me) != 0) {
		struct timespec ts;

		n_contended++;
		ts.tv_sec = 0;
		ts.tv_nsec = 100000;
		do {
			nanosleep(&ts, 0);
		} while (cas_ptr(&gl_owner, 0, me) != 0);
	}
	gl_depth = 1;
	cur = thread_ctx(me);
	S = cur ? &cur->st : &no_ctx;
}

static void gl_leave(void)
{
	struct is1gl_ctx *leaving = 0;
	int yield_loader = 0;

	if (--gl_depth == 0) {
		leaving = cur;
		if (renderer_ctx && leaving && leaving != renderer_ctx &&
		    ((loader_burst_records &&
		      leaving->burst_records >= loader_burst_records) ||
		     (loader_burst_bytes &&
		      leaving->burst_bytes >= loader_burst_bytes))) {
			leaving->burst_records = 0;
			leaving->burst_bytes = 0;
			yield_loader = 1;
		}
		gl_owner = 0;
	}

	/* Never yield with gl_owner held. */
	if (yield_loader && pthread_yield) {
		n_loader_yields++;
		pthread_yield();
	}
}

/* Called under gl_lock by the one GL operation unique to the renderer. */
static void mark_renderer(void)
{
	if (!cur || renderer_ctx == cur) return;

	renderer_ctx = cur;
	say("context %d is renderer; loader burst is %u records or %lu bytes\n",
	    cur->id, loader_burst_records, loader_burst_bytes);
}

static int target_index(GLenum t)
{
	int i;
	for (i = 0; i < NTARGETS; i++)
		if (tex_targets[i] == t) return i;
	return -1;
}

static void set_error(GLenum e)
{
	if (S->error == GL_NO_ERROR) S->error = e;
}

static struct gl_list *find_list(GLuint name)
{
	struct gl_list *list;

	for (list = lists; list; list = list->next)
		if (list->name == name) return list;
	return 0;
}

static void delete_list(GLuint name)
{
	struct gl_list **link = &lists;

	while (*link) {
		struct gl_list *list = *link;

		if (list->name == name) {
			int i;

			for (i = 0; i < MAX_THREADS; i++) {
				if (bindings[i].ctx &&
				    bindings[i].ctx->compiling == list)
					bindings[i].ctx->compiling = 0;
			}
			*link = list->next;
			free(list->ops);
			free(list);
			return;
		}
		link = &list->next;
	}
}

static void record_list_op(struct list_op op)
{
	struct gl_list *list = cur ? cur->compiling : 0;
	struct list_op *ops;
	size_t capacity;

	if (!list) return;
	if (list->count == list->capacity) {
		capacity = list->capacity ? list->capacity * 2 : 8;
		ops = realloc(list->ops, capacity * sizeof *ops);
		if (!ops) {
			set_error(GL_OUT_OF_MEMORY);
			return;
		}
		list->ops = ops;
		list->capacity = capacity;
	}
	list->ops[list->count++] = op;
}

static int list_executes(void)
{
	return !cur || !cur->compiling ||
	       cur->list_mode == GL_COMPILE_AND_EXECUTE;
}

static void state_push_attrib(GLbitfield mask)
{
	if (S->attrib_sp < ATTRIB_DEPTH) {
		S->attrib_stack[S->attrib_sp].mask = mask;
		memcpy(S->attrib_stack[S->attrib_sp].color, S->color,
		       sizeof S->color);
		memcpy(S->attrib_stack[S->attrib_sp].tex, S->tex,
		       sizeof S->tex);
		S->attrib_sp++;
	} else {
		set_error(GL_STACK_OVERFLOW);
	}
}

static void state_pop_attrib(void)
{
	if (S->attrib_sp > 0) {
		S->attrib_sp--;
		if (S->attrib_stack[S->attrib_sp].mask & GL_CURRENT_BIT)
			memcpy(S->color, S->attrib_stack[S->attrib_sp].color,
			       sizeof S->color);
		if (S->attrib_stack[S->attrib_sp].mask & GL_TEXTURE_BIT)
			memcpy(S->tex, S->attrib_stack[S->attrib_sp].tex,
			       sizeof S->tex);
	} else {
		set_error(GL_STACK_UNDERFLOW);
	}
}

static void execute_list(GLuint name, unsigned int depth)
{
	struct gl_list *list = find_list(name);
	size_t i;

	if (!list || depth >= 64) return;
	for (i = 0; i < list->count; i++) {
		const struct list_op *op = &list->ops[i];
		int target;

		switch (op->type) {
		case LIST_BIND_TEXTURE:
			target = target_index(op->target);
			if (target >= 0) S->tex[target] = op->name;
			break;
		case LIST_COLOR:
			memcpy(S->color, op->color, sizeof S->color);
			break;
		case LIST_PUSH_ATTRIB:
			state_push_attrib(op->mask);
			break;
		case LIST_POP_ATTRIB:
			state_pop_attrib();
			break;
		case LIST_CALL_LIST:
			execute_list(op->name, depth + 1);
			break;
		}
	}
}

/*
 * IS1GL_STRESS: stall one context at the points where a thread switch used
 * to do damage, so the fix can be proved rather than inferred from quiet
 * runs. A bitmask - 1: after each glBindTexture, where the other thread then
 * binds its own; 2: halfway through copying a large image into the ring.
 * The context is IS1GL_STRESS_CTX, default 1, which in renderd is the
 * loader's; the stall is IS1GL_STRESS_MS, default 20. A test mode only.
 */
static int stress_mask, stress_ctx = 1, stress_ms = 20;

static void stress(int what, struct is1gl_ctx *c)
{
	struct timespec ts;

	if (!(stress_mask & what) || !c || c->id != stress_ctx) return;
	ts.tv_sec = stress_ms / 1000;
	ts.tv_nsec = (stress_ms % 1000) * 1000000L;
	nanosleep(&ts, 0);
}

/* ------------------------------------------------------------- the ring */

static volatile unsigned int *dev;      /* the device's MMIO page */
static unsigned char         *ring;     /* header + data */
static unsigned int           ring_size;
static unsigned int           head;     /* our write offset into the data */
static unsigned int           fence_seq;
static int                    transport_up;

static unsigned long n_records, n_flushes, n_waits, n_spin_hits;
static double        wait_us_total;

/*
 * A ring of the most recent opcodes. renderd dies on signal 11 rather than
 * exiting, so atexit() never runs and a report written only at exit tells
 * you nothing about the crash. glshim learned this the same way, and its
 * call history is what localised the X server's YCbCr crash to a single
 * glTexImage2D. One store per record; nothing is formatted until something
 * goes wrong.
 */
#define HIST_N 64
static unsigned short hist[HIST_N];
static unsigned long  hist_pos;

static unsigned int hdr(unsigned off)
{
	return *(volatile unsigned int *)(ring + off);
}

static void hdr_set(unsigned off, unsigned int v)
{
	*(volatile unsigned int *)(ring + off) = v;
}

static double now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1e6 + tv.tv_usec;
}

static int transport_open(void)
{
	unsigned int mb, bytes, offset;
	const char *e;
	int fd;

	if (transport_up) return 0;
	verbose = getenv("IS1GL_LOG") != 0 || getenv("IS1GL_VERBOSE") != 0;

	fd = open("/dev/mem", O_RDWR);
	if (fd < 0) {
		fprintf(is1gl_log(), "is1gl: open /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	dev = (volatile unsigned int *)mmap(0, 4096, PROT_READ | PROT_WRITE,
	                                    MAP_SHARED, fd, (off_t)IS1GL_MMIO_BASE);
	if (dev == (volatile unsigned int *)-1) {
		fprintf(is1gl_log(), "is1gl: mmap /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	if (dev[IS1GL_REG_MAGIC / 4] != IS1GL_MAGIC) {
		fprintf(is1gl_log(), "is1gl: device not present (magic 0x%08x)\n",
		        dev[IS1GL_REG_MAGIC / 4]);
		return -1;
	}

	e = getenv("IS1GL_RING_MB");
	mb = e ? (unsigned)atoi(e) : 16;
	if (mb < 1) mb = 1;
	bytes = IS1GL_RING_HDR + mb * 1024 * 1024;

	{
		int top = agp_reserve_top(bytes);
		if (top < 0) {
			fprintf(is1gl_log(), "is1gl: no room for a %u MB ring\n", mb);
			return -1;
		}
		offset = (unsigned int)top;
	}
	ring = (unsigned char *)agp_alloc_at(bytes, offset);
	if (!ring) {
		fprintf(is1gl_log(), "is1gl: cannot allocate a %u MB ring\n", mb);
		return -1;
	}
	ring_size = mb * 1024 * 1024;

	/*
	 * Tell the host the aperture before the ring. Every readback
	 * destination is inside it, so one mapping covers all of them however
	 * many frame buffers renderd decides to allocate.
	 */
	dev[IS1GL_REG_APER_LO / 4] = aper_base;
	dev[IS1GL_REG_APER_HI / 4] = 0;
	dev[IS1GL_REG_APER_SZ / 4] = aper_size;

	dev[IS1GL_REG_RING_LO / 4] = aper_base + offset;
	dev[IS1GL_REG_RING_HI / 4] = 0;
	dev[IS1GL_REG_RING_SZ / 4] = ring_size;      /* arms it */

	if (!dev[IS1GL_REG_STATUS / 4] || hdr(RH_MAGIC) != IS1GL_RING_MAGIC) {
		fprintf(is1gl_log(), "is1gl: host refused the ring "
		        "(status %u, magic 0x%08x)\n",
		        dev[IS1GL_REG_STATUS / 4], hdr(RH_MAGIC));
		return -1;
	}
	head = 0;
	transport_up = 1;
	say("transport up: ring %u MB at phys 0x%08x\n", mb, aper_base + offset);
	return 0;
}

/* Ring the doorbell. A store to a mapped page - not a syscall. */
static void ring_flush(void)
{
	if (!transport_up) return;
	hdr_set(RH_HEAD, head);
	dev[IS1GL_REG_DOORBELL / 4] = head;
	n_flushes++;
}

/*
 * Space, and the wrap.
 *
 * The first version of this added the room before the end of the ring to the
 * room after wrapping and compared the total against the record - which is
 * wrong twice over, because one record can only use one of those, and
 * because it would then happily wrap on top of data the host had not read.
 * It showed up as a 691,240-byte record "not fitting" in a 16 MB ring.
 *
 * head == tail means empty, so one record of slack is always left; otherwise
 * a full ring would be indistinguishable from an empty one.
 */
#define RING_SLACK 8

static int ring_fits(unsigned int len, int *wrap)
{
	unsigned int tail = hdr(RH_TAIL);

	*wrap = 0;
	if (head >= tail) {
		/* Free is [head, ring_size) and then [0, tail). */
		if (head + len + RING_SLACK <= ring_size)
			return 1;
		if (tail > len + RING_SLACK) {   /* room after wrapping */
			*wrap = 1;
			return 1;
		}
		return 0;
	}
	/* Free is [head, tail) only. */
	return head + len + RING_SLACK <= tail;
}

/*
 * Wait for the host to make room. Ring the doorbell first - it may not know
 * there is anything to do - then poll RH_TAIL. No fence is emitted here: a
 * fence would be another record, and needing a record to make room for a
 * record is how the first version of this recursed.
 */
static void ring_stall(unsigned int len, int *wrap)
{
	double t0;
	static unsigned long n_stalls;

	ring_flush();
	t0 = now_us();
	for (;;) {
		if (ring_fits(len, wrap))
			return;
		{
			struct timespec ts;
			ts.tv_sec = 0;
			ts.tv_nsec = 1000000;
			nanosleep(&ts, 0);
		}
		if (now_us() - t0 > 5e6) {
			say("ring stalled 5 s waiting for %u bytes "
			    "(head %u tail %u size %u)\n",
			    len, head, hdr(RH_TAIL), ring_size);
			return;
		}
	}
	(void)n_stalls;
}

/*
 * Reserve a record and return a pointer to its payload. The only thing on
 * the hot path, and it does no I/O and no allocation.
 *
 * Called only under gl_lock, which the caller holds until the payload is
 * complete. That is what keeps `head` consistent, and what stops another
 * thread's doorbell from handing the host a record still being written -
 * not, as this once said, libc_r only switching threads where one blocks.
 *
 * If this record comes from a different context than the last one, the
 * host is told first. That happens here rather than in gl_enter() so that
 * calls answered locally, which are most of the glGets, cost no record.
 */
static void emit_switch(void);

static unsigned char *ring_record(unsigned int opcode, long payload)
{
	unsigned int len = (unsigned int)((8 + payload + 7) & ~7L);
	unsigned char *p;
	int wrap = 0;

	if (!transport_up) return 0;

	if (cur != emitted && cur && cur->bound &&
	    opcode != IS1GL_OP_MAKE_CURRENT)
		emit_switch();

	if (len + RING_SLACK > ring_size) {
		static int warned;
		if (!warned++)
			say("record of %u bytes can never fit in a %u byte ring; "
			    "raise IS1GL_RING_MB\n", len, ring_size);
		return 0;
	}

	if (!ring_fits(len, &wrap)) {
		/*
		 * The ring holds a frame many times over and glReadPixels
		 * drains it every frame, so this should not happen. Stalling is
		 * still better than overwriting records the host has not read.
		 */
		ring_stall(len, &wrap);
		if (!ring_fits(len, &wrap))
			return 0;
	}

	/* A record never straddles the end: write WRAP and restart at 0. */
	if (wrap) {
		unsigned char *w = ring + IS1GL_RING_HDR + head;
		*(unsigned int *)(w + 0) = IS1GL_OP_WRAP;
		*(unsigned int *)(w + 4) = 8;
		head = 0;
	}

	p = ring + IS1GL_RING_HDR + head;
	*(unsigned int *)(p + 0) = opcode;
	*(unsigned int *)(p + 4) = len;
	head += len;
	if (cur) {
		cur->burst_records++;
		cur->burst_bytes += len;
	}
	n_records++;
	hist[hist_pos++ % HIST_N] = (unsigned short)opcode;
	return p + 8;
}

/* memcpy rather than casts: the host reads these the same way, and neither
 * side then depends on the other's alignment. */
static void put_u32(unsigned char *p, unsigned int v) { memcpy(p, &v, 4); }
static void put_i32(unsigned char *p, int v)          { memcpy(p, &v, 4); }
static void put_f32(unsigned char *p, float v)        { memcpy(p, &v, 4); }
static void put_f64(unsigned char *p, double v)       { memcpy(p, &v, 8); }

/* MAKE_CURRENT: context id, then its drawable's size. */
static int emit_make_current(struct is1gl_ctx *c)
{
	unsigned char *r = ring_record(IS1GL_OP_MAKE_CURRENT, 12);

	if (!r) return -1;
	put_u32(r + 0, (unsigned int)c->id);
	put_u32(r + 4, c->w);
	put_u32(r + 8, c->h);
	emitted = c;
	return 0;
}

static void emit_switch(void)
{
	if (emit_make_current(cur) == 0)
		n_switches++;
}

static unsigned int emit_fence(void)
{
	unsigned char *r;

	fence_seq++;
	r = ring_record(IS1GL_OP_FENCE, 4);
	if (r) put_u32(r, fence_seq);
	hdr_set(RH_GUEST_SEQ, fence_seq);
	return fence_seq;
}

/*
 * The wait. Phase 1: the host answers in ~7 us and the sleep quantum is
 * 1000 us, so spin briefly first. The budget bounds the worst case at about
 * 60 us - 0.18% of a frame - and the nanosleep fallback is what keeps a slow
 * host from starving renderd's other threads.
 */
static void wait_fence(unsigned int seq)
{
	static int budget = -1;
	double t0 = 0;
	int n;

	if (!transport_up) return;
	if (budget < 0) {
		const char *e = getenv("IS1GL_SPIN");
		budget = e ? atoi(e) : 20000;
	}
	n_waits++;
	/*
	 * gettimeofday is a real syscall on this guest, and this is the one
	 * place in a frame that could afford none. Time the wait only when
	 * asked: ktrace showed the two calls, and instrumentation that shows
	 * up in the trace of the thing it measures is worth gating.
	 */
	if (verbose) t0 = now_us();

	for (n = budget; n > 0; n--) {
		if (hdr(RH_DONE_SEQ) >= seq) {
			n_spin_hits++;
			if (verbose) wait_us_total += now_us() - t0;
			return;
		}
	}
	if (!t0) t0 = now_us();     /* the slow path needs a timeout anyway */
	while (hdr(RH_DONE_SEQ) < seq) {
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = 1000000;
		nanosleep(&ts, 0);
		if (now_us() - t0 > 5e6) {
			say("fence %u did not complete in 5 s (done %u)\n",
			    seq, hdr(RH_DONE_SEQ));
			break;
		}
	}
	wait_us_total += now_us() - t0;
}

/* --------------------------------------------------------- image helpers */

static int pixel_bytes(GLenum format, GLenum type)
{
	int comps;

	if (format == GL_YCBCR_MESA)
		return 2;                       /* 4:2:2, two bytes a pixel */

	switch (type) {
	case GL_UNSIGNED_INT_8_8_8_8:
	case GL_UNSIGNED_INT_8_8_8_8_REV:
	case GL_UNSIGNED_INT_10_10_10_2:
	case GL_UNSIGNED_INT_2_10_10_10_REV:
		return 4;
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_5_6_5_REV:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_4_4_4_4_REV:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT_1_5_5_5_REV:
		return 2;
	}
	switch (format) {
	case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA:
	case GL_LUMINANCE: case GL_STENCIL_INDEX: case GL_DEPTH_COMPONENT:
		comps = 1; break;
	case GL_LUMINANCE_ALPHA: comps = 2; break;
	case GL_RGB: case GL_BGR:   comps = 3; break;
	case GL_RGBA: case GL_BGRA: comps = 4; break;
	default: return 0;
	}
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE:   return comps;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return comps * 2;
	case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return comps * 4;
	}
	return 0;
}

/* What goes on the wire: tightly packed rows, so the host never has to know
 * the application's unpack state. */
static long is1gl_image_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
	int bpp = pixel_bytes(format, type);

	if (bpp <= 0 || w <= 0 || h <= 0) return 0;
	return (long)w * h * bpp;
}

static void is1gl_pack_image(unsigned char *dst, const void *src,
                             GLsizei w, GLsizei h, GLenum format, GLenum type)
{
	const unsigned char *s = (const unsigned char *)src;
	int bpp = pixel_bytes(format, type);
	long row_pixels, stride, row;

	if (bpp <= 0) {
		/* Diagnostic only. A format/type pair with no entry in
		 * pixel_bytes() packs ZERO bytes into the ring, so the host
		 * uploads an empty texture and every quad sampling it comes
		 * out uniform - which on a glyph atlas is text rendered as
		 * solid boxes. GL_BITMAP and GL_COLOR_INDEX both land here.
		 * Costs nothing unless IS1GL_LOG is set. */
		static int said;
		if (!said++)
			say("image format/type unhandled: format=0x%x type=0x%x"
			    " (%dx%d) - packing 0 bytes\n",
			    (unsigned)format, (unsigned)type, (int)w, (int)h);
	}
	if (!src || bpp <= 0 || w <= 0 || h <= 0) return;

	row_pixels = S->unpack_row_length > 0 ? S->unpack_row_length : w;
	if (row_pixels < w) row_pixels = w;
	stride = row_pixels * bpp;
	if (S->unpack_alignment > 1)
		stride = (stride + S->unpack_alignment - 1) &
		         ~(long)(S->unpack_alignment - 1);
	s += (long)S->unpack_skip_rows * stride +
	     (long)S->unpack_skip_pixels * bpp;

	for (row = 0; row < h; row++) {
		if (row == h / 2 && (long)w * h * bpp > 65536)
			stress(2, cur);
		memcpy(dst + row * (long)w * bpp, s + row * stride, (long)w * bpp);
	}
}

/* ------------------------------------------------------------- the stubs */

static void is1gl_stub(int idx);

#include "is1gl_gen_guest.h"

static void is1gl_stub(int idx)
{
	if (idx >= 0 && idx < IS1GL_STUB_COUNT) {
		if (!is1gl_stub_hits[idx]++)
			say("unimplemented: %s\n", is1gl_stub_names[idx]);
	}
}

/* ------------------------------------------- hand-written GL entry points
 *
 * Each takes gl_lock for its whole length; see "threads and contexts". The
 * generated ones in is1gl_gen_guest.h do the same.
 */

void glEnable(GLenum cap)
{
	gl_enter();
	emit_glEnable(cap);
	gl_leave();
}

void glDisable(GLenum cap)
{
	gl_enter();
	emit_glDisable(cap);
	gl_leave();
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	gl_enter();
	S->viewport[0] = x; S->viewport[1] = y;
	S->viewport[2] = width; S->viewport[3] = height;
	emit_glViewport(x, y, width, height);
	gl_leave();
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	struct list_op op = { .type = LIST_COLOR,
	                      .color = { r, g, b, a } };

	gl_enter();
	record_list_op(op);
	if (list_executes()) {
		S->color[0] = r; S->color[1] = g; S->color[2] = b; S->color[3] = a;
	}
	emit_glColor4f(r, g, b, a);
	gl_leave();
}

void glBindTexture(GLenum target, GLuint texture)
{
	struct is1gl_ctx *c;
	struct list_op op = { .type = LIST_BIND_TEXTURE,
	                      .name = texture, .target = target };
	int i;

	gl_enter();
	record_list_op(op);
	i = target_index(target);
	if (i >= 0 && list_executes())
		S->tex[i] = texture;
	emit_glBindTexture(target, texture);
	c = cur;
	gl_leave();
	/* Outside the lock, so the other thread can run and bind its own. */
	stress(1, c);
}

void glBegin(GLenum mode)
{
	gl_enter();
	S->in_begin = 1;
	emit_glBegin(mode);
	gl_leave();
}

void glEnd(void)
{
	gl_enter();
	S->in_begin = 0;
	emit_glEnd();
	gl_leave();
}

void glNewList(GLuint list, GLenum mode)
{
	struct gl_list *entry;

	gl_enter();
	entry = calloc(1, sizeof *entry);
	if (entry) {
		entry->name = list;
		if (cur) cur->compiling = entry;
		if (cur) cur->list_mode = mode;
	} else {
		set_error(GL_OUT_OF_MEMORY);
	}
	S->in_list = 1;
	emit_glNewList(list, mode);
	gl_leave();
}

void glEndList(void)
{
	struct gl_list *entry;

	gl_enter();
	S->in_list = 0;
	entry = cur ? cur->compiling : 0;
	if (entry) {
		delete_list(entry->name);
		entry->next = lists;
		lists = entry;
		cur->compiling = 0;
	}
	emit_glEndList();
	gl_leave();
}

void glCallList(GLuint list)
{
	struct list_op op = { .type = LIST_CALL_LIST, .name = list };

	gl_enter();
	record_list_op(op);
	if (list_executes()) execute_list(list, 0);
	emit_glCallList(list);
	gl_leave();
}

void glDeleteLists(GLuint list, GLsizei range)
{
	GLsizei i;

	gl_enter();
	if (range > 0) {
		for (i = 0; i < range; i++) delete_list(list + i);
	}
	emit_glDeleteLists(list, range);
	gl_leave();
}

void glPushAttrib(GLbitfield mask)
{
	struct list_op op = { .type = LIST_PUSH_ATTRIB, .mask = mask };

	gl_enter();
	record_list_op(op);
	if (list_executes()) state_push_attrib(mask);
	emit_glPushAttrib(mask);
	gl_leave();
}

void glPopAttrib(void)
{
	struct list_op op = { .type = LIST_POP_ATTRIB };

	gl_enter();
	record_list_op(op);
	if (list_executes()) state_pop_attrib();
	emit_glPopAttrib();
	gl_leave();
}

void glPushClientAttrib(GLbitfield mask)
{
	gl_enter();
	if (S->client_sp < ATTRIB_DEPTH) {
		int n = S->client_sp;

		S->client_stack[n].mask = mask;
		S->client_stack[n].pack_row_length = S->pack_row_length;
		S->client_stack[n].pack_alignment = S->pack_alignment;
		S->client_stack[n].pack_invert = S->pack_invert;
		S->client_stack[n].unpack_row_length = S->unpack_row_length;
		S->client_stack[n].unpack_alignment = S->unpack_alignment;
		S->client_stack[n].unpack_skip_rows = S->unpack_skip_rows;
		S->client_stack[n].unpack_skip_pixels = S->unpack_skip_pixels;
		S->client_sp++;
	} else {
		set_error(GL_STACK_OVERFLOW);
	}
	emit_glPushClientAttrib(mask);
	gl_leave();
}

void glPopClientAttrib(void)
{
	gl_enter();
	if (S->client_sp > 0) {
		int n = --S->client_sp;

		if (S->client_stack[n].mask & GL_CLIENT_PIXEL_STORE_BIT) {
			S->pack_row_length = S->client_stack[n].pack_row_length;
			S->pack_alignment = S->client_stack[n].pack_alignment;
			S->pack_invert = S->client_stack[n].pack_invert;
			S->unpack_row_length = S->client_stack[n].unpack_row_length;
			S->unpack_alignment = S->client_stack[n].unpack_alignment;
			S->unpack_skip_rows = S->client_stack[n].unpack_skip_rows;
			S->unpack_skip_pixels = S->client_stack[n].unpack_skip_pixels;
		}
	} else {
		set_error(GL_STACK_UNDERFLOW);
	}
	emit_glPopClientAttrib();
	gl_leave();
}

/*
 * Pixel store is tracked, never forwarded. Rows go on the wire tightly
 * packed, so the host's unpack state is fixed and the application's is ours
 * to apply. That also disposes of the two pnames libglfix existed for:
 * GL_UNPACK_CLIENT_STORAGE_APPLE is a hint and ignoring it is legal, and
 * GL_PACK_INVERT_MESA is honoured natively by the host's Mesa (Phase 0d), so
 * it travels with the readback instead of being emulated by reversing 480
 * rows by hand.
 *
 * This state is per context too, and it matters more than most: the loader
 * uploads glyphs with an alignment of 1 while the renderer uploads video
 * with a SKIP_ROWS of 2048, and each packs its image with its own settings.
 */
static void store_i(GLenum pname, GLint param)
{
	switch (pname) {
	case GL_PACK_ROW_LENGTH:    S->pack_row_length = param; break;
	case GL_PACK_ALIGNMENT:     S->pack_alignment = param; break;
	case GL_PACK_SKIP_ROWS:     S->pack_skip_rows = param; break;
	case GL_PACK_SKIP_PIXELS:   S->pack_skip_pixels = param; break;
	case GL_PACK_INVERT_MESA:   S->pack_invert = (param != 0); break;
	case GL_UNPACK_ROW_LENGTH:  S->unpack_row_length = param; break;
	case GL_UNPACK_ALIGNMENT:   S->unpack_alignment = param; break;
	case GL_UNPACK_SKIP_ROWS:   S->unpack_skip_rows = param; break;
	case GL_UNPACK_SKIP_PIXELS: S->unpack_skip_pixels = param; break;
	case GL_UNPACK_LSB_FIRST:   S->unpack_lsb_first = param; break;
	case GL_UNPACK_SWAP_BYTES:  S->unpack_swap_bytes = param; break;
	case GL_UNPACK_CLIENT_STORAGE_APPLE: break;   /* a hint; legal to ignore */
	default:
		set_error(GL_INVALID_ENUM);
		break;
	}
}

void glPixelStorei(GLenum pname, GLint param)
{
	gl_enter();
	store_i(pname, param);
	gl_leave();
}

void glPixelStoref(GLenum pname, GLfloat param)
{
	gl_enter();
	store_i(pname, (GLint)param);
	gl_leave();
}

void glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
	gl_enter();
	if (!pixels) {
		/* A null pointer means "allocate, do not initialise". Send it
		 * with a zero-length image rather than reading from NULL. */
		emit_glTexImage2D(target, level, internalFormat, width, height,
		                  border, format, GL_NONE, pixels);
	} else {
		emit_glTexImage2D(target, level, internalFormat, width, height,
		                  border, format, type, pixels);
	}
	gl_leave();
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels)
{
	if (!pixels) return;
	gl_enter();
	emit_glTexSubImage2D(target, level, xoffset, yoffset, width, height,
	                     format, type, pixels);
	gl_leave();
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type,
                  const GLvoid *pixels)
{
	if (!pixels) return;
	gl_enter();
	emit_glDrawPixels(width, height, format, type, pixels);
	gl_leave();
}

/*
 * The one call that waits, and the one that makes the whole design pay: the
 * host writes the finished frame straight into the AGP buffer the
 * Thunderstorm DMAs from, so the 1.38 MB never crosses a socket and there is
 * no readback to invert.
 *
 * The wait is outside gl_lock. The other thread may queue calls meanwhile;
 * they are behind the fence, so they cannot delay it.
 */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels)
{
	unsigned int phys, seq;
	unsigned char *r;

	gl_enter();
	mark_renderer();
	phys = agp_phys(pixels);
	if (!phys) {
		/*
		 * Not AGP memory, so the host cannot write it directly. renderd
		 * always reads back into the card's frame buffers, so this is a
		 * caller we did not expect rather than a case to support.
		 */
		static int warned;
		if (!warned++)
			say("glReadPixels into non-AGP memory %p - ignored\n", pixels);
		set_error(GL_INVALID_OPERATION);
		gl_leave();
		return;
	}

	r = ring_record(IS1GL_OP_READPIXELS, 36);
	if (!r) {
		gl_leave();
		return;
	}
	put_i32(r + 0, x);
	put_i32(r + 4, y);
	put_i32(r + 8, width);
	put_i32(r + 12, height);
	put_u32(r + 16, format);
	put_u32(r + 20, type);
	put_u32(r + 24, phys);
	put_i32(r + 28, S->pack_row_length);
	put_i32(r + 32, S->pack_invert);

	seq = emit_fence();
	ring_flush();
	gl_leave();
	wait_fence(seq);
}

void glFinish(void)
{
	unsigned int seq;

	gl_enter();
	seq = emit_fence();
	ring_flush();
	gl_leave();
	wait_fence(seq);
}

void glFlush(void)
{
	gl_enter();
	ring_flush();
	gl_leave();
}

/* ------------------------------------------------------- answered locally */

GLenum glGetError(void)
{
	GLenum e;

	gl_enter();
	e = S->error;
	S->error = GL_NO_ERROR;
	gl_leave();
	return e;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
	GLsizei i;

	/*
	 * Names are allocated here and used verbatim on the host. GL lets
	 * glBindTexture take any name, so there is no mapping table and no
	 * round trip - which matters because a round trip here would be 8 us.
	 * The counter is the share group's, so both threads draw from it,
	 * under the lock, and a name is never handed out twice.
	 */
	if (!textures || n <= 0) return;
	gl_enter();
	for (i = 0; i < n; i++) textures[i] = next_texture++;
	gl_leave();
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
	GLsizei i;
	int target;

	gl_enter();
	if (n > 0 && textures) {
		for (target = 0; target < NTARGETS; target++) {
			for (i = 0; i < n; i++) {
				if (textures[i] && S->tex[target] == textures[i]) {
					S->tex[target] = 0;
					break;
				}
			}
		}
	}
	emit_glDeleteTextures(n, textures);
	gl_leave();
}

GLuint glGenLists(GLsizei range)
{
	GLuint first;

	if (range <= 0) return 0;
	gl_enter();
	first = next_list;
	next_list += range;
	gl_leave();
	return first;
}

void glGetIntegerv(GLenum pname, GLint *params)
{
	if (!params) return;
	gl_enter();
	switch (pname) {
	/* The calling thread's own binding. renderd's glyph code skips its
	 * glBindTexture when this says the page is already bound, so an
	 * answer from the other context draws text with the wrong texture. */
	case GL_TEXTURE_BINDING_2D:   params[0] = (GLint)S->tex[0]; break;
	case GL_PACK_ROW_LENGTH:      params[0] = S->pack_row_length; break;
	case GL_PACK_ALIGNMENT:       params[0] = S->pack_alignment; break;
	case GL_PACK_SKIP_ROWS:       params[0] = S->pack_skip_rows; break;
	case GL_PACK_SKIP_PIXELS:     params[0] = S->pack_skip_pixels; break;
	case GL_PACK_INVERT_MESA:     params[0] = S->pack_invert; break;
	case GL_UNPACK_ROW_LENGTH:    params[0] = S->unpack_row_length; break;
	case GL_UNPACK_ALIGNMENT:     params[0] = S->unpack_alignment; break;
	case GL_UNPACK_SKIP_ROWS:     params[0] = S->unpack_skip_rows; break;
	case GL_UNPACK_SKIP_PIXELS:   params[0] = S->unpack_skip_pixels; break;
	case GL_UNPACK_LSB_FIRST:     params[0] = S->unpack_lsb_first; break;
	case GL_UNPACK_SWAP_BYTES:    params[0] = S->unpack_swap_bytes; break;
	case GL_MATRIX_MODE:          params[0] = (GLint)S->matrix_mode; break;
	/* What the Radeon 9200 the unit shipped with reported. The application
	 * uses it to size textures, so answering larger than the original
	 * would change what it draws. */
	case GL_MAX_TEXTURE_SIZE:     params[0] = 2048; break;
	case GL_VIEWPORT:
		params[0] = S->viewport[0]; params[1] = S->viewport[1];
		params[2] = S->viewport[2]; params[3] = S->viewport[3];
		break;
	default:
		params[0] = 0;
		say("glGetIntegerv(0x%x) unanswered\n", (unsigned)pname);
		break;
	}
	gl_leave();
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
	if (!params) return;
	gl_enter();
	switch (pname) {
	case GL_CURRENT_COLOR:
		params[0] = S->color[0]; params[1] = S->color[1];
		params[2] = S->color[2]; params[3] = S->color[3];
		break;
	default:
		params[0] = 0.0f;
		say("glGetFloatv(0x%x) unanswered\n", (unsigned)pname);
		break;
	}
	gl_leave();
}

const GLubyte *glGetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR:     return (const GLubyte *)"IS1 project";
	case GL_RENDERER:   return (const GLubyte *)"is1gl (host GL via QEMU)";
	case GL_VERSION:    return (const GLubyte *)"1.4";
	case GL_EXTENSIONS: return (const GLubyte *)
		"GL_MESA_pack_invert GL_MESA_ycbcr_texture "
		"GL_APPLE_client_storage GL_NV_texture_rectangle";
	}
	return (const GLubyte *)"";
}

/* ------------------------------------------------------------------ GLX */

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList)
{
	XVisualInfo tmpl, *vi;
	int n;

	(void)attribList;
	/*
	 * The application still makes its own X window, so it needs a real
	 * visual for it - but nothing is drawn through X, so any TrueColor
	 * visual at the screen's depth will do. The card is the output.
	 */
	memset(&tmpl, 0, sizeof tmpl);
	tmpl.screen = screen;
	tmpl.depth = DefaultDepth(dpy, screen);
	tmpl.class = TrueColor;
	vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask |
	                    VisualClassMask, &tmpl, &n);
	if (!vi || n < 1) {
		say("no TrueColor visual at depth %d\n", DefaultDepth(dpy, screen));
		return 0;
	}
	return vi;
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis,
                            GLXContext shareList, Bool direct)
{
	struct is1gl_ctx *c;

	(void)vis; (void)shareList; (void)direct;
	gl_enter();
	if (transport_open() < 0) {
		gl_leave();
		return 0;
	}
	c = (struct is1gl_ctx *)calloc(1, sizeof *c);
	if (!c) {
		gl_leave();
		return 0;
	}
	c->id = next_ctx_id++;
	c->dpy = dpy;
	state_init(&c->st);
	/*
	 * The application makes two contexts and expects them to share
	 * textures and lists. The host keeps one share group for all of them,
	 * and a host context per guest context for everything else.
	 */
	say("context %d created\n", c->id);
	gl_leave();
	return (GLXContext)c;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	struct is1gl_ctx *c = (struct is1gl_ctx *)ctx;
	int i;

	(void)dpy;
	if (!c) return;
	gl_enter();
	for (i = 0; i < MAX_THREADS; i++) {
		if (bindings[i].ctx == c) {
			bindings[i].ctx = 0;
			bindings[i].drawable = (GLXDrawable)0;
		}
	}
	if (emitted == c) emitted = 0;
	if (renderer_ctx == c) {
		renderer_ctx = 0;
	}
	if (cur == c) {
		cur = 0;
		S = &no_ctx;
	}
	if (c->compiling) {
		free(c->compiling->ops);
		free(c->compiling);
	}
	free(c);
	gl_leave();
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
	struct is1gl_ctx *c = (struct is1gl_ctx *)ctx;
	unsigned int w = 720, h = 480;
	void *me = thr_self();
	Window root;
	int xx, yy, i;
	unsigned int bw, dep;

	/*
	 * The drawable's size, for the host's FBO. Asked before taking the
	 * lock: it is an X round trip, and the other thread may be waiting.
	 */
	if (c && dpy && drawable &&
	    XGetGeometry(dpy, drawable, &root, &xx, &yy, &w, &h, &bw, &dep)) {
		say("make current: context %d drawable %ux%u\n", c->id, w, h);
	}

	gl_enter();
	i = binding_slot(me);
	if (!c) {
		/* Release: this thread has nothing current any more. */
		if (i >= 0) {
			bindings[i].ctx = 0;
			bindings[i].drawable = (GLXDrawable)0;
		}
		cur = 0;
		S = &no_ctx;
		gl_leave();
		return True;
	}
	if (transport_open() < 0) {
		gl_leave();
		return False;
	}
	if (i < 0 && (i = binding_slot(0)) < 0) {
		say("more than %d threads made a context current\n", MAX_THREADS);
		gl_leave();
		return False;
	}

	/*
	 * A newly-created GL context starts with a viewport covering its first
	 * drawable. Thereafter the viewport belongs to the context.
	 */
	if (!c->bound) {
		c->st.viewport[0] = 0;
		c->st.viewport[1] = 0;
		c->st.viewport[2] = (GLint)w;
		c->st.viewport[3] = (GLint)h;
	}
	c->w = w;
	c->h = h;
	c->bound = 1;
	bindings[i].thr = me;
	bindings[i].ctx = c;
	bindings[i].drawable = drawable;
	cur = c;
	S = &c->st;

	if (emit_make_current(c) < 0) {
		gl_leave();
		return False;
	}
	/*
	 * A host with one context for everybody keeps only the viewport per
	 * guest context, and older ones not even that; restating it is what
	 * those need. A host with a context per guest context ignores the
	 * repetition.
	 */
	emit_glViewport(S->viewport[0], S->viewport[1],
	                S->viewport[2], S->viewport[3]);
	ring_flush();
	gl_leave();
	return True;
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
	unsigned char *r;

	(void)dpy; (void)drawable;
	/*
	 * A frame marker, nothing more. The frame has already left through
	 * glReadPixels - notes/is1gl-phase0.md established that the readback
	 * precedes the swap, one of each per frame - and the X window stays
	 * black by design.
	 */
	gl_enter();
	r = ring_record(IS1GL_OP_SWAP, 0);
	(void)r;
	ring_flush();
	gl_leave();
}

GLXContext glXGetCurrentContext(void)
{
	struct is1gl_ctx *c;

	gl_enter();
	c = cur;
	gl_leave();
	return (GLXContext)c;
}

GLXDrawable glXGetCurrentDrawable(void)
{
	GLXDrawable d = (GLXDrawable)0;
	int i;

	gl_enter();
	i = binding_slot(thr_self());
	if (i >= 0 && bindings[i].ctx)
		d = bindings[i].drawable;
	gl_leave();
	return d;
}

Bool glXIsDirect(Display *dpy, GLXContext ctx) { (void)dpy; (void)ctx; return True; }
void glXWaitGL(void) { glFinish(); }
void glXWaitX(void) { }

Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase)
{
	(void)dpy;
	if (errorBase) *errorBase = 0;
	if (eventBase) *eventBase = 0;
	return True;
}

Bool glXQueryVersion(Display *dpy, int *major, int *minor)
{
	(void)dpy;
	if (major) *major = 1;
	if (minor) *minor = 2;
	return True;
}

int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value)
{
	(void)dpy; (void)vis;
	if (!value) return 0;
	switch (attrib) {
	case GLX_USE_GL:      *value = 1; break;
	case GLX_RGBA:        *value = 1; break;
	case GLX_DOUBLEBUFFER:*value = 1; break;
	case GLX_RED_SIZE: case GLX_GREEN_SIZE:
	case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *value = 8; break;
	case GLX_DEPTH_SIZE:  *value = 0; break;
	default:              *value = 0; break;
	}
	return 0;
}

/* The AGP entry points renderd needs, which is why libagpnv existed. */
void *glXAllocateMemoryNV(GLsizei size, GLfloat readfreq, GLfloat writefreq,
                          GLfloat priority)
{
	void *p = 0;

	(void)readfreq; (void)writefreq; (void)priority;
	gl_enter();
	if (transport_open() == 0)
		p = agp_alloc((unsigned int)size, 0);
	gl_leave();
	return p;
}

void glXFreeMemoryNV(GLvoid *pointer)
{
	/* Deliberately not reclaimed: renderd allocates once at start-up, and
	 * a bump allocator that never frees cannot hand back a range the card
	 * still has a VIDIOC_S_AGP pointing into. */
	(void)pointer;
}

GLuint glXGetAGPOffsetMESA(const GLvoid *pointer)
{
	unsigned int phys;

	gl_enter();
	phys = agp_phys(pointer);
	gl_leave();
	return phys ? (phys - aper_base) : ~0u;
}

void (*glXGetProcAddressARB(const GLubyte *name))(void)
{
	(void)name;
	return 0;
}

/* --------------------------------------------------------------- report */

static const char *const is1gl_op_names[] = IS1GL_OP_NAMES;

static void is1gl_report(void)
{
	FILE *f = is1gl_log();
	int i, n = 0;

	if (!n_records) return;
	fprintf(f, "\n[is1gl] records %lu  flushes %lu  waits %lu\n",
	        n_records, n_flushes, n_waits);
	if (n_waits)
		fprintf(f, "[is1gl] wait %.1f us mean, %lu%% answered in the spin\n",
		        wait_us_total / n_waits, 100 * n_spin_hits / n_waits);
	fprintf(f, "[is1gl] contexts %d  switches %lu  calls that waited for "
	        "another thread %lu\n", next_ctx_id - 1, n_switches, n_contended);
	fprintf(f, "[is1gl] loader yields %lu "
	        "(burst %u records or %lu bytes)\n", n_loader_yields,
	        loader_burst_records, loader_burst_bytes);
	if (stress_mask)
		fprintf(f, "[is1gl] STRESS TEST MODE %d: context %d stalls %d ms\n",
		        stress_mask, stress_ctx, stress_ms);
	fprintf(f, "[is1gl] host: done_seq %u  errors %u\n",
	        transport_up ? hdr(RH_DONE_SEQ) : 0,
	        transport_up ? hdr(RH_ERRORS) : 0);
	for (i = 0; i < IS1GL_STUB_COUNT; i++) {
		if (is1gl_stub_hits[i]) {
			if (!n++) fprintf(f, "[is1gl] UNIMPLEMENTED CALLS:\n");
			fprintf(f, "    %-32s %lu\n", is1gl_stub_names[i],
			        is1gl_stub_hits[i]);
		}
	}
	if (!n) fprintf(f, "[is1gl] no unimplemented entry points were called\n");

	/* The last calls, oldest first. */
	fprintf(f, "[is1gl] last %d records (most recent last):\n",
	        (int)(hist_pos < HIST_N ? hist_pos : HIST_N));
	{
		unsigned long start = hist_pos > HIST_N ? hist_pos - HIST_N : 0;
		unsigned long k;
		for (k = start; k < hist_pos; k++) {
			unsigned int op = hist[k % HIST_N];
			fprintf(f, "    %s\n",
			        op < IS1GL_OP_COUNT ? is1gl_op_names[op] : "?");
		}
	}
	fflush(f);
}

static void is1gl_sig(int s) { is1gl_report(); (void)s; }

/*
 * Report and then die properly. Restoring the default handler and re-raising
 * keeps the core file and the exit status honest - istard needs to see the
 * real signal, and a handler that swallowed it would turn a crash into a
 * hang.
 */
static void is1gl_fatal(int s)
{
	fprintf(is1gl_log(), "\n[is1gl] FATAL: signal %d\n", s);
	is1gl_report();
	signal(s, SIG_DFL);
	raise(s);
}

static void is1gl_ctor(void) __attribute__((constructor));
static void is1gl_ctor(void)
{
	const char *e;

	state_init(&no_ctx);
	S = &no_ctx;
	verbose = getenv("IS1GL_LOG") != 0 || getenv("IS1GL_VERBOSE") != 0;
	if ((e = getenv("IS1GL_STRESS")) != 0) stress_mask = atoi(e);
	if ((e = getenv("IS1GL_STRESS_CTX")) != 0) stress_ctx = atoi(e);
	if ((e = getenv("IS1GL_STRESS_MS")) != 0) stress_ms = atoi(e);
	if ((e = getenv("IS1GL_LOADER_BURST_RECORDS")) != 0)
		loader_burst_records = (unsigned int)strtoul(e, 0, 0);
	if ((e = getenv("IS1GL_LOADER_BURST_BYTES")) != 0)
		loader_burst_bytes = strtoul(e, 0, 0);
	signal(SIGUSR2, is1gl_sig);
	/*
	 * The crash report is worth having, but it rewrites the core: the
	 * faulting frame ends up buried under our handler and libc_r's signal
	 * trampoline, and the registers are the handler's. IS1GL_NO_FATAL=1
	 * leaves the core clean for gdb.
	 */
	if (!getenv("IS1GL_NO_FATAL")) {
		signal(SIGSEGV, is1gl_fatal);
		signal(SIGBUS, is1gl_fatal);
		signal(SIGILL, is1gl_fatal);
	}
	atexit(is1gl_report);
}

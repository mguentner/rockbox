/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Nicolas Pennequin
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "buffering.h"

#include "ata.h"
#include "system.h"
#include "thread.h"
#include "file.h"
#include "panic.h"
#include "memory.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "tree.h"
#include "debug.h"
#include "sprintf.h"
#include "settings.h"
#include "codecs.h"
#include "audio.h"
#include "mp3_playback.h"
#include "usb.h"
#include "status.h"
#include "screens.h"
#include "playlist.h"
#include "playback.h"
#include "pcmbuf.h"
#include "buffer.h"

#ifdef SIMULATOR
#define ata_disk_is_active() 1
#endif

#if MEM > 1
#define GUARD_BUFSIZE   (32*1024)
#else
#define GUARD_BUFSIZE  (8*1024)
#endif

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

/* macros to enable logf for queues
   logging on SYS_TIMEOUT can be disabled */
#ifdef SIMULATOR
/* Define this for logf output of all queuing except SYS_TIMEOUT */
#define BUFFERING_LOGQUEUES
/* Define this to logf SYS_TIMEOUT messages */
/* #define BUFFERING_LOGQUEUES_SYS_TIMEOUT */
#endif

#ifdef BUFFERING_LOGQUEUES
#define LOGFQUEUE logf
#else
#define LOGFQUEUE(...)
#endif

#ifdef BUFFERING_LOGQUEUES_SYS_TIMEOUT
#define LOGFQUEUE_SYS_TIMEOUT logf
#else
#define LOGFQUEUE_SYS_TIMEOUT(...)
#endif

/* default point to start buffer refill */
#define BUFFERING_DEFAULT_WATERMARK      (1024*512)
/* amount of data to read in one read() call */
#define BUFFERING_DEFAULT_FILECHUNK      (1024*32)
/* point at which the file buffer will fight for CPU time */
#define BUFFERING_CRITICAL_LEVEL         (1024*128)


/* Ring buffer helper macros */
/* Buffer pointer (p) plus value (v), wrapped if necessary */
#define RINGBUF_ADD(p,v) (((p)+(v))<buffer_len ? (p)+(v) : (p)+(v)-buffer_len)
/* Buffer pointer (p) minus value (v), wrapped if necessary */
#define RINGBUF_SUB(p,v) ((p>=v) ? (p)-(v) : (p)+buffer_len-(v))
/* How far value (v) plus buffer pointer (p1) will cross buffer pointer (p2) */
#define RINGBUF_ADD_CROSS(p1,v,p2) \
((p1<p2) ? (int)((p1)+(v))-(int)(p2) : (int)((p1)+(v)-(p2))-(int)buffer_len)
/* Bytes available in the buffer */
#define BUF_USED RINGBUF_SUB(buf_widx, buf_ridx)

/* assert(sizeof(struct memory_handle)%4==0) */
struct memory_handle {
    unsigned int id;           /* A unique ID for the handle */
    enum data_type type;       /* Type of data buffered with this handle */
    char path[MAX_PATH];       /* Path if data originated in a file */
    int fd;                    /* File descriptor to path (-1 if closed) */
    size_t data;               /* Start index of the handle's data buffer */
    volatile size_t ridx;      /* Read pointer, relative to the main buffer */
    size_t widx;               /* Write pointer */
    size_t filesize;           /* File total length */
    size_t filerem;            /* Remaining bytes of file NOT in buffer */
    volatile size_t available; /* Available bytes to read from buffer */
    size_t offset;             /* Offset at which we started reading the file */
    struct memory_handle *next;
};
/* invariant: filesize == offset + available + filerem */

static char *buffer;
static char *guard_buffer;

static size_t buffer_len;

static volatile size_t buf_widx;  /* current writing position */
static volatile size_t buf_ridx;  /* current reading position */
/* buf_*idx are values relative to the buffer, not real pointers. */

/* Configuration */
static size_t conf_watermark = 0; /* Level to trigger filebuf fill */
static size_t conf_filechunk = 0; /* Bytes-per-read for buffering (impacts
                                     responsiveness of buffering thread) */
static size_t conf_preseek   = 0; /* Distance a codec may look backwards after
                                     seeking, to prevent double rebuffers */
#if MEM > 8
static size_t high_watermark = 0; /* High watermark for rebuffer */
#endif

/* current memory handle in the linked list. NULL when the list is empty. */
static struct memory_handle *cur_handle;
/* first memory handle in the linked list. NULL when the list is empty. */
static struct memory_handle *first_handle;

static int num_handles;  /* number of handles in the list */

static unsigned int base_handle_id;

static struct mutex llist_mutex;

/* Handle cache (makes find_handle faster).
   This is global so that move_handle and rm_handle can invalidate it. */
static struct memory_handle *cached_handle = NULL;

static buffer_low_callback buffer_low_callback_funcs[MAX_BUF_CALLBACKS];
static int buffer_callback_count = 0;

static struct {
    size_t remaining;   /* Amount of data needing to be buffered */
    size_t wasted;      /* Amount of space available for freeing */
    size_t buffered;    /* Amount of data currently in the buffer */
    size_t useful;      /* Amount of data still useful to the user */
} data_counters;


/* Messages available to communicate with the buffering thread */
enum {
    Q_BUFFER_HANDLE = 1, /* Request buffering of a handle */
    Q_RESET_HANDLE,      /* (internal) Request resetting of a handle to its
                            offset (the offset has to be set beforehand) */
    Q_CLOSE_HANDLE,      /* Request closing a handle */
    Q_BASE_HANDLE,       /* Set the reference handle for buf_useful_data */

    /* Configuration: */
    Q_SET_WATERMARK,
    Q_SET_CHUNKSIZE,
    Q_SET_PRESEEK,
};

/* Buffering thread */
void buffering_thread(void);
static long buffering_stack[(DEFAULT_STACK_SIZE + 0x2000)/sizeof(long)];
static const char buffering_thread_name[] = "buffering";
static struct thread_entry *buffering_thread_p;
static struct event_queue buffering_queue;
static struct queue_sender_list buffering_queue_sender_list;


/*
LINKED LIST MANAGEMENT
======================

add_handle  : Add a handle to the list
rm_handle   : Remove a handle from the list
find_handle : Get a handle pointer from an ID
move_handle : Move a handle in the buffer (with or without its data)

These functions only handle the linked list structure. They don't touch the
contents of the struct memory_handle headers. They also change the buf_*idx
pointers when necessary and manage the handle IDs.

The first and current (== last) handle are kept track of.
A new handle is added at buf_widx and becomes the current one.
buf_widx always points to the current writing position for the current handle
buf_ridx always points to the location of the first handle.
buf_ridx == buf_widx means the buffer is empty.
*/


/* Add a new handle to the linked list and return it. It will have become the
   new current handle.
   data_size must contain the size of what will be in the handle.
   can_wrap tells us whether this type of data may wrap on buffer
   alloc_all tells us if we must immediately be able to allocate data_size
   returns a valid memory handle if all conditions for allocation are met.
           NULL if there memory_handle itself cannot be allocated or if the
           data_size cannot be allocated and alloc_all is set.  This function
           has no side effects if NULL is returned.
   */
static struct memory_handle *add_handle(size_t data_size, const bool can_wrap,
                                        const bool alloc_all)
{
    /* gives each handle a unique id, unsigned to handle wraps gracefully */
    static unsigned int cur_handle_id = 1;
    size_t shift;
    size_t new_widx = buf_widx;
    size_t len;
    int overlap;

    mutex_lock(&llist_mutex);

    /* Allocate the remainder of the space for the current handle */
    if (cur_handle)
        new_widx = RINGBUF_ADD(cur_handle->widx, cur_handle->filerem);

    /* align buf_widx to 4 bytes up */
    new_widx = (RINGBUF_ADD(new_widx, 3)) & ~3;

    len = data_size + sizeof(struct memory_handle);

    /* First, will the handle wrap? */
    overlap = RINGBUF_ADD_CROSS(new_widx, sizeof(struct memory_handle),
                                buffer_len - 1);
    /* If the handle would wrap, move to the beginning of the buffer,
     * otherwise check if the data can/would wrap and move it to the
     * beginning if needed */
    if (overlap > 0) {
        new_widx = 0;
    } else if (!can_wrap) {
        overlap = RINGBUF_ADD_CROSS(new_widx, len, buffer_len - 1);
        if (overlap > 0)
            new_widx += data_size - overlap;
    }

    /* This is how far we shifted buf_widx to align things */
    shift = RINGBUF_SUB(new_widx, buf_widx);

    /* How much space are we short in the actual ring buffer? */
    overlap = RINGBUF_ADD_CROSS(buf_widx, shift + len, buf_ridx);
    if (overlap >= 0 && (alloc_all || (unsigned)overlap > data_size)) {
        /* Not enough space for required allocations */
        mutex_unlock(&llist_mutex);
        return NULL;
    }

    /* There is enough space for the required data, advance the buf_widx and
     * initialize the struct */
    buf_widx = new_widx;

    struct memory_handle *new_handle =
        (struct memory_handle *)(&buffer[buf_widx]);

    /* only advance the buffer write index of the size of the struct */
    buf_widx = RINGBUF_ADD(buf_widx, sizeof(struct memory_handle));

    new_handle->id = cur_handle_id;
    /* Use += 2 instead of ++ to guarantee that the low bit is always high and
     * prevent the assignment of a zero id when wrapping. */
    cur_handle_id += 2;
    new_handle->next = NULL;
    num_handles++;

    if (!first_handle)
        /* the new handle is the first one */
        first_handle = new_handle;

    if (cur_handle)
        cur_handle->next = new_handle;

    cur_handle = new_handle;

    mutex_unlock(&llist_mutex);
    return new_handle;
}

/* Delete a given memory handle from the linked list
   and return true for success. Nothing is actually erased from memory. */
static bool rm_handle(const struct memory_handle *h)
{
    if (h == NULL)
        return false;

    mutex_lock(&llist_mutex);

    if (h == first_handle) {
        first_handle = h->next;
        if (h == cur_handle) {
            /* h was the first and last handle: the buffer is now empty */
            cur_handle = NULL;
            buf_ridx = buf_widx = 0;
        } else {
            /* update buf_ridx to point to the new first handle */
            buf_ridx = (void *)first_handle - (void *)buffer;
        }
    } else {
        struct memory_handle *m = first_handle;
        while (m && m->next != h) {
            m = m->next;
        }
        if (m && m->next == h) {
            m->next = h->next;
            if (h == cur_handle) {
                cur_handle = m;
                buf_widx = cur_handle->widx;
            }
        } else {
            mutex_unlock(&llist_mutex);
            return false;
        }
    }

    /* Invalidate the cache to prevent it from keeping the old location of h */
    if (h == cached_handle)
        cached_handle = NULL;

    num_handles--;

    mutex_unlock(&llist_mutex);
    return true;
}

/* Return a pointer to the memory handle of given ID.
   NULL if the handle wasn't found */
static struct memory_handle *find_handle(const unsigned int handle_id)
{
    if (handle_id <= 0)
        return NULL;

    mutex_lock(&llist_mutex);

    /* simple caching because most of the time the requested handle
    will either be the same as the last, or the one after the last */
    if (cached_handle)
    {
        if (cached_handle->id == handle_id) {
            mutex_unlock(&llist_mutex);
            return cached_handle;
        } else if (cached_handle->next &&
                   (cached_handle->next->id == handle_id)) {
            cached_handle = cached_handle->next;
            mutex_unlock(&llist_mutex);
            return cached_handle;
        }
    }

    struct memory_handle *m = first_handle;
    while (m && m->id != handle_id) {
        m = m->next;
    }
    /* This condition can only be reached with !m or m->id == handle_id */
    if (m)
        cached_handle = m;

    mutex_unlock(&llist_mutex);
    return m;
}

/* Move a memory handle and data_size of its data delta bytes along the buffer.
   delta maximum bytes available to move the handle.  If the move is performed
         it is set to the actual distance moved.
   data_size is the amount of data to move along with the struct.
   returns a valid memory_handle if the move is successful
           NULL if the handle is NULL, the  move would be less than the size of
           a memory_handle after correcting for wraps or if the handle is not
           found in the linked list for adjustment.  This function has no side
           effects if NULL is returned. */
static bool move_handle(struct memory_handle const **h,
                        size_t *delta, const size_t data_size)
{
    struct memory_handle *dest;
    const struct memory_handle *src;
    size_t newpos;
    size_t size_to_move;
    size_t final_delta = *delta;
    int overlap;

    if (h == NULL || (src = *h) == NULL)
        return false;

    size_to_move = sizeof(struct memory_handle) + data_size;

    /* Align to four bytes, down */
    final_delta &= ~3;
    if (final_delta < sizeof(struct memory_handle)) {
        /* It's not legal to move less than the size of the struct */
        return false;
    }

    mutex_lock(&llist_mutex);

    newpos = RINGBUF_ADD((void *)src - (void *)buffer, final_delta);
    overlap = RINGBUF_ADD_CROSS(newpos, size_to_move, buffer_len - 1);

    if (overlap > 0) {
        /* Some part of the struct + data would wrap, maybe ok */
        size_t correction;
        /* If the overlap lands inside the memory_handle */
        if ((unsigned)overlap > data_size) {
            /* Correct the position and real delta to prevent the struct from
             * wrapping, this guarantees an aligned delta, I think */
            correction = overlap - data_size;
        } else {
            /* Otherwise the overlap falls in the data area and must all be
             * backed out.  This may become conditional if ever we move
             * data that is allowed to wrap (ie audio) */
            correction = overlap;
            /* Align correction to four bytes, up */
            correction = (correction+3) & ~3;
        }
        if (final_delta < correction + sizeof(struct memory_handle)) {
            /* Delta cannot end up less than the size of the struct */
            mutex_unlock(&llist_mutex);
            return false;
        }

        newpos -= correction;
        overlap -= correction;   /* Used below to know how to split the data */
        final_delta -= correction;
    }

    dest = (struct memory_handle *)(&buffer[newpos]);

    if (src == first_handle) {
        first_handle = dest;
        buf_ridx = newpos;
    } else {
        struct memory_handle *m = first_handle;
        while (m && m->next != src) {
            m = m->next;
        }
        if (m && m->next == src) {
            m->next = dest;
        } else {
            mutex_unlock(&llist_mutex);
            return false;
        }
    }


    /* Update the cache to prevent it from keeping the old location of h */
    if (src == cached_handle)
        cached_handle = dest;

    /* the cur_handle pointer might need updating */
    if (src == cur_handle)
        cur_handle = dest;

    if (overlap > 0) {
        size_t first_part = size_to_move - overlap;
        memmove(dest, src, first_part);
        memmove(buffer, (char *)src + first_part, overlap);
    } else {
        memmove(dest, src, size_to_move);
    }

    /* Update the caller with the new location of h and the distance moved */
    *h = dest;
    *delta = final_delta;
    mutex_unlock(&llist_mutex);
    return dest;
}


/*
BUFFER SPACE MANAGEMENT
=======================

yield_codec     : Used by buffer_handle to know if it should interrupt buffering
buffer_handle   : Buffer data for a handle
reset_handle    : Reset writing position and data buffer of a handle to its
                  current offset
rebuffer_handle : Seek to a nonbuffered part of a handle by rebuffering the data
shrink_handle   : Free buffer space by moving a handle
fill_buffer     : Call buffer_handle for all handles that have data to buffer
can_add_handle  : Indicate whether it's safe to add a handle

These functions are used by the buffering thread to manage buffer space.
*/


static inline bool filebuf_is_lowdata(void)
{
    return BUF_USED < BUFFERING_CRITICAL_LEVEL;
}

/* Yield to the codec thread for as long as possible if it is in need of data.
   Return true if the caller should break to let the buffering thread process
   new queue events */
static bool yield_codec(void)
{
    yield();

    if (!queue_empty(&buffering_queue))
        return true;

    while (pcmbuf_is_lowdata() && !filebuf_is_lowdata())
    {
        sleep(2);

        if (!queue_empty(&buffering_queue))
            return true;
    }

    return false;
}

/* Buffer data for the given handle. Return the amount of data buffered
   or -1 if the handle wasn't found */
static ssize_t buffer_handle(int handle_id)
{
    logf("buffer_handle(%d)", handle_id);
    struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return -1;

    if (h->filerem == 0) {
        /* nothing left to buffer */
        return 0;
    }

    if (h->fd < 0)  /* file closed, reopen */
    {
        if (*h->path)
            h->fd = open(h->path, O_RDONLY);
        else
            return -1;

        if (h->fd < 0)
            return -1;

        if (h->offset)
            lseek(h->fd, h->offset, SEEK_SET);
    }

    trigger_cpu_boost();

    ssize_t ret = 0;
    while (h->filerem > 0)
    {
        /* max amount to copy */
        size_t copy_n = MIN( MIN(h->filerem, conf_filechunk),
                             buffer_len - h->widx);

        /* stop copying if it would overwrite the reading position
           or the next handle */
        if (RINGBUF_ADD_CROSS(h->widx, copy_n, buf_ridx) >= 0 || (h->next &&
            RINGBUF_ADD_CROSS(h->widx, copy_n, (unsigned)
                              ((void *)h->next - (void *)buffer)) > 0))
            break;

        /* rc is the actual amount read */
        int rc = read(h->fd, &buffer[h->widx], copy_n);

        if (rc < 0)
        {
            if (h->type == TYPE_CODEC) {
                logf("Partial codec");
                break;
            }

            DEBUGF("File ended %ld bytes early\n", (long)h->filerem);
            h->filesize -= h->filerem;
            h->filerem = 0;
            break;
        }

        /* Advance buffer */
        h->widx = RINGBUF_ADD(h->widx, rc);
        if (h == cur_handle)
            buf_widx = h->widx;
        h->available += rc;
        ret += rc;
        h->filerem -= rc;

        /* Stop buffering if new queue events have arrived */
        /* FIXME: This may sleep, if it does it will untrigger the
         * cpu boost for this thread.  If the codec's low data
         * situation was very short lived that could leave us filling
         * w/o boost */
        if (yield_codec())
            break;
    }

    if (h->filerem == 0) {
        /* finished buffering the file */
        close(h->fd);
        h->fd = -1;
    }

    return ret;
}

/* Reset writing position and data buffer of a handle to its current offset.
   Use this after having set the new offset to use. */
static void reset_handle(int handle_id)
{
    logf("reset_handle(%d)", handle_id);

    struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return;

    h->widx = h->data;
    if (h == cur_handle)
        buf_widx = h->widx;
    h->available = 0;
    h->filerem = h->filesize - h->offset;

    if (h->fd >= 0) {
        lseek(h->fd, h->offset, SEEK_SET);
    }
}

/* Seek to a nonbuffered part of a handle by rebuffering the data. */
static void rebuffer_handle(int handle_id, size_t newpos)
{
    struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return;

    h->offset = newpos;

    LOGFQUEUE("? >| buffering Q_RESET_HANDLE");
    queue_send(&buffering_queue, Q_RESET_HANDLE, handle_id);

    LOGFQUEUE("? >| buffering Q_BUFFER_HANDLE");
    queue_send(&buffering_queue, Q_BUFFER_HANDLE, handle_id);

    h->ridx = h->data;
}

static bool close_handle(int handle_id)
{
    struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return false;

    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }

    rm_handle(h);
    return true;
}

/* Free buffer space by moving the handle struct right before the useful
   part of its data buffer or by moving all the data. */
static void shrink_handle(int handle_id)
{
    size_t delta;
    struct memory_handle *h = find_handle(handle_id);

    if (!h)
        return;

    if (h->next && (h->type == TYPE_ID3 || h->type == TYPE_CUESHEET ||
                    h->type == TYPE_IMAGE) && h->filerem == 0 )
    {
        /* metadata handle: we can move all of it */
        size_t handle_distance = 
            RINGBUF_SUB((unsigned)((void *)h->next - (void*)buffer), h->data);
        delta = handle_distance - h->available;

        /* The value of delta might change for alignment reasons */
        if (!move_handle(&h, &delta, h->available))
            return;

        size_t olddata = h->data;
        h->data = RINGBUF_ADD(h->data, delta);
        h->ridx = RINGBUF_ADD(h->ridx, delta);
        h->widx = RINGBUF_ADD(h->widx, delta);

        /* when moving a struct mp3entry we need to readjust its pointers. */
        if (h->type == TYPE_ID3 && h->filesize == sizeof(struct mp3entry)) {
            adjust_mp3entry((struct mp3entry *)&buffer[h->data],
                            (void *)&buffer[h->data],
                            (void *)&buffer[olddata]);
        }
    }
    else
    {
        /* only move the handle struct */
        delta = RINGBUF_SUB(h->ridx, h->data);
        if (!move_handle(&h, &delta, 0))
            return;

        h->data = RINGBUF_ADD(h->data, delta);
        h->available -= delta;
        h->offset += delta;
    }
}

/* Fill the buffer by buffering as much data as possible for handles that still
   have data left to buffer */
static void fill_buffer(void)
{
    logf("fill_buffer()");
    struct memory_handle *m = first_handle;
    while (queue_empty(&buffering_queue) && m) {
        if (m->filerem > 0) {
            buffer_handle(m->id);
        }
        m = m->next;
    }

#ifndef SIMULATOR
    if (queue_empty(&buffering_queue)) {
        /* only spin the disk down if the filling wasn't interrupted by an
           event arriving in the queue. */
        ata_sleep();
    }
#endif
}

void update_data_counters(void)
{
    struct memory_handle *m = find_handle(base_handle_id);
    if (!m)
        base_handle_id = 0;

    memset(&data_counters, 0, sizeof(data_counters));

    m = first_handle;
    while (m) {
        data_counters.buffered += m->available;
        data_counters.wasted += RINGBUF_SUB(m->ridx, m->data);
        data_counters.remaining += m->filerem;

        if (m->id >= base_handle_id)
            data_counters.useful += RINGBUF_SUB(m->widx, m->ridx);

        m = m->next;
    }
}


/*
MAIN BUFFERING API CALLS
========================

bufopen     : Request the opening of a new handle for a file
bufalloc    : Open a new handle for data other than a file.
bufclose    : Close an open handle
bufseek     : Set the read pointer in a handle
bufadvance  : Move the read pointer in a handle
bufread     : Copy data from a handle into a given buffer
bufgetdata  : Give a pointer to the handle's data

These functions are exported, to allow interaction with the buffer.
They take care of the content of the structs, and rely on the linked list
management functions for all the actual handle management work.
*/


/* Reserve space in the buffer for a file.
   filename: name of the file to open
   offset: offset at which to start buffering the file, useful when the first
           (offset-1) bytes of the file aren't needed.
   return value: <0 if the file cannot be opened, or one file already
   queued to be opened, otherwise the handle for the file in the buffer
*/
int bufopen(const char *file, size_t offset, enum data_type type)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return ERR_FILE_ERROR;

    size_t size = filesize(fd);

    struct memory_handle *h = add_handle(size-offset, type==TYPE_AUDIO, false);
    if (!h)
    {
        DEBUGF("bufopen: failed to add handle\n");
        close(fd);
        return ERR_BUFFER_FULL;
    }

    strncpy(h->path, file, MAX_PATH);
    h->filesize = size;
    h->filerem = size - offset;
    h->offset = offset;
    h->ridx = buf_widx;
    h->widx = buf_widx;
    h->data = buf_widx;
    h->available = 0;
    h->type = type;

    if (type == TYPE_CODEC || type == TYPE_CUESHEET || type == TYPE_IMAGE) {
        h->fd = fd;
        /* Immediately start buffering those */
        LOGFQUEUE("? >| buffering Q_BUFFER_HANDLE");
        queue_send(&buffering_queue, Q_BUFFER_HANDLE, h->id);
    } else {
        /* Other types will get buffered in the course of normal operations */
        h->fd = -1;
        close(fd);
    }

    logf("bufopen: new hdl %d", h->id);
    return h->id;
}

/* Open a new handle from data that needs to be copied from memory.
   src is the source buffer from which to copy data. It can be NULL to simply
   reserve buffer space.
   size is the requested size. The call will only be successful if the
   requested amount of data can entirely fit in the buffer without wrapping.
   Return value is the handle id for success or <0 for failure.
*/
int bufalloc(const void *src, size_t size, enum data_type type)
{
    struct memory_handle *h = add_handle(size, false, true);

    if (!h)
        return ERR_BUFFER_FULL;

    if (src) {
        if (type == TYPE_ID3 && size == sizeof(struct mp3entry)) {
            /* specially take care of struct mp3entry */
            copy_mp3entry((struct mp3entry *)&buffer[buf_widx],
                          (struct mp3entry *)src);
        } else {
            memcpy(&buffer[buf_widx], src, size);
        }
    }

    h->fd = -1;
    *h->path = 0;
    h->filesize = size;
    h->filerem = 0;
    h->offset = 0;
    h->ridx = buf_widx;
    h->widx = buf_widx + size; /* this is safe because the data doesn't wrap */
    h->data = buf_widx;
    h->available = size;
    h->type = type;

    buf_widx += size;  /* safe too */

    logf("bufalloc: new hdl %d", h->id);
    return h->id;
}

/* Close the handle. Return true for success and false for failure */
bool bufclose(int handle_id)
{
    logf("bufclose(%d)", handle_id);

    LOGFQUEUE("buffering >| Q_CLOSE_HANDLE %d", handle_id);
    return queue_send(&buffering_queue, Q_CLOSE_HANDLE, handle_id);
}

/* Set reading index in handle (relatively to the start of the file).
   Access before the available data will trigger a rebuffer.
   Return 0 for success and < 0 for failure:
     -1 if the handle wasn't found
     -2 if the new requested position was beyond the end of the file
*/
int bufseek(int handle_id, size_t newpos)
{
    struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;

    if (newpos > h->filesize) {
        /* access beyond the end of the file */
        return ERR_INVALID_VALUE;
    }
    else if (newpos < h->offset || h->offset + h->available < newpos) {
        /* access before or after buffered data. A rebuffer is needed. */
        rebuffer_handle(handle_id, newpos);
    }
    else {
        h->ridx = RINGBUF_ADD(h->data, newpos - h->offset);
    }
    return 0;
}

/* Advance the reading index in a handle (relatively to its current position).
   Return 0 for success and < 0 for failure */
int bufadvance(int handle_id, off_t offset)
{
    const struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;

    size_t newpos = h->offset + RINGBUF_SUB(h->ridx, h->data) + offset;
    return bufseek(handle_id, newpos);
}

/* Copy data from the given handle to the dest buffer.
   Return the number of bytes copied or < 0 for failure. */
ssize_t bufread(int handle_id, size_t size, void *dest)
{
    const struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;

    size_t ret;
    size_t copy_n = RINGBUF_SUB(h->widx, h->ridx);

    if (size == 0 && h->filerem > 0 && copy_n == 0)
        /* Data isn't ready */
        return ERR_DATA_NOT_READY;

    if (copy_n < size && h->filerem > 0)
        /* Data isn't ready */
        return ERR_DATA_NOT_READY;

    if (copy_n == 0 && h->filerem == 0)
        /* File is finished reading */
        return 0;

    ret = MIN(size, copy_n);

    if (h->ridx + ret > buffer_len)
    {
        /* the data wraps around the end of the buffer */
        size_t read = buffer_len - h->ridx;
        memcpy(dest, &buffer[h->ridx], read);
        memcpy(dest+read, buffer, ret - read);
    }
    else
    {
        memcpy(dest, &buffer[h->ridx], ret);
    }

    return ret;
}

/* Update the "data" pointer to make the handle's data available to the caller.
   Return the length of the available linear data or < 0 for failure.
   size is the amount of linear data requested. it can be 0 to get as
   much as possible.
   The guard buffer may be used to provide the requested size */
ssize_t bufgetdata(int handle_id, size_t size, void **data)
{
    const struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;

    ssize_t ret;
    size_t copy_n = RINGBUF_SUB(h->widx, h->ridx);

    if (size == 0 && h->filerem > 0 && copy_n == 0)
        /* Data isn't ready */
        return ERR_DATA_NOT_READY;

    if (copy_n < size && h->filerem > 0)
        /* Data isn't ready */
        return ERR_DATA_NOT_READY;

    if (copy_n == 0 && h->filerem == 0)
        /* File is finished reading */
        return 0;

    if (h->ridx + size > buffer_len && copy_n >= size)
    {
        /* the data wraps around the end of the buffer :
           use the guard buffer to provide the requested amount of data. */
        size_t copy_n = MIN(h->ridx + size - buffer_len, GUARD_BUFSIZE);
        memcpy(guard_buffer, (unsigned char *)buffer, copy_n);
        ret = buffer_len - h->ridx + copy_n;
    }
    else
    {
        ret = MIN(copy_n, buffer_len - h->ridx);
    }

    *data = &buffer[h->ridx];
    return ret;
}

/*
SECONDARY EXPORTED FUNCTIONS
============================

buf_get_offset
buf_handle_offset
buf_request_buffer_handle
buf_set_base_handle
buf_used
register_buffer_low_callback
unregister_buffer_low_callback

These functions are exported, to allow interaction with the buffer.
They take care of the content of the structs, and rely on the linked list
management functions for all the actual handle management work.
*/

/* Get a handle offset from a pointer */
ssize_t buf_get_offset(int handle_id, void *ptr)
{
    const struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;

    return (size_t)ptr - (size_t)&buffer[h->ridx];
}

ssize_t buf_handle_offset(int handle_id)
{
    const struct memory_handle *h = find_handle(handle_id);
    if (!h)
        return ERR_HANDLE_NOT_FOUND;
    return h->offset;
}

void buf_request_buffer_handle(int handle_id)
{
    LOGFQUEUE("buffering >| buffering Q_BUFFER_HANDLE %d", handle_id);
    queue_send(&buffering_queue, Q_BUFFER_HANDLE, handle_id);
}

void buf_set_base_handle(int handle_id)
{
    LOGFQUEUE("buffering > buffering Q_BASE_HANDLE %d", handle_id);
    queue_post(&buffering_queue, Q_BASE_HANDLE, handle_id);
}

/* Return the amount of buffer space used */
size_t buf_used(void)
{
    return BUF_USED;
}

void buf_set_conf(int setting, size_t value)
{
    int msg;
    switch (setting)
    {
        case BUFFERING_SET_WATERMARK:
            msg = Q_SET_WATERMARK;
            break;

        case BUFFERING_SET_CHUNKSIZE:
            msg = Q_SET_CHUNKSIZE;
            break;

        case BUFFERING_SET_PRESEEK:
            msg = Q_SET_PRESEEK;
            break;

        default:
            return;
    }
    queue_post(&buffering_queue, msg, value);
}

bool register_buffer_low_callback(buffer_low_callback func)
{
    int i;
    if (buffer_callback_count >= MAX_BUF_CALLBACKS)
        return false;
    for (i = 0; i < MAX_BUF_CALLBACKS; i++)
    {
        if (buffer_low_callback_funcs[i] == NULL)
        {
            buffer_low_callback_funcs[i] = func;
            buffer_callback_count++;
            return true;
        }
        else if (buffer_low_callback_funcs[i] == func)
            return true;
    }
    return false;
}

void unregister_buffer_low_callback(buffer_low_callback func)
{
    int i;
    for (i = 0; i < MAX_BUF_CALLBACKS; i++)
    {
        if (buffer_low_callback_funcs[i] == func)
        {
            buffer_low_callback_funcs[i] = NULL;
            buffer_callback_count--;
        }
    }
    return;
}

static void call_buffer_low_callbacks(void)
{
    int i;
    for (i = 0; i < MAX_BUF_CALLBACKS; i++)
    {
        if (buffer_low_callback_funcs[i])
        {
            buffer_low_callback_funcs[i]();
            buffer_low_callback_funcs[i] = NULL;
            buffer_callback_count--;
        }
    }
}

static void shrink_buffer(bool audio, bool other) {
    /* shrink selected buffers */
    struct memory_handle *m = first_handle;
    while (m) {
        if ((m->type==TYPE_AUDIO && audio) || (m->type!=TYPE_AUDIO && other))
            shrink_handle(m->id);
        m = m->next;
    }
}

void buffering_thread(void)
{
    struct queue_event ev;

    while (true)
    {
        queue_wait_w_tmo(&buffering_queue, &ev, HZ/2);

        switch (ev.id)
        {
            case Q_BUFFER_HANDLE:
                LOGFQUEUE("buffering < Q_BUFFER_HANDLE");
                queue_reply(&buffering_queue, 1);
                buffer_handle((int)ev.data);
                break;

            case Q_RESET_HANDLE:
                LOGFQUEUE("buffering < Q_RESET_HANDLE");
                queue_reply(&buffering_queue, 1);
                reset_handle((int)ev.data);
                break;

            case Q_CLOSE_HANDLE:
                LOGFQUEUE("buffering < Q_CLOSE_HANDLE");
                queue_reply(&buffering_queue, close_handle((int)ev.data));
                break;

            case Q_BASE_HANDLE:
                LOGFQUEUE("buffering < Q_BASE_HANDLE");
                base_handle_id = (int)ev.data;
                break;

            case Q_SET_WATERMARK:
                LOGFQUEUE("buffering < Q_SET_WATERMARK");
                conf_watermark = (size_t)ev.data;
                break;

            case Q_SET_CHUNKSIZE:
                LOGFQUEUE("buffering < Q_SET_CHUNKSIZE");
                conf_filechunk = (size_t)ev.data;
                break;

            case Q_SET_PRESEEK:
                LOGFQUEUE("buffering < Q_SET_PRESEEK");
                conf_preseek = (size_t)ev.data;
                break;

#ifndef SIMULATOR
            case SYS_USB_CONNECTED:
                LOGFQUEUE("buffering < SYS_USB_CONNECTED");
                usb_acknowledge(SYS_USB_CONNECTED_ACK);
                usb_wait_for_disconnect(&buffering_queue);
                break;
#endif

            case SYS_TIMEOUT:
                LOGFQUEUE_SYS_TIMEOUT("buffering < SYS_TIMEOUT");
                break;
        }

        update_data_counters();

        /* If the buffer is low, call the callbacks to get new data */
        if (num_handles > 0 && data_counters.useful < conf_watermark)
        {
            call_buffer_low_callbacks();
        }

#if MEM > 8
        /* If the disk is spinning, take advantage by filling the buffer */
        if (ata_disk_is_active() && queue_empty(&buffering_queue) &&
            data_counters.remaining > 0 &&
            data_counters.buffered < high_watermark)
        {
            fill_buffer();
            update_data_counters();
        }

        if (ata_disk_is_active() && queue_empty(&buffering_queue) &&
            num_handles > 0 && data_counters.useful < high_watermark)
        {
            call_buffer_low_callbacks();
        }
#endif

        if (ev.id == SYS_TIMEOUT && queue_empty(&buffering_queue))
        {
            if (data_counters.remaining > 0 &&
                data_counters.wasted > data_counters.buffered/2)
            {
                /* First work forward, shrinking any unmoveable handles */
                shrink_buffer(true,false);
                /* Then work forward following those up with moveable handles */
                shrink_buffer(false,true);
                update_data_counters();
            }

            if (data_counters.remaining > 0 &&
                data_counters.buffered < conf_watermark)
            {
                fill_buffer();
            }
        }
    }
}

void buffering_init(void) {
    mutex_init(&llist_mutex);

    conf_filechunk = BUFFERING_DEFAULT_FILECHUNK;
    conf_watermark = BUFFERING_DEFAULT_WATERMARK;

    queue_init(&buffering_queue, true);
    queue_enable_queue_send(&buffering_queue, &buffering_queue_sender_list);

    buffering_thread_p = create_thread( buffering_thread, buffering_stack,
            sizeof(buffering_stack), CREATE_THREAD_FROZEN,
            buffering_thread_name IF_PRIO(, PRIORITY_BUFFERING)
            IF_COP(, CPU));
}

/* Initialise the buffering subsystem */
bool buffering_reset(char *buf, size_t buflen)
{
    if (!buf || !buflen)
        return false;

    buffer = buf;
    buffer_len = buflen;
    guard_buffer = buf + buflen;

    buf_widx = 0;
    buf_ridx = 0;

    first_handle = NULL;
    cur_handle = NULL;
    cached_handle = NULL;
    num_handles = 0;
    base_handle_id = 0;

    buffer_callback_count = 0;
    memset(buffer_low_callback_funcs, 0, sizeof(buffer_low_callback_funcs));

    /* Set the high watermark as 75% full...or 25% empty :) */
#if MEM > 8
    high_watermark = 3*buflen / 4;
#endif

    thread_thaw(buffering_thread_p);

    return true;
}

void buffering_get_debugdata(struct buffering_debug *dbgdata)
{
    update_data_counters();
    dbgdata->num_handles = num_handles;
    dbgdata->data_rem = data_counters.remaining;
    dbgdata->wasted_space = data_counters.wasted;
    dbgdata->buffered_data = data_counters.buffered;
    dbgdata->useful_data = data_counters.useful;
}

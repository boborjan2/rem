/**
 * @file aubuf.c  Audio Buffer
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem_au.h>
#include <rem_aulevel.h>
#include <rem_auframe.h>
#include <rem_aubuf.h>
#include "ajb.h"


#define AUBUF_DEBUG 0
#define AUDIO_TIMEBASE 1000000U


/** Locked audio-buffer with almost zero-copy */
struct aubuf {
	struct list afl;
	struct lock *lock;
	size_t wish_sz;
	size_t cur_sz;
	size_t max_sz;
	bool filling;
	bool started;
	uint64_t ts;

#if AUBUF_DEBUG
	struct {
		size_t or;
		size_t ur;
	} stats;
#endif
	enum aubuf_mode mode;
	struct ajb *ajb;         /**< Adaptive jitter buffer statistics      */
	double silence;          /**< Silence volume in negative [dB]        */
};


struct frame {
	struct le le;
	struct mbuf *mb;
	struct auframe af;
};


static void frame_destructor(void *arg)
{
	struct frame *f = arg;

	list_unlink(&f->le);
	mem_deref(f->mb);
}


static void aubuf_destructor(void *arg)
{
	struct aubuf *ab = arg;

	list_flush(&ab->afl);
	mem_deref(ab->lock);
	mem_deref(ab->ajb);
}


static void read_auframe(struct aubuf *ab, struct auframe *af)
{
	struct le *le = ab->afl.head;
	size_t sample_size = aufmt_sample_size(af->fmt);
	size_t sz = auframe_size(af);
	uint8_t *p = af->sampv;

	while (le) {
		struct frame *f = le->data;
		size_t n;

		le = le->next;

		n = min(mbuf_get_left(f->mb), sz);

		(void)mbuf_read_mem(f->mb, p, n);
		ab->cur_sz -= n;

		af->srate     = f->af.srate;
		af->ch	      = f->af.ch;
		af->timestamp = f->af.timestamp;

		if (!mbuf_get_left(f->mb)) {
			mem_deref(f);
		}
		else if (af->srate && af->ch && sample_size) {
			f->af.timestamp += n * AUDIO_TIMEBASE /
				(af->srate * af->ch * sample_size);
		}

		if (n == sz)
			break;

		p  += n;
		sz -= n;
	}
}


/**
 * Allocate a new audio buffer
 *
 * @param abp    Pointer to allocated audio buffer
 * @param min_sz Minimum buffer size
 * @param max_sz Maximum buffer size (0 for no max size)
 *
 * @return 0 for success, otherwise error code
 */
int aubuf_alloc(struct aubuf **abp, size_t min_sz, size_t max_sz)
{
	struct aubuf *ab;
	int err;

	if (!abp || !min_sz)
		return EINVAL;

	ab = mem_zalloc(sizeof(*ab), aubuf_destructor);
	if (!ab)
		return ENOMEM;

	err = lock_alloc(&ab->lock);
	if (err)
		goto out;

	ab->wish_sz = min_sz;
	ab->max_sz = max_sz;
	ab->filling = true;

 out:
	if (err)
		mem_deref(ab);
	else
		*abp = ab;

	return err;
}


void aubuf_set_mode(struct aubuf *ab, enum aubuf_mode mode)
{
	if (!ab)
		return;

	ab->mode = mode;
}


/**
 * Sets the volume level for silence
 *
 * @param ab       Audio buffer
 * @param silence  Volume level in negative [dB]
 */
void aubuf_set_silence(struct aubuf *ab, double silence)
{
	if (!ab)
		return;

	ab->silence = silence;
}


/**
 * Resize audio buffer (flushes aubuf)
 *
 * @param ab     Audio buffer
 * @param min_sz Minimum buffer size
 * @param max_sz Maximum buffer size (0 for no max size)
 *
 * @return 0 for success, otherwise error code
 */
int aubuf_resize(struct aubuf *ab, size_t min_sz, size_t max_sz)
{
	if (!ab || !min_sz)
		return EINVAL;

	lock_write_get(ab->lock);
	ab->wish_sz = min_sz;
	ab->max_sz  = max_sz;
	lock_rel(ab->lock);

	aubuf_flush(ab);

	return 0;
}


static bool frame_less_equal(struct le *le1, struct le *le2, void *arg)
{
	struct frame *frame1 = le1->data;
	struct frame *frame2 = le2->data;
	(void)arg;

	return frame1->af.timestamp <= frame2->af.timestamp;
}


/**
 * Append a PCM-buffer to the end of the audio buffer
 *
 * @param ab Audio buffer
 * @param mb Mbuffer with PCM samples
 * @param af Audio frame (optional)
 *
 * @return 0 for success, otherwise error code
 */
int aubuf_append_auframe(struct aubuf *ab, struct mbuf *mb, struct auframe *af)
{
	struct frame *f;
	size_t max_sz;

	if (!ab || !mb)
		return EINVAL;

	f = mem_zalloc(sizeof(*f), frame_destructor);
	if (!f)
		return ENOMEM;

	f->mb = mem_ref(mb);
	if (af)
		f->af = *af;

	lock_write_get(ab->lock);

	list_insert_sorted(&ab->afl, frame_less_equal, NULL, &f->le, f);
	ab->cur_sz += mbuf_get_left(mb);

	max_sz = ab->started ? ab->max_sz : ab->wish_sz + 1;
	if (ab->max_sz && ab->cur_sz > max_sz) {
#if AUBUF_DEBUG
		if (ab->started) {
			++ab->stats.or;
			(void)re_printf("aubuf: %p overrun (cur=%zu/%zu)\n",
					ab, ab->cur_sz, ab->max_sz);
		}
#endif
		f = list_ledata(ab->afl.head);
		if (f) {
			ab->cur_sz -= mbuf_get_left(f->mb);
			mem_deref(f);
		}
	}

	if (ab->filling && ab->cur_sz >= ab->wish_sz)
		ab->filling = false;

	lock_rel(ab->lock);

	return 0;
}


/**
 * Write PCM samples to the audio buffer
 *
 * @param ab Audio buffer
 * @param af Audio frame
 *
 * @return 0 for success, otherwise error code
 */
int aubuf_write_auframe(struct aubuf *ab, struct auframe *af)
{
	struct mbuf *mb;
	size_t sz;
	size_t sample_size;
	int err;

	if (!ab || !af)
		return EINVAL;
	sample_size = aufmt_sample_size(af->fmt);
	if (sample_size)
		sz = af->sampc * aufmt_sample_size(af->fmt);
	else
		sz = af->sampc;

	mb = mbuf_alloc(sz);

	if (!mb)
		return ENOMEM;

	(void)mbuf_write_mem(mb, af->sampv, sz);
	mb->pos = 0;

	err = aubuf_append_auframe(ab, mb, af);

	lock_write_get(ab->lock);
	mem_deref(mb);
	lock_rel(ab->lock);

	if (!ab->filling && ab->ajb)
		ajb_calc(ab->ajb, af, ab->cur_sz);

	return err;
}


/**
 * Read PCM samples from the audio buffer. If there is not enough data
 * in the audio buffer, silence will be read.
 *
 * @param ab Audio buffer
 * @param af Audio frame (af.sampv, af.sampc and af.fmt needed)
 */
void aubuf_read_auframe(struct aubuf *ab, struct auframe *af)
{
	size_t sz;
	bool filling;
	enum ajb_state as;

	if (!ab || !af)
		return;

	if (!ab->ajb && ab->mode == AUBUF_ADAPTIVE)
		ab->ajb = ajb_alloc(ab->silence);

	lock_write_get(ab->lock);
	as = ajb_get(ab->ajb, af);
	if (as == AJB_LOW) {
#if AUBUF_DEBUG
		(void)re_printf("aubuf: inc buffer due to high jitter\n");
		ajb_debug(ab->ajb);
#endif
		goto out;
	}

	sz = auframe_size(af);
	if (ab->cur_sz < (ab->filling ? ab->wish_sz : sz)) {
#if AUBUF_DEBUG
		if (!ab->filling) {
			++ab->stats.ur;
			(void)re_printf("aubuf: %p underrun (cur=%zu)\n",
					ab, ab->cur_sz);
			plot_underrun(ab->ajb);
		}
#endif
		if (!ab->filling)
			ajb_reset(ab->ajb);

		filling = ab->filling;
		ab->filling = true;
		memset(af->sampv, 0, sz);
		if (filling)
			goto out;
	}

	ab->started = true;
	read_auframe(ab, af);
	if (as == AJB_HIGH) {
#if AUBUF_DEBUG
		(void)re_printf("aubuf: drop a frame to reduce latency\n");
		ajb_debug(ab->ajb);
#endif
		read_auframe(ab, af);
	}

 out:
	lock_rel(ab->lock);
}


/**
 * Timed read PCM samples from the audio buffer. If there is not enough data
 * in the audio buffer, silence will be read.
 *
 * @param ab    Audio buffer
 * @param ptime Packet time in [ms]
 * @param p     Buffer where PCM samples are read into
 * @param sz    Number of bytes to read
 *
 * @note This does the same as aubuf_read() except that it also takes
 *       timing into consideration.
 *
 * @return 0 if valid PCM was read, ETIMEDOUT if no PCM is ready yet
 */
int aubuf_get(struct aubuf *ab, uint32_t ptime, uint8_t *p, size_t sz)
{
	uint64_t now;
	int err = 0;

	if (!ab || !ptime)
		return EINVAL;

	lock_write_get(ab->lock);

	now = tmr_jiffies();
	if (!ab->ts)
		ab->ts = now;

	if (now < ab->ts) {
		err = ETIMEDOUT;
		goto out;
	}

	ab->ts += ptime;

 out:
	lock_rel(ab->lock);

	if (!err)
		aubuf_read(ab, p, sz);

	return err;
}


/**
 * Flush the audio buffer
 *
 * @param ab Audio buffer
 */
void aubuf_flush(struct aubuf *ab)
{
	if (!ab)
		return;

	lock_write_get(ab->lock);

	list_flush(&ab->afl);
	ab->filling = true;
	ab->cur_sz  = 0;
	ab->ts      = 0;

	lock_rel(ab->lock);
	ajb_reset(ab->ajb);
}


/**
 * Audio buffer debug handler, use with fmt %H
 *
 * @param pf Print function
 * @param ab Audio buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int aubuf_debug(struct re_printf *pf, const struct aubuf *ab)
{
	int err;

	if (!ab)
		return 0;

	lock_read_get(ab->lock);
	err = re_hprintf(pf, "wish_sz=%zu cur_sz=%zu filling=%d",
			 ab->wish_sz, ab->cur_sz, ab->filling);

#if AUBUF_DEBUG
	err |= re_hprintf(pf, " [overrun=%zu underrun=%zu]",
			  ab->stats.or, ab->stats.ur);
#endif

	lock_rel(ab->lock);

	return err;
}


/**
 * Get the current number of bytes in the audio buffer
 *
 * @param ab Audio buffer
 *
 * @return Number of bytes in the audio buffer
 */
size_t aubuf_cur_size(const struct aubuf *ab)
{
	size_t sz;

	if (!ab)
		return 0;

	lock_read_get(ab->lock);
	sz = ab->cur_sz;
	lock_rel(ab->lock);

	return sz;
}


/**
 * Reorder aubuf by auframe->timestamp
 *
 * @param ab Audio buffer
 */
void aubuf_sort_auframe(struct aubuf *ab)
{
	if (!ab)
		return;

	list_sort(&ab->afl, frame_less_equal, NULL);
}


void aubuf_drop_auframe(struct aubuf *ab, struct auframe *af)
{
	if (!ab)
		return;

	ajb_drop(ab->ajb, af);
}

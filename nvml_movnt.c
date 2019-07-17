/*
 * Copyright 2014-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Some changes and fixes made by:
 *	Boaz Harrosh <boazh@netapp.com>
 */

#include <cpuid.h>

#include "movnt.h"

#define EAX_IDX 0
#define EBX_IDX 1
#define ECX_IDX 2
#define EDX_IDX 3

#define CLFLUSHOPT_FUNC		0x7
#define CLFLUSHOPT_BIT		(1 << 23)

#define CLWB_FUNC		0x7
#define CLWB_BIT		(1 << 24)

#define CACHELINE_ALIGN ((uintptr_t)64)
#define CACHELINE_MASK	(CACHELINE_ALIGN - 1)

#define	CHUNK_SIZE	128 /* 16*8 */
#define	CHUNK_SHIFT	7
#define	CHUNK_MASK	(CHUNK_SIZE - 1)

#define	DWORD_SIZE	4
#define	DWORD_SHIFT	2
#define	DWORD_MASK	(DWORD_SIZE - 1)

#define	MOVNT_SIZE	16
#define	MOVNT_MASK	(MOVNT_SIZE - 1)
#define	MOVNT_SHIFT	4

#define	MOVNT_THRESHOLD	256

/*
 * flush_clflush -- (internal) flush the CPU cache, using clflush
 * (Boaz: Is only used here for the none aligned tails of movnt, clflush
 *  Is always better than clflushopt in this case, even if clflushopt is
 *  available)
 */
static void
flush_clflush(const void *addr, size_t len)
{
	uintptr_t uptr;

	/*
	 * Loop through cache-line-size (typically 64B) aligned chunks
	 * covering the given range.
	 */
	for (uptr = (uintptr_t)addr & ~(CACHELINE_ALIGN - 1);
		uptr < (uintptr_t)addr + len; uptr += CACHELINE_ALIGN)
		_mm_clflush((char *)uptr);
}

static void
pmem_flush(const void *addr, size_t len)
{
	flush_clflush(addr, len);
}

/*
 * memmove_nodrain_movnt -- (internal) memmove to pmem without hw drain, movnt
 */
static void *
memmove_nodrain_movnt(void *pmemdest, const void *src, size_t len)
{
	__m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
	size_t i;
	__m128i *d;
	const __m128i *s;
	void *dest1 = pmemdest;
	size_t cnt;

	if (len == 0 || src == pmemdest)
		return pmemdest;

	if (len < MOVNT_THRESHOLD) {
		memmove(pmemdest, src, len);
		pmem_flush(pmemdest, len);
		return pmemdest;
	}

	if ((uintptr_t)dest1 - (uintptr_t)src >= len) {
		/*
		 * Copy the range in the forward direction.
		 *
		 * This is the most common, most optimized case, used unless
		 * the overlap specifically prevents it.
		 */

		/* copy up to CACHELINE_ALIGN boundary */
		cnt = (uint64_t)dest1 & CACHELINE_MASK;
		if (cnt > 0) {
			uint8_t *d8;
			const uint8_t *s8;

			cnt = CACHELINE_ALIGN - cnt;

			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			d8 = dest1;
			s8 = src;
			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(dest1, cnt);
			dest1 += cnt;
			src += cnt;
			len -= cnt;
		}

		d = dest1;
		s = src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s);
			xmm1 = _mm_loadu_si128(s + 1);
			xmm2 = _mm_loadu_si128(s + 2);
			xmm3 = _mm_loadu_si128(s + 3);
			xmm4 = _mm_loadu_si128(s + 4);
			xmm5 = _mm_loadu_si128(s + 5);
			xmm6 = _mm_loadu_si128(s + 6);
			xmm7 = _mm_loadu_si128(s + 7);
			s += 8;
			_mm_stream_si128(d,	xmm0);
			_mm_stream_si128(d + 1,	xmm1);
			_mm_stream_si128(d + 2,	xmm2);
			_mm_stream_si128(d + 3,	xmm3);
			_mm_stream_si128(d + 4,	xmm4);
			_mm_stream_si128(d + 5, xmm5);
			_mm_stream_si128(d + 6,	xmm6);
			_mm_stream_si128(d + 7,	xmm7);
			d += 8;
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
				s++;
				d++;
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			int32_t *d32 = (int32_t *)d;
			const int32_t *s32 = (const int32_t *)s;
			uint8_t *d8;
			const uint8_t *s8;

			cnt = len >> DWORD_SHIFT;
			for (i = 0; i < cnt; i++) {
				_mm_stream_si32(d32, *s32);
				d32++;
				s32++;
			}
			cnt = len & DWORD_MASK;
			d8 = (uint8_t *)d32;
			s8 = (const uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(d32, cnt);
		}
	} else {
		/*
		 * Copy the range in the backward direction.
		 *
		 * This prevents overwriting source data due to an
		 * overlapped destination range.
		 */

		dest1 += len;
		src += len;

		cnt = (uint64_t)dest1 & CACHELINE_MASK;
		if (cnt > 0) {
			uint8_t *d8;
			const uint8_t *s8;

			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			d8 = dest1;
			s8 = src;
			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
			dest1 = (char *)dest1 - cnt;
			src = (const char *)src - cnt;
			len -= cnt;
		}

		d = (__m128i *)dest1;
		s = (const __m128i *)src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s - 1);
			xmm1 = _mm_loadu_si128(s - 2);
			xmm2 = _mm_loadu_si128(s - 3);
			xmm3 = _mm_loadu_si128(s - 4);
			xmm4 = _mm_loadu_si128(s - 5);
			xmm5 = _mm_loadu_si128(s - 6);
			xmm6 = _mm_loadu_si128(s - 7);
			xmm7 = _mm_loadu_si128(s - 8);
			s -= 8;
			_mm_stream_si128(d - 1, xmm0);
			_mm_stream_si128(d - 2, xmm1);
			_mm_stream_si128(d - 3, xmm2);
			_mm_stream_si128(d - 4, xmm3);
			_mm_stream_si128(d - 5, xmm4);
			_mm_stream_si128(d - 6, xmm5);
			_mm_stream_si128(d - 7, xmm6);
			_mm_stream_si128(d - 8, xmm7);
			d -= 8;
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				d--;
				s--;
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			int32_t *d32 = (int32_t *)d;
			const int32_t *s32 = (const int32_t *)s;
			uint8_t *d8;
			const uint8_t *s8;

			cnt = len >> DWORD_SHIFT;

			for (i = 0; i < cnt; i++) {
				d32--;
				s32--;
				_mm_stream_si32(d32, *s32);
			}

			cnt = len & DWORD_MASK;
			d8 = (uint8_t *)d32;
			s8 = (const uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
		}
	}

	/* serialize non-temporal store instructions */
	_mm_sfence();

	return pmemdest;
}

/*
 * pmem_memmove_persist -- memmove to pmem
 */
void *
pmem_memmove_persist(void *pmemdest, const void *src, size_t len)
{
	memmove_nodrain_movnt(pmemdest, src, len);

	return pmemdest;
}

static inline void
cpuid(unsigned func, unsigned subfunc, unsigned cpuinfo[4])
{
	__cpuid_count(func, subfunc, cpuinfo[EAX_IDX], cpuinfo[EBX_IDX],
		      cpuinfo[ECX_IDX], cpuinfo[EDX_IDX]);
}

static int cpuid_check(unsigned func, unsigned reg, unsigned bit)
{
	unsigned int cpuinfo[4] = {};

	/* func check */
	cpuid(0x0, 0x0, cpuinfo);
	if (cpuinfo[EAX_IDX] < func)
		return 0;

	cpuid(func, 0x0, cpuinfo);

	return (cpuinfo[reg] & bit) != 0;
}

static int clflushopt_avail(void)
{
	return cpuid_check(CLFLUSHOPT_FUNC, EBX_IDX, CLFLUSHOPT_BIT);
}

static int clwb_avail(void)
{
	return cpuid_check(CLWB_FUNC, EBX_IDX, CLWB_BIT);
}

/* Old processors don't support clflushopt/clwb, so we default to clflush */
void (*cl_flush_opt)(void *buf, uint32_t len) = cl_flush;
void (*cl_flush_wb)(void *buf, uint32_t len) = cl_flush;

__attribute__((constructor))
static void clflush_init(void)
{
	if (clwb_avail()) {
		cl_flush_wb =  __cl_flush_wb;
		cl_flush_opt =  __cl_flush_opt;
	} else if (clflushopt_avail()) {
		cl_flush_wb =  __cl_flush_opt;
		cl_flush_opt = __cl_flush_opt;
	}
}

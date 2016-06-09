/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "os/internal.h"

typedef struct _os_once_waiter_s {
	volatile struct _os_once_waiter_s *volatile dow_next;
	os_semaphore_t dow_sema;
} *_os_once_waiter_t;

#define OS_ONCE_INIT ((_os_once_waiter_t)0l)
#define OS_ONCE_DONE ((_os_once_waiter_t)~0l)

// Atomically resets the once value to zero and then signals all
// pending waiters to return from their semaphore_wait.
void
__os_once_reset(os_once_t *val)
{
	_os_once_waiter_t volatile *vval = (_os_once_waiter_t*)val;
	_os_once_waiter_t next, tmp;
	os_semaphore_t sema;

	os_atomic_maximally_synchronizing_barrier();
	// above assumed to contain release barrier
	next = os_atomic_xchg(vval, OS_ONCE_INIT, relaxed);
	while (next) {
		// While the _os_once implementation has the benefit of knowing
		// when it reaches the end by comparing the list entry with their
		// own entry, we cannot do that here. However, tmp->dow_sema is known
		// to be good because of the store barrier, if it is zero we have
		// reached the end of the list and can break.
		if (!(sema = next->dow_sema)) {
			break;
		}
		_os_wait_until(tmp = (_os_once_waiter_t)next->dow_next);
		next = tmp;
		os_semaphore_signal(sema);
	}
}

void
_os_once(os_once_t *val, void *ctxt, os_function_t func)
{
	_os_once_waiter_t volatile *vval = (_os_once_waiter_t*)val;
	struct _os_once_waiter_s dow = { NULL, 0 };
	_os_once_waiter_t tail = &dow, next, tmp;
	os_semaphore_t sema;

	if (os_atomic_cmpxchg(vval, OS_ONCE_INIT, tail, acquire)) {
		func(ctxt);

		// The next barrier must be long and strong.
		//
		// The scenario: SMP systems with weakly ordered memory models
		// and aggressive out-of-order instruction execution.
		//
		// The problem:
		//
		// The os_once*() wrapper macro causes the callee's
		// instruction stream to look like this (pseudo-RISC):
		//
		//      load r5, pred-addr
		//      cmpi r5, -1
		//      beq  1f
		//      call os_once*()
		//      1f:
		//      load r6, data-addr
		//
		// May be re-ordered like so:
		//
		//      load r6, data-addr
		//      load r5, pred-addr
		//      cmpi r5, -1
		//      beq  1f
		//      call os_once*()
		//      1f:
		//
		// Normally, a barrier on the read side is used to workaround
		// the weakly ordered memory model. But barriers are expensive
		// and we only need to synchronize once! After func(ctxt)
		// completes, the predicate will be marked as "done" and the
		// branch predictor will correctly skip the call to
		// os_once*().
		//
		// A far faster alternative solution: Defeat the speculative
		// read-ahead of peer CPUs.
		//
		// Modern architectures will throw away speculative results
		// once a branch mis-prediction occurs. Therefore, if we can
		// ensure that the predicate is not marked as being complete
		// until long after the last store by func(ctxt), then we have
		// defeated the read-ahead of peer CPUs.
		//
		// In other words, the last "store" by func(ctxt) must complete
		// and then N cycles must elapse before ~0l is stored to *val.
		// The value of N is whatever is sufficient to defeat the
		// read-ahead mechanism of peer CPUs.
		//
		// On some CPUs, the most fully synchronizing instruction might
		// need to be issued.

		os_atomic_maximally_synchronizing_barrier();
		// above assumed to contain release barrier
		next = os_atomic_xchg(vval, OS_ONCE_DONE, relaxed);
		while (next != tail) {
			_os_wait_until(tmp = (_os_once_waiter_t)next->dow_next);
			sema = next->dow_sema;
			next = tmp;
			os_semaphore_signal(sema);
		}
	} else {
		dow.dow_sema = os_get_cached_semaphore();
		next = *vval;
		for (;;) {
			if (next == OS_ONCE_DONE) {
				break;
			}
			if (os_atomic_cmpxchgvw(vval, next, tail, &next, release)) {
				dow.dow_next = next;
				os_semaphore_wait(dow.dow_sema);
				break;
			}
		}
		os_put_cached_semaphore(dow.dow_sema);
	}
}

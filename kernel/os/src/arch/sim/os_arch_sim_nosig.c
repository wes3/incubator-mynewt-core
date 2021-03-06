/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * This file implements the "no-signals" version of sim.  This implementation
 * does not use signals to perform context switches.  This is the less correct
 * version of sim: the OS tick timer only runs while the idle task is active.
 * Therefore, a sleeping high-priority task will not preempt a low-priority
 * task due to a timing event (e.g., delay or callout expired).  However, this
 * version of sim does not suffer from the stability issues that affect the
 * "signals" implementation.
 *
 * To use this version of sim, disable the MCU_NATIVE_USE_SIGNALS syscfg
 * setting.  
 */

#include "syscfg/syscfg.h"

#if !MYNEWT_VAL(MCU_NATIVE_USE_SIGNALS)

#include "os/os.h"
#include "os_priv.h"

#include <hal/hal_bsp.h>

#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <assert.h>
#include "os_arch_sim_priv.h"

static sigset_t nosigs;
static sigset_t suspsigs;   /* signals delivered in sigsuspend() */

static int ctx_sw_pending;
static int interrupts_enabled = 1;

void
os_arch_ctx_sw(struct os_task *next_t)
{
    if (interrupts_enabled) {
        /* Perform the context switch immediately. */
        os_arch_sim_ctx_sw();
    } else {
        /* Remember that we want to perform a context switch.  Perform it when
         * interrupts are re-enabled.
         */
        ctx_sw_pending = 1;
    }
}

/*
 * Enter a critical section.
 *
 * Returns 1 if interrupts were already disabled; 0 otherwise.
 */
os_sr_t
os_arch_save_sr(void)
{
    if (!interrupts_enabled) {
        return 1;
    }

    interrupts_enabled = 0;
    return 0;
}

void
os_arch_restore_sr(os_sr_t osr)
{
    OS_ASSERT_CRITICAL();
    assert(osr == 0 || osr == 1);

    if (osr == 1) {
        /* Exiting a nested critical section */
        return;
    }

    if (ctx_sw_pending) {
        /* A context switch was requested while interrupts were disabled.
         * Perform it now that interrupts are enabled again.
         */
        ctx_sw_pending = 0;
        os_arch_sim_ctx_sw();
    }
    interrupts_enabled = 1;
}

int
os_arch_in_critical(void)
{
    return !interrupts_enabled;
}

/**
 * Unblocks the SIGALRM signal that is delivered by the OS tick timer.
 */
static void
unblock_timer(void)
{
    sigset_t sigs;
    int rc;

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGALRM);

    rc = sigprocmask(SIG_UNBLOCK, &sigs, NULL);
    assert(rc == 0);
}

/**
 * Blocks the SIGALRM signal that is delivered by the OS tick timer.
 */
static void
block_timer(void)
{
    sigset_t sigs;
    int rc;

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGALRM);

    rc = sigprocmask(SIG_BLOCK, &sigs, NULL);
    assert(rc == 0);
}

static void
sig_handler_alrm(int sig)
{
    /* Wake the idle task. */
    sigaddset(&suspsigs, sig);
}

void
os_tick_idle(os_time_t ticks)
{
    int rc;
    struct itimerval it;

    OS_ASSERT_CRITICAL();

    if (ticks > 0) {
        /*
         * Enter tickless regime and set the timer to fire after 'ticks'
         * worth of time has elapsed.
         */
        it.it_value.tv_sec = ticks / OS_TICKS_PER_SEC;
        it.it_value.tv_usec = (ticks % OS_TICKS_PER_SEC) * OS_USEC_PER_TICK;
        it.it_interval.tv_sec = 0;
        it.it_interval.tv_usec = OS_USEC_PER_TICK;
        rc = setitimer(ITIMER_REAL, &it, NULL);
        assert(rc == 0);
    }

    unblock_timer();

    sigemptyset(&suspsigs);
    sigsuspend(&nosigs);        /* Wait for a signal to wake us up */

    block_timer();

    /*
     * Call handlers for signals delivered to the process during sigsuspend().
     * The SIGALRM handler is called before any other handlers to ensure that
     * OS time is always correct.
     */
    if (sigismember(&suspsigs, SIGALRM)) {
        os_arch_sim_tick();
    }

    if (ticks > 0) {
        /*
         * Enable the periodic timer interrupt.
         */
        it.it_value.tv_sec = 0;
        it.it_value.tv_usec = OS_USEC_PER_TICK;
        it.it_interval.tv_sec = 0;
        it.it_interval.tv_usec = OS_USEC_PER_TICK;
        rc = setitimer(ITIMER_REAL, &it, NULL);
        assert(rc == 0);
    }
}

void
os_arch_sim_signals_init(void)
{
    sigset_t sigset_alrm;
    struct sigaction sa;
    int error;

    block_timer();

    sigemptyset(&nosigs);

    sigemptyset(&sigset_alrm);
    sigaddset(&sigset_alrm, SIGALRM);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler_alrm;
    sa.sa_mask = sigset_alrm;
    sa.sa_flags = SA_RESTART;
    error = sigaction(SIGALRM, &sa, NULL);
    assert(error == 0);
}

void
os_arch_sim_signals_cleanup(void)
{
    int error;
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    error = sigaction(SIGALRM, &sa, NULL);
    assert(error == 0);
}

#endif /* !MYNEWT_VAL(MCU_NATIVE_USE_SIGNALS) */

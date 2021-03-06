#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/queue.h>

#include <event2/event.h>
#include <event2/util.h>
#include <event2/bufferevent.h>

#include "token_bucket.h"
#include "evratelim.h"

struct evratelim_bev_s {
    struct bufferevent * bev;
    evratelim_group    * group;

    evratelim_cb suspend_cb;
    evratelim_cb resume_cb;
    void       * cbarg;

    TAILQ_ENTRY(evratelim_bev_s) next;
};

struct evratelim_group_s {
    struct event      * refill_ev;
    struct event_base * evbase;
    t_bucket_cfg      * t_cfg;
    t_bucket          * rate_limit;
    pthread_mutex_t     lock;
    uint64_t            total_read;
    uint64_t            total_written;
    bool                read_suspended;
    bool                write_suspended;
    int                 n_members;

    TAILQ_HEAD(, evratelim_bev_s) members;
};

static evratelim_bev *
_group_get_random_rlbev(evratelim_group * group) {
    evratelim_bev * rl_bev;
    int             where;

    pthread_mutex_lock(&group->lock);

    if (TAILQ_EMPTY(&group->members)) {
        pthread_mutex_unlock(&group->lock);
        return NULL;
    }

    where  = rand() % group->n_members;
    rl_bev = TAILQ_FIRST(&group->members);

    while (where--) {
        rl_bev = TAILQ_NEXT(rl_bev, next);
    }

    pthread_mutex_unlock(&group->lock);
    return rl_bev;
}

static void
_group_resume(evratelim_group * group, short what) {
    evratelim_bev * first;
    evratelim_bev * rl_bev;
    evratelim_cb  * cb;

    pthread_mutex_lock(&group->lock);

    if (!(first = _group_get_random_rlbev(group))) {
        pthread_mutex_unlock(&group->lock);
        return;
    }

    for (rl_bev = first; rl_bev; rl_bev = TAILQ_NEXT(rl_bev, next)) {
        if (rl_bev->resume_cb) {
            (rl_bev->resume_cb)(rl_bev, what, rl_bev->cbarg);
        }
    }

    for (rl_bev = TAILQ_FIRST(&group->members); rl_bev && rl_bev != first;
         rl_bev = TAILQ_NEXT(rl_bev, next)) {
        if (rl_bev->resume_cb) {
            (rl_bev->resume_cb)(rl_bev, what, rl_bev->cbarg);
        }
    }

    pthread_mutex_unlock(&group->lock);
}

static void
_group_suspend(evratelim_group * group, short what) {
    evratelim_bev * rl_bev;

    pthread_mutex_lock(&group->lock);

    TAILQ_FOREACH(rl_bev, &group->members, next) {
        if (rl_bev->suspend_cb) {
            (rl_bev->suspend_cb)(rl_bev, what, rl_bev->cbarg);
        }
    }

    pthread_mutex_unlock(&group->lock);
}

static void
_group_resume_writing(evratelim_group * group) {
    pthread_mutex_lock(&group->lock);

    group->write_suspended = false;

    _group_resume(group, EV_WRITE);

    pthread_mutex_unlock(&group->lock);
}

static void
_group_resume_reading(evratelim_group * group) {
    pthread_mutex_lock(&group->lock);

    group->read_suspended = false;

    _group_resume(group, EV_READ);

    pthread_mutex_unlock(&group->lock);
}

static void
_group_suspend_writing(evratelim_group * group) {
    pthread_mutex_lock(&group->lock);

    group->write_suspended = true;

    _group_suspend(group, EV_WRITE);

    pthread_mutex_unlock(&group->lock);
}

static void
_group_suspend_reading(evratelim_group * group) {
    pthread_mutex_lock(&group->lock);

    group->read_suspended = true;

    _group_suspend(group, EV_READ);

    pthread_mutex_unlock(&group->lock);
}

static void
_group_refill_evcb(int sock, short which, void * arg) {
    evratelim_group * group;

    group = (evratelim_group *)arg;

    pthread_mutex_lock(&group->lock);
    {
        t_bucket_update(group->rate_limit);

        if (group->read_suspended == true &&
            (t_bucket_read_limit(group->rate_limit) >= 1)) {
            _group_resume_reading(group);
        }

        if (group->write_suspended == true &&
            (t_bucket_write_limit(group->rate_limit) >= 1)) {
            _group_resume_writing(group);
        }
    }
    pthread_mutex_unlock(&group->lock);
}

void
evratelim_bev_write(evratelim_bev * rl_bev, ssize_t bytes) {
    pthread_mutex_lock(&rl_bev->group->lock);
    {
        t_bucket_update_write(rl_bev->group->rate_limit, bytes);

        if (t_bucket_write_limit(rl_bev->group->rate_limit) <= 0) {
            _group_suspend_writing(rl_bev->group);
        } else if (rl_bev->group->write_suspended == true) {
            _group_resume_writing(rl_bev->group);
        }
    }
    pthread_mutex_unlock(&rl_bev->group->lock);
}

void
evratelim_bev_read(evratelim_bev * rl_bev, ssize_t bytes) {
    pthread_mutex_lock(&rl_bev->group->lock);
    {
        t_bucket_update_read(rl_bev->group->rate_limit, bytes);

        if (t_bucket_read_limit(rl_bev->group->rate_limit) <= 0) {
            _group_suspend_reading(rl_bev->group);
        } else if (rl_bev->group->write_suspended == true) {
            _group_resume_reading(rl_bev->group);
        }
    }
    pthread_mutex_unlock(&rl_bev->group->lock);
}

struct bufferevent *
evratelim_bev_bufferevent(evratelim_bev * bev) {
    return bev->bev;
}

evratelim_group *
evratelim_group_new(struct event_base * evbase, size_t read_rate, size_t write_rate) {
    evratelim_group   * group;
    pthread_mutexattr_t attr;

    group                  = calloc(sizeof(evratelim_group), 1);
    group->evbase          = evbase;
    group->t_cfg           = t_bucket_cfg_new(read_rate, write_rate);
    group->rate_limit      = t_bucket_new(group->t_cfg);
    group->refill_ev       = event_new(evbase, -1, EV_PERSIST, _group_refill_evcb, group);
    group->read_suspended  = false;
    group->write_suspended = false;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&group->lock, &attr);

    event_add(group->refill_ev, t_bucket_cfg_tick_timeout(group->t_cfg));

    TAILQ_INIT(&group->members);

    return group;
}

evratelim_bev *
evratelim_add_bufferevent(struct bufferevent * bev, evratelim_group * group) {
    evratelim_bev * rl_bev;

    pthread_mutex_lock(&group->lock);
    {
        rl_bev            = calloc(sizeof(evratelim_bev), 1);
        rl_bev->bev       = bev;
        rl_bev->group     = group;

        group->n_members += 1;

        TAILQ_INSERT_TAIL(&group->members, rl_bev, next);
    }
    pthread_mutex_unlock(&group->lock);

    return rl_bev;
}

void
evratelim_bev_setcb(evratelim_bev * rl_bev,
                    evratelim_cb s_cb,
                    evratelim_cb r_cb, void * cbarg) {
    rl_bev->suspend_cb = s_cb;
    rl_bev->resume_cb  = r_cb;
    rl_bev->cbarg      = cbarg;
}

void
evratelim_bev_remove(evratelim_bev * rl_bev) {
    evratelim_group * group;

    group = rl_bev->group;

    pthread_mutex_lock(&group->lock);
    {
        group->n_members -= 1;

        TAILQ_REMOVE(&group->members, rl_bev, next);

        free(rl_bev);
    }
    pthread_mutex_unlock(&group->lock);
}

bool
evratelim_read_suspended(evratelim_group * group) {
    bool ret;

    pthread_mutex_lock(&group->lock);
    ret = group->read_suspended;
    pthread_mutex_unlock(&group->lock);

    return ret;
}

bool
evratelim_write_suspended(evratelim_group * group) {
    bool ret;

    pthread_mutex_lock(&group->lock);
    ret = group->write_suspended;
    pthread_mutex_unlock(&group->lock);

    return ret;
}


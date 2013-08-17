#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

#include "evratelim.h"

static void
_suspendcb(evratelim_bev * rl_bev, short what, void * arg) {
    struct bufferevent * bev = arg;

    printf("Suspending %s\n", (what == EV_READ) ? "read" : "write");

    bufferevent_disable(bev, what);
}

static void
_resumecb(evratelim_bev * rl_bev, short what, void * arg) {
    struct bufferevent * bev = arg;

    printf("Resuming %s\n", (what == EV_READ) ? "read" : "write");

    bufferevent_enable(bev, what);
}

static void
_readcb(struct bufferevent * bev, void * arg) {
    struct evbuffer * in     = bufferevent_get_input(bev);
    evratelim_bev   * rl_bev = arg;

    evratelim_bev_read(rl_bev, evbuffer_get_length(in));

    bufferevent_write_buffer(bev, in);
}

void
_writecb(struct bufferevent * bev, void * arg) {
    evratelim_bev * rl_bev = arg;

    /* evratelim_bev_write(rl_bev, written); */
}

void
_eventcb(struct bufferevent * bev, short what, void * arg) {
    evratelim_bev * rl_bev = arg;

    bufferevent_free(evratelim_bev_bufferevent(rl_bev));
    evratelim_bev_remove(rl_bev);
}

static void
_listener_cb(struct evconnlistener * listener, int sock,
             struct sockaddr * saddr, int socklen, void * arg) {
    struct bufferevent * bev;
    evratelim_bev      * rl_bev;
    evratelim_group    * group;

    group  = (evratelim_group *)arg;

    bev    = bufferevent_socket_new(evconnlistener_get_base(listener),
                                    sock, BEV_OPT_CLOSE_ON_FREE);
    rl_bev = evratelim_add_bufferevent(bev, group);

    evratelim_bev_setcb(rl_bev, _suspendcb, _resumecb, bev);
    bufferevent_setcb(bev, _readcb, _writecb, _eventcb, rl_bev);
    bufferevent_enable(bev, EV_READ);
}

int
main(int argc, char **argv) {
    struct event_base     * evbase;
    struct evconnlistener * listener;
    struct sockaddr_in      sin = { 0 };
    evratelim_group       * rl_group;

    srand(time(NULL));

    evbase              = event_base_new();

    /* set to 1Mb/s (262144 B/s) */
    rl_group            = evratelim_group_new(evbase, 1048576, 1048576);

    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001);
    sin.sin_port        = htons(55555);

    listener            = evconnlistener_new_bind(evbase, _listener_cb, rl_group,
                                                  LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
                                                  (struct sockaddr *)&sin, sizeof(sin));

    event_base_loop(evbase, 0);

    return 0;
}


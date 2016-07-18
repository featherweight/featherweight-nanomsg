/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2015-2016 Jack R. Dunaway.  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "poller.h"

#if defined NN_USE_EPOLL

#include "../utils/err.h"
#include "../utils/closefd.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int nn_poller_init (struct nn_poller *self)
{
#ifndef EPOLL_CLOEXEC
    int rc;
#endif

#ifdef EPOLL_CLOEXEC
    self->ep = epoll_create1 (EPOLL_CLOEXEC);
#else
    /*  Size parameter is unused, we can safely set it to 1. */
    self->ep = epoll_create (1);
    rc = fcntl (self->ep, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif
    if (self->ep == -1) {
        if (errno == ENFILE || errno == EMFILE)
            return -EMFILE;
        errno_assert (0);
    }
    self->nevents = 0;
    self->index = 0;

    return 0;
}

void nn_poller_term (struct nn_poller *self)
{
    nn_closefd (self->ep);
}

void nn_poller_add (struct nn_poller *self, int fd,
    struct nn_poller_hndl *hndl)
{
    int rc;
    struct epoll_event ev;

    /*  Initialise the handle and add the file descriptor to the pollset. */
    hndl->fd = fd;
    hndl->events = 0;
    memset (&ev, 0, sizeof (ev));
    ev.events = 0;
    ev.data.ptr = (void*) hndl;
    epoll_ctl (self->ep, EPOLL_CTL_ADD, fd, &ev);
}

void nn_poller_rm (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int i;

    /*  Remove the file descriptor from the pollset. */
    epoll_ctl (self->ep, EPOLL_CTL_DEL, hndl->fd, NULL);

    /*  Invalidate any subsequent events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].data.ptr == hndl)
            self->events [i].events = 0;
}

void nn_poller_set_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    struct epoll_event ev;

    /*  If already polling for IN, do nothing. */
    if (nn_slow (hndl->events & EPOLLIN))
        return;

    /*  Start polling for IN. */
    hndl->events |= EPOLLIN;
    memset (&ev, 0, sizeof (ev));
    ev.events = hndl->events;
    ev.data.ptr = (void*) hndl;
    epoll_ctl (self->ep, EPOLL_CTL_MOD, hndl->fd, &ev);
}

void nn_poller_reset_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int i;
    struct epoll_event ev;

    /*  If not polling for IN, do nothing. */
    if (nn_slow (!(hndl->events & EPOLLIN)))
        return;

    /*  Stop polling for IN. */
    hndl->events &= ~EPOLLIN;
    memset (&ev, 0, sizeof (ev));
    ev.events = hndl->events;
    ev.data.ptr = (void*) hndl;
    epoll_ctl (self->ep, EPOLL_CTL_MOD, hndl->fd, &ev);

    /*  Invalidate any subsequent IN events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].data.ptr == hndl)
            self->events [i].events &= ~EPOLLIN;
}

void nn_poller_set_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    struct epoll_event ev;
    int fd = hndl->fd;

    /*  If already polling for OUT, do nothing. */
    if (nn_slow (hndl->events & EPOLLOUT))
        return;

    /*  Start polling for OUT. */
    hndl->events |= EPOLLOUT;
    memset (&ev, 0, sizeof (ev));
    ev.events = hndl->events;
    ev.data.ptr = (void*) hndl;
    epoll_ctl (self->ep, EPOLL_CTL_MOD, fd, &ev);
}

void nn_poller_reset_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int i;
    struct epoll_event ev;

    /*  If not polling for OUT, do nothing. */
    if (nn_slow (!(hndl->events & EPOLLOUT)))
        return;

    /*  Stop polling for OUT. */
    hndl->events &= ~EPOLLOUT;
    memset (&ev, 0, sizeof (ev));
    ev.events = hndl->events;
    ev.data.ptr = (void*) hndl;
    epoll_ctl (self->ep, EPOLL_CTL_MOD, hndl->fd, &ev);

    /*  Invalidate any subsequent OUT events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].data.ptr == hndl)
            self->events [i].events &= ~EPOLLOUT;
}

int nn_poller_wait (struct nn_poller *self, int timeout)
{
    int nevents;

    /*  Clear all existing events. */
    self->nevents = 0;
    self->index = 0;

    /*  Wait for new events. */
    while (1) {
        nevents = epoll_wait (self->ep, self->events,
            NN_POLLER_MAX_EVENTS, timeout);
        if (nn_slow (nevents == -1 && errno == EINTR))
            continue;
        break;
    }
    errno_assert (self->nevents != -1);
    self->nevents = nevents;
    return 0;
}

int nn_poller_event (struct nn_poller *self, int *event,
    struct nn_poller_hndl **hndl)
{
    /*  Skip over empty events. */
    while (self->index < self->nevents) {
        if (self->events [self->index].events != 0)
            break;
        ++self->index;
    }

    /*  If there is no stored event, let the caller know. */
    if (nn_slow (self->index >= self->nevents))
        return -EAGAIN;

    /*  Return next event to the caller. Remove the event from the set. */
    *hndl = (struct nn_poller_hndl*) self->events [self->index].data.ptr;
    if (nn_fast (self->events [self->index].events & EPOLLIN)) {
        *event = NN_POLLER_IN;
        self->events [self->index].events &= ~EPOLLIN;
        return 0;
    }
    else if (nn_fast (self->events [self->index].events & EPOLLOUT)) {
        *event = NN_POLLER_OUT;
        self->events [self->index].events &= ~EPOLLOUT;
        return 0;
    }
    else {
        *event = NN_POLLER_ERR;
        ++self->index;
        return 0;
    }
}

#elif defined NN_USE_KQUEUE

#include "../utils/attr.h"
#include "../utils/err.h"
#include "../utils/closefd.h"

#include <unistd.h>

/*  NetBSD has different definition of udata. */
#if defined NN_HAVE_NETBSD
#define nn_poller_udata intptr_t
#else
#define nn_poller_udata void*
#endif

int nn_poller_init (struct nn_poller *self)
{
    self->kq = kqueue ();
    if (self->kq == -1) {
         if (errno == ENFILE || errno == EMFILE)
              return -EMFILE;
         errno_assert (0);
    }
    self->nevents = 0;
    self->index = 0;

    return 0;
}

void nn_poller_term (struct nn_poller *self)
{
    nn_closefd (self->kq);
}

void nn_poller_add (NN_UNUSED struct nn_poller *self, int fd,
    struct nn_poller_hndl *hndl)
{
    /*  Initialise the handle. */
    hndl->fd = fd;
    hndl->events = 0;
}

void nn_poller_rm (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    struct kevent ev;
    int i;

    if (hndl->events & NN_POLLER_EVENT_IN) {
        EV_SET (&ev, hndl->fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
        kevent (self->kq, &ev, 1, NULL, 0, NULL);
    }

    if (hndl->events & NN_POLLER_EVENT_OUT) {
        EV_SET (&ev, hndl->fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
        kevent (self->kq, &ev, 1, NULL, 0, NULL);
    }

    /*  Invalidate any subsequent events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].ident == (unsigned) hndl->fd)
            self->events [i].udata = (nn_poller_udata) NULL;
}

void nn_poller_set_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int rc;
    struct kevent ev;

    if (!(hndl->events & NN_POLLER_EVENT_IN)) {
        EV_SET (&ev, hndl->fd, EVFILT_READ, EV_ADD, 0, 0,
            (nn_poller_udata) hndl);
        rc = kevent (self->kq, &ev, 1, NULL, 0, NULL);
        if (rc != -1)
            hndl->events |= NN_POLLER_EVENT_IN;
    }
}

void nn_poller_reset_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int rc;
    struct kevent ev;
    int i;

    if (hndl->events & NN_POLLER_EVENT_IN) {
        EV_SET (&ev, hndl->fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
        rc = kevent (self->kq, &ev, 1, NULL, 0, NULL);
        hndl->events &= ~NN_POLLER_EVENT_IN;
    }

    /*  Invalidate any subsequent IN events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].ident == (unsigned) hndl->fd &&
              self->events [i].filter == EVFILT_READ)
            self->events [i].udata = (nn_poller_udata) NULL;
}

void nn_poller_set_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int rc;
    struct kevent ev;
    int fd = hndl->fd;

    if (!(hndl->events & NN_POLLER_EVENT_OUT)) {
        EV_SET (&ev, fd, EVFILT_WRITE, EV_ADD, 0, 0, (nn_poller_udata) hndl);
        rc = kevent (self->kq, &ev, 1, NULL, 0, NULL);
        if (rc != -1)
            hndl->events |= NN_POLLER_EVENT_OUT;
    }
}

void nn_poller_reset_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    int rc;
    struct kevent ev;
    int i;
    int fd = hndl->fd;

    if (hndl->events & NN_POLLER_EVENT_OUT) {
        EV_SET (&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
        rc = kevent (self->kq, &ev, 1, NULL, 0, NULL);
        if (rc != -1) {
            hndl->events &= ~NN_POLLER_EVENT_OUT;
        }
    }

    /*  Invalidate any subsequent OUT events on this file descriptor. */
    for (i = self->index; i != self->nevents; ++i)
        if (self->events [i].ident == (unsigned) hndl->fd &&
              self->events [i].filter == EVFILT_WRITE)
            self->events [i].udata = (nn_poller_udata) NULL;
}

int nn_poller_wait (struct nn_poller *self, int timeout)
{
    struct timespec ts;
    int nevents;

    /*  Clear all existing events. */
    self->nevents = 0;
    self->index = 0;

    /*  Wait for new events. */
#if defined NN_IGNORE_EINTR
again:
#endif
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (timeout % 1000) * 1000000;
    nevents = kevent (self->kq, NULL, 0, &self->events [0],
        NN_POLLER_MAX_EVENTS, timeout >= 0 ? &ts : NULL);
    if (nevents == -1 && errno == EINTR)
#if defined NN_IGNORE_EINTR
        goto again;
#else
        return -EINTR;
#endif
    errno_assert (nevents != -1);

    self->nevents = nevents;
    return 0;
}

int nn_poller_event (struct nn_poller *self, int *event,
    struct nn_poller_hndl **hndl)
{
    /*  Skip over empty events. */
    while (self->index < self->nevents) {
        if (self->events [self->index].udata)
            break;
        ++self->index;
    }

    /*  If there is no stored event, let the caller know. */
    if (nn_slow (self->index >= self->nevents))
        return -EAGAIN;

    /*  Return next event to the caller. Remove the event from the set. */
    *hndl = (struct nn_poller_hndl*) self->events [self->index].udata;
    if (self->events [self->index].flags & EV_EOF)
        *event = NN_POLLER_ERR;
    else if (self->events [self->index].filter == EVFILT_WRITE)
        *event = NN_POLLER_OUT;
    else if (self->events [self->index].filter == EVFILT_READ)
        *event = NN_POLLER_IN;
    else
        nn_assert (0);
    ++self->index;

    return 0;
}

#elif defined NN_USE_POLL

#include "../utils/alloc.h"
#include "../utils/err.h"

#define NN_POLLER_GRANULARITY 16

int nn_poller_init (struct nn_poller *self)
{
    self->size = 0;
    self->index = 0;
    self->capacity = NN_POLLER_GRANULARITY;
    self->pollset =
        nn_alloc (sizeof (struct pollfd) * NN_POLLER_GRANULARITY,
            "pollset");
    alloc_assert (self->pollset);
    self->hndls =
        nn_alloc (sizeof (struct nn_hndls_item) * NN_POLLER_GRANULARITY,
            "hndlset");
    alloc_assert (self->hndls);
    self->removed = -1;

    return 0;
}

void nn_poller_term (struct nn_poller *self)
{
    nn_free (self->pollset);
    nn_free (self->hndls);
}

void nn_poller_add (struct nn_poller *self, int fd,
    struct nn_poller_hndl *hndl)
{
    int rc;

    /*  If the capacity is too low to accommodate the next item, resize it. */
    if (nn_slow (self->size >= self->capacity)) {
        self->capacity *= 2;
        self->pollset = nn_realloc (self->pollset,
            sizeof (struct pollfd) * self->capacity);
        alloc_assert (self->pollset);
        self->hndls = nn_realloc (self->hndls,
            sizeof (struct nn_hndls_item) * self->capacity);
        alloc_assert (self->hndls);
    }

    /*  Add the fd to the pollset. */
    self->pollset [self->size].fd = fd;
    self->pollset [self->size].events = 0;
    self->pollset [self->size].revents = 0;
    hndl->index = self->size;
    self->hndls [self->size].hndl = hndl;
    ++self->size;
}

void nn_poller_rm (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    /*  No more events will be reported on this fd. */
    self->pollset [hndl->index].revents = 0;

    /*  Add the fd into the list of removed fds. */
    if (self->removed != -1)
        self->hndls [self->removed].prev = hndl->index;
    self->hndls [hndl->index].hndl = NULL;
    self->hndls [hndl->index].prev = -1;
    self->hndls [hndl->index].next = self->removed;
    self->removed = hndl->index;
}

void nn_poller_set_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    self->pollset [hndl->index].events |= POLLIN;
}

void nn_poller_reset_in (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    self->pollset [hndl->index].events &= ~POLLIN;
    self->pollset [hndl->index].revents &= ~POLLIN;
}

void nn_poller_set_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    self->pollset [hndl->index].events |= POLLOUT;
}

void nn_poller_reset_out (struct nn_poller *self, struct nn_poller_hndl *hndl)
{
    self->pollset [hndl->index].events &= ~POLLOUT;
    self->pollset [hndl->index].revents &= ~POLLOUT;
}

int nn_poller_wait (struct nn_poller *self, int timeout)
{
    int rc;
    int i;

    /*  First, get rid of removed fds. */
    while (self->removed != -1) {

        /*  Remove the fd from the list of removed fds. */
        i = self->removed;
        self->removed = self->hndls [i].next;

        /*  Replace the removed fd by the one at the end of the pollset. */
        --self->size;
        if (i != self->size) {
            self->pollset [i] = self->pollset [self->size];
            if (self->hndls [i].next != -1)
                self->hndls [self->hndls [i].next].prev = -1;
            self->hndls [i] = self->hndls [self->size];
            if (self->hndls [i].hndl)
                self->hndls [i].hndl->index = i;
        }

        /*  The fd from the end of the pollset may have been on removed fds
            list itself. If so, adjust the removed list. */
        if (nn_slow (!self->hndls [i].hndl)) {
            if (self->hndls [i].prev != -1)
               self->hndls [self->hndls [i].prev].next = i;
            if (self->hndls [i].next != -1)
               self->hndls [self->hndls [i].next].prev = i;
            if (self->removed == self->size)
                self->removed = i;
        }
    }

    self->index = 0;

    /*  Wait for new events. */
#if defined NN_IGNORE_EINTR
again:
#endif
    rc = poll (self->pollset, self->size, timeout);
    if (nn_slow (rc < 0 && errno == EINTR))
#if defined NN_IGNORE_EINTR
        goto again;
#else
        return -EINTR;
#endif
    errno_assert (rc >= 0);
    return 0;
}

int nn_poller_event (struct nn_poller *self, int *event,
    struct nn_poller_hndl **hndl)
{
    int rc;

    /*  Skip over empty events. This will also skip over removed fds as they
        have their revents nullified. */
    while (self->index < self->size) {
        if (self->pollset [self->index].revents != 0)
            break;
        ++self->index;
    }

    /*  If there is no available event, let the caller know. */
    if (nn_slow (self->index >= self->size))
        return -EAGAIN;

    /*  Return next event to the caller. Remove the event from revents. */
    *hndl = self->hndls [self->index].hndl;
    if (nn_fast (self->pollset [self->index].revents & POLLIN)) {
        *event = NN_POLLER_IN;
        self->pollset [self->index].revents &= ~POLLIN;
        return 0;
    }
    else if (nn_fast (self->pollset [self->index].revents & POLLOUT)) {
        *event = NN_POLLER_OUT;
        self->pollset [self->index].revents &= ~POLLOUT;
        return 0;
    }
    else {
        *event = NN_POLLER_ERR;
        ++self->index;
        return 0;
    }
}

#else
    #error
#endif

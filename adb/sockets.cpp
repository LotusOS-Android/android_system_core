/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define TRACE_TAG SOCKETS

#include "sysdeps.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#if !ADB_HOST
#include <android-base/properties.h>
#include <log/log_properties.h>
#endif

#include "adb.h"
#include "adb_io.h"
#include "transport.h"

static std::recursive_mutex& local_socket_list_lock = *new std::recursive_mutex();
static unsigned local_socket_next_id = 1;

static asocket local_socket_list = {
    .next = &local_socket_list, .prev = &local_socket_list,
};

/* the the list of currently closing local sockets.
** these have no peer anymore, but still packets to
** write to their fd.
*/
static asocket local_socket_closing_list = {
    .next = &local_socket_closing_list, .prev = &local_socket_closing_list,
};

// Parse the global list of sockets to find one with id |local_id|.
// If |peer_id| is not 0, also check that it is connected to a peer
// with id |peer_id|. Returns an asocket handle on success, NULL on failure.
asocket* find_local_socket(unsigned local_id, unsigned peer_id) {
    asocket* s;
    asocket* result = NULL;

    std::lock_guard<std::recursive_mutex> lock(local_socket_list_lock);
    for (s = local_socket_list.next; s != &local_socket_list; s = s->next) {
        if (s->id != local_id) {
            continue;
        }
        if (peer_id == 0 || (s->peer && s->peer->id == peer_id)) {
            result = s;
        }
        break;
    }

    return result;
}

static void insert_local_socket(asocket* s, asocket* list) {
    s->next = list;
    s->prev = s->next->prev;
    s->prev->next = s;
    s->next->prev = s;
}

void install_local_socket(asocket* s) {
    std::lock_guard<std::recursive_mutex> lock(local_socket_list_lock);

    s->id = local_socket_next_id++;

    // Socket ids should never be 0.
    if (local_socket_next_id == 0) {
        fatal("local socket id overflow");
    }

    insert_local_socket(s, &local_socket_list);
}

void remove_socket(asocket* s) {
    // socket_list_lock should already be held
    if (s->prev && s->next) {
        s->prev->next = s->next;
        s->next->prev = s->prev;
        s->next = 0;
        s->prev = 0;
        s->id = 0;
    }
}

void close_all_sockets(atransport* t) {
    asocket* s;

    /* this is a little gross, but since s->close() *will* modify
    ** the list out from under you, your options are limited.
    */
    std::lock_guard<std::recursive_mutex> lock(local_socket_list_lock);
restart:
    for (s = local_socket_list.next; s != &local_socket_list; s = s->next) {
        if (s->transport == t || (s->peer && s->peer->transport == t)) {
            s->close(s);
            goto restart;
        }
    }
}

static int local_socket_enqueue(asocket* s, apacket* p) {
    D("LS(%d): enqueue %zu", s->id, p->len);

    p->ptr = p->data;

    /* if there is already data queue'd, we will receive
    ** events when it's time to write.  just add this to
    ** the tail
    */
    if (s->pkt_first) {
        goto enqueue;
    }

    /* write as much as we can, until we
    ** would block or there is an error/eof
    */
    while (p->len > 0) {
        int r = adb_write(s->fd, p->ptr, p->len);
        if (r > 0) {
            p->len -= r;
            p->ptr += r;
            continue;
        }
        if ((r == 0) || (errno != EAGAIN)) {
            D("LS(%d): not ready, errno=%d: %s", s->id, errno, strerror(errno));
            put_apacket(p);
            s->has_write_error = true;
            s->close(s);
            return 1; /* not ready (error) */
        } else {
            break;
        }
    }

    if (p->len == 0) {
        put_apacket(p);
        return 0; /* ready for more data */
    }

enqueue:
    p->next = 0;
    if (s->pkt_first) {
        s->pkt_last->next = p;
    } else {
        s->pkt_first = p;
    }
    s->pkt_last = p;

    /* make sure we are notified when we can drain the queue */
    fdevent_add(&s->fde, FDE_WRITE);

    return 1; /* not ready (backlog) */
}

static void local_socket_ready(asocket* s) {
    /* far side is ready for data, pay attention to
       readable events */
    fdevent_add(&s->fde, FDE_READ);
}

// be sure to hold the socket list lock when calling this
static void local_socket_destroy(asocket* s) {
    apacket *p, *n;
    int exit_on_close = s->exit_on_close;

    D("LS(%d): destroying fde.fd=%d", s->id, s->fde.fd);

    /* IMPORTANT: the remove closes the fd
    ** that belongs to this socket
    */
    fdevent_remove(&s->fde);

    /* dispose of any unwritten data */
    for (p = s->pkt_first; p; p = n) {
        D("LS(%d): discarding %zu bytes", s->id, p->len);
        n = p->next;
        put_apacket(p);
    }
    remove_socket(s);
    free(s);

    if (exit_on_close) {
        D("local_socket_destroy: exiting");
        exit(1);
    }
}

static void local_socket_close(asocket* s) {
    D("entered local_socket_close. LS(%d) fd=%d", s->id, s->fd);
    std::lock_guard<std::recursive_mutex> lock(local_socket_list_lock);
    if (s->peer) {
        D("LS(%d): closing peer. peer->id=%d peer->fd=%d", s->id, s->peer->id, s->peer->fd);
        /* Note: it's important to call shutdown before disconnecting from
         * the peer, this ensures that remote sockets can still get the id
         * of the local socket they're connected to, to send a CLOSE()
         * protocol event. */
        if (s->peer->shutdown) {
            s->peer->shutdown(s->peer);
        }
        s->peer->peer = nullptr;
        s->peer->close(s->peer);
        s->peer = nullptr;
    }

    /* If we are already closing, or if there are no
    ** pending packets, destroy immediately
    */
    if (s->closing || s->has_write_error || s->pkt_first == NULL) {
        int id = s->id;
        local_socket_destroy(s);
        D("LS(%d): closed", id);
        return;
    }

    /* otherwise, put on the closing list
    */
    D("LS(%d): closing", s->id);
    s->closing = 1;
    fdevent_del(&s->fde, FDE_READ);
    remove_socket(s);
    D("LS(%d): put on socket_closing_list fd=%d", s->id, s->fd);
    insert_local_socket(s, &local_socket_closing_list);
    CHECK_EQ(FDE_WRITE, s->fde.state & FDE_WRITE);
}

static void local_socket_event_func(int fd, unsigned ev, void* _s) {
    asocket* s = reinterpret_cast<asocket*>(_s);
    D("LS(%d): event_func(fd=%d(==%d), ev=%04x)", s->id, s->fd, fd, ev);

    /* put the FDE_WRITE processing before the FDE_READ
    ** in order to simplify the code.
    */
    if (ev & FDE_WRITE) {
        apacket* p;
        while ((p = s->pkt_first) != nullptr) {
            while (p->len > 0) {
                int r = adb_write(fd, p->ptr, p->len);
                if (r == -1) {
                    /* returning here is ok because FDE_READ will
                    ** be processed in the next iteration loop
                    */
                    if (errno == EAGAIN) {
                        return;
                    }
                } else if (r > 0) {
                    p->ptr += r;
                    p->len -= r;
                    continue;
                }

                D(" closing after write because r=%d and errno is %d", r, errno);
                s->has_write_error = true;
                s->close(s);
                return;
            }

            if (p->len == 0) {
                s->pkt_first = p->next;
                if (s->pkt_first == 0) {
                    s->pkt_last = 0;
                }
                put_apacket(p);
            }
        }

        /* if we sent the last packet of a closing socket,
        ** we can now destroy it.
        */
        if (s->closing) {
            D(" closing because 'closing' is set after write");
            s->close(s);
            return;
        }

        /* no more packets queued, so we can ignore
        ** writable events again and tell our peer
        ** to resume writing
        */
        fdevent_del(&s->fde, FDE_WRITE);
        s->peer->ready(s->peer);
    }

    if (ev & FDE_READ) {
        apacket* p = get_apacket();
        char* x = p->data;
        const size_t max_payload = s->get_max_payload();
        size_t avail = max_payload;
        int r = 0;
        int is_eof = 0;

        while (avail > 0) {
            r = adb_read(fd, x, avail);
            D("LS(%d): post adb_read(fd=%d,...) r=%d (errno=%d) avail=%zu", s->id, s->fd, r,
              r < 0 ? errno : 0, avail);
            if (r == -1) {
                if (errno == EAGAIN) {
                    break;
                }
            } else if (r > 0) {
                avail -= r;
                x += r;
                continue;
            }

            /* r = 0 or unhandled error */
            is_eof = 1;
            break;
        }
        D("LS(%d): fd=%d post avail loop. r=%d is_eof=%d forced_eof=%d", s->id, s->fd, r, is_eof,
          s->fde.force_eof);
        if ((avail == max_payload) || (s->peer == 0)) {
            put_apacket(p);
        } else {
            p->len = max_payload - avail;

            // s->peer->enqueue() may call s->close() and free s,
            // so save variables for debug printing below.
            unsigned saved_id = s->id;
            int saved_fd = s->fd;
            r = s->peer->enqueue(s->peer, p);
            D("LS(%u): fd=%d post peer->enqueue(). r=%d", saved_id, saved_fd, r);

            if (r < 0) {
                /* error return means they closed us as a side-effect
                ** and we must return immediately.
                **
                ** note that if we still have buffered packets, the
                ** socket will be placed on the closing socket list.
                ** this handler function will be called again
                ** to process FDE_WRITE events.
                */
                return;
            }

            if (r > 0) {
                /* if the remote cannot accept further events,
                ** we disable notification of READs.  They'll
                ** be enabled again when we get a call to ready()
                */
                fdevent_del(&s->fde, FDE_READ);
            }
        }
        /* Don't allow a forced eof if data is still there */
        if ((s->fde.force_eof && !r) || is_eof) {
            D(" closing because is_eof=%d r=%d s->fde.force_eof=%d", is_eof, r, s->fde.force_eof);
            s->close(s);
            return;
        }
    }

    if (ev & FDE_ERROR) {
        /* this should be caught be the next read or write
        ** catching it here means we may skip the last few
        ** bytes of readable data.
        */
        D("LS(%d): FDE_ERROR (fd=%d)", s->id, s->fd);
        return;
    }
}

asocket* create_local_socket(int fd) {
    asocket* s = reinterpret_cast<asocket*>(calloc(1, sizeof(asocket)));
    if (s == NULL) {
        fatal("cannot allocate socket");
    }
    s->fd = fd;
    s->enqueue = local_socket_enqueue;
    s->ready = local_socket_ready;
    s->shutdown = NULL;
    s->close = local_socket_close;
    install_local_socket(s);

    fdevent_install(&s->fde, fd, local_socket_event_func, s);
    D("LS(%d): created (fd=%d)", s->id, s->fd);
    return s;
}

asocket* create_local_service_socket(const char* name, const atransport* transport) {
#if !ADB_HOST
    if (!strcmp(name, "jdwp")) {
        return create_jdwp_service_socket();
    }
    if (!strcmp(name, "track-jdwp")) {
        return create_jdwp_tracker_service_socket();
    }
#endif
    int fd = service_to_fd(name, transport);
    if (fd < 0) {
        return nullptr;
    }

    asocket* s = create_local_socket(fd);
    D("LS(%d): bound to '%s' via %d", s->id, name, fd);

#if !ADB_HOST
    if ((!strncmp(name, "root:", 5) && getuid() != 0 && __android_log_is_debuggable()) ||
        (!strncmp(name, "unroot:", 7) && getuid() == 0) ||
        !strncmp(name, "usb:", 4) ||
        !strncmp(name, "tcpip:", 6)) {
        D("LS(%d): enabling exit_on_close", s->id);
        s->exit_on_close = 1;
    }
#endif

    return s;
}

#if ADB_HOST
static asocket* create_host_service_socket(const char* name, const char* serial) {
    asocket* s;

    s = host_service_to_socket(name, serial);

    if (s != NULL) {
        D("LS(%d) bound to '%s'", s->id, name);
        return s;
    }

    return s;
}
#endif /* ADB_HOST */

static int remote_socket_enqueue(asocket* s, apacket* p) {
    D("entered remote_socket_enqueue RS(%d) WRITE fd=%d peer.fd=%d", s->id, s->fd, s->peer->fd);
    p->msg.command = A_WRTE;
    p->msg.arg0 = s->peer->id;
    p->msg.arg1 = s->id;
    p->msg.data_length = p->len;
    send_packet(p, s->transport);
    return 1;
}

static void remote_socket_ready(asocket* s) {
    D("entered remote_socket_ready RS(%d) OKAY fd=%d peer.fd=%d", s->id, s->fd, s->peer->fd);
    apacket* p = get_apacket();
    p->msg.command = A_OKAY;
    p->msg.arg0 = s->peer->id;
    p->msg.arg1 = s->id;
    send_packet(p, s->transport);
}

static void remote_socket_shutdown(asocket* s) {
    D("entered remote_socket_shutdown RS(%d) CLOSE fd=%d peer->fd=%d", s->id, s->fd,
      s->peer ? s->peer->fd : -1);
    apacket* p = get_apacket();
    p->msg.command = A_CLSE;
    if (s->peer) {
        p->msg.arg0 = s->peer->id;
    }
    p->msg.arg1 = s->id;
    send_packet(p, s->transport);
}

static void remote_socket_close(asocket* s) {
    if (s->peer) {
        s->peer->peer = 0;
        D("RS(%d) peer->close()ing peer->id=%d peer->fd=%d", s->id, s->peer->id, s->peer->fd);
        s->peer->close(s->peer);
    }
    D("entered remote_socket_close RS(%d) CLOSE fd=%d peer->fd=%d", s->id, s->fd,
      s->peer ? s->peer->fd : -1);
    D("RS(%d): closed", s->id);
    free(s);
}

// Create a remote socket to exchange packets with a remote service through transport
// |t|. Where |id| is the socket id of the corresponding service on the other
//  side of the transport (it is allocated by the remote side and _cannot_ be 0).
// Returns a new non-NULL asocket handle.
asocket* create_remote_socket(unsigned id, atransport* t) {
    if (id == 0) {
        fatal("invalid remote socket id (0)");
    }
    asocket* s = reinterpret_cast<asocket*>(calloc(1, sizeof(asocket)));

    if (s == NULL) {
        fatal("cannot allocate socket");
    }
    s->id = id;
    s->enqueue = remote_socket_enqueue;
    s->ready = remote_socket_ready;
    s->shutdown = remote_socket_shutdown;
    s->close = remote_socket_close;
    s->transport = t;

    D("RS(%d): created", s->id);
    return s;
}

void connect_to_remote(asocket* s, const char* destination) {
    D("Connect_to_remote call RS(%d) fd=%d", s->id, s->fd);
    apacket* p = get_apacket();
    size_t len = strlen(destination) + 1;

    if (len > (s->get_max_payload() - 1)) {
        fatal("destination oversized");
    }

    D("LS(%d): connect('%s')", s->id, destination);
    p->msg.command = A_OPEN;
    p->msg.arg0 = s->id;
    p->msg.data_length = len;
    strcpy((char*)p->data, destination);
    send_packet(p, s->transport);
}

/* this is used by magic sockets to rig local sockets to
   send the go-ahead message when they connect */
static void local_socket_ready_notify(asocket* s) {
    s->ready = local_socket_ready;
    s->shutdown = NULL;
    s->close = local_socket_close;
    SendOkay(s->fd);
    s->ready(s);
}

/* this is used by magic sockets to rig local sockets to
   send the failure message if they are closed before
   connected (to avoid closing them without a status message) */
static void local_socket_close_notify(asocket* s) {
    s->ready = local_socket_ready;
    s->shutdown = NULL;
    s->close = local_socket_close;
    SendFail(s->fd, "closed");
    s->close(s);
}

static unsigned unhex(char* s, int len) {
    unsigned n = 0, c;

    while (len-- > 0) {
        switch ((c = *s++)) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                c -= '0';
                break;
            case 'a':
            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'f':
                c = c - 'a' + 10;
                break;
            case 'A':
            case 'B':
            case 'C':
            case 'D':
            case 'E':
            case 'F':
                c = c - 'A' + 10;
                break;
            default:
                return 0xffffffff;
        }

        n = (n << 4) | c;
    }

    return n;
}

#if ADB_HOST

namespace internal {

// Returns the position in |service| following the target serial parameter. Serial format can be
// any of:
//   * [tcp:|udp:]<serial>[:<port>]:<command>
//   * <prefix>:<serial>:<command>
// Where <port> must be a base-10 number and <prefix> may be any of {usb,product,model,device}.
//
// The returned pointer will point to the ':' just before <command>, or nullptr if not found.
char* skip_host_serial(char* service) {
    static const std::vector<std::string>& prefixes =
        *(new std::vector<std::string>{"usb:", "product:", "model:", "device:"});

    for (const std::string& prefix : prefixes) {
        if (!strncmp(service, prefix.c_str(), prefix.length())) {
            return strchr(service + prefix.length(), ':');
        }
    }

    // For fastboot compatibility, ignore protocol prefixes.
    if (!strncmp(service, "tcp:", 4) || !strncmp(service, "udp:", 4)) {
        service += 4;
    }

    // Check for an IPv6 address. `adb connect` creates the serial number from the canonical
    // network address so it will always have the [] delimiters.
    if (service[0] == '[') {
        char* ipv6_end = strchr(service, ']');
        if (ipv6_end != nullptr) {
            service = ipv6_end;
        }
    }

    // The next colon we find must either begin the port field or the command field.
    char* colon_ptr = strchr(service, ':');
    if (!colon_ptr) {
        // No colon in service string.
        return nullptr;
    }

    // If the next field is only decimal digits and ends with another colon, it's a port.
    char* serial_end = colon_ptr;
    if (isdigit(serial_end[1])) {
        serial_end++;
        while (*serial_end && isdigit(*serial_end)) {
            serial_end++;
        }
        if (*serial_end != ':') {
            // Something other than "<port>:" was found, this must be the command field instead.
            serial_end = colon_ptr;
        }
    }
    return serial_end;
}

}  // namespace internal

#endif  // ADB_HOST

static int smart_socket_enqueue(asocket* s, apacket* p) {
    unsigned len;
#if ADB_HOST
    char* service = nullptr;
    char* serial = nullptr;
    TransportType type = kTransportAny;
#endif

    D("SS(%d): enqueue %zu", s->id, p->len);

    if (s->pkt_first == 0) {
        s->pkt_first = p;
        s->pkt_last = p;
    } else {
        if ((s->pkt_first->len + p->len) > s->get_max_payload()) {
            D("SS(%d): overflow", s->id);
            put_apacket(p);
            goto fail;
        }

        memcpy(s->pkt_first->data + s->pkt_first->len, p->data, p->len);
        s->pkt_first->len += p->len;
        put_apacket(p);

        p = s->pkt_first;
    }

    /* don't bother if we can't decode the length */
    if (p->len < 4) {
        return 0;
    }

    len = unhex(p->data, 4);
    if ((len < 1) || (len > MAX_PAYLOAD_V1)) {
        D("SS(%d): bad size (%d)", s->id, len);
        goto fail;
    }

    D("SS(%d): len is %d", s->id, len);
    /* can't do anything until we have the full header */
    if ((len + 4) > p->len) {
        D("SS(%d): waiting for %zu more bytes", s->id, len + 4 - p->len);
        return 0;
    }

    p->data[len + 4] = 0;

    D("SS(%d): '%s'", s->id, (char*)(p->data + 4));

#if ADB_HOST
    service = (char*)p->data + 4;
    if (!strncmp(service, "host-serial:", strlen("host-serial:"))) {
        char* serial_end;
        service += strlen("host-serial:");

        // serial number should follow "host:" and could be a host:port string.
        serial_end = internal::skip_host_serial(service);
        if (serial_end) {
            *serial_end = 0;  // terminate string
            serial = service;
            service = serial_end + 1;
        }
    } else if (!strncmp(service, "host-usb:", strlen("host-usb:"))) {
        type = kTransportUsb;
        service += strlen("host-usb:");
    } else if (!strncmp(service, "host-local:", strlen("host-local:"))) {
        type = kTransportLocal;
        service += strlen("host-local:");
    } else if (!strncmp(service, "host:", strlen("host:"))) {
        type = kTransportAny;
        service += strlen("host:");
    } else {
        service = nullptr;
    }

    if (service) {
        asocket* s2;

        /* some requests are handled immediately -- in that
        ** case the handle_host_request() routine has sent
        ** the OKAY or FAIL message and all we have to do
        ** is clean up.
        */
        if (handle_host_request(service, type, serial, s->peer->fd, s) == 0) {
            /* XXX fail message? */
            D("SS(%d): handled host service '%s'", s->id, service);
            goto fail;
        }
        if (!strncmp(service, "transport", strlen("transport"))) {
            D("SS(%d): okay transport", s->id);
            p->len = 0;
            return 0;
        }

        /* try to find a local service with this name.
        ** if no such service exists, we'll fail out
        ** and tear down here.
        */
        s2 = create_host_service_socket(service, serial);
        if (s2 == 0) {
            D("SS(%d): couldn't create host service '%s'", s->id, service);
            SendFail(s->peer->fd, "unknown host service");
            goto fail;
        }

        /* we've connected to a local host service,
        ** so we make our peer back into a regular
        ** local socket and bind it to the new local
        ** service socket, acknowledge the successful
        ** connection, and close this smart socket now
        ** that its work is done.
        */
        SendOkay(s->peer->fd);

        s->peer->ready = local_socket_ready;
        s->peer->shutdown = nullptr;
        s->peer->close = local_socket_close;
        s->peer->peer = s2;
        s2->peer = s->peer;
        s->peer = 0;
        D("SS(%d): okay", s->id);
        s->close(s);

        /* initial state is "ready" */
        s2->ready(s2);
        return 0;
    }
#else /* !ADB_HOST */
    if (s->transport == nullptr) {
        std::string error_msg = "unknown failure";
        s->transport = acquire_one_transport(kTransportAny, nullptr, nullptr, &error_msg);
        if (s->transport == nullptr) {
            SendFail(s->peer->fd, error_msg);
            goto fail;
        }
    }
#endif

    if (!s->transport) {
        SendFail(s->peer->fd, "device offline (no transport)");
        goto fail;
    } else if (s->transport->GetConnectionState() == kCsOffline) {
        /* if there's no remote we fail the connection
         ** right here and terminate it
         */
        SendFail(s->peer->fd, "device offline (transport offline)");
        goto fail;
    }

    /* instrument our peer to pass the success or fail
    ** message back once it connects or closes, then
    ** detach from it, request the connection, and
    ** tear down
    */
    s->peer->ready = local_socket_ready_notify;
    s->peer->shutdown = nullptr;
    s->peer->close = local_socket_close_notify;
    s->peer->peer = 0;
    /* give him our transport and upref it */
    s->peer->transport = s->transport;

    connect_to_remote(s->peer, (char*)(p->data + 4));
    s->peer = 0;
    s->close(s);
    return 1;

fail:
    /* we're going to close our peer as a side-effect, so
    ** return -1 to signal that state to the local socket
    ** who is enqueueing against us
    */
    s->close(s);
    return -1;
}

static void smart_socket_ready(asocket* s) {
    D("SS(%d): ready", s->id);
}

static void smart_socket_close(asocket* s) {
    D("SS(%d): closed", s->id);
    if (s->pkt_first) {
        put_apacket(s->pkt_first);
    }
    if (s->peer) {
        s->peer->peer = 0;
        s->peer->close(s->peer);
        s->peer = 0;
    }
    free(s);
}

static asocket* create_smart_socket(void) {
    D("Creating smart socket");
    asocket* s = reinterpret_cast<asocket*>(calloc(1, sizeof(asocket)));
    if (s == NULL) fatal("cannot allocate socket");
    s->enqueue = smart_socket_enqueue;
    s->ready = smart_socket_ready;
    s->shutdown = NULL;
    s->close = smart_socket_close;

    D("SS(%d)", s->id);
    return s;
}

void connect_to_smartsocket(asocket* s) {
    D("Connecting to smart socket");
    asocket* ss = create_smart_socket();
    s->peer = ss;
    ss->peer = s;
    s->ready(s);
}

size_t asocket::get_max_payload() const {
    size_t max_payload = MAX_PAYLOAD;
    if (transport) {
        max_payload = std::min(max_payload, transport->get_max_payload());
    }
    if (peer && peer->transport) {
        max_payload = std::min(max_payload, peer->transport->get_max_payload());
    }
    return max_payload;
}

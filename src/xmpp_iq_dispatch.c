#include "xmpp_iq_dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strophe.h"
#include "log.h"
#include "xmpp_iq_buf.h"

/* ------------------------------------------------------------------ */
/*  Handler registry                                                  */
/* ------------------------------------------------------------------ */

#define MAX_HANDLERS 64

static struct {
    iq_handler_entry_t* handlers[MAX_HANDLERS];
    int                 count;
    int                 sorted;
} g_iq_registry = { { 0 }, 0, 1 };

/* Sort handlers by priority (insertion sort, descending). */
static void sort_handlers(void) {
    if (g_iq_registry.sorted) return;

    int count = g_iq_registry.count;
    for (int i = 1; i < count; i++) {
        iq_handler_entry_t* key = g_iq_registry.handlers[i];
        int j = i - 1;
        while (j >= 0 && g_iq_registry.handlers[j]->priority < key->priority) {
            g_iq_registry.handlers[j + 1] = g_iq_registry.handlers[j];
            j--;
        }
        g_iq_registry.handlers[j + 1] = key;
    }
    g_iq_registry.sorted = 1;
}

int iq_handler_register(const iq_handler_entry_t* entry) {
    if (!entry || !entry->handler || !entry->ns) {
        stump_er("iq_handler_register: invalid entry (ns=%s, handler=%p)",
                 entry ? entry->ns : "NULL",
                 entry ? entry->handler : NULL);
        return -1;
    }

    if (g_iq_registry.count >= MAX_HANDLERS) {
        stump_er("iq_handler_register: registry full (%d handlers)", MAX_HANDLERS);
        return -1;
    }

    /* Make a copy of the entry to store. */
    iq_handler_entry_t* copy = malloc(sizeof(*copy));
    if (!copy) {
        stump_er("iq_handler_register: out of memory");
        return -1;
    }
    *copy = *entry;
    g_iq_registry.handlers[g_iq_registry.count++] = copy;
    g_iq_registry.sorted = 0;  /* Need to re-sort */

    stump_d("iq_handler_register: %s (ns=%s, type=%s, prio=%d)",
            entry->name ? entry->name : "?",
            entry->ns,
            entry->type ? entry->type : "*",
            entry->priority);
    return 0;
}

int iq_handler_register_all(const iq_handler_entry_t* entries) {
    if (!entries) return 0;

    int count = 0;
    for (int i = 0; entries[i].ns; i++) {
        if (iq_handler_register(&entries[i]) != 0) {
            stump_er("iq_handler_register_all: failed at index %d", i);
            return -1;
        }
        count++;
    }
    return 0;
}

void iq_handler_unregister(const iq_handler_entry_t* entry) {
    if (!entry) return;

    for (int i = 0; i < g_iq_registry.count; i++) {
        if (strcmp(g_iq_registry.handlers[i]->ns, entry->ns) == 0 &&
            g_iq_registry.handlers[i]->handler == entry->handler) {
            /* Remove by shifting */
            free(g_iq_registry.handlers[i]);
            for (int j = i; j < g_iq_registry.count - 1; j++) {
                g_iq_registry.handlers[j] = g_iq_registry.handlers[j + 1];
            }
            g_iq_registry.count--;
            g_iq_registry.sorted = 0;
            return;
        }
    }
}

void iq_dispatch_shutdown(void) {
    for (int i = 0; i < g_iq_registry.count; i++) {
        free(g_iq_registry.handlers[i]);
    }
    g_iq_registry.count = 0;
    g_iq_registry.sorted = 1;
}

/* ------------------------------------------------------------------ */
/*  Dispatcher                                                        */
/* ------------------------------------------------------------------ */

static void send_error_iq(xmpp_session_t* ctx, const char* iq_id,
                          const char* err_type, const char* condition) {
    char buf[IQ_BUF_SIZE];
    size_t len = 0;
    int rc;
    if (iq_id) {
        rc = iq_append(buf, &len, sizeof(buf),
                       "<iq type='error' id='%s'><error type='%s'>"
                       "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                       "</error></iq>",
                       iq_id, err_type, condition);
    } else {
        rc = iq_append(buf, &len, sizeof(buf),
                       "<iq type='error'><error type='%s'>"
                       "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                       "</error></iq>",
                       err_type, condition);
    }
    if (rc == 0) iq_flush(ctx, buf, len);
}

/* Extract first element child (skip text nodes). */
static xmpp_stanza_t* get_first_element_child(xmpp_stanza_t* stanza) {
    xmpp_stanza_t* child = xmpp_stanza_get_children(stanza);
    while (child) {
        const char* ct = xmpp_stanza_get_type(child);
        if (!ct || strcmp(ct, "text") != 0) {
            return child;
        }
        child = xmpp_stanza_get_next(child);
    }
    return NULL;
}

/* Check if handler matches the given namespace and type. */
static int handler_matches(const iq_handler_entry_t* h, const char* ns, const char* type) {
    if (!h->ns || strcmp(h->ns, ns) != 0) {
        return 0;
    }
    if (!h->type) {
        return 1;  /* No type restriction */
    }
    /* Support "get|set" pipe-separated syntax without strtok: scan for exact
     * token boundaries so "get" doesn't match "getaway". */
    const char* p = h->type;
    size_t tlen = strlen(type);
    while (*p) {
        const char* end = strchr(p, '|');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg == tlen && memcmp(p, type, tlen) == 0) {
            return 1;
        }
        p = end ? end + 1 : p + seg;
    }
    return 0;
}

void iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
    if (!ctx || !stanza) {
        stump_er("iq_dispatch: null ctx or stanza");
        return;
    }

    const char* iq_type = xmpp_stanza_get_attribute(stanza, "type");
    const char* iq_id = xmpp_stanza_get_attribute(stanza, "id");

    /* Default to "get" if no type (per RFC 6120 §8.1.1). */
    if (!iq_type) {
        iq_type = "get";
    }

    /* Ignore result/error IQs silently per RFC 6120 §8.2.3. */
    if (strcmp(iq_type, "result") == 0 || strcmp(iq_type, "error") == 0) {
        return;
    }

    /* Get namespace from first element child. */
    xmpp_stanza_t* child = get_first_element_child(stanza);
    const char* ns = child ? xmpp_stanza_get_ns(child) : NULL;

    if (!ns) {
        send_error_iq(ctx, iq_id, "modify", "bad-request");
        return;
    }

    /* Sort if needed. */
    sort_handlers();

    /* Iterate handlers in priority order. */
    for (int i = 0; i < g_iq_registry.count; i++) {
        const iq_handler_entry_t* h = g_iq_registry.handlers[i];

        if (!handler_matches(h, ns, iq_type)) {
            continue;
        }

        stump_d("iq_dispatch: trying handler '%s' for ns='%s' type='%s'",
                h->name ? h->name : "?",
                ns, iq_type);

        iq_handler_result_t result = h->handler(ctx, stanza, child, iq_id);

        if (result == IQ_HANDLED) {
            stump_d("iq_dispatch: handled by '%s'", h->name ? h->name : "?");
            return;
        }
        if (result == IQ_ERROR) {
            stump_d("iq_dispatch: error from '%s'", h->name ? h->name : "?");
            return;
        }
        /* IQ_NOT_MINE: continue to next handler */
    }

    /* No handler matched — return feature-not-implemented. */
    stump_d("iq_dispatch: no handler for ns='%s' type='%s', returning error",
            ns, iq_type);

    send_error_iq(ctx, iq_id, "cancel", "feature-not-implemented");
}

int iq_dispatch_init(void) {
    return 0;
}

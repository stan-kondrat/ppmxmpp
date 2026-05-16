#ifndef XMPP_IQ_DISPATCH_H
#define XMPP_IQ_DISPATCH_H

#include "xmpp.h"

/* Opaque libstrophe stanza type. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* ------------------------------------------------------------------ */
/*  Handler return values                                              */
/* ------------------------------------------------------------------ */

/* Handler return values for iq_handler_fn:
 *   0  = handled, response sent
 *  -1  = not my namespace/type, continue to next handler
 *  >0  = error, error response already sent
 */
typedef int iq_handler_result_t;

#define IQ_HANDLED      0
#define IQ_NOT_MINE    (-1)
#define IQ_ERROR        1

/* Handler function signature.
 * - ctx:     XMPP session
 * - stanza:  Full IQ stanza (for extracting 'to', 'node' attributes)
 * - child:   First element child of IQ (the <query/>, <ping/>, etc.)
 * - iq_id:   The 'id' attribute of the IQ (may be NULL)
 * Returns: IQ_HANDLED (0), IQ_NOT_MINE (-1), or IQ_ERROR (>0)
 */
typedef iq_handler_result_t (*iq_handler_fn)(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                            xmpp_stanza_t* child, const char* iq_id);

/* ------------------------------------------------------------------ */
/*  Handler registration                                              */
/* ------------------------------------------------------------------ */

/* Priority levels for handler ordering.
 * Lower number = called later (after higher priority).
 * Interceptors (logging, validation) should use HIGH priority.
 * XEP handlers should use NORMAL priority.
 * Catch-all handlers should use LOW priority.
 */
typedef enum {
    IQ_PRIORITY_HIGHEST = 200,  /* Logging, stanza interception */
    IQ_PRIORITY_HIGH    = 150,  /* Auth checks, rate limiting */
    IQ_PRIORITY_NORMAL  = 100,  /* Standard XEP handlers */
    IQ_PRIORITY_LOW     = 50,   /* Fallback handlers */
    IQ_PRIORITY_LOWEST  = 10,   /* Catch-all for unhandled */
} iq_handler_priority_t;

/* Handler entry for registration.
 * - ns:      Namespace to match (e.g., "http://jabber.org/protocol/disco#info")
 * - type:    IQ type to match ("get", "set", or NULL for any)
 * - handler: Function pointer to call
 * - priority: Order (higher = called first; ties broken by registration order)
 * - name:    Debug name (e.g., "disco#info")
 */
typedef struct {
    const char*       ns;
    const char*       type;
    iq_handler_fn     handler;
    int               priority;
    const char*       name;
} iq_handler_entry_t;

/* Register a single handler. */
int iq_handler_register(const iq_handler_entry_t* entry);

/* Register multiple handlers (array must be NULL-terminated). */
int iq_handler_register_all(const iq_handler_entry_t* entries);

/* Unregister a handler (by pointer). */
void iq_handler_unregister(const iq_handler_entry_t* entry);

/* ------------------------------------------------------------------ */
/*  Dispatcher                                                        */
/* ------------------------------------------------------------------ */

/* Dispatch an IQ stanza to registered handlers.
 * Iterates handlers in priority order (highest first).
 * Stops on first IQ_HANDLED or IQ_ERROR return.
 */
void iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

/* Initialize the IQ handler system. Must be called before registering handlers.
 * Returns 0 on success, -1 on error.
 */
int iq_dispatch_init(void);

/* Shutdown the IQ handler system. */
void iq_dispatch_shutdown(void);

/* ------------------------------------------------------------------ */
/*  Convenience macros                                                */
/* ------------------------------------------------------------------ */

/* Macro to simplify handler registration.
 * Usage: IQ_HANDLER("namespace", "get|set", priority, my_handler)
 */
#define IQ_HANDLER(ns, type, prio, fn) \
    { ns, type, (iq_handler_fn)fn, prio, #fn }

/* Terminate a NULL-terminated handler array. */
#define IQ_HANDLERS_END \
    { NULL, NULL, NULL, 0, NULL }

#endif /* XMPP_IQ_DISPATCH_H */

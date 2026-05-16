#include "xmpp_presence.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/roster.h"
#include "strophe.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"
#include "xmpp_session.h"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Broadcast a presence stanza string to all registered sessions whose
 * bare JID matches target_bare (i.e. all resources of that user). */
static void broadcast_to_bare_jid(const char* target_bare, const char* stanza,
                                   size_t stanza_len) {
  xmpp_session_table_broadcast_to_bare(target_bare, stanza, stanza_len);
}

/* Callback context for roster_list broadcast. */
typedef struct {
  const char* stanza;
  size_t stanza_len;
} broadcast_ctx_t;

/* storage_roster_item_cb: deliver presence to all online resources of
 * each contact whose subscription is "from" or "both". */
static int broadcast_to_subscriber(const storage_roster_item_t* item, const char** groups,
                                    int group_count, void* ud) {
  (void)groups;
  (void)group_count;
  broadcast_ctx_t* bc = (broadcast_ctx_t*)ud;

  /* RFC 6121 §4.2.2: only broadcast to contacts subscribed to our presence
   * (subscription "from" or "both" on our roster means THEY subscribed to us). */
  if (strcmp(item->subscription, "from") != 0 && strcmp(item->subscription, "both") != 0) {
    return 0;
  }

  broadcast_to_bare_jid(item->contact_jid, bc->stanza, bc->stanza_len);
  return 0;
}

/* Build a <presence> or <presence type='unavailable'> stanza from a full JID.
 * Returns allocated buffer (caller must free) and sets *out_len. */
static char* build_presence_stanza(const char* from_full_jid, const char* type, size_t* out_len) {
  char* buf = malloc(IQ_BUF_SIZE);
  if (!buf) return NULL;
  size_t len = 0;
  int rc;
  if (type) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' type='%s'/>",
                   from_full_jid, type);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s'/>", from_full_jid);
  }
  if (rc != 0) {
    free(buf);
    return NULL;
  }
  *out_len = len;
  return buf;
}

/* ------------------------------------------------------------------ */
/*  Subscription helpers (RFC 6121 §3)                                */
/* ------------------------------------------------------------------ */

/* Send a roster push to all online resources of owner_bare. */
static void push_roster_to_bare_jid(const char* owner_bare, const storage_roster_item_t* item) {
  char buf[4096];
  size_t len = 0;

  /* <iq type='set' id='push-sub'><query xmlns='jabber:iq:roster'><item .../></query></iq> */
  if (iq_append(buf, &len, sizeof(buf),
                "<iq type='set' id='push-sub'>"
                "<query xmlns='jabber:iq:roster'>") != 0) {
    return;
  }
  if (item->name[0]) {
    if (iq_append(buf, &len, sizeof(buf), "<item jid='%s' name='%s' subscription='%s'",
                  item->contact_jid, item->name, item->subscription) != 0) {
      return;
    }
  } else {
    if (iq_append(buf, &len, sizeof(buf), "<item jid='%s' subscription='%s'",
                  item->contact_jid, item->subscription) != 0) {
      return;
    }
  }
  if (item->ask) {
    if (iq_append(buf, &len, sizeof(buf), " ask='subscribe'") != 0) return;
  }
  if (iq_append(buf, &len, sizeof(buf), "/></query></iq>") != 0) return;

  broadcast_to_bare_jid(owner_bare, buf, len);
}

/* Upsert a roster entry without touching the name or groups. Only updates
 * subscription and ask fields, preserving existing name. */
static int update_subscription(const char* owner, const char* contact,
                                const char* subscription, int ask) {
  storage_roster_item_t existing;
  int rc = storage_roster_get(owner, contact, &existing);

  storage_roster_item_t item;
  if (rc == 0) {
    item = existing;
  } else {
    memset(&item, 0, sizeof(item));
    strncpy(item.contact_jid, contact, sizeof(item.contact_jid) - 1);
  }
  strncpy(item.subscription, subscription, sizeof(item.subscription) - 1);
  item.ask = ask;

  sqlite3* db;
  if (storage_db_open(&db) != 0) {
    stump_er("update_subscription: cannot open database");
    return -1;
  }
  rc = storage_roster_upsert(owner, &item, NULL, 0);
  storage_db_close();
  return rc;
}

/* xmpp_session_table_for_each_resource callback: send A's current presence to B.
 * ud is a const char* pointing to the bare JID of the recipient. */
static void send_presence_to_subscriber(const char* full_jid, xmpp_write_fn write_fn,
                                        void* write_ud, const void* ud) {
  const char* to_bare = (const char*)ud;
  size_t plen = 0;
  char* pstanza = build_presence_stanza(full_jid, NULL, &plen);
  if (pstanza) {
    xmpp_session_table_broadcast_to_bare(to_bare, pstanza, plen);
    free(pstanza);
  }
  (void)write_fn;
  (void)write_ud;
}

/* Apply RFC 6121 §3.1 state transitions and send roster pushes / stanzas.
 *
 * type: "subscribe" | "subscribed" | "unsubscribe" | "unsubscribed"
 * from_bare: sender's bare JID
 * to_bare:   target's bare JID (from stanza to= attribute, stripped to bare)
 */
static void handle_subscription(xmpp_stanza_t* stanza, const char* type,
                                  const char* from_bare) {
  const char* to_attr = xmpp_stanza_get_attribute(stanza, "to");
  if (!to_attr || to_attr[0] == '\0') {
    stump_d("presence %s: missing to= attribute from %s, ignoring", type, from_bare);
    return;
  }

  /* Strip resource from to= to get target bare JID. */
  char to_bare[3073];
  xmpp_session_bare_jid(to_attr, to_bare, sizeof(to_bare));

  stump_d("presence %s: from=%s to=%s", type, from_bare, to_bare);

  /* Read current roster states from DB. */
  sqlite3* db;

  /* A's view of B (from_bare → to_bare) */
  storage_roster_item_t a_item;
  memset(&a_item, 0, sizeof(a_item));
  strncpy(a_item.contact_jid, to_bare, sizeof(a_item.contact_jid) - 1);
  strncpy(a_item.subscription, "none", sizeof(a_item.subscription) - 1);

  if (storage_db_open(&db) == 0) {
    storage_roster_item_t tmp;
    if (storage_roster_get(from_bare, to_bare, &tmp) == 0) {
      a_item = tmp;
    }
    storage_db_close();
  }

  /* B's view of A (to_bare → from_bare) */
  storage_roster_item_t b_item;
  memset(&b_item, 0, sizeof(b_item));
  strncpy(b_item.contact_jid, from_bare, sizeof(b_item.contact_jid) - 1);
  strncpy(b_item.subscription, "none", sizeof(b_item.subscription) - 1);

  if (storage_db_open(&db) == 0) {
    storage_roster_item_t tmp;
    if (storage_roster_get(to_bare, from_bare, &tmp) == 0) {
      b_item = tmp;
    }
    storage_db_close();
  }

  const char* a_sub = a_item.subscription; /* sender's current sub state for target */
  const char* b_sub = b_item.subscription; /* target's current sub state for sender */

  if (strcmp(type, "subscribe") == 0) {
    /* RFC 6121 §3.1.2: A sends subscribe to B.
     * - If A already has "to" or "both", the server may re-send but no state change.
     * - Set ask=1 on A's roster entry for B.
     * - If B does not already have subscription "from" or "both", forward the subscribe. */

    int a_already_subscribed = (strcmp(a_sub, "to") == 0 || strcmp(a_sub, "both") == 0);
    if (!a_already_subscribed) {
      /* Mark outbound request on A's side. */
      update_subscription(from_bare, to_bare, a_sub, 1 /* ask */);

      /* Re-read to get updated item for push. */
      storage_roster_item_t updated_a = a_item;
      if (storage_db_open(&db) == 0) {
        storage_roster_get(from_bare, to_bare, &updated_a);
        storage_db_close();
      }
      push_roster_to_bare_jid(from_bare, &updated_a);
    }

    /* Deliver subscribe stanza to B if they don't already have "from" or "both". */
    int b_already_has_from = (strcmp(b_sub, "from") == 0 || strcmp(b_sub, "both") == 0);
    if (!b_already_has_from) {
      char buf[512];
      size_t len = 0;
      iq_append(buf, &len, sizeof(buf), "<presence from='%s' to='%s' type='subscribe'/>",
                from_bare, to_bare);
      broadcast_to_bare_jid(to_bare, buf, len);
      /* TODO (Step 11): queue for offline B */
    }

  } else if (strcmp(type, "subscribed") == 0) {
    /* RFC 6121 §3.1.3: from_bare (B) grants subscription request from to_bare (A).
     * Variable naming: a_item = B's roster for A; b_item = A's roster for B.
     * - A's sub for B (b_sub / b_item): "none"→"to", "from"→"both"; clear ask.
     * - B's sub for A (a_sub / a_item): "none"→"from", "to"→"both". */

    const char* new_a_roster = b_sub; /* A's new sub for B */
    if (strcmp(b_sub, "none") == 0) new_a_roster = "to";
    else if (strcmp(b_sub, "from") == 0) new_a_roster = "both";

    const char* new_b_roster = a_sub; /* B's new sub for A */
    if (strcmp(a_sub, "none") == 0) new_b_roster = "from";
    else if (strcmp(a_sub, "to") == 0) new_b_roster = "both";

    int a_had_ask = b_item.ask;
    int state_changed = (new_a_roster != b_sub || new_b_roster != a_sub || a_had_ask);
    if (!state_changed) return;

    /* Update A's roster for B (to_bare → from_bare), clear ask. */
    update_subscription(to_bare, from_bare, new_a_roster, 0);
    {
      storage_roster_item_t updated_a = b_item;
      strncpy(updated_a.subscription, new_a_roster, sizeof(updated_a.subscription) - 1);
      updated_a.ask = 0;
      push_roster_to_bare_jid(to_bare, &updated_a);
    }

    /* Update B's roster for A (from_bare → to_bare). */
    update_subscription(from_bare, to_bare, new_b_roster, 0);
    {
      storage_roster_item_t updated_b = a_item;
      strncpy(updated_b.subscription, new_b_roster, sizeof(updated_b.subscription) - 1);
      updated_b.ask = 0;
      push_roster_to_bare_jid(from_bare, &updated_b);
    }

    /* Deliver subscribed stanza to A. */
    {
      char buf[512];
      size_t len = 0;
      iq_append(buf, &len, sizeof(buf), "<presence from='%s' to='%s' type='subscribed'/>",
                from_bare, to_bare);
      broadcast_to_bare_jid(to_bare, buf, len);
    }

    /* If B now receives A's presence (new_b_roster is "from" or "both") and A is online,
     * send A's current presence to B. */
    if (strcmp(new_b_roster, "from") == 0 || strcmp(new_b_roster, "both") == 0) {
      xmpp_session_table_for_each_resource(to_bare, NULL, send_presence_to_subscriber, from_bare);
    }

  } else if (strcmp(type, "unsubscribe") == 0) {
    /* RFC 6121 §3.1.4: A cancels its subscription to B.
     * - A's sub: "to"→"none", "both"→"from"; clear ask.
     * - B's sub: "from"→"none", "both"→"to".
     * Deliver unsubscribe to B; send unsubscribed back to A. */

    const char* new_a_sub = a_sub;
    if (strcmp(a_sub, "to") == 0) new_a_sub = "none";
    else if (strcmp(a_sub, "both") == 0) new_a_sub = "from";

    const char* new_b_sub = b_sub;
    if (strcmp(b_sub, "from") == 0) new_b_sub = "none";
    else if (strcmp(b_sub, "both") == 0) new_b_sub = "to";

    if (new_a_sub != a_sub || a_item.ask) {
      update_subscription(from_bare, to_bare, new_a_sub, 0);
      storage_roster_item_t updated_a = a_item;
      strncpy(updated_a.subscription, new_a_sub, sizeof(updated_a.subscription) - 1);
      updated_a.ask = 0;
      push_roster_to_bare_jid(from_bare, &updated_a);
    }

    if (new_b_sub != b_sub) {
      update_subscription(to_bare, from_bare, new_b_sub, 0);
      storage_roster_item_t updated_b = b_item;
      strncpy(updated_b.subscription, new_b_sub, sizeof(updated_b.subscription) - 1);
      updated_b.ask = 0;
      push_roster_to_bare_jid(to_bare, &updated_b);
    }

    /* Forward unsubscribe to B. */
    {
      char buf[512];
      size_t len = 0;
      iq_append(buf, &len, sizeof(buf), "<presence from='%s' to='%s' type='unsubscribe'/>",
                from_bare, to_bare);
      broadcast_to_bare_jid(to_bare, buf, len);
    }

    /* Auto-reply unsubscribed to A. */
    {
      char buf[512];
      size_t len = 0;
      iq_append(buf, &len, sizeof(buf), "<presence from='%s' to='%s' type='unsubscribed'/>",
                to_bare, from_bare);
      broadcast_to_bare_jid(from_bare, buf, len);
    }

  } else if (strcmp(type, "unsubscribed") == 0) {
    /* RFC 6121 §3.1.5: from_bare (B) refuses/cancels to_bare (A)'s subscription.
     * Variable naming: b_item = A's roster for B; a_item = B's roster for A.
     * - A's sub for B (b_sub / b_item): "to"→"none", "both"→"from"; clear ask.
     * - B's sub for A (a_sub / a_item): "from"→"none", "both"→"to".
     * Deliver unsubscribed to A. */

    const char* new_a_roster = b_sub; /* A's new sub for B */
    if (strcmp(b_sub, "to") == 0) new_a_roster = "none";
    else if (strcmp(b_sub, "both") == 0) new_a_roster = "from";

    const char* new_b_roster = a_sub; /* B's new sub for A */
    if (strcmp(a_sub, "from") == 0) new_b_roster = "none";
    else if (strcmp(a_sub, "both") == 0) new_b_roster = "to";

    if (new_a_roster != b_sub || b_item.ask) {
      update_subscription(to_bare, from_bare, new_a_roster, 0);
      storage_roster_item_t updated_a = b_item;
      strncpy(updated_a.subscription, new_a_roster, sizeof(updated_a.subscription) - 1);
      updated_a.ask = 0;
      push_roster_to_bare_jid(to_bare, &updated_a);
    }

    if (new_b_roster != a_sub) {
      update_subscription(from_bare, to_bare, new_b_roster, 0);
      storage_roster_item_t updated_b = a_item;
      strncpy(updated_b.subscription, new_b_roster, sizeof(updated_b.subscription) - 1);
      updated_b.ask = 0;
      push_roster_to_bare_jid(from_bare, &updated_b);
    }

    /* Deliver unsubscribed to A. */
    {
      char buf[512];
      size_t len = 0;
      iq_append(buf, &len, sizeof(buf), "<presence from='%s' to='%s' type='unsubscribed'/>",
                from_bare, to_bare);
      broadcast_to_bare_jid(to_bare, buf, len);
    }
  }
}

/* Broadcast presence (initial or unavailable) to:
 *   1. All contacts with subscription "from" or "both" (their online resources).
 *   2. All other online resources of the same user.
 * from_bare_jid: the sender's bare JID (used to look up the roster).
 * from_full_jid: the sender's full JID (used as the from= attribute).
 * type: NULL for available, "unavailable" for unavailable. */
static void broadcast_presence(const char* from_bare_jid, const char* from_full_jid,
                                 const char* type) {
  size_t stanza_len;
  char* stanza = build_presence_stanza(from_full_jid, type, &stanza_len);
  if (!stanza) {
    stump_er("presence: out of memory building stanza for %s", from_full_jid);
    return;
  }

  /* 1. Roster contacts. */
  sqlite3* db;
  if (storage_db_open(&db) == 0) {
    broadcast_ctx_t bc = {stanza, stanza_len};
    storage_roster_list(from_bare_jid, broadcast_to_subscriber, &bc);
    storage_db_close();
  } else {
    stump_er("presence: cannot open DB for broadcast from %s", from_bare_jid);
  }

  /* 2. Own other resources (RFC 6121 §4.2.2 / §4.4.2). */
  xmpp_session_table_broadcast_except(from_bare_jid, from_full_jid, stanza, stanza_len);

  free(stanza);
}

/* ------------------------------------------------------------------ */
/*  Presence stanza handler                                            */
/* ------------------------------------------------------------------ */

void xmpp_presence_handle(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  const char* type = xmpp_stanza_get_attribute(stanza, "type");
  const char* to   = xmpp_stanza_get_attribute(stanza, "to");

  char bare_jid[3073];
  xmpp_session_bare_jid(ctx->bound_jid, bare_jid, sizeof(bare_jid));

  /* Subscription stanzas: RFC 6121 §3.
   * Must be checked before the directed-presence path because they also
   * carry a to= attribute but require state-machine processing. */
  if (type && (strcmp(type, "subscribe") == 0 || strcmp(type, "subscribed") == 0 ||
               strcmp(type, "unsubscribe") == 0 || strcmp(type, "unsubscribed") == 0)) {
    handle_subscription(stanza, type, bare_jid);
    return;
  }

  /* Directed presence: to= attribute is set. RFC 6121 §4.6. */
  if (to && to[0] != '\0') {
    size_t len = 0;
    char* buf = malloc(IQ_BUF_SIZE);
    if (!buf) return;
    int rc;
    if (type) {
      rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' to='%s' type='%s'/>",
                     ctx->bound_jid, to, type);
    } else {
      rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' to='%s'/>",
                     ctx->bound_jid, to);
    }
    if (rc == 0 && xmpp_session_table_write(to, buf, len) != 0) {
      broadcast_to_bare_jid(to, buf, len);
    }
    free(buf);
    return;
  }

  /* Unavailable presence: RFC 6121 §4.4. */
  if (type && strcmp(type, "unavailable") == 0) {
    broadcast_presence(bare_jid, ctx->bound_jid, "unavailable");
    xmpp_session_table_unregister(ctx->bound_jid);
    return;
  }

  /* Initial presence (no type, no to): RFC 6121 §4.2.
   * Register the session now — a client is not considered available until
   * it sends initial presence, so we defer registration to this point. */
  if (!type || type[0] == '\0') {
    xmpp_session_table_register(ctx, ctx->write_fn, ctx->write_ud);

    /* RFC 6121 §4.7.2.1: parse optional <priority> child. */
    xmpp_stanza_t* prio_el = xmpp_stanza_get_child_by_name(stanza, "priority");
    if (prio_el) {
      char* prio_text = xmpp_stanza_get_text(prio_el);
      if (prio_text) {
        int prio = (int)strtol(prio_text, NULL, 10);
        if (prio < -128) prio = -128;
        if (prio > 127) prio = 127;
        xmpp_session_table_update_priority(ctx->bound_jid, prio);
        free(prio_text);
      }
    }

    broadcast_presence(bare_jid, ctx->bound_jid, NULL);
    return;
  }

  stump_d("presence: unhandled type='%s' from %s", type, ctx->bound_jid);
}

void xmpp_presence_on_disconnect(const char* bound_jid, const char* bare_jid) {
  /* Deliver unavailable only if this session had sent initial presence (is registered). */
  if (!xmpp_session_table_is_registered(bound_jid)) return;

  /* RFC 6121 §4.4.2: server generates unavailable on ungraceful disconnect. */
  broadcast_presence(bare_jid, bound_jid, "unavailable");
  xmpp_session_table_unregister(bound_jid);
}

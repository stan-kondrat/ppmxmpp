#include "storage/users.h"
#include "storage/db.h"

#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "log.h"

/* Static storage for user data returned by get_by_jid. */
static char static_jid[512];
static char static_password[512];

int storage_users_get_by_jid(const char *bare_jid, storage_user_t *user_out) {
    sqlite3 *db;
    storage_stmt_t *stmt = NULL;
    int rc;

    if (!bare_jid || !user_out) {
        return -1;
    }

    if (storage_db_open(&db) != 0) {
        return -1;
    }

    rc = storage_db_prepare(db,
        "SELECT jid, password_plain FROM users WHERE jid = ? AND disabled = 0",
        &stmt);
    if (rc != 0) {
        return -1;
    }

    storage_db_bind_text(stmt, 1, bare_jid);
    rc = storage_db_step(stmt);

    if (rc == SQLITE_ROW) {
        char *jid_copy = storage_db_column_text_copy(stmt, 0);
        char *pass_copy = storage_db_column_text_copy(stmt, 1);

        if (jid_copy && pass_copy) {
            strncpy(static_jid, jid_copy, sizeof(static_jid) - 1);
            static_jid[sizeof(static_jid) - 1] = '\0';
            strncpy(static_password, pass_copy, sizeof(static_password) - 1);
            static_password[sizeof(static_password) - 1] = '\0';

            user_out->jid = static_jid;
            user_out->password_plain = static_password;
            free(jid_copy);
            free(pass_copy);
            storage_db_reset(stmt);
            return 0;
        }

        free(jid_copy);
        free(pass_copy);
    }

    storage_db_reset(stmt);
    return 1; /* not found */
}

int storage_users_check_password(const char *bare_jid, const char *password) {
    if (!password) {
        return 0;
    }

    storage_user_t user;
    int rc = storage_users_get_by_jid(bare_jid, &user);

    if (rc != 0) {
        return 0; /* not found or error */
    }

    if (strcmp(user.password_plain, password) == 0) {
        return 1;
    }

    return 0;
}

int storage_users_create(const char *bare_jid, const char *password_plain) {
    sqlite3 *db;
    storage_stmt_t *stmt = NULL;
    int rc;
    long long now;

    if (!bare_jid || !password_plain) {
        return -1;
    }

    if (storage_db_open(&db) != 0) {
        return -1;
    }

    now = (long long)time(NULL);

    rc = storage_db_prepare(db,
        "INSERT INTO users (jid, password_plain, created_at, disabled) VALUES (?, ?, ?, 0)",
        &stmt);
    if (rc != 0) {
        return -1;
    }

    storage_db_bind_text(stmt, 1, bare_jid);
    storage_db_bind_text(stmt, 2, password_plain);
    storage_db_bind_int64(stmt, 3, now);

    rc = storage_db_step(stmt);
    storage_db_reset(stmt);

    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            stump_w("user '%s' already exists", bare_jid);
            return -1;
        }
        stump_er("failed to create user '%s': %s", bare_jid, sqlite3_errmsg(db));
        return -1;
    }

    stump_i("created user: %s", bare_jid);
    return 0;
}

int storage_users_disable(const char *bare_jid) {
    sqlite3 *db;
    storage_stmt_t *stmt = NULL;
    int rc;

    if (storage_db_open(&db) != 0) {
        return -1;
    }

    rc = storage_db_prepare(db,
        "UPDATE users SET disabled = 1 WHERE jid = ?",
        &stmt);
    if (rc != 0) {
        return -1;
    }

    storage_db_bind_text(stmt, 1, bare_jid);
    rc = storage_db_step(stmt);
    storage_db_reset(stmt);

    if (rc != SQLITE_DONE) {
        stump_er("failed to disable user '%s': %s", bare_jid, sqlite3_errmsg(db));
        return -1;
    }

    if (storage_db_changes(db) == 0) {
        stump_w("user '%s' not found for disabling", bare_jid);
        return -1;
    }

    stump_i("disabled user: %s", bare_jid);
    return 0;
}

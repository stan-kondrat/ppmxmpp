#ifndef STORAGE_DB_H
#define STORAGE_DB_H

#include <sqlite3.h>
#include <stddef.h>

#define STORAGE_MAX_VERSION 3

/* Prepared statement handle for the cache. */
typedef struct {
  sqlite3_stmt* stmt;
  char sql[512];
} storage_stmt_t;

/* Open (or create) the database, run migrations, return 0 on success. */
int storage_db_open(sqlite3** db_out);

/* Close the database and free all cached statements. */
void storage_db_close(void);

/* Look up a prepared statement by SQL text; cache it if not found.
 * Returns 0 on success, negative on error. */
int storage_db_prepare(sqlite3* db, const char* sql, storage_stmt_t** stmt_out);

/* Reset and clear all bindings on a cached statement. */
void storage_db_reset(storage_stmt_t* stmt);

/* Bind a NULL parameter at 1-based index. */
void storage_db_bind_null(storage_stmt_t* stmt, int idx);

/* Bind a 64-bit integer at 1-based index. */
void storage_db_bind_int64(storage_stmt_t* stmt, int idx, long long value);

/* Bind a string at 1-based index. */
void storage_db_bind_text(storage_stmt_t* stmt, int idx, const char* value);

/* Step the statement; returns SQLITE_ROW on success, or error code. */
int storage_db_step(storage_stmt_t* stmt);

/* Return the 64-bit integer value of the given column. */
long long storage_db_column_int64(storage_stmt_t* stmt, int col);

/* Return the text value of column 0 as a null-terminated string.
 * Caller must not free the returned pointer. */
const char* storage_db_column_text(storage_stmt_t* stmt);

/* Return the text value of the given column as a null-terminated string.
 * Caller must not free the returned pointer. */
const char* storage_db_column_text_col(storage_stmt_t* stmt, int col);

/* Return a copy of the text value of the given column.
 * Caller must free the returned pointer. */
char* storage_db_column_text_copy(storage_stmt_t* stmt, int col);

/* Return the number of rows changed by the last statement. */
int storage_db_changes(sqlite3* db);

#endif /* STORAGE_DB_H */

# ppmxmpp - xmpp-server

## Dependencies

### Build tools

- `cmake`
- `make`
- `gcc` or `clang`
- `autoconf`, `automake`, `libtool` — required by libstrophe (Autotools-based)
- `python3-jsonschema` — required by mbedtls to generate PSA crypto driver wrappers

On Void Linux:
```
sudo xbps-install -S cmake make gcc autoconf automake libtool python3-jsonschema
```

### Third-party libraries (bundled as git submodules)

| Library      | Build system | Notes                              |
|--------------|--------------|------------------------------------|
| mbedtls      | CMake        | TLS / crypto                       |
| libuv        | CMake        | Async I/O                          |
| libstrophe   | Autotools    | XMPP protocol; requires expat/libxml2 + OpenSSL |
| sqlite       | Custom make  | Embedded database                  |
| stumpless    | CMake        | Logging                            |
| cmocka       | CMake        | Unit testing                       |
| libconfig    | CMake        | Configuration file parsing         |

Initialize submodules after cloning:
```
git submodule update --init --recursive
```

## Building

```
make              # debug build (default)
make BUILD=release
make BUILD=asan
```

Build individual third-party libraries:
```
make mbedtls
make libuv
make libstrophe
make sqlite
make stumpless
make cmocka
make libconfig
```

Override static/shared per library (default: static=YES, shared=NO):
```
make third-party LIBUV_SHARED=YES SQLITE_SHARED=YES
```

## Configuration

The default config file is `config/ppmxmpp.conf`. It is created with defaults on first run if it does not exist. A different path can be passed with `--config <file>`.

Command-line arguments always override values from the config file.

### db_path

Config file: `db_path`
Argument: `--db-path`

Path to the SQLite database file. Directories are created automatically if they do not exist.
Default: `data/ppmxmpp.db`.

### log_level

Config file: `log_level`
Argument: `--log-level`

Sets the logging verbosity. Accepted values: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`.
Default: `INFO`.

## E2E Testing

Scripts live in `test_e2e/`. Each test starts a temporary server instance and cleans up after itself. Pass `--debug` to any script to keep the temp directory on failure for inspection.

Shared helpers are in `test_e2e/_common.sh` (sourced by every test, not run directly).

### Tests

- `tls_auto_generation.sh` — verifies ppmxmpp auto-generates a self-signed cert and key when TLS is enabled but no files exist; checks CN=localhost and SAN=localhost (requires: `openssl`)
- `tls_connection.sh` — generates a cert with openssl, starts ppmxmpp configured to use it, and verifies TCP reachability on the TLS port and that the server log references the configured cert path (requires: `openssl`)

### Cleaning

```
make clean
```

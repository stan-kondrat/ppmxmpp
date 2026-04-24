# xmpp-server

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

The default config file is `config/xmpp.conf`. It is created with defaults on first run if it does not exist. A different path can be passed with `--config <file>`.

Command-line arguments always override values from the config file.

### log_level

Config file: `log_level`
Argument: `--log-level`

Sets the logging verbosity. Accepted values: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`.
Default: `INFO`.

## Cleaning

```
make clean
```

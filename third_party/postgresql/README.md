# PostgreSQL Client Runtime

This directory is reserved for PostgreSQL client runtime files used by
`openems-rtdb-service` when it loads configuration from PostgreSQL.

OpenEMS does not build PostgreSQL or libpq from source. Put PostgreSQL 15 client
runtime files here when you want the project to run without relying on system
`PATH` or `LD_LIBRARY_PATH`.

## Windows x64

Expected directory:

```text
third_party/postgresql/windows/x64/bin/
```

Place `libpq.dll` in this directory. Also place the DLL files required by your
PostgreSQL client package, commonly including files such as:

```text
libssl-*.dll
libcrypto-*.dll
zlib1.dll
libiconv-2.dll
libintl-*.dll
```

The exact dependency list depends on how the PostgreSQL client package was
built. Files from a PostgreSQL 15 Windows installation `bin/` directory are a
good source.

During `cmake --install`, all `*.dll` files in this directory are copied to:

```text
install/bin/
```

## Linux x64

Expected directory:

```text
third_party/postgresql/linux/x64/lib/
```

Place `libpq.so.5` or a compatible `libpq.so` in this directory. If your target
machine does not provide libpq dependencies through the system package manager,
place the required dependency `.so` files here as well.

During `cmake --install`, `libpq.so*` files in this directory are copied to:

```text
install/lib/
```

The installed `start_rtdb_service.sh` prepends `install/lib` to
`LD_LIBRARY_PATH` before starting the service.

## Runtime Search Order

`openems-rtdb-service` searches for libpq in this order:

1. The executable directory.
2. The installed sibling `lib/` directory.
3. The current working directory's `bin/` and `lib/`.
4. The matching `third_party/postgresql/<platform>/x64/` runtime directory.
5. The operating system library search path.

If libpq cannot be loaded, the service logs a warning and falls back to CSV
configuration.

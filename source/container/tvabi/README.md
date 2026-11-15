# tvabi: the interface of the TV's libraries, as text

The browser links against 76 shared libraries that the sysroot used to copy from a webOS 10.3 TV (`TV-REPLACED.txt`
of the `sysroot-compat` snapshot of 2026-11-04, without glibc, which comes from Debian). A linker only reads a shared
library's interface, never its code. Each `<soname>.abi` file here writes that interface down:

- `soname`, `needed`: the library's SONAME and DT_NEEDED entries
- `verdef`: its symbol versions (index, base/weak flag, name, parents)
- `def name version @|@@|- type binding visibility size code|data|tls|abs:N`: a symbol it defines (`@@` default
  version, `@` old version, `-` unversioned)
- `und name version file type binding`: a symbol it references. These are kept exactly, because lld exports a
  program's symbol when a linked library references it unversioned.

`TV-SONAMES.txt` lists the library file names on that TV. `collect.sh` copies it into each build's artifacts.

There's no code or data from the TV in here, only names, versions, types and sizes, which is what a linker needs to
interoperate with the TV's libraries.

## Use

`../tvabi.py mkstubs container/tvabi OUT` builds one stub `.so` per file with GNU `as` and `ld` for ARM (Ubuntu's
`binutils-arm-linux-gnueabi`), then checks each stub against its file. A stub has the same SONAME, DT_NEEDED,
versions, symbols and references as the TV's library, but no code. A program linked against the stubs comes out the
same as one linked against the TV's files, and on the TV it loads the real libraries.

## Regenerating

Only needed if the TV's libraries change, and it needs the sysroot built from a TV (`SYSROOT_SOURCE=tv`):

```
python3 container/tvabi.py extract <sysroot-tv> <list of sonames> container/tvabi
python3 container/tvabi.py mkstubs container/tvabi /tmp/stubs      # must end with "76 match, 0 differ"
```

Last checked on 2026-11-12: all 76 stubs match with `tvabi.py check`, and with a separate `readelf` comparison of
symbols (versions, types, bindings, visibility, sizes), DT_NEEDED, SONAME and version definitions.

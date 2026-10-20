# mac2ios

`mac2ios FILE` rewrites platform version metadata in a standalone thin
little-endian Mach-O object or an archive of such objects. Input content selects
the format; the filename extension does not matter.

```sh
make                         # List targets and configuration variables.
make all                     # Build the converter explicitly.
./mac2ios library.a           # Convert an archive in place.
./mac2ios object.o            # Convert one object in place.
./mac2ios -s object.o         # Select the modern iOS simulator platform.
make check                   # Run metadata-only regression tests.
make all CC=clang CFLAGS='-O0 -g'
```

The default modern platform is iOS (`2`); `-s` selects the iOS simulator (`7`).
Legacy-only macOS minimum-version commands retain the existing conversion to
`LC_VERSION_MIN_IPHONEOS`.

When an object contains both a modern build-version command and legacy minimum
commands, the converter retains the modern command and removes the legacy ones.
It compacts only the load-command table, updates its header counts, and zeros the
freed padding. Object/archive lengths and section, relocation, and symbol offsets
remain unchanged. Malformed headers or command bounds fail before that object
is written; earlier converted archive members are not rolled back if a later
member fails. Other archive metadata is left unchanged. Fat/big-endian standalone
inputs are unsupported. The original 64 KiB header/command buffer remains the
limit; tables that do not fit are rejected before writes.

`make` and `make help` generate the target/variable list from the Makefile's `##`
descriptions. `CC`, `CFLAGS`, and `PYTHON` can be overridden on the command line.
The Nix package explicitly builds `all`, so its behavior does not depend on the
interactive default target.

Tests cover x86_64 and ARM64 objects, both platform selections, direct files and
archives, mixed command orders, layout and payload preservation, malformed input,
and idempotence. On macOS they also use Apple `nm` and `otool` to read converted
data. No Mach-O fixture is linked or executed. Set
`MAC2IOS_REAL_OBJECT=/path/to/member.o` to include a real-member regression.

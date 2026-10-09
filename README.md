# havoc

A minimal terminal emulator for Wayland on Linux.

## Install

You will need the libwayland-client and libxkbcommon development files
as well as wayland-protocols and the wayland-scanner(comes with the
`wayland-devel` package on many distros.)

Then run make:
```
make CFLAGS="-O2 -DNDEBUG"
make install
```

## Configure

havoc will search for a file called `havoc.cfg` in `$XDG_CONFIG_HOME/havoc/` first,
then in `$HOME/.config/havoc/` and last in the current working directory.

See the example `havoc.cfg` for available options.


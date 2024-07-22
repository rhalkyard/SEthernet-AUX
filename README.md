# SEthernet/30 driver for A/UX

This repo contains an A/UX driver for the
[SEthernet/30](https://github.com/rhalkyard/SEthernet/) ethernet card. It has
been tested on A/UX 3, and should also function on A/UX 2.

## Building and installation

Your kernel must have networking support enabled. This is the default, but if
you have disabled it, you will first need to run `newconfig bnet appletalk nfs`
and restart.

Copy the contents of this repo to your home directory on your A/UX machine, then
`cd` into the source directory and run:

```sh
make

su              # Become root for the next commands:
make conf       # Answer prompts about IP address etc.
shutdown -r now
```

**NOTE** By default, all files copied through the A/UX Finder silently get
encoded as AppleSingle, whether or not they have a resource fork! If you are
seeing garbage prepended to your files, use the `fcnvt` utility to strip the
AppleSingle header off. I strongly recommend configuring the Finder to use
AppleDouble instead, as described in the [A/UX
FAQ](https://christtrekker.users.sourceforge.net/doc/aux/faq.html#AdminAppleDouble).

## Uninstallation

```sh
su              # Become root for the next commands:
make unconf
shutdown -r now
```

## Details

The driver is, for the most part, a standard 4.3 BSD-style ethernet driver. The
'special' bits are:

* Support for A/UX's autoconfig mechanism (amusingly this is done with data
  structures that clearly were intended for use with Unibus systems)

* Support for Ethernet multicast (`SIOCSMAR`, `SIOCUMAR`, `SIOCGMAR` ioctls)

* Support for AppleTalk (address family `AF_APPLETALK`)

* Support for raw ethernet access (address family `AF_ETHERLINK`)

* Protocol-switch helper function for raw ethernet output (`ren_output`)

Since the compiler shipped with A/UX pre-dates ANSI C, the code looks pretty
horrendous in places. No function prototypes, `const`, or `volatile`, and
K&R-style function definitions. Lots of use of `register` too, because the
optimiser doesn't give us much help here (in fact, presumably due to the lack of
`volatile`, the sample driver makefile recommends that the optimiser be disabled
entirely for driver code!). Don't write C like this if you can help it. Don't
write C if you can help it.

In an attempt at making things clearer, I have added commented-out prototypes in
all forward declarations of functions.

## Debugging

Extra debugging messages are availble by building the driver with `make
EXTRA_CFLAGS=-DDEBUG`. This also enables export of the driver's internal
symbols, for use with the kernel debugger.

To use the kernel debugger, first configure your kernel to enable the debugger
(`newconfig debugger`), then in the A/UX Launcher, add `-s` to the launch
command line in Preferences->Booting, which enables debug symbols.

With the debugger enabled, pressing the interrupt button will drop the system
into a very rudimentary kernel debugger on the modem port (9600 baud, 8N1). Type
`?` for help, or `e` to exit the debugger and continue. Pray that you do not
have to use it.

## References and useful info

* https://github.com/neozeed/aux2 - contains a partial source tree for A/UX 2,
  and the sample driver shipped with the (presumed lost) documentation on
  developing drivers for A/UX.

* https://github.com/SolraBizna/testc - a simple example driver written for
  A/UX.

* https://github.com/cheesestraws/aux-minivnc - a VNC server for A/UX, with a
  kernel component.

* https://gist.github.com/mietek/174b27e879a7b83d502a351ea3aaa831 - an index of
  all known A/UX documentation and software, with links to digital copies where
  availbale.

* A/UX man page for `master(4)` describing the A/UX autoconfiguration mechanism
  and its configuration files.

If anybody, anywhere, has a copy of APDA publication M8037/C "Building A/UX
Device Drivers," please let me know. There is a lot that remains unclear without
it.

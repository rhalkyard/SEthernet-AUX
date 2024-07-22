###############################################################################
# pragma ident "@(#)ddk:2.0/sample/Makefile"
#
# Copyright 1990 Apple Computer, Inc.
# All Rights Reserved.
###############################################################################
#
# Makefile for SEthernet/30 driver. Derived from Driver Developer Kit sample
# makefile.
#
# You can use this makefile to make any of these goals:
#
#   all         - Compiles the driver.
#   install     - Installs the driver and configuration files into /.
#   conf        - Installs the driver and configures the system.
#   unconf      - Uninstalls and configures the system.
#   clean       - Removes all object files.
#   clobber     - Removes all files that were created during the build.
#   depend      - Builds the 'Makefile.Deps' dependencies file.
#

VERSION= 	1.0.0
DATE=

OBJS=		if_se.o

#
# This Makefile has been tested under the Bourne shell only.
#

SHELL=		/bin/sh

#
# This tells make to look in the conf directory for files.
#

VPATH=		conf

#
# All module names must be lowercase because the newunix(1M) command only
# looks for lowercase names.
#

MODULE_NAME=	se

#
# Slot Manager board ID and version, for hardware detection using autoconfig.
# Gets templated into /etc/master.d/se
#

BOARD_ID=	9635
BOARD_VERNUM=	0000

#
# Extra defines for this module
#

MODULE_DEFINES=	-DAPPLETALK -DVERSION="\"$(VERSION)\""

#
# These are the standard values that are used in making the A/UX kernel, so
# they are reproduced here for your convenience.
#

INC=		/usr/include
INCDIRS=	-I. -I$(INC)
VERBOSE=
ZFLAGS=		-Zn -F
LDFLAGS=	-N -r
LDSCRIPT=	 /usr/lib/unshared.ld
LINTFLAGS=	-bn
DEFINES=	-DKERNEL -Uvax -Usun -Dm68k -Dmc68020 -Dmc68881 -DINET \
		-DETHERLINK -DSTREAMS -UQUOTA -DPAGING -DNFS -DBSD=43 \
		-DAUTOCONFIG -DSCREEN -DSLOTS -DPASS_MAJOR -DPOSIX -DSIG43 \
		$(MODULE_DEFINES)
CFLAGS_O=	-A 2 $(DEFINES) $(INCDIRS) $(VERBOSE) $(ZFLAGS) -O
CFLAGS_NO_O=	-A 2 $(DEFINES) $(INCDIRS) $(VERBOSE) $(ZFLAGS)
CFLAGS=		$(CFLAGS_NO_O) $(EXTRA_CFLAGS)
AR=		/bin/ar
AS=		/bin/as
AUTOCONFIG=	/etc/autoconfig
CC=		/bin/cc
CPP=		/lib/cpp
KCONFIG=	/etc/kconfig
LD=		/bin/ld
LINT=		/usr/bin/lint
MKDIR=		/bin/mkdir
MV=		/bin/mv
CPP=		/lib/cpp
NEWUNIX=	/etc/newunix
NM=		/bin/nm

#
# The 'all' goal
#

all:		.FAKE $(MODULE_NAME).o

#
# The 'install' goal
# Install the scripts in their proper places. Notice that all the scripts have
# the same name, but reside in different directories.  This is the standard
# naming scheme.
#

install:	.FAKE all \
		/etc/install.d/boot.d/$(MODULE_NAME)     \
		/etc/install.d/startup.d/$(MODULE_NAME)  \
		/etc/install.d/master.d/$(MODULE_NAME)

#
# The 'conf' goal
# Install and configure the new driver into the kernel.
#

conf:		.FAKE install install_scripts autoconfig

install_scripts: .FAKE
		$(NEWUNIX) $(MODULE_NAME)

#
# The 'unconf' goal
# Uninstall and configure the driver out of the kernel.
#
uninstall:	.FAKE unconf
unconf:		.FAKE uninstall_scripts autoconfig

uninstall_scripts: .FAKE
		if [ -f /etc/boot.d/$(MODULE_NAME) ]; then \
		  $(NEWUNIX) no$(MODULE_NAME); \
		fi
		rm -f /etc/install.d/boot.d/$(MODULE_NAME)
		rm -f /etc/install.d/startup.d/$(MODULE_NAME)
		rm -f /etc/install.d/master.d/$(MODULE_NAME)

#
# Do the actual autoconfig.
#

autoconfig:	.FAKE
		# Save any kconfig changes
		@if [ -f "/unix" ]; then \
		  echo "\t# saving current kconfig values..."; \
		  $(KCONFIG) -a -n /unix \
		    > /tmp/$(MODULE_NAME)_driver_tmp; \
		else \
		  rm -f /tmp/$(MODULE_NAME)_driver_tmp; \
		fi

		#
		# Generate a new kernel. The -v, -i, -o, -m, -b, -s, and -d,
		# options aren't necessary and are here so you can see how they
		# are used.
		#

		$(AUTOCONFIG) -I -v \
		  -i /etc/config.d/newunix \
		  -o /unix \
		  -m /etc/master.d \
		  -b /etc/boot.d \
		  -S /etc/startup \
		  -s /etc/startup.d \
		  -d /etc/init.d \
		  -M /etc/master 

		#
		# Restore the saved kconfig values.
		#

		@if [ -f "/tmp/$(MODULE_NAME)_driver_tmp" ]; then \
		  echo "\t# restoring kconfig values..."; \
		  $(KCONFIG) -n /unix \
		  < /tmp/$(MODULE_NAME)_driver_tmp; \
		fi
		@rm -f /tmp/$(MODULE_NAME)_driver_tmp

		#
		# Tell the user to reboot
		#

		sync; sync; sync
		@echo
		@echo "You are now ready to shutdown and boot the new kernel.\c"
		@echo

#
# The 'clean' goal
#

clean:		.FAKE
		rm -f *.o

#
# The 'clobber' goal
#

clobber:	.FAKE clean

#
# Handle 'lint' and 'tags'
#

lint tags:	.FAKE

#
# The 'depend' goal, which updates the 'Makefile.Deps' file.
#

depend:		.FAKE .SILENT $(SOURCES)
		echo "\t# creating the Makefile.Deps file ...";\
		echo "# Automatically Generated on `date`" > Makefile.Deps;\
		set -- "$(>:=*.[csfply])";\
		for _x in $${@};do\
		  $(CPP) -Y -M$$'(INC)' $(INCDIRS) $(DEFINES) \
		    $${_x} >> Makefile.Deps;\
		done

#
# Compile the driver.  In general, drivers are not compiled with the -O flag and
# the -F flag is specified to tell the 'C' compiler not to save and restore the
# floating-point registers during function calls.  Your driver should not using
# floating-point instructions.
#

$(MODULE_NAME).o: $(OBJS)
		$(LD) $(LDFLAGS) -o $(@) $(LDSCRIPT) $(OBJS)


/etc/install.d/boot.d/$(MODULE_NAME): $(MODULE_NAME).o
		cp $(?) $(@)
		chmod 0640 $(@)
		chown bin $(@)
		chgrp bin $(@)

/etc/install.d/startup.d/$(MODULE_NAME): conf/startup
		cp $(?) $(@)
		chmod 0750 $(@)
		chown bin $(@)
		chgrp bin $(@)

# The master file MUST be world-readable!! The Mac environment reads it while
# starting AppleTalk (something to do with mapping slots to interface names)
/etc/install.d/master.d/$(MODULE_NAME): conf/master
		sed -e "s/%BOARD_ID%/$(BOARD_ID)/" \
		    -e "s/%BOARD_VERNUM%/$(BOARD_VERNUM)/" $(?) \
		    > /tmp/$(MODULE_NAME)_driver_tmp
		cp /tmp/$(MODULE_NAME)_driver_tmp $(@)
		chmod 0644 $(@)
		chown bin $(@)
		chgrp bin $(@)
		rm -f /tmp/$(MODULE_NAME)_driver_tmp


RELEASE_FILES = if_se.c if_se.h Makefile README.md conf/
release:	.FAKE sethernet-aux-$(VERSION).tar

sethernet-aux-$(VERSION).tar: $(RELEASE_FILES)
	mkdir sethernet-aux-$(VERSION)
	cp -r $(RELEASE_FILES) sethernet-aux-$(VERSION)/
	tar cvf sethernet-aux-$(VERSION).tar sethernet-aux-$(VERSION)
	rm -rf sethernet-aux-$(VERSION)

#
# The following is a "soft" include directive, which that if the included file
# does not exist, make will not halt with a fatal error.
#

Include Makefile.Deps


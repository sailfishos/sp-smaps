# This file is part of sp-smaps.
#
# Copyright (C) 2004-2007,2011 by Nokia Corporation
#
# Contact: Eero Tamminen <eero.tamminen@nokia.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# version 2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
# 02110-1301 USA

# =============================================================================
# File: Makefile
#
# Author: Simo Piiroinen
#
# -----------------------------------------------------------------------------
#
# History:
#
# 2011-11-18 Eero Tamminen
# - code should always be build with -g, use -O2 by default, remove
#   stripping, debug symbols are split by packaging, -Werror must
#   be used only in non-final builds
#
# 2011-02-07 Tommi Rantala
# - remove header & so install, install sp_smaps_sorted_totals
#
# 25-Feb-2009 Simo Piiroinen
# - fixed changelog source list
#
# 18-Jan-2007 Simo Piiroinen
# - sp_smaps_filter added to package
#
# 07-Apr-2006 Simo Piiroinen
# - added symlinking for multimode postprocessor
#
# 12-Sep-2005 Simo Piiroinen
# - added sp_smaps_diff
#
# 09-Sep-2005 Simo Piiroinen
# - added C version of sp_smaps_snapshot
#
# 07-Jul-2005 Simo Piiroinen
# - copied from sp-memtransfer
# =============================================================================

# -----------------------------------------------------------------------------
# Directory Configuration
# -----------------------------------------------------------------------------

NAME   ?= sp-smaps-visualize
DESTDIR?= /tmp/$(NAME)-testing
PREFIX ?= /usr
BIN    ?= $(PREFIX)/bin
MAN1   ?= $(PREFIX)/share/man/man1
DATA   ?= $(PREFIX)/share/sp-smaps-visualize
TESTS  ?= /opt/tests/sp-smaps

# -----------------------------------------------------------------------------
# Common Compiler Options
# -----------------------------------------------------------------------------

LDFLAGS += -g
CFLAGS += -Wall -g
CFLAGS += -std=c99
CFLAGS += -D_GNU_SOURCE
CFLAGS += -D_THREAD_SAFE

BUILD  ?= final

ifeq ($(BUILD),debug)
CFLAGS  += -O0
CFLAGS  += -fno-inline
else
ifeq ($(BUILD),gprof)
CFLAGS  += -O0
CFLAGS  += -pg
LDFLAGS += -pg
else
# default to -O2, everything except final is built with -Werror
CFLAGS  += -O2
ifneq ($(BUILD),final)
CFLAGS += -Werror
endif
endif
endif


# -----------------------------------------------------------------------------
# Measurement Package Files
# -----------------------------------------------------------------------------

BIN_MEASURE += sp_smaps_snapshot

MAN_MEASURE += $(patsubst %,%.1.gz,$(BIN_MEASURE))
ALL_MEASURE += $(BIN_MEASURE) $(MAN_MEASURE)

# -----------------------------------------------------------------------------
# Normalization Package Files
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Visualization Package Files
# -----------------------------------------------------------------------------

## QUARANTINE BIN_VISUALIZE += sp_smaps_normalize
## QUARANTINE BIN_VISUALIZE += sp_smaps_analyze
## QUARANTINE BIN_VISUALIZE += sp_smaps_diff

BIN_VISUALIZE += sp_smaps_expand
BIN_VISUALIZE += sp_smaps_filter

LNK_VISUALIZE += sp_smaps_analyze
LNK_VISUALIZE += sp_smaps_flatten
LNK_VISUALIZE += sp_smaps_normalize
LNK_VISUALIZE += sp_smaps_appvals
LNK_VISUALIZE += sp_smaps_diff

MAN_VISUALIZE += $(patsubst %,%.1.gz,$(BIN_VISUALIZE))
MAN_VISUALIZE += sp_smaps_sorted_totals.1.gz

ALL_VISUALIZE += $(BIN_VISUALIZE) $(MAN_VISUALIZE) $(LNK_VISUALIZE)

# -----------------------------------------------------------------------------
# Targets From All Packages
# -----------------------------------------------------------------------------

ALL_TARGETS += $(ALL_MEASURE) $(ALL_NORMALIZE) $(ALL_VISUALIZE)

# -----------------------------------------------------------------------------
# Top Level Targets
# -----------------------------------------------------------------------------

.PHONY: build install clean distclean mostlyclean tags

build:: $(ALL_TARGETS)

install:: install-measure install-visualize install-tests

mostlyclean::
	$(RM) *.o *~

clean:: mostlyclean
	$(RM) $(ALL_TARGETS)

distclean:: clean
	$(RM) tags

tags::
	ctags *.c *.h

.PHONY: tree stats changelog.1

tree:: install
	tree $(DESTDIR)

changelog.1:
	sp_gen_changelog >$@ *.c *.py Makefile

# -----------------------------------------------------------------------------
# Installation Macros & Rules
# -----------------------------------------------------------------------------

INSTALL_DIR = $(if $1, install -m755 -d $2/)
INSTALL_MAN = $(if $1, install -m644 $1 $2/)
INSTALL_BIN = $(if $1, install -m755 $1 $2/)

install-%-man::
	$(call INSTALL_DIR,$^,$(DESTDIR)$(MAN1))
	$(call INSTALL_MAN,$^,$(DESTDIR)$(MAN1))

install-%-bin::
	$(call INSTALL_DIR,$^,$(DESTDIR)$(BIN))
	$(call INSTALL_BIN,$^,$(DESTDIR)$(BIN))

# -----------------------------------------------------------------------------
# Compilation Macros & Rules
# -----------------------------------------------------------------------------

# C -> asm
%.q	: %.c
	$(CC) -S -o $@ $(CFLAGS) $<

# PY -> installable script
%	: %.py
	sp_fix_tool_vers -f$< -o$@
	@chmod a-w $@

# generating MAN pages
%.1.gz : %.1
	gzip -9 -c <$< >$@
%.1    : %
	sp_gen_manfile -c ./$< -o $@

# updating static libraries
lib%.a :
	$(AR) ru $@ $^

# dynamic libraries
%.so : %.o
	$(CC) -shared -o $@ $(LDFLAGS) $^ $(LD_LIBS)

# -----------------------------------------------------------------------------
# Measurement Package Installation
# -----------------------------------------------------------------------------

install-measure:: $(addprefix install-measure-,bin man)

install-measure-bin:: $(BIN_MEASURE)
install-measure-man:: $(MAN_MEASURE)

# -----------------------------------------------------------------------------
# Visualization Package Installation
# -----------------------------------------------------------------------------

install-visualize:: $(addprefix install-visualize-,bin lnk man data)

install-visualize-bin:: $(BIN_VISUALIZE) sp_smaps_sorted_totals
install-visualize-lnk:: install-visualize-bin $(addprefix $(DESTDIR)$(BIN)/,$(LNK_VISUALIZE))
install-visualize-man:: $(MAN_VISUALIZE)
	$(call INSTALL_DIR,$^,$(DESTDIR)$(MAN1))
	$(call INSTALL_MAN,$^,$(DESTDIR)$(MAN1))
	ln -fs sp_smaps_filter.1.gz $(DESTDIR)$(MAN1)/sp_smaps_analyze.1.gz
	ln -fs sp_smaps_filter.1.gz $(DESTDIR)$(MAN1)/sp_smaps_appvals.1.gz
	ln -fs sp_smaps_filter.1.gz $(DESTDIR)$(MAN1)/sp_smaps_diff.1.gz
	ln -fs sp_smaps_filter.1.gz $(DESTDIR)$(MAN1)/sp_smaps_flatten.1.gz
	ln -fs sp_smaps_filter.1.gz $(DESTDIR)$(MAN1)/sp_smaps_normalize.1.gz

install-visualize-data::
	install -m755 -d $(DESTDIR)$(DATA)
	install -m644 data/jquery.metadata.js          $(DESTDIR)$(DATA)/jquery.metadata.js
	install -m644 data/jquery.min.js               $(DESTDIR)$(DATA)/jquery.min.js
	install -m644 data/jquery.tablesorter.js       $(DESTDIR)$(DATA)/jquery.tablesorter.js
	install -m644 data/tablesorter.css             $(DESTDIR)$(DATA)/tablesorter.css
	install -m644 data/expander.js                 $(DESTDIR)$(DATA)/expander.js
	install -m644 data/asc.gif                     $(DESTDIR)$(DATA)/asc.gif
	install -m644 data/desc.gif                    $(DESTDIR)$(DATA)/desc.gif
	install -m644 data/bg.gif                      $(DESTDIR)$(DATA)/bg.gif

# -----------------------------------------------------------------------------
# Test Package Installation
# -----------------------------------------------------------------------------

install-tests::
	install -m755 -d $(DESTDIR)$(TESTS)
	install -m755 tests/snapshot_procfs.sh $(DESTDIR)$(TESTS)/snapshot_procfs.sh
	install -m755 tests/test_compact.sh    $(DESTDIR)$(TESTS)/test_compact.sh
	install -m644 tests/tests.xml          $(DESTDIR)$(TESTS)/tests.xml


# -----------------------------------------------------------------------------
# Target specific Rules
# -----------------------------------------------------------------------------

sp_smaps_snapshot : LDLIBS += -lsysperf
sp_smaps_snapshot : sp_smaps_snapshot.o

$(addprefix $(DESTDIR)$(BIN)/,$(LNK_VISUALIZE)): sp_smaps_filter
	ln -fs $< $@

$(LNK_VISUALIZE): sp_smaps_filter
	ln -fs $< $@

# -----------------------------------------------------------------------------
# Dependency Scanning
# -----------------------------------------------------------------------------

ifneq ($(MAKECMDGOALS),depend)
include .depend
endif

depend:
	gcc -MM *.c > .depend

# -----------------------------------------------------------------------------
# EOF
# -----------------------------------------------------------------------------

sp_smaps_filter : LDLIBS += -lsysperf -lm
sp_smaps_filter : sp_smaps_filter.o symtab.o

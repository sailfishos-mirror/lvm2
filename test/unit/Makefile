# Copyright (C) 2011-2026 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# NOTE: this Makefile only works as 'include' for toplevel Makefile
#       which defined all top_* variables

UNIT_SOURCE=\
	test/unit/blkio_engine_t.c \
	test/unit/blkio_t.c \
	test/unit/daemon_stray_t.c \
	test/unit/dev_cache_t.c \
	test/unit/dmeventd_t.c \
	test/unit/blkio_byterange_t.c \
	test/unit/bitset_t.c \
	test/unit/config_t.c \
	test/unit/config_cascade_t.c \
	test/unit/dmhash_t.c \
	test/unit/dmlist_t.c \
	test/unit/dmstats_list_t.c \
	test/unit/dmstatus_t.c \
	test/unit/dmeventd_mirror_t.c \
	test/unit/dmeventd_raid_t.c \
	test/unit/dmeventd_thin_t.c \
	test/unit/dmeventd_vdo_t.c \
	test/unit/dmeventd_snapshot_t.c \
	test/unit/framework.c \
	test/unit/matcher_t.c \
	test/unit/metadata_security_t.c \
	test/unit/parse_rx_t.c \
	test/unit/percent_t.c \
	test/unit/raid_region_t.c \
	test/unit/radix_tree_t.c \
	test/unit/run.c \
	test/unit/string_t.c \
	test/unit/vdo_t.c \
	test/unit/vdo_stats_t.c

UNIT_TARGET = test/unit/unit-test
UNIT_DEPENDS = $(UNIT_SOURCE:%.c=%.d)
UNIT_OBJECTS = $(UNIT_SOURCE:%.c=%.o)
CLEAN_TARGETS += $(UNIT_DEPENDS) $(UNIT_OBJECTS) \
	$(UNIT_SOURCE:%.c=%.gcda) \
	$(UNIT_SOURCE:%.c=%.gcno) \
	$(UNIT_TARGET)

lib/liblvm-internal.a: lib
libdaemon/client/libdaemonclient.a: libdaemon

# toolcontext.h requires the generated cmds.h.
test/unit/dev_cache_t.o: | include
test/unit/config_cascade_t.o: | include

$(UNIT_TARGET): $(UNIT_OBJECTS) $(LVMINTERNAL_LIBS)
	$(SHOW) "    [LD] $@"
	$(Q) $(CC) $(CFLAGS) $(LDFLAGS) $(EXTRA_EXEC_LDFLAGS) \
	      -o $@ $+ $(LVMLIBS) $(PTHREAD_LIBS) $(DL_LIBS)

.PHONY: run-unit-test unit-test
unit-test: $(UNIT_TARGET)
run-unit-test: $(UNIT_TARGET)
	@echo "Running unit tests"
	test -n "$$LVM_TEST_DIR" || LVM_TEST_DIR=$${TMPDIR:-/tmp} ;\
		TESTDIR=$$(mktemp -d -t -p "$$LVM_TEST_DIR" "LVMTEST.XXXXXXXXXX") ;\
		cd "$$TESTDIR" ;\
		LD_LIBRARY_PATH=$(abs_top_builddir)/libdm:$(abs_top_builddir)/daemons/dmeventd $(abs_top_builddir)/$(UNIT_TARGET) run ;\
		TEST_RESULT=$$? ;\
		cd $$OLDPWD ;\
		$(RM) -r "$${TESTDIR:?}" ;\
		exit $$TEST_RESULT

ifeq ("$(USE_TRACKING)","yes")
-include $(UNIT_DEPENDS)
endif

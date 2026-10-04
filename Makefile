# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds fwfilterusb.sys against the installed Wine (any version).
#
# Single-step winegcc flow per arch:
#   winegcc -b <triplet> -o foo.sys foo.c <defines/includes> \
#     -Wl,--wine-builtin -shared -Wl,--subsystem,native -lntoskrnl ...
# winegcc injects the correct header/lib paths for the active Wine;
# WINEINC only locates wine/debug.h.
#
# Usage:
#   make [all|x64|i386|clean] [WINEGCC=...] [WINEINC=...]
#   make test [TEST_PID=0020]   # full check in a throwaway wine prefix

WINEGCC ?= winegcc
# Directory D such that D/wine/debug.h exists (eselect slot, Debian, ...).
WINEINC ?= $(shell for d in /etc/eselect/wine/include /usr/include /usr/local/include; do \
	test -f "$$d/wine/debug.h" && echo "$$d" && break; done)

SRC = fwfilterusb.c

CFLAGS = -D_UCRT -D__WINESRC__ -D__WINE_PE_BUILD -fno-strict-aliasing -O2 $(addprefix -I,$(WINEINC))
LDFLAGS = -Wl,--wine-builtin -shared -Wl,--subsystem,native
LIBS = -lntoskrnl -ladvapi32 -lwinecrt0 -lucrtbase -lkernel32 -lntdll

X64_OUT = fwfilterusb-x64.sys
I386_OUT = fwfilterusb.sys

# Plain mingw test clients (no Wine headers needed).
MINGW_X64 ?= x86_64-w64-mingw32-gcc
MINGW_I386 ?= i686-w64-mingw32-gcc
TEST_X64 = test/test_fwfilterusb-x64.exe
TEST_I386 = test/test_fwfilterusb.exe

# Wheel PID the throwaway test prefix is set up with.
TEST_PID ?= 0020

$(X64_OUT): TRIPLET := x86_64-w64-mingw32
$(I386_OUT): TRIPLET := i686-w64-mingw32

.PHONY: all x64 i386 clean test test-clients

all: x64 i386

test-clients: $(TEST_X64) $(TEST_I386)

$(TEST_X64): test/test_fwfilterusb.c
	$(MINGW_X64) -o $@ $<

$(TEST_I386): test/test_fwfilterusb.c
	$(MINGW_I386) -o $@ $<

# Full check: build everything, install into a throwaway wine prefix
# (removed afterwards), reboot so the auto-start driver loads, then run
# the SDK-mimicking client. Only the 64-bit path is exercised: a 32-bit
# .sys cannot load in a WoW64 prefix (see README).
test: all test-clients
	WINEPREFIX="$$(mktemp -d)"; trap 'rm -rf "$$WINEPREFIX"' EXIT; \
	export WINEPREFIX; \
	wine wineboot -u && \
	./setup_ftec_fwfilter.sh --pid $(TEST_PID) && \
	(wineserver -k || true) && \
	wine wineboot && sleep 5 && \
	wine $(TEST_X64) | tee test/test.log; \
	grep -q "PASS: fwfilterusb.sys responded correctly" test/test.log

x64: $(X64_OUT)

i386: $(I386_OUT)

$(X64_OUT) $(I386_OUT): $(SRC)
	$(WINEGCC) -o $@ $< -b $(TRIPLET) $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(X64_OUT) $(I386_OUT) fwfilterusb-x64.o fwfilterusb.o
	rm -f $(TEST_X64) $(TEST_I386) test/test.log

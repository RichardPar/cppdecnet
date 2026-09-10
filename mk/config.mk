# mk/config.mk -- toolchain selection, build flavours and feature probes.
#
# Nothing in here builds anything; it only computes variables that
# Makefile and mk/rules.mk consume.

# ---------------------------------------------------------------- toolchain
CXX      ?= g++
AR       ?= ar
RANLIB   ?= ranlib
INSTALL  ?= install

# ------------------------------------------------------------------ layout
BUILD    ?= debug
BUILDDIR ?= build/$(BUILD)
OBJDIR    = $(BUILDDIR)/obj
BINDIR    = $(BUILDDIR)/bin
LIBDIR    = $(BUILDDIR)/lib

prefix      ?= /usr/local
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin
libdir      ?= $(exec_prefix)/lib
includedir  ?= $(prefix)/include
sysconfdir  ?= $(prefix)/etc

# ------------------------------------------------------------------- flags
# Warnings are deliberately loud: this is a port of a dynamically typed
# program, so the compiler is the only thing standing in for the tests we
# have not written yet.
WARNFLAGS := -Wall -Wextra -Wpedantic \
             -Wcast-qual -Wcast-align -Wshadow -Wnon-virtual-dtor \
             -Woverloaded-virtual -Wold-style-cast -Wdouble-promotion \
             -Wformat=2 -Wundef -Wno-unused-parameter

STDFLAGS  := -std=c++20
DEPFLAGS   = -MMD -MP -MF $(@:.o=.d)
INCFLAGS  := -Iinclude

ifeq ($(BUILD),debug)
  OPTFLAGS  := -O0 -g3 -fno-omit-frame-pointer
  DEFFLAGS  := -DDN_BUILD_DEBUG=1
  SANITIZE  ?= address,undefined
else ifeq ($(BUILD),release)
  OPTFLAGS  := -O2 -g -DNDEBUG
  DEFFLAGS  :=
  SANITIZE  ?=
else ifeq ($(BUILD),coverage)
  OPTFLAGS  := -O0 -g --coverage
  DEFFLAGS  := -DDN_BUILD_DEBUG=1
  SANITIZE  ?=
  LDLIBS    += --coverage
else
  $(error unknown BUILD "$(BUILD)"; use debug, release or coverage)
endif

ifneq ($(strip $(SANITIZE)),)
  SANFLAGS := -fsanitize=$(SANITIZE)
endif

CXXFLAGS ?= 
ALL_CXXFLAGS = $(STDFLAGS) $(OPTFLAGS) $(WARNFLAGS) $(DEFFLAGS) $(SANFLAGS) \
               $(INCFLAGS) $(FEATURE_FLAGS) $(CXXFLAGS)
ALL_LDFLAGS  = $(SANFLAGS) $(LDFLAGS)
ALL_LDLIBS   = $(LDLIBS) $(FEATURE_LIBS) -lpthread

# --------------------------------------------------------------- features
# Optional dependencies are probed once and cached in $(BUILDDIR)/features.mk
# so that a plain `make` does not re-run the compiler probes every time.
# Force a re-probe with `make features`.

# probe-header <macro> <header> <libs>
define probe-header
$(shell printf '#include <%s>\nint main(void){return 0;}\n' $(2) > /tmp/dnprobe$$$$.cc; \
        if $(CXX) $(STDFLAGS) /tmp/dnprobe$$$$.cc $(3) -o /dev/null >/dev/null 2>&1; \
        then echo yes; else echo no; fi; rm -f /tmp/dnprobe$$$$.cc)
endef

HAVE_PCAP  ?= $(call probe-header,HAVE_PCAP,pcap.h,-lpcap)
HAVE_EPOLL ?= $(call probe-header,HAVE_EPOLL,sys/epoll.h,)
HAVE_KQUEUE?= $(call probe-header,HAVE_KQUEUE,sys/event.h,)

ifeq ($(HAVE_PCAP),yes)
  FEATURE_FLAGS += -DDN_HAVE_PCAP=1
  FEATURE_LIBS  += -lpcap
endif
ifeq ($(HAVE_EPOLL),yes)
  FEATURE_FLAGS += -DDN_HAVE_EPOLL=1
endif
ifeq ($(HAVE_KQUEUE),yes)
  FEATURE_FLAGS += -DDN_HAVE_KQUEUE=1
endif

# Pretty printing.  `make V=1` shows the full command lines.
V ?= 0
ifeq ($(V),0)
  Q      := @
  say     = @printf '  %-8s %s\n' $(1) $(2)
else
  Q      :=
  say     = @:
endif

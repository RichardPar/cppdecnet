# cppdecnet -- a C++ port of PyDECnet.
#
#   make                 build the library, the daemon and the tools (release)
#   make BUILD=debug     unoptimised build with ASan and UBSan
#   make check           build and run the unit tests
#   make V=1 ...         echo full command lines
#
# The build is non-recursive: this makefile sees every source file in the
# tree, so dependency tracking across modules is exact and parallel builds
# (-j) are safe.

PROJECT := cppdecnet
VERSION := 0.1.0

include mk/config.mk

# ------------------------------------------------------------------ sources
# Library: everything under src/ except the entry points in src/main/.
LIB_SRCS   := $(filter-out src/main/%,$(shell find src -name '*.cc' 2>/dev/null))
LIB_OBJS   := $(LIB_SRCS:%.cc=$(OBJDIR)/%.o)
LIB        := $(LIBDIR)/libdecnet.a

# The daemon: src/main/pydecnet-equivalent entry point.
DAEMON_SRC := src/main/main.cc
DAEMON_OBJ := $(DAEMON_SRC:%.cc=$(OBJDIR)/%.o)
DAEMON     := $(BINDIR)/decnetd

# Command line tools: one binary per tools/*.cc (ncp, nft, tlk, dnping, ...).
TOOL_SRCS  := $(wildcard tools/*.cc)
TOOL_OBJS  := $(TOOL_SRCS:%.cc=$(OBJDIR)/%.o)
TOOLS      := $(patsubst tools/%.cc,$(BINDIR)/%,$(TOOL_SRCS))

# Tests: one binary per tests/test_*.cc, sharing tests/harness.cc.
TEST_SRCS      := $(wildcard tests/test_*.cc)
TEST_SUPPORT   := $(filter-out $(TEST_SRCS),$(wildcard tests/*.cc))
TEST_SUP_OBJS  := $(TEST_SUPPORT:%.cc=$(OBJDIR)/%.o)
TEST_OBJS      := $(TEST_SRCS:%.cc=$(OBJDIR)/%.o)
TESTS          := $(patsubst tests/%.cc,$(BINDIR)/%,$(TEST_SRCS))

ALL_OBJS := $(LIB_OBJS) $(DAEMON_OBJ) $(TOOL_OBJS) $(TEST_OBJS) $(TEST_SUP_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

# ------------------------------------------------------------------ targets
.PHONY: all lib tools daemon tests check clean distclean install uninstall \
        features help compile_commands todo

# The default target builds the tests too.  They are cheap to build, and
# leaving them out invites running a stale test binary against a library
# that has since changed -- which looks exactly like a real failure.
all: lib daemon tools tests

lib: $(LIB)

daemon: $(DAEMON)

tools: $(TOOLS)

tests: $(TESTS)

$(LIB): $(LIB_OBJS)

$(DAEMON): $(DAEMON_OBJ) $(LIB)
	$(call say,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(ALL_LDFLAGS) $^ $(ALL_LDLIBS) -o $@

$(BINDIR)/%: $(OBJDIR)/tools/%.o $(LIB)
	$(call say,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(ALL_LDFLAGS) $^ $(ALL_LDLIBS) -o $@

$(BINDIR)/test_%: $(OBJDIR)/tests/test_%.o $(TEST_SUP_OBJS) $(LIB)
	$(call say,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(ALL_LDFLAGS) $^ $(ALL_LDLIBS) -o $@

# Run every test binary, keep going, then report.  A failing test leaves a
# non-zero exit status so CI notices.
check: tests
	$(Q)fail=0; total=0; \
	for t in $(TESTS); do \
	  total=$$((total+1)); \
	  printf '  %-8s %s\n' RUN "$$t"; \
	  $$t || fail=$$((fail+1)); \
	done; \
	if [ $$total -eq 0 ]; then echo "  no tests built"; exit 0; fi; \
	if [ $$fail -ne 0 ]; then \
	  printf '\n%s of %s test binaries FAILED\n' $$fail $$total; exit 1; \
	fi; \
	printf '\nall %s test binaries passed\n' $$total

# ------------------------------------------------------------------ install
install: all
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(libdir) \
	              $(DESTDIR)$(includedir)/decnet $(DESTDIR)$(sysconfdir)/decnet
	$(INSTALL) -m 755 $(DAEMON) $(TOOLS) $(DESTDIR)$(bindir)
	$(INSTALL) -m 644 $(LIB) $(DESTDIR)$(libdir)
	$(Q)cd include && find decnet -name '*.h' -exec \
	    $(INSTALL) -D -m 644 {} $(DESTDIR)$(includedir)/{} \;

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/,$(notdir $(DAEMON) $(TOOLS)))
	rm -f $(DESTDIR)$(libdir)/$(notdir $(LIB))
	rm -rf $(DESTDIR)$(includedir)/decnet

# ----------------------------------------------------------------- utility
# compile_commands.json for clangd, without needing bear installed.
compile_commands:
	$(Q)printf '[\n' > compile_commands.json
	$(Q)first=1; for s in $(LIB_SRCS) $(DAEMON_SRC) $(TOOL_SRCS) $(TEST_SRCS) $(TEST_SUPPORT); do \
	  [ $$first -eq 1 ] || printf ',\n' >> compile_commands.json; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
	    "$(CURDIR)" "$$s" "$(CXX)" "$(ALL_CXXFLAGS)" "$$s" >> compile_commands.json; \
	done
	$(Q)printf '\n]\n' >> compile_commands.json
	$(call say,GEN,compile_commands.json)

features:
	@echo "CXX        = $(CXX)"
	@echo "BUILD      = $(BUILD)  ($(OPTFLAGS))"
	@echo "sanitizers = $(if $(SANFLAGS),$(SANITIZE),none)"
	@echo "libpcap    = $(HAVE_PCAP)"
	@echo "epoll      = $(HAVE_EPOLL)"
	@echo "kqueue     = $(HAVE_KQUEUE)"
	@echo "sources    = $(words $(LIB_SRCS)) library, $(words $(TEST_SRCS)) tests"

# Where the port still has holes.
todo:
	$(Q)grep -rn 'TODO\|PORT:' src include tests tools 2>/dev/null || echo "  none"

clean:
	$(call say,CLEAN,$(BUILDDIR))
	$(Q)rm -rf $(BUILDDIR)

distclean:
	$(call say,CLEAN,build compile_commands.json)
	$(Q)rm -rf build compile_commands.json

help:
	@echo "$(PROJECT) $(VERSION)"
	@echo
	@echo "targets:"
	@echo "  all        library + daemon + tools + tests   (default)"
	@echo "  lib        static library only"
	@echo "  tests      build the unit test binaries"
	@echo "  check      build and run the unit tests"
	@echo "  install    install under \$$prefix ($(prefix))"
	@echo "  features   show detected toolchain and options"
	@echo "  todo       list unfinished port sites"
	@echo "  clean      remove $(BUILDDIR)"
	@echo "  distclean  remove all build output"
	@echo
	@echo "variables:"
	@echo "  BUILD=debug|release|coverage   (now: $(BUILD))"
	@echo "  SANITIZE=address,undefined     (now: $(if $(SANITIZE),$(SANITIZE),none))"
	@echo "  V=1                            echo command lines"
	@echo "  prefix=/usr/local              install location"

include mk/rules.mk

-include $(DEPS)

# mk/rules.mk -- pattern rules shared by every target in the tree.

# Compile a source file into $(OBJDIR), mirroring its path under src/ or
# tests/ so that identically named files in different modules cannot
# collide.
$(OBJDIR)/%.o: %.cc
	$(call say,CXX,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(ALL_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Archive rule for the one static library we build.
$(LIBDIR)/%.a:
	$(call say,AR,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $^
	$(Q)$(RANLIB) $@

.SUFFIXES:

# Object files are chained through implicit rules to reach the link rules;
# without this make treats them as intermediates and deletes them, forcing a
# recompile on every build.
.SECONDARY:

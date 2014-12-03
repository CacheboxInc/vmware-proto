all-or-objs:
	$(MAKE) -s xml-checks >& /dev/null; \
        if [ $$? == 0 ]; then               \
            $(MAKE) beta package-release;   \
        else                                \
            $(MAKE) release beta;           \
        fi

all-or-objs-debug:
	$(MAKE) -s xml-checks >& /dev/null; \
        if [ $$? == 0 ]; then               \
            $(MAKE) release package-beta;   \
        else                                \
            $(MAKE) beta release;           \
        fi

package-release: release
	$(MAKE) -f Makefile DEV_BUILDTYPE=release package

package-beta: beta
	$(MAKE) -f Makefile DEV_BUILDTYPE=beta package

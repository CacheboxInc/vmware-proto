
VIB_TESTCERTS := $(VMW_VIB_BASE)/testcerts

CERTIFIED_CERTPATH = $(VIB_TESTCERTS)/vmware.cert
CERTIFIED_KEYPATH = $(VIB_TESTCERTS)/vmware.key

ACCEPTED_CERTPATH = $(VIB_TESTCERTS)/accepted.cert
ACCEPTED_KEYPATH = $(VIB_TESTCERTS)/accepted.key

PARTNER_CERTPATH = $(VIB_TESTCERTS)/partner.cert
PARTNER_KEYPATH = $(VIB_TESTCERTS)/partner.key

vib-bundle-level:
	@ac=`grep "<acceptance-level>.*</acceptance-level>" $(DEV_VIB_XML) | $(SED) -e "s@.*<acceptance-level>\(.*\)</acceptance-level>.*@\1@"`; \
        if [ "$${ac}" == "community" ] ; \
        then \
           $(MAKE) CERTPATH= KEYPATH= -f Makefile vib-bundle ; \
        elif [ "$${ac}" == "certified" ] ; \
        then \
           $(MAKE) CERTPATH='$(CERTIFIED_CERTPATH)' KEYPATH='$(CERTIFIED_KEYPATH)' -f Makefile vib-bundle ; \
        elif [ "$${ac}" == "accepted" ] ; \
        then \
           $(MAKE) CERTPATH='$(ACCEPTED_CERTPATH)' KEYPATH='$(ACCEPTED_KEYPATH)' -f Makefile vib-bundle ; \
        elif [ "$${ac}" == "partner" ] ; \
        then \
           $(MAKE) CERTPATH='$(PARTNER_CERTPATH)' KEYPATH='$(PARTNER_KEYPATH)' -f Makefile vib-bundle ; \
        else \
           echo 'Error: Bad acceptance level is specified in the descriptor file [$(DEV_VIB_XML)]' 1>&2 ; \
           exit 1; \
        fi;


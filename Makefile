PACKAGE_NAME = babysitter
PACKAGE_VERSION = 0.1
RELDIR = releases/$(PACKAGE_NAME)-$(PACKAGE_VERSION)
 
ifeq ($(shell uname),Linux)
	ARCH = linux
else
	ARCH = macosx
endif
 
all:
	(cd c;$(MAKE))
	(cd erl;$(MAKE))
 
clean:
	(cd c;$(MAKE) clean)
	(cd erl;$(MAKE) clean)
 
test:
	(cd c; $(MAKE) test)
	(cd erl; $(MAKE) test)
 
install: all
	(cd c; $(MAKE) install)
	(cd erl; $(MAKE) install)
 
debug:
	(cd c;$(MAKE) debug)
	(cd erl;$(MAKE) debug)

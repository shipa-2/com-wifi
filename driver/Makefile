obj-m += comwifi.o
KDIR ?= /lib/modules/$(shell uname -r)/build
ccflags-y += -I$(src)/../proto

all:
	@test -d "$(KDIR)" || { echo "no kernel headers at $(KDIR) (arch: $(shell uname -m)) - install the headers package matching this running kernel" >&2; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

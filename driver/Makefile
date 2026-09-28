# Vendored from akadata/nct6687d, pinned at 5695970.
#
# The fan control registers for MSI boards are not documented, so the driver
# carries its own build logic and its own test suite, and this file is kept
# identical to the upstream one rather than merged into the parent's. The parent
# invokes it through dkms/build.
#
# The out-of-tree targets are retained so the vendored copy can be built and
# tested on its own: `make -C driver build` and `make -C driver check`.
#
# See driver/README.md for the differences between this copy and the release it
# came from, and for the evidence behind the temperature-signedness fix.

obj-m += nct6687.o

curpwd      := $(shell pwd)
kver        ?= $(shell uname -r)

kdir    := /lib/modules/${kver}/build
objdir  := ${curpwd}/build/${kver}
objmake := ${objdir}/Makefile
objsrc  := ${objdir}/nct6687.c

# Detect if the kernel was built with clang/LLVM and use the same compiler.
KERNEL_CC := $(shell grep -qs CONFIG_CC_IS_CLANG=y ${kdir}/.config && echo clang)
ifeq ($(KERNEL_CC),clang)
  LLVM_FLAGS := LLVM=1
endif

.PHONY: check-kdir
check-kdir:
	@if [ ! -d "${kdir}" ]; then \
		echo "error: no kernel build tree at ${kdir}"; \
		echo "       install the headers for ${kver} (Arch: pacman -S linux-headers),"; \
		echo "       or build another kernel with: make kver=\$$(uname -r)"; \
		exit 1; \
	fi

# kbuild resolves a module's source relative to the object directory, so the
# source is symlinked rather than copied. The per-kernel subdirectory is not
# cosmetic: kbuild leaves .o and .cmd files that are not reusable across kernel
# versions, and mixing them links against the wrong symbols.
objmkmake:
	@mkdir -p ${objdir}
	@printf '%s\n' \
		'# Generated. Do not edit.' \
		'obj-m += nct6687.o' \
		> ${objmake}
	@ln -sfn ${curpwd}/nct6687.c ${objsrc}

.PHONY: build
build: check-kdir objmkmake
	$(MAKE) -C ${kdir} M=${objdir} $(LLVM_FLAGS) modules

# DKMS builds in tree because that is where it looks for the resulting .ko.
dkms/build: check-kdir
	$(MAKE) -C ${kdir} M=${curpwd} $(LLVM_FLAGS) modules

clean:
	@if [ -d "${kdir}" ]; then \
		for d in ${curpwd}/build/*/; do \
			[ -d "$$d" ] && $(MAKE) -C ${kdir} M=$${d%/} $(LLVM_FLAGS) clean; \
		done; \
	fi
	@rm -rf ${curpwd}/build

check:
	@./tests/smoke.sh $(kver)

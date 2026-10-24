/* the guest image (tools/ios_build.py), inside the host executable, in a
section of its __TEXT on a page boundary (16 KB): host_loader.c maps the
image's code and constants from the executable's file at the guest's
addresses, the pages the kernel checks against the app's signature (iOS
gives apps no memory that was writable and is executable). The image's
segments lie at offsets that are their addresses' modulo 64 KB (ld.lld's
alignment), so its code is on a page boundary of the file too.
port/linux/host/host_main.c loads it. */

	.section __TEXT,__halo_guest,regular
	.p2align 14
	.globl _halo_guest_image_start
	.globl _halo_guest_image_end
_halo_guest_image_start:
	.incbin GUEST_IMAGE
_halo_guest_image_end:
	.p2align 14

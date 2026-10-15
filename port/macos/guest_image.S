/* the guest image (tools/macos_build.py), inside the host executable;
port/linux/host/host_main.c loads it */

	.section __DATA,__const
	.p2align 4
	.globl _halo_guest_image_start
	.globl _halo_guest_image_end
_halo_guest_image_start:
	.incbin GUEST_IMAGE
_halo_guest_image_end:

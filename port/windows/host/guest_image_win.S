/* the guest image (tools/windows_build.py), inside the host executable;
host_main_win.c loads it */

	.section .rdata, "dr"
	.balign 16
	.globl halo_guest_image_start
	.globl halo_guest_image_end
halo_guest_image_start:
	.incbin GUEST_IMAGE
halo_guest_image_end:

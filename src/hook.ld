ENTRY(launch_hook)
SECTIONS {
 . = 0;
 .blob : { *(.text*) *(.rodata*) *(.data*) *(.bss*) }
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}

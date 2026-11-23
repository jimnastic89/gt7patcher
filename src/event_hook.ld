ENTRY(eh_entry)
/* The loader's instruction after the displaced prologue, and the log in the
 * zero padding after .bss (both RVAs; see event_hook.h). */
gt7_load_continue = 0x0086a326;
eh_log_entries = 0x07156f00 + 0x40;
eh_table = 0x07156f00 + 0x640;
SECTIONS {
 . = 0x055e7b40;
 .blob : { *(.text*) }
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) *(.data*) *(.bss*) }
}

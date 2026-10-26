# Mach-O → ELF assembly (Makefile.linux): C symbols lose the leading underscore, sym@PAGE / sym@PAGEOFF become sym / :lo12:sym, .private_extern becomes .hidden
s/(^|[^A-Za-z0-9_])_([a-z][A-Za-z0-9_]*)@PAGEOFF/\1:lo12:\2/g
s/(^|[^A-Za-z0-9_])_([a-z][A-Za-z0-9_]*)@PAGE/\1\2/g
s/(^|[^A-Za-z0-9_:])_([a-z][A-Za-z0-9_]*)/\1\2/g
s/\.private_extern/.hidden/g
s/^([[:space:]]*)\.cstring[[:space:]]*$/\1.section .rodata.str1.1,"aMS",@progbits,1/

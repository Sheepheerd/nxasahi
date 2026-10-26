/* Linux build: brackets the host's registration tables. The shared sources put them in section("__DATA,<name>") (a Mach-O segment,section pair; on ELF just a
   section with that name, which is not a C identifier, so the linker makes no __start_ / __stop_ symbols for it). getsectiondata in darwin_compat.c reads these */
SECTIONS {
  .dm_swhle   : { __dm_start_swhle   = .; KEEP(*("__DATA,swhle"))   __dm_stop_swhle   = .; }
  .dm_swforce : { __dm_start_swforce = .; KEEP(*("__DATA,swforce")) __dm_stop_swforce = .; }
  .dm_swnvn   : { __dm_start_swnvn   = .; KEEP(*("__DATA,swnvn"))   __dm_stop_swnvn   = .; }
  .dm_swgl    : { __dm_start_swgl    = .; KEEP(*("__DATA,swgl"))    __dm_stop_swgl    = .; }
} INSERT AFTER .data;

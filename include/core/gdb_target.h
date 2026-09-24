/* Laplace core: the gdb target (#227, #228). See kernel/core/gdb_target.c. */

#ifndef CORE_GDB_TARGET_H
#define CORE_GDB_TARGET_H

/* Install the target as the gdb stub's operations. */
void gdb_target_init(void);
/* The machine stopped live: the next stop reply reports that stop. */
void gdb_target_live_stop(void);

#endif /* CORE_GDB_TARGET_H */

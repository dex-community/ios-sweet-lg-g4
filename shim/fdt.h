#pragma once
#include <stdint.h>

/*
 * Just enough flattened-device-tree parsing to find what the bootloader put
 * in /chosen (initrd location) and /memory (the RAM map it handed us).
 * Big-endian, 4-byte aligned tokens.
 */

/*
 * Is this a device tree that the shim may read?
 *
 * The magic, the version and the size are checked and the two blocks
 * must be inside the tree. The bootloader hands over a pointer, and a
 * pointer that is not a tree has to be refused once, here, instead of
 * being walked.
 */
int fdt_valid(uintptr_t fdt);

/* Look up a property of a node, by full path ("/memory", "/chosen"). */
int fdt_prop_u64(uintptr_t fdt, const char *path, const char *prop, uint64_t *out);
int fdt_prop_bytes(uintptr_t fdt, const char *path, const char *prop,
		   uintptr_t *out, uint32_t *out_len);
/* Read a multi-cell property (reg = <addr size>). Returns the cell count. */
int fdt_prop_cells(uintptr_t fdt, const char *path, const char *prop,
		   uint64_t *out, int max);

/*
 * Walk every property of a node. The callback gets the node path, the
 * property name, its length and a pointer to its value. Returning non-zero
 * from the callback stops the walk and that value is returned.
 */
typedef int (*fdt_prop_cb)(const char *node_path, const char *name,
			   uint32_t len, const uint8_t *val, void *ctx);
int fdt_walk_props(uintptr_t fdt, const char *path, fdt_prop_cb cb, void *ctx);
/* path "" matches every node in the tree. */
/* Same, but also visits the properties of every descendant of path. */
int fdt_walk_props_under(uintptr_t fdt, const char *path, fdt_prop_cb cb, void *ctx);

/* Convenience: the initrd location the bootloader published in /chosen. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * The RAM map of the bootloader (/memory).
 *
 * The callback gets one (address, size) pair at a time. Returning
 * non-zero from it stops the walk. It is the map that says where the
 * shim may write: everything else may be the secure region, where a
 * write is a data abort.
 */
typedef int (*fdt_mem_cb)(uint64_t addr, uint64_t size, void *ctx);
int fdt_memory_map(uintptr_t fdt, fdt_mem_cb cb, void *ctx);

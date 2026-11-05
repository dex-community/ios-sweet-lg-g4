#pragma once
#include <stdint.h>

/*
 * Just enough flattened-device-tree parsing to find what the bootloader put
 * in /chosen (initrd location) and /memory (the RAM map it handed us).
 * Big-endian, 4-byte aligned tokens.
 */

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

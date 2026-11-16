/*
 * Just enough flattened-device-tree parsing to find what the bootloader put
 * in /chosen (initrd location) and /memory (the RAM map it handed us).
 * Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

#define FDT_VERSION	17	/* the version this parser understands */
#define FDT_V17_SIZE	40	/* the size of the header of a v17 tree */

/* The biggest tree that is accepted: the LK hands over a tree of a few
 * tens of kilobytes, and a bigger one means that the pointer is not a
 * tree at all. */
#define FDT_MAX_SIZE	(4UL * 1024 * 1024)

#define MAX_DEPTH	32
#define PATH_MAX	160

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int starts_with(const char *s, const char *prefix)
{
	while (*prefix && *s == *prefix)
		s++, prefix++;
	return *prefix == 0;
}

static int slen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

/*
 * Walk the structure block once, calling cb for every property of every node
 * that matches want_path. When under is set, descendants of want_path match
 * too. Returning non-zero from cb stops the walk and that value is returned.
 */
/*
 * The walk of the structure block.
 *
 * EVERY READ IS INSIDE A BLOCK: the tree comes from the bootloader and
 * it may be anything, so a malformed tree must not make the shim read
 * past the end of it (and into the secure region, where a read is a
 * data abort).
 *
 * The bounds are the ones of the header:
 *
 *   off_dt_struct + size_dt_struct   the structure block
 *   off_dt_strings + size_dt_strings the strings block
 */
static int walk(uintptr_t fdt, const char *want_path, int under,
		fdt_prop_cb cb, void *ctx)
{
	uintptr_t p = fdt + be32(fdt + 8);	/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	uintptr_t struct_end = fdt + be32(fdt + 8) + be32(fdt + 36);
	uintptr_t strings_end = fdt + be32(fdt + 12) + be32(fdt + 32);
	char path[PATH_MAX];
	int saved[MAX_DEPTH];
	int depth = 0;
	int pathlen = 0;
	int matched = 0;

	path[0] = 0;
	for (;;) {
		uint32_t tok;

		if (p + 4 > struct_end)
			return 0;	/* the tree ends in the middle of it */
		tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			int n = 0;
			if (depth >= MAX_DEPTH)
				return 0;
			if (p >= struct_end)
				return 0;
			saved[depth] = pathlen;
			if (depth > 0)
				path[pathlen++] = '/';
			while (name[n] && pathlen < PATH_MAX - 1)
				path[pathlen++] = name[n++];
			path[pathlen] = 0;
			depth++;
			matched = want_path[0] == 0 || streq(path, want_path) ||
				  (under && starts_with(path, want_path) &&
				   path[slen(want_path)] == '/');
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (depth == 0)
				return 0;
			depth--;
			pathlen = saved[depth];
			path[pathlen] = 0;
			matched = depth > 0 &&
				  (want_path[0] == 0 || streq(path, want_path) ||
				   (under && starts_with(path, want_path) &&
				    path[slen(want_path)] == '/'));
			break;
		case FDT_PROP: {
			uint32_t len, nameoff;
			uintptr_t val;

			if (p + 8 > struct_end)
				return 0;
			len = be32(p);
			nameoff = be32(p + 4);
			val = p + 8;
			if (val + len > struct_end)
				return 0;
			if (strings + nameoff >= strings_end)
				return 0;
			if (matched) {
				int r = cb(path, (const char *)(strings + nameoff), len,
					   (const uint8_t *)val, ctx);
				if (r)
					return r;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

struct prop_ctx {
	const char *prop;
	uint64_t u64;
	uintptr_t bytes;
	uint32_t len;
	int found;
};

static int prop_cb(const char *node_path, const char *name, uint32_t len,
		   const uint8_t *val, void *ctx)
{
	struct prop_ctx *c = ctx;

	(void)node_path;
	if (!streq(name, c->prop))
		return 0;
	c->found = 1;
	c->len = len;
	c->bytes = (uintptr_t)val;
	if (len == 4)
		c->u64 = be32((uintptr_t)val);
	else if (len == 8)
		c->u64 = (uint64_t)be32((uintptr_t)val) << 32 | be32((uintptr_t)val + 4);
	return 1;
}

/*
 * Is this a tree that the shim may read?
 *
 * The magic, the version and the size are checked, and the two blocks
 * must be inside the tree. A pointer that is not a tree is refused
 * here, once, instead of being walked.
 */
int fdt_valid(uintptr_t fdt)
{
	uint32_t total, off_struct, off_strings, size_struct, size_strings;

	if (!fdt)
		return 0;
	if (be32(fdt) != FDT_MAGIC)
		return 0;
	if (be32(fdt + 20) != FDT_VERSION)
		return 0;
	if (be32(fdt + 24) > FDT_VERSION)
		return 0;
	total = be32(fdt + 4);
	if (total < FDT_V17_SIZE || total > FDT_MAX_SIZE)
		return 0;
	off_struct = be32(fdt + 8);
	off_strings = be32(fdt + 12);
	size_struct = be32(fdt + 36);
	size_strings = be32(fdt + 32);
	if (off_struct + size_struct > total)
		return 0;
	if (off_strings + size_strings > total)
		return 0;
	return 1;
}

int fdt_prop_u64(uintptr_t fdt, const char *path, const char *prop, uint64_t *out)
{
	struct prop_ctx c = { .prop = prop };

	if (!fdt_valid(fdt))
		return 0;
	walk(fdt, path, 0, prop_cb, &c);
	if (!c.found || (c.len != 4 && c.len != 8))
		return 0;
	*out = c.u64;
	return 1;
}

int fdt_prop_bytes(uintptr_t fdt, const char *path, const char *prop,
		   uintptr_t *out, uint32_t *out_len)
{
	struct prop_ctx c = { .prop = prop };

	if (!fdt_valid(fdt))
		return 0;
	walk(fdt, path, 0, prop_cb, &c);
	if (!c.found)
		return 0;
	*out = c.bytes;
	*out_len = c.len;
	return 1;
}

int fdt_prop_cells(uintptr_t fdt, const char *path, const char *prop,
		   uint64_t *out, int max)
{
	uintptr_t val;
	uint32_t len;

	if (!fdt_prop_bytes(fdt, path, prop, &val, &len))
		return 0;
	int cells = len / 4;
	if (cells > max)
		cells = max;
	for (int i = 0; i < cells; i++)
		out[i] = be32(val + 4 * i);
	return cells;
}

int fdt_walk_props(uintptr_t fdt, const char *path, fdt_prop_cb cb, void *ctx)
{
	if (!fdt_valid(fdt))
		return 0;
	return walk(fdt, path, 0, cb, ctx);
}

int fdt_walk_props_under(uintptr_t fdt, const char *path, fdt_prop_cb cb, void *ctx)
{
	if (!fdt_valid(fdt))
		return 0;
	return walk(fdt, path, 1, cb, ctx);
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	return fdt_prop_u64(fdt, "/chosen", prop, out);
}

/*
 * The RAM map of the bootloader (/memory).
 *
 * The node has one "reg" property with the (address, size) pairs, in
 * cells of four bytes. The number of the address and of the size cells
 * is the one of the root node, and the LK fills the node in before it
 * jumps, so the map it publishes is the real one.
 *
 * The callback gets the pairs one by one: the tree is walked once, and
 * the caller decides what to do with them.
 */
int fdt_memory_map(uintptr_t fdt, fdt_mem_cb cb, void *ctx)
{
	struct prop_ctx c = { .prop = "reg" };
	uint64_t cells[64];
	int n, i;

	if (!fdt_valid(fdt))
		return 0;
	if (!fdt_prop_bytes(fdt, "/memory", "reg", &c.bytes, &c.len))
		return 0;
	n = (int)(c.len / 4);
	if (n > 64)
		n = 64;
	for (i = 0; i < n; i++)
		cells[i] = be32(c.bytes + 4 * i);

	/*
	 * The pairs are <address size>, but a node with two cells of
	 * address and two of size has <0 address 0 size> instead. Both are
	 * accepted: the one that looks like an address wins.
	 */
	for (i = 0; i + 1 < n; i += 2) {
		uint64_t addr = cells[i];
		uint64_t size = cells[i + 1];

		if (addr == 0 && i + 3 < n && cells[i + 2] == 0) {
			addr = cells[i + 1];
			size = cells[i + 3];
			i += 2;
		}
		if (!size)
			continue;
		if (cb(addr, size, ctx))
			return 1;
	}
	return 0;
}

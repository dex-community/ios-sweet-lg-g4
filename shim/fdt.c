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
static int walk(uintptr_t fdt, const char *want_path, int under,
		fdt_prop_cb cb, void *ctx)
{
	uintptr_t p = fdt + be32(fdt + 8);	/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	char path[PATH_MAX];
	int saved[MAX_DEPTH];
	int depth = 0;
	int pathlen = 0;
	int matched = 0;

	path[0] = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			int n = 0;
			if (depth >= MAX_DEPTH)
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
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
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

int fdt_prop_u64(uintptr_t fdt, const char *path, const char *prop, uint64_t *out)
{
	struct prop_ctx c = { .prop = prop };

	if (!fdt || be32(fdt) != FDT_MAGIC)
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

	if (!fdt || be32(fdt) != FDT_MAGIC)
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
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	return walk(fdt, path, 0, cb, ctx);
}

int fdt_walk_props_under(uintptr_t fdt, const char *path, fdt_prop_cb cb, void *ctx)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	return walk(fdt, path, 1, cb, ctx);
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	return fdt_prop_u64(fdt, "/chosen", prop, out);
}

#include "svn.h"
#include "commit.h"
#include "attr.h"
#include "cache-tree.h"
#include "unpack-trees.h"
#include "quote.h"
#include <zlib.h>

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

static const char *cmt_to_hex(struct commit *c) {
	return sha1_to_hex(c ? c->object.sha1 : null_sha1);
}

static const unsigned char *cmt_tree(struct commit *c) {
	if (parse_commit(c))
		die("invalid commit %s", cmt_to_hex(c));
	return c->tree->object.sha1;
}

void svn_checkout_index(struct index_state *idx, struct commit *c) {
	if (c && idx->cache_tree
		&& cache_tree_fully_valid(idx->cache_tree)
		&& !hashcmp(idx->cache_tree->sha1, cmt_tree(c)))
	{
		return;
	}

	discard_index(idx);

	if (c) {
		struct unpack_trees_options op;
		struct tree_desc desc;

		if (parse_commit(c) || parse_tree(c->tree))
			die("invalid commit %s", cmt_to_hex(c));

		memset(&op, 0, sizeof(op));
		op.src_index = idx;
		op.dst_index = idx;
		op.index_only = 1;

		init_tree_desc(&desc, c->tree->buffer, c->tree->size);
		unpack_trees(1, &desc, &op);
	}

	/* force a reset of the attr stack */
	if (idx == &the_index) {
		git_attr_set_direction(GIT_ATTR_INDEX, NULL);
		git_attr_set_direction(GIT_ATTR_INDEX, idx);
	}
}

/* cleans the path to how remote-svn uses paths. Either empty or:
 * 1. Leading /
 * 2. No trailing /
 * 3. No //
 */
void clean_svn_path(struct strbuf *b) {
	char* p;
	while (b->len && b->buf[0] == '/') {
		strbuf_remove(b, 0, 1);
	}

	while ((p = strstr(b->buf, "//")) != NULL) {
		strbuf_remove(b, p - b->buf, 1);
	}

	while (b->len && b->buf[b->len-1] == '/') {
		strbuf_setlen(b, b->len-1);
	}

	if (b->len) {
		strbuf_insert(b, 0, "/", 1);
	}
}

static const char *svn_field(struct commit *c, const char *field) {
	char *p;

	if (!c) return "";
	if (parse_commit(c)) goto err;

	p = strstr(c->buffer, field);
	return p ? p + strlen(field) : "";
err:
	die("invalid svn commit %s", cmt_to_hex(c));
}

int get_svn_revision(struct commit *c) {
	return atoi(svn_field(c, "\nrevision "));
}

const char *get_svn_path(struct commit *c) {
	static struct strbuf buf = STRBUF_INIT;
	const char *p = svn_field(c, "\npath ");
	strbuf_reset(&buf);
	strbuf_add(&buf, p, strcspn(p, "\n"));
	clean_svn_path(&buf);
	return buf.buf;
}

/* mergeinfo is the implicit mergeinfo ranges from the revision trail
 * and copy history */
struct mergeinfo *get_mergeinfo(struct commit *c) {
	static struct strbuf buf = STRBUF_INIT;
	const char *p = svn_field(c, "\nmergeinfo ");
	strbuf_reset(&buf);
	unquote_c_style(&buf, p, NULL);
	return parse_svn_mergeinfo(buf.buf);
}

/* svn:mergeinfo is a copy of the explicit mergeinfo stored in svn */
struct mergeinfo *get_svn_mergeinfo(struct commit *c) {
	static struct strbuf buf = STRBUF_INIT;
	const char *p = svn_field(c, "\nsvn:mergeinfo ");
	strbuf_reset(&buf);
	unquote_c_style(&buf, p, NULL);
	return parse_svn_mergeinfo(buf.buf);
}

struct mergeinfo {
	struct range *ranges;
	struct strbuf buf;
	unsigned int dirty : 1;
};

struct range {
	struct range *next;
	char *path;
	int from, to;
};

static int cmp_range(const struct range *ra, const struct range *rb) {
	int d = strcmp(ra->path, rb->path);
	return d ? d : ra->from - rb->from;
}

static void compact_svn_mergeinfo(struct mergeinfo *m) {
	struct range **mr = &m->ranges;
	while (*mr && (*mr)->next) {
		struct range *a = *mr;
		struct range *b = (*mr)->next;

		if (strcmp(a->path, b->path) || a->to < b->from-1) {
			mr = &b->next;
		} else {
			*mr = b;
			b->from = a->from;
			free(a->path);
			free(a);
		}
	}
}

/* merge add into m, but don't include any ranges in rm */
void merge_svn_mergeinfo(struct mergeinfo *m, const struct mergeinfo *add, const struct mergeinfo *rm) {
	const struct range *ar = add->ranges, *rr = rm ? rm->ranges : NULL;
	struct range **mr = &m->ranges;

	while (ar) {
		struct range *r = xmalloc(sizeof(*r));
		r->from = ar->from;
		r->to = ar->to;

		while (rr && r->from <= r->to) {
			int d = cmp_range(rr, r);

			if (d < 0) {
				rr = rr->next;
			} else if (d == 0) {
				r->from = rr->to + 1;
			} else if (rr->from <= r->to) {
				r->to = rr->from - 1;
				break;
			} else {
				break;
			}
		}

		if (r->from > r->to) {
			free(r);
			continue;
		}

		while (*mr && cmp_range(*mr, r) < 0) {
			mr = &(*mr)->next;
		}

		r->path = xstrdup(ar->path);
		r->next = *mr;
		*mr = r;
		m->dirty = 1;
	}

	compact_svn_mergeinfo(m);
}

void add_svn_mergeinfo(struct mergeinfo *m, const char *path, int from, int to) {
	struct range newrange = {NULL, (char*)path, from, to};
	struct mergeinfo newinfo = {&newrange};
	merge_svn_mergeinfo(m, &newinfo, NULL);
}

void free_svn_mergeinfo(struct mergeinfo *m) {
	if (m) {
		while (m->ranges) {
			struct range *r = m->ranges;
			m->ranges = r->next;
			free(r->path);
			free(r);
		}
		strbuf_release(&m->buf);
		free(m);
	}
}

struct mergeinfo *parse_svn_mergeinfo(const char *info) {
	/* format is path:rev,frev-trev\npath2:.... */
	struct mergeinfo *m = xcalloc(1, sizeof(*m));
	const char *p = info;

	while (*p) {
		const char *line, *colon;

		line = p;
		colon = strchr(p, ':');
		if (!colon) continue;

		for (p = colon; *p != '\0' && *p != '\n';) {
			struct range *r, **mr;

			r = xmalloc(sizeof(*r));
			r->from = strtol(p, (char**)&p, 10);

			if (*p == '-') {
				r->to = strtol(p, (char**)&p, 10);
			} else {
				r->to = r->from;
			}

			if (*p == ',')
				p++;

			mr = &m->ranges;
			while (*mr && cmp_range(*mr, r) < 0) {
				mr = &(*mr)->next;
			}

			r->path = xmemdupz(line, colon - line);
			r->next = *mr;
			*mr = r;
		}

		if (*p == '\0')
			break;

		p++;
	}

	compact_svn_mergeinfo(m);
	strbuf_addstr(&m->buf, info);
	return m;
}

const char *make_svn_mergeinfo(struct mergeinfo *m) {
	struct range *r;
	char *path = NULL;

	if (!m->dirty) {
		return m->buf.buf;
	}

	strbuf_reset(&m->buf);

	for (r = m->ranges; r != NULL; r = r->next) {
		if (strcmp(r->path, path)) {
			strbuf_complete_line(&m->buf);
			strbuf_addstr(&m->buf, r->path);
			strbuf_addch(&m->buf, ':');
			path = r->path;
		} else {
			strbuf_addch(&m->buf, ',');
		}

		if (r->from == r->to) {
			strbuf_addf(&m->buf, "%d", r->from);
		} else {
			strbuf_addf(&m->buf, "%d-%d", r->from, r->to);
		}
	}

	m->dirty = 0;
	return m->buf.buf;
}

struct commit* svn_commit(struct commit *c) {
	if (parse_commit(c))
		die("invalid svn commit %s", cmt_to_hex(c));
	return c->parents ? c->parents->item : NULL;
}

struct commit* svn_parent(struct commit* c) {
	if (parse_commit(c))
		die("invalid svn commit %s", cmt_to_hex(c));
	return c->parents && c->parents->next ? c->parents->next->item : NULL;
}

int write_svn_commit(
	struct commit *svn, struct commit *git,
	const unsigned char *tree, const char *ident,
	const char *path, int rev,
	struct mergeinfo *mi, struct mergeinfo *mi_svn,
	unsigned char *ret)
{
	int err;
	struct strbuf buf = STRBUF_INIT;
	strbuf_addf(&buf, "tree %s\nparent %s\n",
			sha1_to_hex(tree), cmt_to_hex(git));

	if (svn)
		strbuf_addf(&buf, "parent %s\n", cmt_to_hex(svn));

	strbuf_addf(&buf,
		"author %s\n"
		"committer %s\n"
		"revision %d\n",
		ident, ident, rev);

	if (path && *path)
		strbuf_addf(&buf, "path %s\n", path);

	if (mi) {
		strbuf_addstr(&buf, "mergeinfo \"");
		quote_c_style(make_svn_mergeinfo(mi), &buf, NULL, 1);
		strbuf_addstr(&buf, "\"\n");
	}

	if (mi_svn) {
		strbuf_addstr(&buf, "svn:mergeinfo \"");
		quote_c_style(make_svn_mergeinfo(mi_svn), &buf, NULL, 1);
		strbuf_addstr(&buf, "\"\n");
	}

	strbuf_addch(&buf, '\n');
	err = write_sha1_file(buf.buf, buf.len, "commit", ret);
	strbuf_release(&buf);
	return err;
}


#define MAX_VARINT_LEN 9

static unsigned char* parse_varint(unsigned char *p, unsigned char *e, size_t *v) {
	*v = 0;
	for (;;) {
		if (p == e || *v > (maximum_unsigned_value_of_type(size_t) >> 7))
			die("invalid svndiff");

		*v = (*v << 7) | (*p & 0x7F);

		if (!(*(p++) & 0x80))
			return p;
	}
}

static unsigned char* encode_varint(unsigned char* p, size_t n) {
	if (n >= (INT64_C(1) << 56)) *(p++) = ((n >> 56) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 49)) *(p++) = ((n >> 49) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 42)) *(p++) = ((n >> 42) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 35)) *(p++) = ((n >> 35) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 28)) *(p++) = ((n >> 28) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 21)) *(p++) = ((n >> 21) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 14)) *(p++) = ((n >> 14) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 7)) *(p++) = ((n >> 7) & 0x7F) | 0x80;
	*(p++) = n & 0x7F;
	return p;
}

static size_t encoded_length(size_t n) {
	unsigned char b[MAX_VARINT_LEN];
	return encode_varint(b, n) - b;
}

#define FROM_SOURCE (0 << 6)
#define FROM_TARGET (1 << 6)
#define FROM_NEW    (2 << 6)

static unsigned char* parse_instruction(unsigned char *p, unsigned char *e, int* ins, size_t* off, size_t* len) {
	int hdr;

	if (p >= e) die("invalid svndiff");
	hdr = *p++;

	*len = hdr & 0x3F;
	if (*len == 0) {
		p = parse_varint(p, e, len);
	}

	*ins = hdr & 0xC0;
	*off = 0;
	if (*ins == FROM_SOURCE || *ins == FROM_TARGET) {
		p = parse_varint(p, e, off);
	}

	return p;
}

#define MAX_INS_LEN (1 + 2 * MAX_VARINT_LEN)

static unsigned char* encode_instruction(unsigned char* p, int ins, size_t off, size_t len) {
	if (len < 0x3F) {
		*(p++) = ins | len;
	} else {
		*(p++) = ins;
		p = encode_varint(p, len);
	}

	if (ins == FROM_SOURCE || ins == FROM_TARGET) {
		p = encode_varint(p, off);
	}

	return p;
}

static unsigned char *parse_svndiff_chunk(unsigned char *p, size_t *sz, struct strbuf *buf, int ver) {
	unsigned char *e = p + *sz;
	size_t inflated = *sz;
	z_stream z;

	if (ver > 0) {
		p = parse_varint(p, e, &inflated);
	}

	*sz = inflated;
	if (p + inflated == e)
		return p;

	memset(&z, 0, sizeof(z));
	inflateInit(&z);

	strbuf_grow(buf, inflated);

	z.next_in = p;
	z.avail_in = e - p;
	z.next_out = (unsigned char*) buf->buf;
	z.avail_out = inflated;

	if (inflate(&z, Z_FINISH) != Z_STREAM_END) {
		die("zlib error");
	}
	strbuf_setlen(buf, inflated - z.avail_out);
	inflateEnd(&z);

	return (unsigned char*) buf->buf;
}

static unsigned char* apply_svndiff_win(struct strbuf *tgt, const void *src, size_t sz, unsigned char *d, unsigned char *e, int ver) {
	struct strbuf insbuf = STRBUF_INIT;
	struct strbuf databuf = STRBUF_INIT;
	unsigned char *insp, *inse, *datap, *datae;
	size_t srco, srcl, tgtl, insl, datal, w = 0;

	d = parse_varint(d, e, &srco);
	d = parse_varint(d, e, &srcl);
	d = parse_varint(d, e, &tgtl);
	d = parse_varint(d, e, &insl);
	d = parse_varint(d, e, &datal);

	if (unsigned_add_overflows(srco, srcl) || srco + srcl > sz)
		goto err;

	if (unsigned_add_overflows(insl, datal) || insl + datal > e - d)
		goto err;

	insp = d;
	datap = insp + insl;
	d = datap + datal;

	insp = parse_svndiff_chunk(insp, &insl, &insbuf, ver);
	datap = parse_svndiff_chunk(datap, &datal, &databuf, ver);

	inse = insp + insl;
	datae = datap + datal;

	strbuf_grow(tgt, tgt->len + tgtl);

	while (insp < inse) {
		size_t off, len;
		ssize_t tgtr;
		int ins;

		insp = parse_instruction(insp, inse, &ins, &off, &len);

		switch (ins) {
		case FROM_SOURCE:
			if (off > srcl || len > srcl - off) goto err;
			strbuf_add(tgt, (char*) src + srco + off, len);
			break;

		case FROM_TARGET:
			tgtr = min(w - off, len);
			if (tgtr <= 0) goto err;

			off = tgt->len - w + off;

			/* len may be greater than tgtr. In this case we
			 * just repeat [tgto,tgto+tgtr]
			 */
			while (len) {
				int n = min(len, tgtr);
				strbuf_add(tgt, tgt->buf + off, n);
				len -= n;
			}
			break;

		case FROM_NEW:
			if (datae - datap < len) goto err;
			strbuf_add(tgt, datap, len);
			datap += len;
			break;

		default:
			goto err;
		}

		w += len;
	}

	if (w != tgtl || datap != datae) goto err;

	strbuf_release(&insbuf);
	strbuf_release(&databuf);
	return d;
err:
	die("invalid svndiff");
}

void apply_svndiff(struct strbuf *tgt, const void *src, size_t sz, const void *delta, size_t dsz) {
	unsigned char *d = (unsigned char*) delta;
	unsigned char *e = d + dsz;
	int ver;

	strbuf_reset(tgt);

	if (dsz < 4 || memcmp(d, "SVN", 3))
		goto err;

	ver = d[3];
	if (ver > 1)
		goto err;

	d += 4;

	while (d < e) {
		d = apply_svndiff_win(tgt, src, sz, d, e, ver);
	}

	return;

err:
	die(_("invalid svndiff"));
}

#define MAX_WINDOW_SIZE (64*1024)

void create_svndiff(struct strbuf *diff, const void *src, size_t sz) {
	z_stream z;
	memset(&z, 0, sizeof(z));
	deflateInit(&z, Z_DEFAULT_COMPRESSION);

	/* leave some room for the headers */
	strbuf_grow(diff, diff->len + 64 + deflateBound(&z, sz) * 1025/1024);
	strbuf_add(diff, "SVN\1", 4);

	while (sz > 0) {
		/* leave some room for the header */
		size_t dsz = min(MAX_WINDOW_SIZE, sz);
		size_t csz = deflateBound(&z, dsz);
		size_t insz;
		unsigned char ins[MAX_INS_LEN], *p, *he, *hb;

		deflateReset(&z);

		/* Create the header using the max bounded size. If the
		 * compressed data ends up being small enough such that
		 * the size of the size field shrinks by a byte then we
		 * just shift the data down.
		 */

		insz = encode_instruction(ins, FROM_NEW, 0, dsz) - ins;

		p = (unsigned char*) diff->buf + diff->len;
		p = encode_varint(p, 0); /* source off */
		p = encode_varint(p, 0); /* source len */
		p = encode_varint(p, dsz); /* target len */
		p = encode_varint(p, insz + 1); /* instruction len */
		hb = p;
		p = encode_varint(p, csz + encoded_length(dsz)); /* compressed data size */
		p += 1; /* instruction len */
		p += insz; /* instructions */
		he = p;
		p = encode_varint(p, dsz); /* decompressed data size */

		z.next_in = (unsigned char*) src;
		z.avail_in = dsz;
		z.next_out = p;
		z.avail_out = csz;

		if (deflate(&z, Z_FINISH) != Z_STREAM_END)
			die("deflate failed");

		csz -= z.avail_out;
		p += csz;
		strbuf_setlen(diff, p - (unsigned char*) diff->buf);

		p = encode_varint(hb, p - he); /* compressed data size */
		p = encode_varint(p, insz); /* instruction len */
		memcpy(p, ins, insz); /* instructions */
		p += insz;

		if (p < he) {
			strbuf_remove(diff, (char*) p - diff->buf, he - p);
		}

		src = (char*) src + dsz;
		sz -= dsz;
	}
}

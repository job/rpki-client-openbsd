/*	$OpenBSD: spl.c,v 1.3 2024/05/15 14:43:32 claudio Exp $ */
/*
 * Copyright (c) 2024 Job Snijders <job@fastly.com>
 * Copyright (c) 2022 Theo Buehler <tb@openbsd.org>
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/stack.h>
#include <openssl/safestack.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "extern.h"

extern ASN1_OBJECT	*spl_oid;

/*
 * Types and templates for the SPL eContent.
 */

ASN1_ITEM_EXP AddressFamilyPrefixes_it;
ASN1_ITEM_EXP SignedPrefixList_it;

DECLARE_STACK_OF(ASN1_BIT_STRING);

typedef struct {
	ASN1_OCTET_STRING		*addressFamily;
	STACK_OF(ASN1_BIT_STRING)	*addressPrefixes;
} AddressFamilyPrefixes;

DECLARE_STACK_OF(AddressFamilyPrefixes);

ASN1_SEQUENCE(AddressFamilyPrefixes) = {
	ASN1_SIMPLE(AddressFamilyPrefixes, addressFamily, ASN1_OCTET_STRING),
	ASN1_SEQUENCE_OF(AddressFamilyPrefixes, addressPrefixes,
	    ASN1_BIT_STRING),
} ASN1_SEQUENCE_END(AddressFamilyPrefixes);

#ifndef DEFINE_STACK_OF
#define sk_ASN1_BIT_STRING_num(st)	SKM_sk_num(ASN1_BIT_STRING, (st))
#define sk_ASN1_BIT_STRING_value(st, i)	SKM_sk_value(ASN1_BIT_STRING, (st), (i))

#define sk_AddressFamilyPrefixes_num(st)	\
    SKM_sk_num(AddressFamilyPrefixes, (st))
#define sk_AddressFamilyPrefixes_value(st, i)	\
    SKM_sk_value(AddressFamilyPrefixes, (st), (i))
#endif

typedef struct {
	ASN1_INTEGER			*version;
	ASN1_INTEGER			*asid;
	STACK_OF(AddressFamilyPrefixes)	*prefixBlocks;
} SignedPrefixList;

ASN1_SEQUENCE(SignedPrefixList) = {
	ASN1_EXP_OPT(SignedPrefixList, version, ASN1_INTEGER, 0),
	ASN1_SIMPLE(SignedPrefixList, asid, ASN1_INTEGER),
	ASN1_SEQUENCE_OF(SignedPrefixList, prefixBlocks, AddressFamilyPrefixes)
} ASN1_SEQUENCE_END(SignedPrefixList);

DECLARE_ASN1_FUNCTIONS(SignedPrefixList);
IMPLEMENT_ASN1_FUNCTIONS(SignedPrefixList);

/*
 * Comparator to help sorting elements in SPL prefixBlocks and VSPs.
 * Returns -1 if 'a' should precede 'b', 1 if 'b' should precede 'a',
 * or '0' if a and b are equal.
 */
static int
prefix_cmp(enum afi afi, const struct ip_addr *a, const struct ip_addr *b)
{
	int cmp;

	switch (afi) {
	case AFI_IPV4:
		cmp = memcmp(&a->addr, &b->addr, 4);
		if (cmp < 0)
			return -1;
		if (cmp > 0)
			return 1;
		break;
	case AFI_IPV6:
		cmp = memcmp(&a->addr, &b->addr, 16);
		if (cmp < 0)
			return -1;
		if (cmp > 0)
			return 1;
		break;
	default:
		break;
	}

	if (a->prefixlen < b->prefixlen)
		return -1;
	if (a->prefixlen > b->prefixlen)
		return 1;

	return 0;
}

/*
 * Parses the eContent section of a SPL file,
 * draft-ietf-sidrops-rpki-prefixlist-02 section 3.
 * Returns zero on failure, non-zero on success.
 */
static int
spl_parse_econtent(const char *fn, struct spl *spl, const unsigned char *d,
    size_t dsz)
{
	const unsigned char		*oder;
	SignedPrefixList		*spl_asn1;
	const AddressFamilyPrefixes	*afp;
	const STACK_OF(ASN1_BIT_STRING)	*prefixes;
	const ASN1_BIT_STRING		*prefix_asn1;
	int				 afpsz, prefixesz;
	enum afi			 afi;
	struct ip_addr			 ip_addr;
	struct spl_pfx			*prefix;
	int				 ipv4_seen = 0, ipv6_seen = 0;
	int				 i, j, rc = 0;

	oder = d;
	if ((spl_asn1 = d2i_SignedPrefixList(NULL, &d, dsz)) == NULL) {
		warnx("%s: RFC 6482 section 3: failed to parse "
		    "SignedPrefixList", fn);
		goto out;
	}
	if (d != oder + dsz) {
		warnx("%s: %td bytes trailing garbage in eContent", fn,
		    oder + dsz - d);
		goto out;
	}

	if (!valid_econtent_version(fn, spl_asn1->version, 0))
		goto out;

	if (!as_id_parse(spl_asn1->asid, &spl->asid)) {
		warnx("%s: asid: malformed AS identifier", fn);
		goto out;
	}

	afpsz = sk_AddressFamilyPrefixes_num(spl_asn1->prefixBlocks);
	if (afpsz < 0 || afpsz > 2) {
		warnx("%s: unexpected number of AddressFamilyAddressPrefixes"
		    "(got %d, expected 0, 1, or 2)", fn, afpsz);
		goto out;
	}

	for (i = 0; i < afpsz; i++) {
		struct ip_addr *prev_ip_addr = NULL;

		afp = sk_AddressFamilyPrefixes_value(spl_asn1->prefixBlocks, i);
		prefixes = afp->addressPrefixes;
		prefixesz = sk_ASN1_BIT_STRING_num(afp->addressPrefixes);

		if (prefixesz == 0) {
			warnx("%s: empty AddressFamilyAddressPrefixes", fn);
			goto out;
		}
		if (spl->pfxsz + prefixesz >= MAX_IP_SIZE) {
			warnx("%s: too many addressPrefixes entries", fn);
			goto out;
		}

		if (!ip_addr_afi_parse(fn, afp->addressFamily, &afi))
			goto out;

		switch (afi) {
		case AFI_IPV4:
			if (ipv4_seen++ > 0) {
				warnx("%s: addressFamilyIPv4 appeared twice",
				    fn);
				goto out;
			}
			if (ipv6_seen > 0) {
				warnx("%s: invalid sorting, IPv6 before IPv4",
				    fn);
				goto out;
			}
			break;
		case AFI_IPV6:
			if (ipv6_seen++ > 0) {
				warnx("%s: addressFamilyIPv6 appeared twice",
				    fn);
				goto out;
			}
		}

		spl->pfxs = recallocarray(spl->pfxs, spl->pfxsz,
		    spl->pfxsz + prefixesz, sizeof(struct spl_pfx));
		if (spl->pfxs == NULL)
			err(1, NULL);

		for (j = 0; j < prefixesz; j++) {
			prefix_asn1 = sk_ASN1_BIT_STRING_value(prefixes, j);

			if (!ip_addr_parse(prefix_asn1, afi, fn, &ip_addr))
				goto out;

			if (j > 0 &&
			    prefix_cmp(afi, prev_ip_addr, &ip_addr) != -1) {
				warnx("%s: invalid addressPrefixes sorting", fn);
				goto out;
			}

			prefix = &spl->pfxs[spl->pfxsz++];
			prefix->prefix = ip_addr;
			prefix->afi = afi;
			prev_ip_addr = &prefix->prefix;
		}
	}

	rc = 1;
 out:
	SignedPrefixList_free(spl_asn1);
	return rc;
}

/*
 * Parse a full Signed Prefix List file.
 * Returns the SPL, or NULL if the object was malformed.
 */
struct spl *
spl_parse(X509 **x509, const char *fn, int talid, const unsigned char *der,
    size_t len)
{
	struct spl	*spl;
	size_t		 cmsz;
	unsigned char	*cms;
	struct cert	*cert = NULL;
	time_t		 signtime = 0;
	int		 rc = 0;

	cms = cms_parse_validate(x509, fn, der, len, spl_oid, &cmsz, &signtime);
	if (cms == NULL)
		return NULL;

	if ((spl = calloc(1, sizeof(*spl))) == NULL)
		err(1, NULL);
	spl->signtime = signtime;

	if (!x509_get_aia(*x509, fn, &spl->aia))
		goto out;
	if (!x509_get_aki(*x509, fn, &spl->aki))
		goto out;
	if (!x509_get_sia(*x509, fn, &spl->sia))
		goto out;
	if (!x509_get_ski(*x509, fn, &spl->ski))
		goto out;
	if (spl->aia == NULL || spl->aki == NULL || spl->sia == NULL ||
	    spl->ski == NULL) {
		warnx("%s: RFC 6487 section 4.8: "
		    "missing AIA, AKI, SIA, or SKI X509 extension", fn);
		goto out;
	}

	if (!x509_get_notbefore(*x509, fn, &spl->notbefore))
		goto out;
	if (!x509_get_notafter(*x509, fn, &spl->notafter))
		goto out;

	if (!spl_parse_econtent(fn, spl, cms, cmsz))
		goto out;

	if (x509_any_inherits(*x509)) {
		warnx("%s: inherit elements not allowed in EE cert", fn);
		goto out;
	}

	if ((cert = cert_parse_ee_cert(fn, talid, *x509)) == NULL)
		goto out;

	if (cert->asz == 0) {
		warnx("%s: AS Resources extension missing", fn);
		goto out;
	}

	if (cert->ipsz > 0) {
		warnx("%s: superfluous IP Resources extension present", fn);
		goto out;
	}

	/*
	 * If the SPL isn't valid, we accept it anyway and depend upon
	 * the code around spl_read() to check the "valid" field itself.
	 */
	spl->valid = valid_spl(fn, cert, spl);

	rc = 1;
 out:
	if (rc == 0) {
		spl_free(spl);
		spl = NULL;
		X509_free(*x509);
		*x509 = NULL;
	}
	cert_free(cert);
	free(cms);
	return spl;
}

void
spl_free(struct spl *s)
{
	if (s == NULL)
		return;

	free(s->aia);
	free(s->aki);
	free(s->sia);
	free(s->ski);
	free(s->pfxs);
	free(s);
}

/*
 * Serialize parsed SPL content.
 * See spl_read() for reader.
 */
void
spl_buffer(struct ibuf *b, const struct spl *s)
{
	io_simple_buffer(b, &s->valid, sizeof(s->valid));
	io_simple_buffer(b, &s->asid, sizeof(s->asid));
	io_simple_buffer(b, &s->talid, sizeof(s->talid));
	io_simple_buffer(b, &s->pfxsz, sizeof(s->pfxsz));
	io_simple_buffer(b, &s->expires, sizeof(s->expires));

	io_simple_buffer(b, s->pfxs, s->pfxsz * sizeof(s->pfxs[0]));

	io_str_buffer(b, s->aia);
	io_str_buffer(b, s->aki);
	io_str_buffer(b, s->ski);
}

/*
 * Read parsed SPL content from descriptor.
 * See spl_buffer() for writer.
 * Result must be passed to spl_free().
 */
struct spl *
spl_read(struct ibuf *b)
{
	struct spl *s;

	if ((s = calloc(1, sizeof(struct spl))) == NULL)
		err(1, NULL);

	io_read_buf(b, &s->valid, sizeof(s->valid));
	io_read_buf(b, &s->asid, sizeof(s->asid));
	io_read_buf(b, &s->talid, sizeof(s->talid));
	io_read_buf(b, &s->pfxsz, sizeof(s->pfxsz));
	io_read_buf(b, &s->expires, sizeof(s->expires));

	if ((s->pfxs = calloc(s->pfxsz, sizeof(struct spl_pfx))) == NULL)
		err(1, NULL);
	io_read_buf(b, s->pfxs, s->pfxsz * sizeof(s->pfxs[0]));

	io_read_str(b, &s->aia);
	io_read_str(b, &s->aki);
	io_read_str(b, &s->ski);
	assert(s->aia && s->aki && s->ski);

	return s;
}

static int
spl_pfx_cmp(const struct spl_pfx *a, const struct spl_pfx *b)
{
	if (a->afi > b->afi)
		return 1;
	if (a->afi < b->afi)
		return -1;

	return prefix_cmp(a->afi, &a->prefix, &b->prefix);
}

static void
insert_vsp(struct vsp *vsp, size_t idx, struct spl_pfx *pfx)
{
	if (idx < vsp->prefixesz)
		memmove(vsp->prefixes + idx + 1, vsp->prefixes + idx,
		    (vsp->prefixesz - idx) * sizeof(*vsp->prefixes));
	vsp->prefixes[idx] = *pfx;
	vsp->prefixesz++;
}

/*
 * Add each prefix in the SPL into the VSP tree.
 * Updates "vsps" to be the number of VSPs and "uniqs" to be the unique
 * number of prefixes.
 */
void
spl_insert_vsps(struct vsp_tree *tree, struct spl *spl, struct repo *rp)
{
	struct vsp	*vsp, *found;
	size_t		 i, j;
	int		 cmp;

	if ((vsp = calloc(1, sizeof(*vsp))) == NULL)
		err(1, NULL);

	vsp->asid = spl->asid;
	vsp->talid = spl->talid;
	vsp->expires = spl->expires;
	if (rp != NULL)
		vsp->repoid = repo_id(rp);

	if ((found = RB_INSERT(vsp_tree, tree, vsp)) != NULL) {
		/* already exists */
		if (found->expires < vsp->expires) {
			/* adjust unique count */
			repo_stat_inc(repo_byid(found->repoid),
			    found->talid, RTYPE_SPL, STYPE_DEC_UNIQUE);
			found->expires = vsp->expires;
			found->talid = vsp->talid;
			found->repoid = vsp->repoid;
			repo_stat_inc(rp, vsp->talid, RTYPE_SPL,
			    STYPE_UNIQUE);
		}
		free(vsp);
		vsp = found;
	} else
		repo_stat_inc(rp, vsp->talid, RTYPE_SPL, STYPE_UNIQUE);
	repo_stat_inc(rp, spl->talid, RTYPE_SPL, STYPE_TOTAL);

	/* merge content of multiple SPLs */
	vsp->prefixes = reallocarray(vsp->prefixes,
	    vsp->prefixesz + spl->pfxsz, sizeof(struct spl_pfx));
	if (vsp->prefixes == NULL)
		err(1, NULL);

	/*
	 * Merge all data from the new SPL at hand into 'vsp': loop over
	 * all SPL->pfxs, and insert them in the right place in
	 * vsp->prefixes while keeping the order of the array.
	 */
	for (i = 0, j = 0; i < spl->pfxsz; ) {
		cmp = -1;
		if (j == vsp->prefixesz ||
		    (cmp = spl_pfx_cmp(&spl->pfxs[i], &vsp->prefixes[j])) < 0) {
			insert_vsp(vsp, j, &spl->pfxs[i]);
			i++;
		} else if (cmp == 0)
			i++;

		if (j < vsp->prefixesz)
			j++;
	}
}

/*
 * Comparison function for the RB tree
 */
static inline int
vspcmp(const struct vsp *a, const struct vsp *b)
{
	if (a->asid > b->asid)
		return 1;
	if (a->asid < b->asid)
		return -1;

	return 0;
}

RB_GENERATE(vsp_tree, vsp, entry, vspcmp);

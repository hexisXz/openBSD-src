/* $OpenBSD: p12_npas.c,v 1.25 2024/01/25 14:09:26 tb Exp $ */
/* Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL
 * project 1999.
 */
/* ====================================================================
 * Copyright (c) 1999 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>

#include "pkcs12_local.h"
#include "x509_local.h"

/* PKCS#12 password change routine */

static int newpass_bags(STACK_OF(PKCS12_SAFEBAG) *bags, const char *oldpass,
    const char *newpass);
static int newpass_bag(PKCS12_SAFEBAG *bag, const char *oldpass,
    const char *newpass);
static int alg_get(X509_ALGOR *alg, int *pnid, int *piter, int *psaltlen);

/*
 * Change the password on a PKCS#12 structure.
 */

static int
pkcs7_repack_data(PKCS7 *pkcs7, STACK_OF(PKCS7) *safes, const char *oldpass,
    const char *newpass)
{
	STACK_OF(PKCS12_SAFEBAG) *bags;
	PKCS7 *data = NULL;
	int ret = 0;

	if ((bags = PKCS12_unpack_p7data(pkcs7)) == NULL)
		goto err;
	if (!newpass_bags(bags, oldpass, newpass))
		goto err;
	if ((data = PKCS12_pack_p7data(bags)) == NULL)
		goto err;
	if (sk_PKCS7_push(safes, data) == 0)
		goto err;
	data = NULL;

	ret = 1;

 err:
	sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
	PKCS7_free(data);

	return ret;
}

static int
pkcs7_repack_encdata(PKCS7 *pkcs7, STACK_OF(PKCS7) *safes, const char *oldpass,
    const char *newpass)
{
	STACK_OF(PKCS12_SAFEBAG) *bags;
	int pbe_nid = 0, pbe_iter = 0, pbe_saltlen = 0;
	PKCS7 *data = NULL;
	int ret = 0;

	if ((bags = PKCS12_unpack_p7encdata(pkcs7, oldpass, -1)) == NULL)
		goto err;
	if (!alg_get(pkcs7->d.encrypted->enc_data->algorithm, &pbe_nid,
	    &pbe_iter, &pbe_saltlen))
		goto err;
	if (!newpass_bags(bags, oldpass, newpass))
		goto err;
	if ((data = PKCS12_pack_p7encdata(pbe_nid, newpass, -1, NULL,
	    pbe_saltlen, pbe_iter, bags)) == NULL)
		goto err;
	if (!sk_PKCS7_push(safes, data))
		goto err;
	data = NULL;

	ret = 1;

 err:
	sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
	PKCS7_free(data);

	return ret;
}

static int
pkcs12_repack_authsafes(PKCS12 *pkcs12, STACK_OF(PKCS7) *safes,
    const char *newpass)
{
	ASN1_OCTET_STRING *old_data;
	ASN1_OCTET_STRING *new_mac = NULL;
	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned int mac_len;
	int ret = 0;

	if ((old_data = pkcs12->authsafes->d.data) == NULL)
		goto err;
	if ((pkcs12->authsafes->d.data = ASN1_OCTET_STRING_new()) == NULL)
		goto err;
	if (!PKCS12_pack_authsafes(pkcs12, safes))
		goto err;
	if (!PKCS12_gen_mac(pkcs12, newpass, -1, mac, &mac_len))
		goto err;
	if ((new_mac = ASN1_OCTET_STRING_new()) == NULL)
		goto err;
	if (!ASN1_OCTET_STRING_set(new_mac, mac, mac_len))
		goto err;

	ASN1_OCTET_STRING_free(pkcs12->mac->dinfo->digest);
	pkcs12->mac->dinfo->digest = new_mac;
	new_mac = NULL;

	ASN1_OCTET_STRING_free(old_data);
	old_data = NULL;

	ret = 1;

 err:
	if (old_data != NULL) {
		ASN1_OCTET_STRING_free(pkcs12->authsafes->d.data);
		pkcs12->authsafes->d.data = old_data;
	}
	explicit_bzero(mac, sizeof(mac));
	ASN1_OCTET_STRING_free(new_mac);

	return ret;
}

int
PKCS12_newpass(PKCS12 *pkcs12, const char *oldpass, const char *newpass)
{
	STACK_OF(PKCS7) *authsafes = NULL, *safes = NULL;
	int i;
	int ret = 0;

	if (pkcs12 == NULL) {
		PKCS12error(PKCS12_R_INVALID_NULL_PKCS12_POINTER);
		goto err;
	}

	if (!PKCS12_verify_mac(pkcs12, oldpass, -1)) {
		PKCS12error(PKCS12_R_MAC_VERIFY_FAILURE);
		goto err;
	}

	if ((authsafes = PKCS12_unpack_authsafes(pkcs12)) == NULL)
		goto err;
	if ((safes = sk_PKCS7_new_null()) == NULL)
		goto err;

	for (i = 0; i < sk_PKCS7_num(authsafes); i++) {
		PKCS7 *pkcs7 = sk_PKCS7_value(authsafes, i);

		switch (OBJ_obj2nid(pkcs7->type)) {
		case NID_pkcs7_data:
			if (pkcs7_repack_data(pkcs7, safes, oldpass, newpass))
				goto err;
			break;
		case NID_pkcs7_encrypted:
			if (pkcs7_repack_encdata(pkcs7, safes, oldpass, newpass))
				goto err;
			break;
		}
	}

	if (!pkcs12_repack_authsafes(pkcs12, safes, newpass))
		goto err;

	ret = 1;

 err:
	sk_PKCS7_pop_free(authsafes, PKCS7_free);
	sk_PKCS7_pop_free(safes, PKCS7_free);

	return ret;
}
LCRYPTO_ALIAS(PKCS12_newpass);

static int
newpass_bags(STACK_OF(PKCS12_SAFEBAG) *bags, const char *oldpass,
    const char *newpass)
{
	int i;

	for (i = 0; i < sk_PKCS12_SAFEBAG_num(bags); i++) {
		if (!newpass_bag(sk_PKCS12_SAFEBAG_value(bags, i),
		    oldpass, newpass))
			return 0;
	}
	return 1;
}

/* Change password of safebag: only needs handle shrouded keybags */

static int
newpass_bag(PKCS12_SAFEBAG *bag, const char *oldpass, const char *newpass)
{
	PKCS8_PRIV_KEY_INFO *p8;
	X509_SIG *p8new;
	int p8_nid, p8_saltlen, p8_iter;

	if (OBJ_obj2nid(bag->type) != NID_pkcs8ShroudedKeyBag)
		return 1;

	if (!(p8 = PKCS8_decrypt(bag->value.shkeybag, oldpass, -1)))
		return 0;
	if (!alg_get(bag->value.shkeybag->algor, &p8_nid, &p8_iter,
	    &p8_saltlen))
		return 0;
	if (!(p8new = PKCS8_encrypt(p8_nid, NULL, newpass, -1, NULL, p8_saltlen,
	    p8_iter, p8))) return 0;
	X509_SIG_free(bag->value.shkeybag);
	bag->value.shkeybag = p8new;
	return 1;
}

static int
alg_get(X509_ALGOR *alg, int *pnid, int *piter, int *psaltlen)
{
	PBEPARAM *pbe;
	const unsigned char *p;

	p = alg->parameter->value.sequence->data;
	pbe = d2i_PBEPARAM(NULL, &p, alg->parameter->value.sequence->length);
	if (!pbe)
		return 0;
	*pnid = OBJ_obj2nid(alg->algorithm);
	*piter = ASN1_INTEGER_get(pbe->iter);
	*psaltlen = pbe->salt->length;
	PBEPARAM_free(pbe);
	return 1;
}

/*
 * XML Security Library (http://www.aleksey.com/xmlsec).
 *
 * X509 certificates verification support functions for GnuTLS.
 *
 * This is free software; see Copyright file in the source
 * distribution for preciese wording.
 *
 * Copyright (C) 2002-2022 Aleksey Sanin <aleksey@aleksey.com>. All Rights Reserved.
 */
/**
 * SECTION:x509
 */

#include "globals.h"

#ifndef XMLSEC_NO_X509

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <xmlsec/xmlsec.h>
#include <xmlsec/keys.h>
#include <xmlsec/keyinfo.h>
#include <xmlsec/keysmngr.h>
#include <xmlsec/base64.h>
#include <xmlsec/errors.h>
#include <xmlsec/private.h>

#include <xmlsec/gnutls/crypto.h>
#include <xmlsec/gnutls/x509.h>

#include "private.h"
#include "../cast_helpers.h"

/**************************************************************************
 *
 * Internal GnuTLS X509 store CTX
 *
 *************************************************************************/
typedef struct _xmlSecGnuTLSX509StoreCtx                xmlSecGnuTLSX509StoreCtx,
                                                        *xmlSecGnuTLSX509StoreCtxPtr;
struct _xmlSecGnuTLSX509StoreCtx {
    xmlSecPtrList certsTrusted;
    xmlSecPtrList certsUntrusted;
    xmlSecPtrList crls;
};

/****************************************************************************
 *
 * xmlSecGnuTLSKeyDataStoreX509Id:
 *
 ***************************************************************************/
XMLSEC_KEY_DATA_STORE_DECLARE(GnuTLSX509Store, xmlSecGnuTLSX509StoreCtx)
#define xmlSecGnuTLSX509StoreSize XMLSEC_KEY_DATA_STORE_SIZE(GnuTLSX509Store)

static int              xmlSecGnuTLSX509StoreInitialize                 (xmlSecKeyDataStorePtr store);
static void             xmlSecGnuTLSX509StoreFinalize                   (xmlSecKeyDataStorePtr store);

static xmlSecKeyDataStoreKlass xmlSecGnuTLSX509StoreKlass = {
    sizeof(xmlSecKeyDataStoreKlass),
    xmlSecGnuTLSX509StoreSize,

    /* data */
    xmlSecNameX509Store,                        /* const xmlChar* name; */

    /* constructors/destructor */
    xmlSecGnuTLSX509StoreInitialize,            /* xmlSecKeyDataStoreInitializeMethod initialize; */
    xmlSecGnuTLSX509StoreFinalize,              /* xmlSecKeyDataStoreFinalizeMethod finalize; */

    /* reserved for the future */
    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

static gnutls_x509_crt_t xmlSecGnuTLSX509FindCert                       (xmlSecPtrListPtr certs,
                                                                         xmlSecGnuTLSX509FindCertCtxPtr findCertCtx);
static gnutls_x509_crt_t xmlSecGnuTLSX509FindSignedCert                 (xmlSecPtrListPtr certs,
                                                                         gnutls_x509_crt_t cert);
static gnutls_x509_crt_t xmlSecGnuTLSX509FindSignerCert                 (xmlSecPtrListPtr certs,
                                                                         gnutls_x509_crt_t cert);


/**
 * xmlSecGnuTLSX509StoreGetKlass:
 *
 * The GnuTLS X509 certificates key data store klass.
 *
 * Returns: pointer to GnuTLS X509 certificates key data store klass.
 */
xmlSecKeyDataStoreId
xmlSecGnuTLSX509StoreGetKlass(void) {
    return(&xmlSecGnuTLSX509StoreKlass);
}

/**
 * xmlSecGnuTLSX509StoreFindCert:
 * @store:              the pointer to X509 key data store klass.
 * @subjectName:        the desired certificate name.
 * @issuerName:         the desired certificate issuer name.
 * @issuerSerial:       the desired certificate issuer serial number.
 * @ski:                the desired certificate SKI.
 * @keyInfoCtx:         the pointer to &lt;dsig:KeyInfo/&gt; element processing context.
 *
 * Searches @store for a certificate that matches given criteria.
 *
 * Returns: pointer to found certificate or NULL if certificate is not found
 * or an error occurs.
 */
gnutls_x509_crt_t
xmlSecGnuTLSX509StoreFindCert(const xmlSecKeyDataStorePtr store, const xmlChar *subjectName,
                              const xmlChar *issuerName, const xmlChar *issuerSerial,
                              const xmlChar *ski, const xmlSecKeyInfoCtx* keyInfoCtx ) {
    if(ski != NULL) {
        gnutls_x509_crt_t res;
        xmlChar* skiDup;
        xmlSecSize skiDecodedSize = 0;
        int ret;

        skiDup = xmlStrdup(ski);
        if(skiDup == NULL) {
            xmlSecStrdupError(ski, NULL);
            return(NULL);
        }

        /* our usual trick with base64 decode */
        ret = xmlSecBase64DecodeInPlace(skiDup, &skiDecodedSize);
        if(ret < 0) {
            xmlSecInternalError2("xmlSecBase64DecodeInPlace", NULL,
                "ski=%s", xmlSecErrorsSafeString(skiDup));
            xmlFree(skiDup);
            return(NULL);
        }

        res = xmlSecGnuTLSX509StoreFindCert_ex(store, subjectName, issuerName, issuerSerial,
            (xmlSecByte*)skiDup, skiDecodedSize, keyInfoCtx);
        xmlFree(skiDup);
        return(res);
    } else {
        return(xmlSecGnuTLSX509StoreFindCert_ex(store, subjectName, issuerName, issuerSerial,
            NULL, 0, keyInfoCtx));

    }
}

/**
 * xmlSecGnuTLSX509StoreFindCert_ex:
 * @store:              the pointer to X509 key data store klass.
 * @subjectName:        the desired certificate name.
 * @issuerName:         the desired certificate issuer name.
 * @issuerSerial:       the desired certificate issuer serial number.
 * @ski:                the desired certificate SKI.
 * @skiSize:            the desired certificate SKI size.
 * @keyInfoCtx:         the pointer to &lt;dsig:KeyInfo/&gt; element processing context.
 *
 * Searches @store for a certificate that matches given criteria.
 *
 * Returns: pointer to found certificate or NULL if certificate is not found
 * or an error occurs.
 */
gnutls_x509_crt_t
xmlSecGnuTLSX509StoreFindCert_ex(const xmlSecKeyDataStorePtr store, const xmlChar *subjectName,
                              const xmlChar *issuerName, const xmlChar *issuerSerial,
                              const xmlSecByte * ski, xmlSecSize skiSize,
                              const xmlSecKeyInfoCtx* keyInfoCtx ATTRIBUTE_UNUSED) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    xmlSecGnuTLSX509FindCertCtx findCertCtx;
    gnutls_x509_crt_t res = NULL;
    int ret;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), NULL);
    UNREFERENCED_PARAMETER(keyInfoCtx);

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, NULL);

    ret = xmlSecGnuTLSX509FindCertCtxInitialize(&findCertCtx,
            subjectName,
            issuerName, issuerSerial,
            ski, skiSize);
    if(ret < 0) {
        xmlSecInternalError("xmlSecGnuTLSX509FindCertCtxInitialize", NULL);
        xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
        return(NULL);
    }

    if(res == NULL) {
        res = xmlSecGnuTLSX509FindCert(&(ctx->certsTrusted), &findCertCtx);
    }
    if(res == NULL) {
        res = xmlSecGnuTLSX509FindCert(&(ctx->certsUntrusted), &findCertCtx);
    }

    /* done */
    xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
    return(res);
}

gnutls_x509_crt_t
xmlSecGnuTLSX509StoreFindCertByValue(xmlSecKeyDataStorePtr store, xmlSecKeyX509DataValuePtr x509Value) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    xmlSecGnuTLSX509FindCertCtx findCertCtx;
    int ret;
    gnutls_x509_crt_t res = NULL;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), NULL);

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, NULL);

    ret = xmlSecGnuTLSX509FindCertCtxInitializeFromValue(&findCertCtx, x509Value);
    if(ret < 0) {
        xmlSecInternalError("xmlSecGnuTLSX509FindCertCtxInitializeFromValue", NULL);
        xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
        return(NULL);
    }

    if(res == NULL) {
        res = xmlSecGnuTLSX509FindCert(&(ctx->certsTrusted), &findCertCtx);
    }
    if(res == NULL) {
        res = xmlSecGnuTLSX509FindCert(&(ctx->certsUntrusted), &findCertCtx);
    }

    /* done */
    xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
    return(res);
}

xmlSecKeyPtr
xmlSecGnuTLSX509FindKeyByValue(xmlSecPtrListPtr keysList, xmlSecKeyX509DataValuePtr x509Value) {
    xmlSecGnuTLSX509FindCertCtx findCertCtx;
    xmlSecSize keysListSize, ii;
    xmlSecKeyPtr res = NULL;
    int ret;

    xmlSecAssert2(keysList != NULL, NULL);
    xmlSecAssert2(x509Value != NULL, NULL);

    ret = xmlSecGnuTLSX509FindCertCtxInitializeFromValue(&findCertCtx, x509Value);
    if(ret < 0) {
        xmlSecInternalError("xmlSecGnuTLSX509FindCertCtxInitializeFromValue", NULL);
        xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
        return(NULL);
    }

    keysListSize = xmlSecPtrListGetSize(keysList);
    for(ii = 0; ii < keysListSize; ++ii) {
        xmlSecKeyPtr key;
        xmlSecKeyDataPtr keyData;
        gnutls_x509_crt_t keyCert;

        /* get key's cert from x509 key data */
        key = (xmlSecKeyPtr)xmlSecPtrListGetItem(keysList, ii);
        if(key == NULL) {
            continue;
        }
        keyData = xmlSecKeyGetData(key, xmlSecGnuTLSKeyDataX509Id);
        if(keyData == NULL) {
            continue;
        }
        keyCert = xmlSecGnuTLSKeyDataX509GetKeyCert(keyData);
        if(keyCert == NULL) {
            continue;
        }

        /* does it match? */
        ret = xmlSecGnuTLSX509FindCertCtxMatch(&findCertCtx, keyCert);
        if(ret < 0) {
            xmlSecInternalError("xmlSecGnuTLSX509FindCertCtxMatch", NULL);
            xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
            return(NULL);
        } else if(ret == 1) {
            res = key;
            break;
        }
    }

    /* done */
    xmlSecGnuTLSX509FindCertCtxFinalize(&findCertCtx);
    return(res);
}

static int
xmlSecGnuTLSX509CheckCrtTime(const gnutls_x509_crt_t cert, time_t ts) {
    time_t notValidBefore, notValidAfter;

    xmlSecAssert2(cert != NULL, -1);

    /* get expiration times */
    notValidBefore = gnutls_x509_crt_get_activation_time(cert);
    if(notValidBefore == (time_t)-1) {
        xmlSecGnuTLSError2("gnutls_x509_crt_get_activation_time", GNUTLS_E_SUCCESS, NULL,
            "cert activation time is invalid: %.lf",
            difftime(notValidBefore, (time_t)0));
        return(-1);
    }
    notValidAfter = gnutls_x509_crt_get_expiration_time(cert);
    if(notValidAfter == (time_t)-1) {
        xmlSecGnuTLSError2("gnutls_x509_crt_get_expiration_time", GNUTLS_E_SUCCESS, NULL,
            "cert expiration time is invalid: %.lf",
            difftime(notValidAfter, (time_t)0));
        return(-1);
    }

    /* check */
    if(ts < notValidBefore) {
        /* TODO: print cert subject */
        xmlSecOtherError(XMLSEC_ERRORS_R_CERT_NOT_YET_VALID, NULL, NULL);
        return(0);
    }
    if(ts > notValidAfter) {
        /* TODO: print cert subject */
        xmlSecOtherError(XMLSEC_ERRORS_R_CERT_HAS_EXPIRED, NULL, NULL);
        return(0);
    }

    /* good! */
    return(1);
}


static int
xmlSecGnuTLSX509CheckCrtsTime(const gnutls_x509_crt_t * cert_list, xmlSecSize cert_list_size, time_t ts) {
    xmlSecSize ii;
    int ret;

    xmlSecAssert2(cert_list != NULL, -1);

    for(ii = 0; ii < cert_list_size; ++ii) {
        const gnutls_x509_crt_t cert = cert_list[ii];
        if(cert == NULL) {
            continue;
        }

        ret = xmlSecGnuTLSX509CheckCrtTime(cert, ts);
        if(ret < 0) {
            xmlSecInternalError("", NULL);
            return(-1);
        } else if(ret == 0) {
            /* cert not valid yet or expired */
            return(0);
        }
    }

    /* GOOD! */
    return(1);
}

/**
 * xmlSecGnuTLSX509StoreVerify:
 * @store:              the pointer to X509 key data store klass.
 * @certs:              the untrusted certificates.
 * @crls:               the crls.
 * @keyInfoCtx:         the pointer to &lt;dsig:KeyInfo/&gt; element processing context.
 *
 * Verifies @certs list.
 *
 * Returns: pointer to the first verified certificate from @certs.
 */
gnutls_x509_crt_t
xmlSecGnuTLSX509StoreVerify(xmlSecKeyDataStorePtr store,
                            xmlSecPtrListPtr certs,
                            xmlSecPtrListPtr crls,
                            const xmlSecKeyInfoCtx* keyInfoCtx) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    gnutls_x509_crt_t res = NULL;
    xmlSecSize certs_size = 0;
    gnutls_x509_crt_t * cert_list = NULL;
    xmlSecSize cert_list_size;
    gnutls_x509_crl_t * crl_list = NULL;
    gnutls_x509_crl_t crl;
    xmlSecSize crl_list_size;
    xmlSecSize crl_ctx_list_size;
    xmlSecSize crl_actual_list_size = 0;
    gnutls_x509_crt_t * ca_list = NULL;
    xmlSecSize ca_list_size;
    time_t verification_time;
    unsigned int flags = 0;
    xmlSecSize ii;
    int ret;
    int err;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), NULL);
    xmlSecAssert2(certs != NULL, NULL);
    xmlSecAssert2(crls != NULL, NULL);
    xmlSecAssert2(keyInfoCtx != NULL, NULL);

    certs_size = xmlSecPtrListGetSize(certs);
    if(certs_size <= 0) {
        /* nothing to do */
        return(NULL);
    }

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, NULL);

    /* gnutls doesn't allow to specify "verification" timestamp so
     * we have to do it ourselves */
    verification_time = (keyInfoCtx->certsVerificationTime > 0) ?
                        keyInfoCtx->certsVerificationTime :
                        time(0);

    /* Prepare */
    cert_list_size = certs_size + xmlSecPtrListGetSize(&(ctx->certsUntrusted));
    if(cert_list_size > 0) {
        cert_list = (gnutls_x509_crt_t *)xmlMalloc(sizeof(gnutls_x509_crt_t) * cert_list_size);
        if(cert_list == NULL) {
            xmlSecMallocError(sizeof(gnutls_x509_crt_t) * cert_list_size,
                xmlSecKeyDataStoreGetName(store));
            goto done;
        }
    }

    crl_list_size = xmlSecPtrListGetSize(crls);
    crl_ctx_list_size = xmlSecPtrListGetSize(&(ctx->crls));
    if((crl_list_size + crl_ctx_list_size) > 0) {
        crl_list = (gnutls_x509_crl_t *)xmlMalloc(sizeof(gnutls_x509_crl_t) * (crl_list_size + crl_ctx_list_size));
        if(crl_list == NULL) {
            xmlSecMallocError(sizeof(gnutls_x509_crl_t) * (crl_list_size + crl_ctx_list_size),
                xmlSecKeyDataStoreGetName(store));
            goto done;
        }
        for(ii = 0; ii < crl_list_size; ++ii) {
            crl = xmlSecPtrListGetItem(crls, ii);
            if(crl == NULL) {
                xmlSecInternalError("xmlSecPtrListGetItem(crls)",
                    xmlSecKeyDataStoreGetName(store));
                goto done;
            }
            crl_list[crl_actual_list_size++] = crl;
        }
        for(ii = 0; ii < crl_ctx_list_size; ++ii) {
            crl =  xmlSecPtrListGetItem(&(ctx->crls), ii);
            if(crl == NULL) {
                xmlSecInternalError("xmlSecPtrListGetItem(crls)",
                    xmlSecKeyDataStoreGetName(store));
                goto done;
            }
            crl_list[crl_actual_list_size++] = crl;
        }
    }

    ca_list_size = xmlSecPtrListGetSize(&(ctx->certsTrusted));
    if(ca_list_size > 0) {
        ca_list = (gnutls_x509_crt_t *)xmlMalloc(sizeof(gnutls_x509_crt_t) * ca_list_size);
        if(ca_list == NULL) {
            xmlSecMallocError(sizeof(gnutls_x509_crt_t) * ca_list_size,
                              xmlSecKeyDataStoreGetName(store));
            goto done;
        }
        for(ii = 0; ii < ca_list_size; ++ii) {
            ca_list[ii] = xmlSecPtrListGetItem(&(ctx->certsTrusted), ii);
            if(ca_list[ii] == NULL) {
                xmlSecInternalError("xmlSecPtrListGetItem(certsTrusted)",
                                    xmlSecKeyDataStoreGetName(store));
                goto done;
            }
        }
    }


    /* gnutls doesn't allow to specify "verification" timestamp so
     * we have to do it ourselves. Unfortunately it doesn't work
     * for CRLs yet: https://github.com/lsh123/xmlsec/issues/579
     */
    if(keyInfoCtx->certsVerificationTime > 0) {
        flags |= GNUTLS_VERIFY_DISABLE_TIME_CHECKS;
    }

    flags |= GNUTLS_VERIFY_ALLOW_UNSORTED_CHAIN;
    if((keyInfoCtx->flags & XMLSEC_KEYINFO_FLAGS_X509DATA_SKIP_STRICT_CHECKS) != 0) {
        flags |= GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD2;
        flags |= GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD5;
#if GNUTLS_VERSION_NUMBER >= 0x030600
        flags |= GNUTLS_VERIFY_ALLOW_SIGN_WITH_SHA1;
#endif /* GNUTLS_VERSION_NUMBER >= 0x030600 */
    }

    /* We are going to build all possible cert chains and try to verify them */
    for(ii = 0; (ii < certs_size) && (res == NULL); ++ii) {
        gnutls_x509_crt_t cert, cert2;
        xmlSecSize cert_list_cur_size = 0;
        unsigned int verify = 0;

        cert = xmlSecPtrListGetItem(certs, ii);
        if(cert == NULL) {
            xmlSecInternalError("xmlSecPtrListGetItem(certs)",
                                xmlSecKeyDataStoreGetName(store));
            goto done;
        }

        /* check if we are the "leaf" node in the certs chain */
        if(xmlSecGnuTLSX509FindSignedCert(certs, cert) != NULL) {
            continue;
        }

        /* build the chain */
        for(cert2 = cert, cert_list_cur_size = 0;
            (cert2 != NULL) && (cert_list_cur_size < cert_list_size);
            ++cert_list_cur_size)
        {
            gnutls_x509_crt_t tmp;

            /* store */
            cert_list[cert_list_cur_size] = cert2;

            /* find next */
            tmp = xmlSecGnuTLSX509FindSignerCert(certs, cert2);
            if(tmp == NULL) {
                tmp = xmlSecGnuTLSX509FindSignerCert(&(ctx->certsUntrusted), cert2);
            }
            cert2 = tmp;
        }

        /* try to verify */
        if((keyInfoCtx->flags & XMLSEC_KEYINFO_FLAGS_X509DATA_DONT_VERIFY_CERTS) == 0) {
            unsigned int cert_list_cur_len, ca_list_len, crl_list_len;

            XMLSEC_SAFE_CAST_SIZE_TO_UINT(cert_list_cur_size, cert_list_cur_len, goto done, NULL);
            XMLSEC_SAFE_CAST_SIZE_TO_UINT(ca_list_size, ca_list_len, goto done, NULL);
            XMLSEC_SAFE_CAST_SIZE_TO_UINT(crl_actual_list_size, crl_list_len, goto done, NULL);

            err = gnutls_x509_crt_list_verify(
                    cert_list, cert_list_cur_len, /* certs chain */
                    ca_list, ca_list_len, /* trusted cas */
                    crl_list, crl_list_len, /* crls */
                    flags, /* flags */
                    &verify);
        } else {
            err = GNUTLS_E_SUCCESS;
        }
        if(err != GNUTLS_E_SUCCESS) {
            xmlSecGnuTLSError("gnutls_x509_crt_list_verify", err, NULL);
            /* ignore error, don't stop, continue! */
            continue;
        } else if(verify != 0) {
            xmlSecOtherError2(XMLSEC_ERRORS_R_CERT_VERIFY_FAILED, NULL,
                "gnutls_x509_crt_list_verify: verification failed: status=%u", verify);
            /* ignore error, don't stop, continue! */
            continue;
        }

        /* gnutls doesn't allow to specify "verification" timestamp so
           we have to do it ourselves */
        ret = xmlSecGnuTLSX509CheckCrtsTime(cert_list, cert_list_cur_size, verification_time);
        if(ret != 1) {
            xmlSecInternalError("xmlSecGnuTLSX509CheckCrtsTime", NULL);
            /* ignore error, don't stop, continue! */
            continue;
        }

        /* DONE! */
        res = cert;
    }

done:
    /* cleanup */
    if(ca_list != NULL) {
        xmlFree(ca_list);
    }
    if(crl_list != NULL) {
        xmlFree(crl_list);
    }
    if(cert_list != NULL) {
        xmlFree(cert_list);
    }

    return(res);
}

/**
 * xmlSecGnuTLSX509StoreAdoptCert:
 * @store:              the pointer to X509 key data store klass.
 * @cert:               the pointer to GnuTLS X509 certificate.
 * @type:               the certificate type (trusted/untrusted).
 *
 * Adds trusted (root) or untrusted certificate to the store.
 *
 * Returns: 0 on success or a negative value if an error occurs.
 */
int
xmlSecGnuTLSX509StoreAdoptCert(xmlSecKeyDataStorePtr store, gnutls_x509_crt_t cert, xmlSecKeyDataType type) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    int ret;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), -1);
    xmlSecAssert2(cert != NULL, -1);

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, -1);

    if((type & xmlSecKeyDataTypeTrusted) != 0) {
        ret = xmlSecPtrListAdd(&(ctx->certsTrusted), cert);
        if(ret < 0) {
            xmlSecInternalError("xmlSecPtrListAdd(trusted)",
                                xmlSecKeyDataStoreGetName(store));
            return(-1);
        }
    } else {
        ret = xmlSecPtrListAdd(&(ctx->certsUntrusted), cert);
        if(ret < 0) {
            xmlSecInternalError("xmlSecPtrListAdd(untrusted)",
                                xmlSecKeyDataStoreGetName(store));
            return(-1);
        }
    }

    /* done */
    return(0);
}


/**
 * xmlSecGnuTLSX509StoreAdoptCrl:
 * @store:              the pointer to X509 key data store klass.
 * @crl:                the pointer to GnuTLS X509 CRL.
 *
 * Adds CRL to the store.
 *
 * Returns: 0 on success or a negative value if an error occurs.
 */
int
xmlSecGnuTLSX509StoreAdoptCrl(xmlSecKeyDataStorePtr store, gnutls_x509_crl_t crl) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    int ret;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), -1);
    xmlSecAssert2(crl != NULL, -1);

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, -1);

   ret = xmlSecPtrListAdd(&(ctx->crls), crl);
    if(ret < 0) {
        xmlSecInternalError("xmlSecPtrListAdd(crls)", xmlSecKeyDataStoreGetName(store));
        return(-1);
    }

    /* done */
    return(0);
}


static int
xmlSecGnuTLSX509StoreInitialize(xmlSecKeyDataStorePtr store) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    int ret;

    xmlSecAssert2(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId), -1);

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert2(ctx != NULL, -1);

    memset(ctx, 0, sizeof(xmlSecGnuTLSX509StoreCtx));

    ret = xmlSecPtrListInitialize(&(ctx->certsTrusted), xmlSecGnuTLSX509CrtListId);
    if(ret < 0) {
        xmlSecInternalError("xmlSecPtrListInitialize(trusted)",
                            xmlSecKeyDataStoreGetName(store));
        return(-1);
    }

    ret = xmlSecPtrListInitialize(&(ctx->certsUntrusted), xmlSecGnuTLSX509CrtListId);
    if(ret < 0) {
        xmlSecInternalError("xmlSecPtrListInitialize(untrusted)",
                            xmlSecKeyDataStoreGetName(store));
        return(-1);
    }

    ret = xmlSecPtrListInitialize(&(ctx->crls), xmlSecGnuTLSX509CrlListId);
    if(ret < 0) {
        xmlSecInternalError("xmlSecPtrListInitialize(crls)",
                            xmlSecKeyDataStoreGetName(store));
        return(-1);
    }

    return(0);
}

static void
xmlSecGnuTLSX509StoreFinalize(xmlSecKeyDataStorePtr store) {
    xmlSecGnuTLSX509StoreCtxPtr ctx;
    xmlSecAssert(xmlSecKeyDataStoreCheckId(store, xmlSecGnuTLSX509StoreId));

    ctx = xmlSecGnuTLSX509StoreGetCtx(store);
    xmlSecAssert(ctx != NULL);

    xmlSecPtrListFinalize(&(ctx->certsTrusted));
    xmlSecPtrListFinalize(&(ctx->certsUntrusted));
    xmlSecPtrListFinalize(&(ctx->crls));

    memset(ctx, 0, sizeof(xmlSecGnuTLSX509StoreCtx));
}


/*****************************************************************************
 *
 * Low-level x509 functions
 *
 *****************************************************************************/
#define XMLSEC_GNUTLS_DN_ATTRS_SIZE             1024

int
xmlSecGnuTLSX509DnsEqual(const xmlChar * ll, const xmlChar * rr) {
    xmlSecGnuTLSDnAttr ll_attrs[XMLSEC_GNUTLS_DN_ATTRS_SIZE];
    xmlSecGnuTLSDnAttr rr_attrs[XMLSEC_GNUTLS_DN_ATTRS_SIZE];
    int ret;
    int res = -1;

    xmlSecAssert2(ll != NULL, -1);
    xmlSecAssert2(rr != NULL, -1);

    /* fast version first */
    if(xmlStrEqual(ll, rr)) {
        return(1);
    }

    /* prepare */
    xmlSecGnuTLSDnAttrsInitialize(ll_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    xmlSecGnuTLSDnAttrsInitialize(rr_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);

    /* parse */
    ret = xmlSecGnuTLSDnAttrsParse(ll, ll_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    if(ret < 0) {
        xmlSecInternalError("xmlSecGnuTLSDnAttrsParse(ll)", NULL);
        goto done;
    }

    ret = xmlSecGnuTLSDnAttrsParse(rr, rr_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    if(ret < 0) {
        xmlSecInternalError("xmlSecGnuTLSDnAttrsParse(rr)", NULL);
        goto done;
    }

    /* compare */
    ret = xmlSecGnuTLSDnAttrsEqual(ll_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE,
                                   rr_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    if(ret == 1) {
        res = 1;
    } else if(ret == 0) {
        res = 0;
    } else {
        xmlSecInternalError("xmlSecGnuTLSDnAttrsEqual", NULL);
        goto done;
    }

done:
    xmlSecGnuTLSDnAttrsDeinitialize(ll_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    xmlSecGnuTLSDnAttrsDeinitialize(rr_attrs, XMLSEC_GNUTLS_DN_ATTRS_SIZE);
    return(res);
}


/**
 * xmlSecGnuTLSX509CertCompareSKI:
 *
 * Returns 0 if SKI matches, 1 if SKI doesn't match and a negative value if an error occurs.
 */
int
xmlSecGnuTLSX509CertCompareSKI(gnutls_x509_crt_t cert, const xmlSecByte * ski, xmlSecSize skiSize) {
    xmlSecByte* buf = NULL;
    size_t bufSizeT = 0;
    xmlSecSize bufSize;
    unsigned int critical = 0;
    int err;
    int res = -1;

    xmlSecAssert2(cert != NULL, -1);
    xmlSecAssert2(ski != NULL, -1);
    xmlSecAssert2(skiSize > 0, -1);

    /* get ski size */
    err = gnutls_x509_crt_get_subject_key_id(cert, NULL, &bufSizeT, &critical);
    if((err != GNUTLS_E_SHORT_MEMORY_BUFFER) || (bufSizeT <= 0)) {
        xmlSecGnuTLSError("gnutls_x509_crt_get_subject_key_id", err, NULL);
        goto done;
    }
    XMLSEC_SAFE_CAST_SIZE_T_TO_SIZE(bufSizeT, bufSize, goto done, NULL);

    if(skiSize != bufSize) {
        /* doesn't match */
        res = 1;
        goto done;
    }

    /* allocate buffer */
    buf = (xmlSecByte *)xmlMalloc(bufSizeT + 1);
    if(buf == NULL) {
        xmlSecMallocError(bufSizeT + 1, NULL);
        goto done;
    }

    /* write ski out */
    err = gnutls_x509_crt_get_subject_key_id(cert, buf, &bufSizeT, &critical);
    if(err != GNUTLS_E_SUCCESS) {
        xmlSecGnuTLSError("gnutls_x509_crt_get_subject_key_id", err, NULL);
        goto done;
    }

    /* compare */
    if(memcmp(ski, buf, bufSize) != 0) {
        /* doesn't match */
        res = 1;
        goto done;
    }

    /* match! */
    res = 0;

done:
    /* cleanup */
    if(buf != NULL) {
        xmlFree(buf);
    }
    return(res);
}

static gnutls_x509_crt_t
xmlSecGnuTLSX509FindCert(xmlSecPtrListPtr certs, xmlSecGnuTLSX509FindCertCtxPtr findCertCtx) {
    xmlSecSize ii, sz;
    int ret;

    xmlSecAssert2(certs != NULL, NULL);
    xmlSecAssert2(findCertCtx != NULL, NULL);

    /* todo: this is not the fastest way to search certs */
    sz = xmlSecPtrListGetSize(certs);
    for(ii = 0; (ii < sz); ++ii) {
        gnutls_x509_crt_t cert = xmlSecPtrListGetItem(certs, ii);
        if(cert == NULL) {
            xmlSecInternalError2("xmlSecPtrListGetItem", NULL, "pos=" XMLSEC_SIZE_FMT, ii);
            return(NULL);
        }


        /* returns 1 for match, 0 for no match, and a negative value if an error occurs */
        ret = xmlSecGnuTLSX509FindCertCtxMatch(findCertCtx, cert);
        if(ret < 0) {
            xmlSecInternalError2("xmlSecGnuTLSX509FindCertCtxMatch", NULL, "pos=" XMLSEC_SIZE_FMT, ii);
            return(NULL);
        } else if(ret == 1) {
            return(cert);
        }
    }
    /* not found */
    return(NULL);
}

/* signed cert has issuer dn equal to our's subject dn */
static gnutls_x509_crt_t
xmlSecGnuTLSX509FindSignedCert(xmlSecPtrListPtr certs, gnutls_x509_crt_t cert) {
    gnutls_x509_crt_t res = NULL;
    xmlChar * subject = NULL;
    xmlSecSize ii, sz;

    xmlSecAssert2(certs != NULL, NULL);
    xmlSecAssert2(cert != NULL, NULL);

    /* get subject */
    subject = xmlSecGnuTLSX509CertGetSubjectDN(cert);
    if(subject == NULL) {
        xmlSecInternalError("xmlSecGnuTLSX509CertGetSubjectDN", NULL);
        goto done;
    }

    /* todo: this is not the fastest way to search certs */
    sz = xmlSecPtrListGetSize(certs);
    for(ii = 0; (ii < sz) && (res == NULL); ++ii) {
        gnutls_x509_crt_t tmp;
        xmlChar * issuer;

        tmp = xmlSecPtrListGetItem(certs, ii);
        if(tmp == NULL) {
            xmlSecInternalError2("xmlSecPtrListGetItem", NULL,
                "pos=" XMLSEC_SIZE_FMT, ii);
            goto done;
        }

        issuer = xmlSecGnuTLSX509CertGetIssuerDN(tmp);
        if(issuer == NULL) {
            xmlSecInternalError2("xmlSecGnuTLSX509CertGetIssuerDN", NULL,
                "pos=" XMLSEC_SIZE_FMT, ii);
            goto done;
        }

        /* are we done? */
        if(xmlSecGnuTLSX509DnsEqual(subject, issuer) == 1) {
            res = tmp;
        }
        xmlFree(issuer);
    }

done:
    if(subject != NULL) {
        xmlFree(subject);
    }
    return(res);
}

/* signer cert has subject dn equal to our's issuer dn */
static gnutls_x509_crt_t
xmlSecGnuTLSX509FindSignerCert(xmlSecPtrListPtr certs, gnutls_x509_crt_t cert) {
    gnutls_x509_crt_t res = NULL;
    xmlChar * issuer = NULL;
    xmlSecSize ii, sz;

    xmlSecAssert2(certs != NULL, NULL);
    xmlSecAssert2(cert != NULL, NULL);

    /* get issuer */
    issuer = xmlSecGnuTLSX509CertGetIssuerDN(cert);
    if(issuer == NULL) {
        xmlSecInternalError("xmlSecGnuTLSX509CertGetIssuerDN", NULL);
        goto done;
    }

    /* todo: this is not the fastest way to search certs */
    sz = xmlSecPtrListGetSize(certs);
    for(ii = 0; (ii < sz) && (res == NULL); ++ii) {
        gnutls_x509_crt_t tmp;
        xmlChar * subject;

        tmp = xmlSecPtrListGetItem(certs, ii);
        if(tmp == NULL) {
            xmlSecInternalError2("xmlSecPtrListGetItem", NULL,
                "pos=" XMLSEC_SIZE_FMT, ii);
            goto done;
        }

        subject = xmlSecGnuTLSX509CertGetSubjectDN(tmp);
        if(subject == NULL) {
            xmlSecInternalError2("xmlSecGnuTLSX509CertGetSubjectDN", NULL,
                "pos=" XMLSEC_SIZE_FMT, ii);
            goto done;
        }

        /* are we done? */
        if((xmlSecGnuTLSX509DnsEqual(issuer, subject) == 1)) {
            res = tmp;
        }
        xmlFree(subject);
    }

done:
    if(issuer != NULL) {
        xmlFree(issuer);
    }
    return(res);
}

#endif /* XMLSEC_NO_X509 */

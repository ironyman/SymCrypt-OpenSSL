//
// Copyright (c) Microsoft Corporation. Licensed under the MIT license.
//

#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/proverr.h>

#include "scossl_rsa.h"
#include "p_scossl_rsa.h"
#include "p_scossl_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    SCOSSL_RSA_KEY_CTX *keyCtx;
    UINT padding;
    UINT operation;

    // Needed for fetching md
    OSSL_LIB_CTX *libctx;
    char* propq;

    EVP_MD_CTX *mdctx;
    EVP_MD *md;
    const OSSL_ITEM *mdInfo; // Informational, must match md if set
    BOOL allowMdUpdates;

    // PSS params
    const OSSL_ITEM *mgf1MdInfo; // Informational, must match md if set
    int cbSalt;
} SCOSSL_RSA_SIGN_CTX;

#define SCOSSL_RSA_SIGNATURE_GETTABLE_PARAMS                        \
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0), \
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),

#define SCOSSL_RSA_PSS_SIGNATURE_GETTABLE_PARAMS                       \
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0), \
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL, 0), \
    OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL),

#define SCOSSL_RSA_PSS_SIGNATURE_SETTABLE_PARAMS \
    SCOSSL_RSA_PSS_SIGNATURE_GETTABLE_PARAMS     \
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_PROPERTIES, NULL, 0),

static const OSSL_PARAM p_scossl_rsa_sig_ctx_gettable_param_types[] = {
    SCOSSL_RSA_SIGNATURE_GETTABLE_PARAMS
    OSSL_PARAM_END};

static const OSSL_PARAM p_scossl_rsa_pss_sig_ctx_gettable_param_types[] = {
    SCOSSL_RSA_SIGNATURE_GETTABLE_PARAMS
    SCOSSL_RSA_PSS_SIGNATURE_GETTABLE_PARAMS
    OSSL_PARAM_END};

// Padding may not be set at the time of querying settable params, so PSS params
// are always accepted. The provider will check the padding before attempting
// to set the PSS parameters
static const OSSL_PARAM p_scossl_rsa_sig_ctx_settable_param_types[] = {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PROPERTIES, NULL, 0),
    SCOSSL_RSA_PSS_SIGNATURE_SETTABLE_PARAMS
    OSSL_PARAM_END};

// Digest cannot be set during digest sign update
static const OSSL_PARAM p_scossl_rsa_sig_ctx_settable_param_types_no_digest[] = {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
    SCOSSL_RSA_PSS_SIGNATURE_SETTABLE_PARAMS
    OSSL_PARAM_END};

static OSSL_ITEM p_scossl_rsa_sign_padding_modes[] = {
    {RSA_PKCS1_PADDING, OSSL_PKEY_RSA_PAD_MODE_PKCSV15},
    {RSA_PKCS1_PSS_PADDING, OSSL_PKEY_RSA_PAD_MODE_PSS},
    {0, NULL}};

static SCOSSL_STATUS p_scossl_rsa_set_ctx_params(_Inout_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const OSSL_PARAM params[]);

static SCOSSL_RSA_SIGN_CTX *p_scossl_rsa_newctx(_In_ SCOSSL_PROVCTX *provctx, _In_ const char *propq)
{
    SCOSSL_RSA_SIGN_CTX *ctx = OPENSSL_zalloc(sizeof(SCOSSL_RSA_SIGN_CTX));
    if (ctx != NULL)
    {
        if (propq != NULL && ((ctx->propq = OPENSSL_strdup(propq)) == NULL))
        {
            OPENSSL_free(ctx);
            ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
            return NULL;
        }

        ctx->libctx = provctx->libctx;
        ctx->allowMdUpdates = TRUE;
        ctx->padding = RSA_PKCS1_PADDING;
    }

    return ctx;
}

static void p_scossl_rsa_freectx(SCOSSL_RSA_SIGN_CTX *ctx)
{
    EVP_MD_CTX_free(ctx->mdctx);
    EVP_MD_free(ctx->md);
    OPENSSL_free(ctx->propq);
    OPENSSL_free(ctx);
}

static SCOSSL_RSA_SIGN_CTX *p_scossl_rsa_dupctx(_In_ SCOSSL_RSA_SIGN_CTX *ctx)
{
    SCOSSL_RSA_SIGN_CTX *copyCtx = OPENSSL_zalloc(sizeof(SCOSSL_RSA_SIGN_CTX));
    if (copyCtx != NULL)
    {
        if ((ctx->propq != NULL && ((copyCtx->propq = OPENSSL_strdup(ctx->propq)) == NULL)) ||
            (ctx->mdctx != NULL && ((copyCtx->mdctx = EVP_MD_CTX_dup((const EVP_MD_CTX *)ctx->mdctx)) == NULL)) ||
            (ctx->md    != NULL && !EVP_MD_up_ref(ctx->md)))
        {
            p_scossl_rsa_freectx(copyCtx);
            ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
            copyCtx = NULL;
        }

        copyCtx->libctx = ctx->libctx;
        copyCtx->padding = ctx->padding;
        copyCtx->keyCtx = ctx->keyCtx;
        copyCtx->md = ctx->md;
        copyCtx->cbSalt = ctx->cbSalt;
        copyCtx->mdInfo = ctx->mdInfo;
        copyCtx->mgf1MdInfo = ctx->mgf1MdInfo;
    }

    return copyCtx;
}

static SCOSSL_STATUS p_scossl_rsa_signverify_init(_Inout_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ SCOSSL_RSA_KEY_CTX *keyCtx,
                                                  _In_ const OSSL_PARAM params[], int operation)
{
    if (ctx == NULL ||
        (keyCtx == NULL && ctx->keyCtx == NULL) ||
        !keyCtx->initialized)
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_NO_KEY_SET);
        return SCOSSL_FAILURE;
    }

    ctx->cbSalt = RSA_PSS_SALTLEN_AUTO_DIGEST_MAX;
    ctx->operation = operation;
    if (keyCtx != NULL)
    {
        ctx->keyCtx = keyCtx;
    }

    return p_scossl_rsa_set_ctx_params(ctx, params);
}

static SCOSSL_STATUS p_scossl_rsa_sign_init(_Inout_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ SCOSSL_RSA_KEY_CTX *keyCtx,
                                            _In_ const OSSL_PARAM params[])
{
    return p_scossl_rsa_signverify_init(ctx, keyCtx, params, EVP_PKEY_OP_SIGN);
}

static SCOSSL_STATUS p_scossl_rsa_verify_init(_Inout_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ SCOSSL_RSA_KEY_CTX *keyCtx,
                                              _In_ const OSSL_PARAM params[])
{
    return p_scossl_rsa_signverify_init(ctx, keyCtx, params, EVP_PKEY_OP_VERIFY);
}

static SCOSSL_STATUS p_scossl_rsa_sign(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                       _Out_writes_bytes_(*siglen) unsigned char *sig, _Out_ size_t *siglen, size_t sigsize,
                                       _In_reads_bytes_(tbslen) const unsigned char *tbs, size_t tbslen)
{
    if (sig != NULL && sigsize < SymCryptRsakeySizeofModulus(ctx->keyCtx->key))
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_OUTPUT_BUFFER_TOO_SMALL);
        return SCOSSL_FAILURE;
    }

    if (ctx->mdInfo == NULL)
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
        return SCOSSL_FAILURE;
    }

    switch (ctx->padding)
    {
    case RSA_PKCS1_PADDING:
        return scossl_rsa_pkcs1_sign(ctx->keyCtx, ctx->mdInfo->id, tbs, tbslen, sig, siglen);
    case RSA_PKCS1_PSS_PADDING:
        return scossl_rsapss_sign(ctx->keyCtx, ctx->mdInfo->id, ctx->cbSalt, tbs, tbslen, sig, siglen);
    default:
        ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_PADDING_MODE);
    }

    return SCOSSL_FAILURE;
}

static SCOSSL_STATUS p_scossl_rsa_verify(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                         _In_reads_bytes_(siglen) const unsigned char *sig, size_t siglen,
                                         _In_reads_bytes_(tbslen) const unsigned char *tbs, size_t tbslen)
{
    if (ctx->mdInfo == NULL)
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
        return SCOSSL_FAILURE;
    }

    switch (ctx->padding)
    {
    case RSA_PKCS1_PADDING:
        return scossl_rsa_pkcs1_verify(ctx->keyCtx, ctx->mdInfo->id, tbs, tbslen, sig, siglen);
    case RSA_PKCS1_PSS_PADDING:
        return scossl_rsapss_verify(ctx->keyCtx, ctx->mdInfo->id, ctx->cbSalt, tbs, tbslen, sig, siglen);
    default:
        ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_PADDING_MODE);
    }

    return SCOSSL_FAILURE;
}

static SCOSSL_STATUS p_scossl_rsa_digest_signverify_init(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const char *mdname,
                                                         _In_ SCOSSL_RSA_KEY_CTX *keyCtx, _In_ const OSSL_PARAM params[], int operation)
{
    if (!p_scossl_rsa_signverify_init(ctx, keyCtx, params, operation))
    {
        return SCOSSL_FAILURE;
    }

    if (mdname != NULL &&
        (mdname[0] == '\0' || !EVP_MD_is_a(ctx->md, mdname)))
    {
        // Different digest specified than what was previously set by paramters.
        EVP_MD *md;
        const OSSL_ITEM *mdInfo = p_scossl_rsa_get_supported_md(ctx->libctx, mdname, NULL, &md);

        if (mdInfo == NULL ||
            (ctx->mgf1MdInfo != NULL && mdInfo->id != ctx->mgf1MdInfo->id))
        {
            EVP_MD_free(md);
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return SCOSSL_FAILURE;
        }

        EVP_MD_free(ctx->md);
        ctx->md = md;
        ctx->mdInfo = mdInfo;
    }

    if (ctx->mdctx == NULL &&
        ((ctx->mdctx = EVP_MD_CTX_new()) == NULL))
    {
        ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
        return SCOSSL_FAILURE;
    }

    if (!EVP_DigestInit_ex2(ctx->mdctx, ctx->md, params))
    {
        EVP_MD_CTX_free(ctx->mdctx);
        ctx->mdctx = NULL;
        return SCOSSL_FAILURE;
    }

    ctx->allowMdUpdates = FALSE;

    return SCOSSL_SUCCESS;
}

static SCOSSL_STATUS p_scossl_rsa_digest_sign_init(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const char *mdname,
                                                   _In_ SCOSSL_RSA_KEY_CTX *keyCtx, _In_ const OSSL_PARAM params[])
{
    return p_scossl_rsa_digest_signverify_init(ctx, mdname, keyCtx, params, EVP_PKEY_OP_SIGN);
}

static SCOSSL_STATUS p_scossl_rsa_digest_verify_init(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const char *mdname,
                                                     _In_ SCOSSL_RSA_KEY_CTX *keyCtx, _In_ const OSSL_PARAM params[])
{
    return p_scossl_rsa_digest_signverify_init(ctx, mdname, keyCtx, params, EVP_PKEY_OP_VERIFY);
}

static SCOSSL_STATUS p_scossl_rsa_digest_signverify_update(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                                           _In_reads_bytes_(datalen) const unsigned char *data, size_t datalen)
{
    if (ctx->mdctx == NULL)
    {
        return SCOSSL_FAILURE;
    }

    return EVP_DigestUpdate(ctx->mdctx, data, datalen);
}

static SCOSSL_STATUS p_scossl_rsa_digest_sign_final(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                                    _Out_writes_bytes_(*siglen) unsigned char *sig, _Out_ size_t *siglen, size_t sigsize)
{
    SCOSSL_STATUS ret = SCOSSL_FAILURE;
    BYTE digest[EVP_MAX_MD_SIZE];
    UINT cbDigest = 0;

    if (ctx->mdctx == NULL)
    {
        return ret;
    }

    // If sig is NULL, this is a size fetch, and the digest does not need to be computed
    if (sig == NULL || EVP_DigestFinal(ctx->mdctx, digest, &cbDigest))
    {
        ctx->allowMdUpdates = sig != NULL;
        ret = p_scossl_rsa_sign(ctx, sig, siglen, sigsize, digest, cbDigest);
    }

    return ret;
}

static SCOSSL_STATUS p_scossl_rsa_digest_verify_final(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                                      _In_reads_bytes_(siglen) unsigned char *sig, size_t siglen)
{
    BYTE digest[EVP_MAX_MD_SIZE];
    UINT cbDigest = 0;

    if (ctx->mdctx == NULL)
    {
        return SCOSSL_FAILURE;
    }

    ctx->allowMdUpdates = TRUE;

    return EVP_DigestFinal(ctx->mdctx, digest, &cbDigest) &&
           p_scossl_rsa_verify(ctx, sig, siglen, digest, cbDigest);
}

static const OSSL_PARAM *p_scossl_rsa_settable_ctx_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx,
                                                          ossl_unused void *provctx)
{
    return ctx->allowMdUpdates ? p_scossl_rsa_sig_ctx_settable_param_types : p_scossl_rsa_sig_ctx_settable_param_types_no_digest;
}

static SCOSSL_STATUS p_scossl_rsa_set_ctx_params(_Inout_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const OSSL_PARAM params[])
{
    const OSSL_PARAM *p;
    const OSSL_PARAM *param_propq;
    const char *mdName, *mdProps;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST)) != NULL)
    {
        EVP_MD *md;
        const OSSL_ITEM *mdInfo;

        if (!OSSL_PARAM_get_utf8_string_ptr(p, &mdName))
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        mdProps = NULL;
        if ((param_propq = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PROPERTIES)) != NULL &&
            !OSSL_PARAM_get_utf8_string_ptr(p, &mdProps))
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        // ScOSSL does not support distinct MD and MGF1 MD
        mdInfo = p_scossl_rsa_get_supported_md(ctx->libctx, mdName, mdProps, &md);
        if (mdInfo == NULL ||
            (ctx->mgf1MdInfo != NULL && mdInfo->id != ctx->mgf1MdInfo->id))
        {
            EVP_MD_free(md);
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return SCOSSL_FAILURE;
        }

        ctx->md = md;
        ctx->mdInfo = mdInfo;
    }

    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PAD_MODE)) != NULL)
    {
        // Padding mode may be passed as legacy NID or string, and is
        // checked against the padding modes the ScOSSL provider supports
        int i = 0;
        UINT padding;

        switch (p->data_type)
        {
        case OSSL_PARAM_INTEGER:
            if (!OSSL_PARAM_get_uint(p, &padding))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
                return SCOSSL_FAILURE;
            }

            while (p_scossl_rsa_sign_padding_modes[i].id != 0 &&
                   padding != p_scossl_rsa_sign_padding_modes[i].id)
            {
                i++;
            }
            break;
        case OSSL_PARAM_UTF8_STRING:
            while (p_scossl_rsa_sign_padding_modes[i].id != 0 &&
                   OPENSSL_strcasecmp(p->data, p_scossl_rsa_sign_padding_modes[i].ptr) != 0)
            {
                i++;
            }
            break;
        default:
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        padding = p_scossl_rsa_sign_padding_modes[i].id;

        if (padding == 0)
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE);
            return SCOSSL_FAILURE;
        }

        ctx->padding = padding;
    }

    //
    // PSS paramaters
    //
    if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_PSS_SALTLEN)) != NULL)
    {
        // PSS padding must be set before setting PSS parameters
        if (ctx->padding != RSA_PKCS1_PSS_PADDING)
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED);
            return SCOSSL_FAILURE;
        }

        int saltlen;
        // Padding mode may be passed as legacy NID or string
        switch (p->data_type)
        {
        case OSSL_PARAM_INTEGER:
            if (!OSSL_PARAM_get_int(p, &saltlen))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
                return SCOSSL_FAILURE;
            }
            break;
        case OSSL_PARAM_UTF8_STRING:
            if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_DIGEST) == 0)
            {
                saltlen = RSA_PSS_SALTLEN_DIGEST;
            }
            else if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_AUTO) == 0)
            {
                // Sign: Maximized salt length
                // Verify: Autorecovered salt length (not supported)
                saltlen = RSA_PSS_SALTLEN_AUTO;
            }
            else if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_MAX) == 0)
            {
                saltlen = RSA_PSS_SALTLEN_MAX;
            }
            else if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_AUTO_DIGEST_MAX) == 0)
            {
                // Sign: Smaller of digest length or maximized salt length
                // Verify: Autorecovered salt length (not supported)
                saltlen = RSA_PSS_SALTLEN_AUTO_DIGEST_MAX;
            }
            else
            {
                saltlen = atoi(p->data);
            }
            break;
        default:
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        if (saltlen < RSA_PSS_SALTLEN_AUTO_DIGEST_MAX ||
            (ctx->operation == EVP_PKEY_OP_VERIFY &&
              (saltlen == RSA_PSS_SALTLEN_AUTO ||
               saltlen == RSA_PSS_SALTLEN_AUTO_DIGEST_MAX)))
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_SALT_LENGTH);
            return SCOSSL_FAILURE;
        }

        ctx->cbSalt = saltlen;
    }

    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST)) != NULL)
    {
        EVP_MD *md;
        const OSSL_ITEM *mgf1MdInfo;

        if (!OSSL_PARAM_get_utf8_string_ptr(p, &mdName))
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        mdProps = NULL;
        if ((param_propq = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PROPERTIES)) != NULL &&
            !OSSL_PARAM_get_utf8_string_ptr(p, &mdProps))
        {
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return SCOSSL_FAILURE;
        }

        // ScOSSL does not support distinct MD and MGF1 MD
        mgf1MdInfo = p_scossl_rsa_get_supported_md(ctx->libctx, mdName, mdProps, &md);
        if (mgf1MdInfo == NULL ||
            (ctx->mdInfo != NULL && mgf1MdInfo->id != ctx->mdInfo->id))
        {
            EVP_MD_free(md);
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return SCOSSL_FAILURE;
        }

        EVP_MD_free(ctx->md);
        ctx->md = md;
        ctx->mgf1MdInfo = mgf1MdInfo;
    }

    return SCOSSL_SUCCESS;
}

static const OSSL_PARAM *p_scossl_rsa_gettable_ctx_params(_In_ SCOSSL_RSA_SIGN_CTX  *ctx,
                                                          ossl_unused void *provctx)
{
    return ctx->padding == RSA_PKCS1_PSS_PADDING ? p_scossl_rsa_pss_sig_ctx_gettable_param_types : p_scossl_rsa_sig_ctx_gettable_param_types;
}

static SCOSSL_STATUS p_scossl_rsa_get_ctx_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _Inout_ OSSL_PARAM params[])
{
    if (params == NULL)
    {
        return SCOSSL_SUCCESS;
    }

    OSSL_PARAM *p;

    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST)) != NULL &&
        !OSSL_PARAM_set_utf8_string(p, ctx->mdInfo == NULL ? "" : ctx->mdInfo->ptr))
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
        return SCOSSL_FAILURE;
    }

    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_PAD_MODE)) != NULL)
    {
        int i = 0;

        // Padding mode may be retrieved as legacy NID or string
        switch (p->data_type)
        {
        case OSSL_PARAM_INTEGER:
            if (!OSSL_PARAM_set_int(p, ctx->padding))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
                return SCOSSL_FAILURE;
            }
            break;
        case OSSL_PARAM_UTF8_STRING:
            while (p_scossl_rsa_sign_padding_modes[i].id != 0 &&
                   ctx->padding != p_scossl_rsa_sign_padding_modes[i].id)
            {
                i++;
            }

            if (!OSSL_PARAM_set_utf8_string(p, p_scossl_rsa_sign_padding_modes[i].ptr))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
                return SCOSSL_FAILURE;
            }
            break;
        default:
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
            return SCOSSL_FAILURE;
        }
    }

    //
    // PSS paramaters
    //
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_RSA_PSS_SALTLEN)) != NULL)
    {
        const char* saltLenText = NULL;
        int len;

        // Padding mode may be accepted as legacy NID or string
        switch (p->data_type)
        {
        case OSSL_PARAM_INTEGER:
            if (!OSSL_PARAM_set_int(p, ctx->cbSalt))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
                return SCOSSL_FAILURE;
            }
            break;
        case OSSL_PARAM_UTF8_STRING:
            switch (ctx->cbSalt)
            {
                case RSA_PSS_SALTLEN_DIGEST:
                    saltLenText = OSSL_PKEY_RSA_PSS_SALT_LEN_DIGEST;
                    break;
                case RSA_PSS_SALTLEN_AUTO:
                    saltLenText = OSSL_PKEY_RSA_PSS_SALT_LEN_AUTO;
                    break;
                case RSA_PSS_SALTLEN_MAX:
                    saltLenText = OSSL_PKEY_RSA_PSS_SALT_LEN_MAX;
                    break;
                case RSA_PSS_SALTLEN_AUTO_DIGEST_MAX:
                    saltLenText = OSSL_PKEY_RSA_PSS_SALT_LEN_AUTO_DIGEST_MAX;
                    break;
                default:
                    len = BIO_snprintf(p->data, p->data_size, "%d",
                                           ctx->cbSalt);
                    if (len <= 0)
                        return 0;
                    p->return_size = len;
            }

            if (saltLenText != NULL &&
                !OSSL_PARAM_set_utf8_string(p, saltLenText))
            {
                ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
                return SCOSSL_FAILURE;
            }

            break;
        default:
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
            return SCOSSL_FAILURE;
        }
    }

    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST)) != NULL &&
        !OSSL_PARAM_set_utf8_string(p, ctx->mgf1MdInfo == NULL ? "" : ctx->mgf1MdInfo->ptr))
    {
        ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_SET_PARAMETER);
        return SCOSSL_FAILURE;
    }

    return SCOSSL_SUCCESS;
}

static const OSSL_PARAM *p_scossl_rsa_gettable_ctx_md_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx)
{
    if (ctx->md == NULL)
    {
        return SCOSSL_FAILURE;
    }

    return EVP_MD_gettable_ctx_params(ctx->md);
}

static SCOSSL_STATUS p_scossl_rsa_get_ctx_md_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _Inout_ OSSL_PARAM *params)
{
    if (ctx->mdctx == NULL)
    {
        return SCOSSL_FAILURE;
    }

    return EVP_MD_CTX_get_params(ctx->mdctx, params);
}

static const OSSL_PARAM *p_scossl_rsa_settable_ctx_md_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx)
{
    if (ctx->md == NULL)
    {
        return SCOSSL_FAILURE;
    }

    return EVP_MD_settable_ctx_params(ctx->md);
}

static SCOSSL_STATUS p_scossl_rsa_set_ctx_md_params(_In_ SCOSSL_RSA_SIGN_CTX *ctx, _In_ const OSSL_PARAM params[])
{
    if (ctx->mdctx == NULL)
    {
        return SCOSSL_FAILURE;
    }

    return EVP_MD_CTX_set_params(ctx->mdctx, params);
}

const OSSL_DISPATCH p_scossl_rsa_signature_functions[] = {
    {OSSL_FUNC_SIGNATURE_NEWCTX, (void (*)(void))p_scossl_rsa_newctx},
    {OSSL_FUNC_SIGNATURE_DUPCTX, (void (*)(void))p_scossl_rsa_dupctx},
    {OSSL_FUNC_SIGNATURE_FREECTX, (void (*)(void))p_scossl_rsa_freectx},
    {OSSL_FUNC_SIGNATURE_SIGN_INIT, (void (*)(void))p_scossl_rsa_sign_init},
    {OSSL_FUNC_SIGNATURE_SIGN, (void (*)(void))p_scossl_rsa_sign},
    {OSSL_FUNC_SIGNATURE_VERIFY_INIT, (void (*)(void))p_scossl_rsa_verify_init},
    {OSSL_FUNC_SIGNATURE_VERIFY, (void (*)(void))p_scossl_rsa_verify},
    {OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT, (void (*)(void))p_scossl_rsa_digest_sign_init},
    {OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE, (void (*)(void))p_scossl_rsa_digest_signverify_update},
    {OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL, (void (*)(void))p_scossl_rsa_digest_sign_final},
    {OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_INIT, (void (*)(void))p_scossl_rsa_digest_verify_init},
    {OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_UPDATE, (void (*)(void))p_scossl_rsa_digest_signverify_update},
    {OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_FINAL, (void (*)(void))p_scossl_rsa_digest_verify_final},
    {OSSL_FUNC_SIGNATURE_GET_CTX_PARAMS, (void (*)(void))p_scossl_rsa_get_ctx_params},
    {OSSL_FUNC_SIGNATURE_GETTABLE_CTX_PARAMS, (void (*)(void))p_scossl_rsa_gettable_ctx_params},
    {OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS, (void (*)(void))p_scossl_rsa_set_ctx_params},
    {OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS, (void (*)(void))p_scossl_rsa_settable_ctx_params},
    {OSSL_FUNC_SIGNATURE_GET_CTX_MD_PARAMS, (void (*)(void))p_scossl_rsa_get_ctx_md_params},
    {OSSL_FUNC_SIGNATURE_GETTABLE_CTX_MD_PARAMS, (void (*)(void))p_scossl_rsa_gettable_ctx_md_params},
    {OSSL_FUNC_SIGNATURE_SET_CTX_MD_PARAMS, (void (*)(void))p_scossl_rsa_set_ctx_md_params},
    {OSSL_FUNC_SIGNATURE_SETTABLE_CTX_MD_PARAMS, (void (*)(void))p_scossl_rsa_settable_ctx_md_params},
    {0, NULL}};

#ifdef __cplusplus
}
#endif
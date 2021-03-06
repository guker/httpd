/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <apr_strings.h>
#include <apr_optional.h>
#include <apr_optional_hooks.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_connection.h>
#include <http_protocol.h>
#include <http_log.h>

#include "h2_private.h"

#include "h2_stream.h"
#include "h2_task.h"
#include "h2_config.h"
#include "h2_ctx.h"
#include "h2_conn.h"
#include "h2_h2.h"

const char *h2_tls_protos[] = {
    "h2", NULL
};

const char *h2_clear_protos[] = {
    "h2c", NULL
};

const char *H2_MAGIC_TOKEN = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/*******************************************************************************
 * The optional mod_ssl functions we need. 
 */
APR_DECLARE_OPTIONAL_FN(int, ssl_engine_disable, (conn_rec*));
APR_DECLARE_OPTIONAL_FN(int, ssl_is_https, (conn_rec*));

static int (*opt_ssl_engine_disable)(conn_rec*);
static int (*opt_ssl_is_https)(conn_rec*);
/*******************************************************************************
 * SSL var lookup
 */
APR_DECLARE_OPTIONAL_FN(char *, ssl_var_lookup,
                        (apr_pool_t *, server_rec *,
                         conn_rec *, request_rec *,
                         char *));
static char *(*opt_ssl_var_lookup)(apr_pool_t *, server_rec *,
                                   conn_rec *, request_rec *,
                                   char *);

/*******************************************************************************
 * Check connection security requirements of RFC 7540
 */

/*
 * Black Listed Ciphers from RFC 7549 Appendix A
 *
 */
static const char *RFC7540_names[] = {
    /* ciphers with NULL encrpytion */
    "NULL-MD5",                         /* TLS_NULL_WITH_NULL_NULL */
    /* same */                          /* TLS_RSA_WITH_NULL_MD5 */
    "NULL-SHA",                         /* TLS_RSA_WITH_NULL_SHA */
    "NULL-SHA256",                      /* TLS_RSA_WITH_NULL_SHA256 */
    "PSK-NULL-SHA",                     /* TLS_PSK_WITH_NULL_SHA */
    "DHE-PSK-NULL-SHA",                 /* TLS_DHE_PSK_WITH_NULL_SHA */
    "RSA-PSK-NULL-SHA",                 /* TLS_RSA_PSK_WITH_NULL_SHA */
    "PSK-NULL-SHA256",                  /* TLS_PSK_WITH_NULL_SHA256 */
    "PSK-NULL-SHA384",                  /* TLS_PSK_WITH_NULL_SHA384 */
    "DHE-PSK-NULL-SHA256",              /* TLS_DHE_PSK_WITH_NULL_SHA256 */
    "DHE-PSK-NULL-SHA384",              /* TLS_DHE_PSK_WITH_NULL_SHA384 */
    "RSA-PSK-NULL-SHA256",              /* TLS_RSA_PSK_WITH_NULL_SHA256 */
    "RSA-PSK-NULL-SHA384",              /* TLS_RSA_PSK_WITH_NULL_SHA384 */
    "ECDH-ECDSA-NULL-SHA",              /* TLS_ECDH_ECDSA_WITH_NULL_SHA */
    "ECDHE-ECDSA-NULL-SHA",             /* TLS_ECDHE_ECDSA_WITH_NULL_SHA */
    "ECDH-RSA-NULL-SHA",                /* TLS_ECDH_RSA_WITH_NULL_SHA */
    "ECDHE-RSA-NULL-SHA",               /* TLS_ECDHE_RSA_WITH_NULL_SHA */
    "AECDH-NULL-SHA",                   /* TLS_ECDH_anon_WITH_NULL_SHA */
    "ECDHE-PSK-NULL-SHA",               /* TLS_ECDHE_PSK_WITH_NULL_SHA */
    "ECDHE-PSK-NULL-SHA256",            /* TLS_ECDHE_PSK_WITH_NULL_SHA256 */
    "ECDHE-PSK-NULL-SHA384",            /* TLS_ECDHE_PSK_WITH_NULL_SHA384 */
    
    /* DES/3DES ciphers */
    "PSK-3DES-EDE-CBC-SHA",             /* TLS_PSK_WITH_3DES_EDE_CBC_SHA */
    "DHE-PSK-3DES-EDE-CBC-SHA",         /* TLS_DHE_PSK_WITH_3DES_EDE_CBC_SHA */
    "RSA-PSK-3DES-EDE-CBC-SHA",         /* TLS_RSA_PSK_WITH_3DES_EDE_CBC_SHA */
    "ECDH-ECDSA-DES-CBC3-SHA",          /* TLS_ECDH_ECDSA_WITH_3DES_EDE_CBC_SHA */
    "ECDHE-ECDSA-DES-CBC3-SHA",         /* TLS_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA */
    "ECDH-RSA-DES-CBC3-SHA",            /* TLS_ECDH_RSA_WITH_3DES_EDE_CBC_SHA */
    "ECDHE-RSA-DES-CBC3-SHA",           /* TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA */
    "AECDH-DES-CBC3-SHA",               /* TLS_ECDH_anon_WITH_3DES_EDE_CBC_SHA */
    "SRP-3DES-EDE-CBC-SHA",             /* TLS_SRP_SHA_WITH_3DES_EDE_CBC_SHA */
    "SRP-RSA-3DES-EDE-CBC-SHA",         /* TLS_SRP_SHA_RSA_WITH_3DES_EDE_CBC_SHA */
    "SRP-DSS-3DES-EDE-CBC-SHA",         /* TLS_SRP_SHA_DSS_WITH_3DES_EDE_CBC_SHA */
    "ECDHE-PSK-3DES-EDE-CBC-SHA",       /* TLS_ECDHE_PSK_WITH_3DES_EDE_CBC_SHA */
    "DES-CBC-SHA",                      /* TLS_RSA_WITH_DES_CBC_SHA */
    "DES-CBC3-SHA",                     /* TLS_RSA_WITH_3DES_EDE_CBC_SHA */
    "DHE-DSS-DES-CBC3-SHA",             /* TLS_DHE_DSS_WITH_3DES_EDE_CBC_SHA */
    "DHE-RSA-DES-CBC-SHA",              /* TLS_DHE_RSA_WITH_DES_CBC_SHA */
    "DHE-RSA-DES-CBC3-SHA",             /* TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA */
    "ADH-DES-CBC-SHA",                  /* TLS_DH_anon_WITH_DES_CBC_SHA */
    "ADH-DES-CBC3-SHA",                 /* TLS_DH_anon_WITH_3DES_EDE_CBC_SHA */
    "EXP-DH-DSS-DES-CBC-SHA",           /* TLS_DH_DSS_EXPORT_WITH_DES40_CBC_SHA */
    "DH-DSS-DES-CBC-SHA",               /* TLS_DH_DSS_WITH_DES_CBC_SHA */
    "DH-DSS-DES-CBC3-SHA",              /* TLS_DH_DSS_WITH_3DES_EDE_CBC_SHA */
    "EXP-DH-RSA-DES-CBC-SHA",           /* TLS_DH_RSA_EXPORT_WITH_DES40_CBC_SHA */
    "DH-RSA-DES-CBC-SHA",               /* TLS_DH_RSA_WITH_DES_CBC_SHA */
    "DH-RSA-DES-CBC3-SHA",              /* TLS_DH_RSA_WITH_3DES_EDE_CBC_SHA */

    /* blacklisted EXPORT ciphers */
    "EXP-RC4-MD5",                      /* TLS_RSA_EXPORT_WITH_RC4_40_MD5 */
    "EXP-RC2-CBC-MD5",                  /* TLS_RSA_EXPORT_WITH_RC2_CBC_40_MD5 */
    "EXP-DES-CBC-SHA",                  /* TLS_RSA_EXPORT_WITH_DES40_CBC_SHA */
    "EXP-DHE-DSS-DES-CBC-SHA",          /* TLS_DHE_DSS_EXPORT_WITH_DES40_CBC_SHA */
    "EXP-DHE-RSA-DES-CBC-SHA",          /* TLS_DHE_RSA_EXPORT_WITH_DES40_CBC_SHA */
    "EXP-ADH-DES-CBC-SHA",              /* TLS_DH_anon_EXPORT_WITH_DES40_CBC_SHA */
    "EXP-ADH-RC4-MD5",                  /* TLS_DH_anon_EXPORT_WITH_RC4_40_MD5 */

    /* blacklisted RC4 encryption */
    "RC4-MD5",                          /* TLS_RSA_WITH_RC4_128_MD5 */
    "RC4-SHA",                          /* TLS_RSA_WITH_RC4_128_SHA */
    "ADH-RC4-MD5",                      /* TLS_DH_anon_WITH_RC4_128_MD5 */
    "KRB5-RC4-SHA",                     /* TLS_KRB5_WITH_RC4_128_SHA */
    "KRB5-RC4-MD5",                     /* TLS_KRB5_WITH_RC4_128_MD5 */
    "EXP-KRB5-RC4-SHA",                 /* TLS_KRB5_EXPORT_WITH_RC4_40_SHA */
    "EXP-KRB5-RC4-MD5",                 /* TLS_KRB5_EXPORT_WITH_RC4_40_MD5 */
    "PSK-RC4-SHA",                      /* TLS_PSK_WITH_RC4_128_SHA */
    "DHE-PSK-RC4-SHA",                  /* TLS_DHE_PSK_WITH_RC4_128_SHA */
    "RSA-PSK-RC4-SHA",                  /* TLS_RSA_PSK_WITH_RC4_128_SHA */
    "ECDH-ECDSA-RC4-SHA",               /* TLS_ECDH_ECDSA_WITH_RC4_128_SHA */
    "ECDHE-ECDSA-RC4-SHA",              /* TLS_ECDHE_ECDSA_WITH_RC4_128_SHA */
    "ECDH-RSA-RC4-SHA",                 /* TLS_ECDH_RSA_WITH_RC4_128_SHA */
    "ECDHE-RSA-RC4-SHA",                /* TLS_ECDHE_RSA_WITH_RC4_128_SHA */
    "AECDH-RC4-SHA",                    /* TLS_ECDH_anon_WITH_RC4_128_SHA */
    "ECDHE-PSK-RC4-SHA",                /* TLS_ECDHE_PSK_WITH_RC4_128_SHA */

    /* blacklisted AES128 encrpytion ciphers */
    "AES128-SHA256",                    /* TLS_RSA_WITH_AES_128_CBC_SHA */
    "DH-DSS-AES128-SHA",                /* TLS_DH_DSS_WITH_AES_128_CBC_SHA */
    "DH-RSA-AES128-SHA",                /* TLS_DH_RSA_WITH_AES_128_CBC_SHA */
    "DHE-DSS-AES128-SHA",               /* TLS_DHE_DSS_WITH_AES_128_CBC_SHA */
    "DHE-RSA-AES128-SHA",               /* TLS_DHE_RSA_WITH_AES_128_CBC_SHA */
    "ADH-AES128-SHA",                   /* TLS_DH_anon_WITH_AES_128_CBC_SHA */
    "AES128-SHA256",                    /* TLS_RSA_WITH_AES_128_CBC_SHA256 */
    "DH-DSS-AES128-SHA256",             /* TLS_DH_DSS_WITH_AES_128_CBC_SHA256 */
    "DH-RSA-AES128-SHA256",             /* TLS_DH_RSA_WITH_AES_128_CBC_SHA256 */
    "DHE-DSS-AES128-SHA256",            /* TLS_DHE_DSS_WITH_AES_128_CBC_SHA256 */
    "DHE-RSA-AES128-SHA256",            /* TLS_DHE_RSA_WITH_AES_128_CBC_SHA256 */
    "ECDH-ECDSA-AES128-SHA",            /* TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA */
    "ECDHE-ECDSA-AES128-SHA",           /* TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA */
    "ECDH-RSA-AES128-SHA",              /* TLS_ECDH_RSA_WITH_AES_128_CBC_SHA */
    "ECDHE-RSA-AES128-SHA",             /* TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA */
    "AECDH-AES128-SHA",                 /* TLS_ECDH_anon_WITH_AES_128_CBC_SHA */
    "ECDHE-ECDSA-AES128-SHA256",        /* TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 */
    "ECDH-ECDSA-AES128-SHA256",         /* TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA256 */
    "ECDHE-RSA-AES128-SHA256",          /* TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256 */
    "ECDH-RSA-AES128-SHA256",           /* TLS_ECDH_RSA_WITH_AES_128_CBC_SHA256 */
    "ADH-AES128-SHA256",                /* TLS_DH_anon_WITH_AES_128_CBC_SHA256 */
    "PSK-AES128-CBC-SHA",               /* TLS_PSK_WITH_AES_128_CBC_SHA */
    "DHE-PSK-AES128-CBC-SHA",           /* TLS_DHE_PSK_WITH_AES_128_CBC_SHA */
    "RSA-PSK-AES128-CBC-SHA",           /* TLS_RSA_PSK_WITH_AES_128_CBC_SHA */
    "PSK-AES128-CBC-SHA256",            /* TLS_PSK_WITH_AES_128_CBC_SHA256 */
    "DHE-PSK-AES128-CBC-SHA256",        /* TLS_DHE_PSK_WITH_AES_128_CBC_SHA256 */
    "RSA-PSK-AES128-CBC-SHA256",        /* TLS_RSA_PSK_WITH_AES_128_CBC_SHA256 */
    "ECDHE-PSK-AES128-CBC-SHA",         /* TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA */
    "ECDHE-PSK-AES128-CBC-SHA256",      /* TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256 */
    "AES128-CCM",                       /* TLS_RSA_WITH_AES_128_CCM */
    "AES128-CCM8",                      /* TLS_RSA_WITH_AES_128_CCM_8 */
    "PSK-AES128-CCM",                   /* TLS_PSK_WITH_AES_128_CCM */
    "PSK-AES128-CCM8",                  /* TLS_PSK_WITH_AES_128_CCM_8 */
    "AES128-GCM-SHA256",                /* TLS_RSA_WITH_AES_128_GCM_SHA256 */
    "DH-RSA-AES128-GCM-SHA256",         /* TLS_DH_RSA_WITH_AES_128_GCM_SHA256 */
    "DH-DSS-AES128-GCM-SHA256",         /* TLS_DH_DSS_WITH_AES_128_GCM_SHA256 */
    "ADH-AES128-GCM-SHA256",            /* TLS_DH_anon_WITH_AES_128_GCM_SHA256 */
    "PSK-AES128-GCM-SHA256",            /* TLS_PSK_WITH_AES_128_GCM_SHA256 */
    "RSA-PSK-AES128-GCM-SHA256",        /* TLS_RSA_PSK_WITH_AES_128_GCM_SHA256 */
    "ECDH-ECDSA-AES128-GCM-SHA256",     /* TLS_ECDH_ECDSA_WITH_AES_128_GCM_SHA256 */
    "ECDH-RSA-AES128-GCM-SHA256",       /* TLS_ECDH_RSA_WITH_AES_128_GCM_SHA256 */
    "SRP-AES-128-CBC-SHA",              /* TLS_SRP_SHA_WITH_AES_128_CBC_SHA */
    "SRP-RSA-AES-128-CBC-SHA",          /* TLS_SRP_SHA_RSA_WITH_AES_128_CBC_SHA */
    "SRP-DSS-AES-128-CBC-SHA",          /* TLS_SRP_SHA_DSS_WITH_AES_128_CBC_SHA */
    
    /* blacklisted AES256 encrpytion ciphers */
    "AES256-SHA",                       /* TLS_RSA_WITH_AES_256_CBC_SHA */
    "DH-DSS-AES256-SHA",                /* TLS_DH_DSS_WITH_AES_256_CBC_SHA */
    "DH-RSA-AES256-SHA",                /* TLS_DH_RSA_WITH_AES_256_CBC_SHA */
    "DHE-DSS-AES256-SHA",               /* TLS_DHE_DSS_WITH_AES_256_CBC_SHA */
    "DHE-RSA-AES256-SHA",               /* TLS_DHE_RSA_WITH_AES_256_CBC_SHA */
    "ADH-AES256-SHA",                   /* TLS_DH_anon_WITH_AES_256_CBC_SHA */
    "AES256-SHA256",                    /* TLS_RSA_WITH_AES_256_CBC_SHA256 */
    "DH-DSS-AES256-SHA256",             /* TLS_DH_DSS_WITH_AES_256_CBC_SHA256 */
    "DH-RSA-AES256-SHA256",             /* TLS_DH_RSA_WITH_AES_256_CBC_SHA256 */
    "DHE-DSS-AES256-SHA256",            /* TLS_DHE_DSS_WITH_AES_256_CBC_SHA256 */
    "DHE-RSA-AES256-SHA256",            /* TLS_DHE_RSA_WITH_AES_256_CBC_SHA256 */
    "ADH-AES256-SHA256",                /* TLS_DH_anon_WITH_AES_256_CBC_SHA256 */
    "ECDH-ECDSA-AES256-SHA",            /* TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA */
    "ECDHE-ECDSA-AES256-SHA",           /* TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA */
    "ECDH-RSA-AES256-SHA",              /* TLS_ECDH_RSA_WITH_AES_256_CBC_SHA */
    "ECDHE-RSA-AES256-SHA",             /* TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA */
    "AECDH-AES256-SHA",                 /* TLS_ECDH_anon_WITH_AES_256_CBC_SHA */
    "ECDHE-ECDSA-AES256-SHA384",        /* TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384 */
    "ECDH-ECDSA-AES256-SHA384",         /* TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA384 */
    "ECDHE-RSA-AES256-SHA384",          /* TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384 */
    "ECDH-RSA-AES256-SHA384",           /* TLS_ECDH_RSA_WITH_AES_256_CBC_SHA384 */
    "PSK-AES256-CBC-SHA",               /* TLS_PSK_WITH_AES_256_CBC_SHA */
    "DHE-PSK-AES256-CBC-SHA",           /* TLS_DHE_PSK_WITH_AES_256_CBC_SHA */
    "RSA-PSK-AES256-CBC-SHA",           /* TLS_RSA_PSK_WITH_AES_256_CBC_SHA */
    "PSK-AES256-CBC-SHA384",            /* TLS_PSK_WITH_AES_256_CBC_SHA384 */
    "DHE-PSK-AES256-CBC-SHA384",        /* TLS_DHE_PSK_WITH_AES_256_CBC_SHA384 */
    "RSA-PSK-AES256-CBC-SHA384",        /* TLS_RSA_PSK_WITH_AES_256_CBC_SHA384 */
    "ECDHE-PSK-AES256-CBC-SHA",         /* TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA */
    "ECDHE-PSK-AES256-CBC-SHA384",      /* TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA384 */
    "SRP-AES-256-CBC-SHA",              /* TLS_SRP_SHA_WITH_AES_256_CBC_SHA */
    "SRP-RSA-AES-256-CBC-SHA",          /* TLS_SRP_SHA_RSA_WITH_AES_256_CBC_SHA */
    "SRP-DSS-AES-256-CBC-SHA",          /* TLS_SRP_SHA_DSS_WITH_AES_256_CBC_SHA */
    "AES256-CCM",                       /* TLS_RSA_WITH_AES_256_CCM */
    "AES256-CCM8",                      /* TLS_RSA_WITH_AES_256_CCM_8 */
    "PSK-AES256-CCM",                   /* TLS_PSK_WITH_AES_256_CCM */
    "PSK-AES256-CCM8",                  /* TLS_PSK_WITH_AES_256_CCM_8 */
    "AES256-GCM-SHA384",                /* TLS_RSA_WITH_AES_256_GCM_SHA384 */
    "DH-RSA-AES256-GCM-SHA384",         /* TLS_DH_RSA_WITH_AES_256_GCM_SHA384 */
    "DH-DSS-AES256-GCM-SHA384",         /* TLS_DH_DSS_WITH_AES_256_GCM_SHA384 */
    "ADH-AES256-GCM-SHA384",            /* TLS_DH_anon_WITH_AES_256_GCM_SHA384 */
    "PSK-AES256-GCM-SHA384",            /* TLS_PSK_WITH_AES_256_GCM_SHA384 */
    "RSA-PSK-AES256-GCM-SHA384",        /* TLS_RSA_PSK_WITH_AES_256_GCM_SHA384 */
    "ECDH-ECDSA-AES256-GCM-SHA384",     /* TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384 */
    "ECDH-RSA-AES256-GCM-SHA384",       /* TLS_ECDH_RSA_WITH_AES_256_GCM_SHA384 */
    
    /* blacklisted CAMELLIA128 encrpytion ciphers */
    "CAMELLIA128-SHA",                  /* TLS_RSA_WITH_CAMELLIA_128_CBC_SHA */
    "DH-DSS-CAMELLIA128-SHA",           /* TLS_DH_DSS_WITH_CAMELLIA_128_CBC_SHA */
    "DH-RSA-CAMELLIA128-SHA",           /* TLS_DH_RSA_WITH_CAMELLIA_128_CBC_SHA */
    "DHE-DSS-CAMELLIA128-SHA",          /* TLS_DHE_DSS_WITH_CAMELLIA_128_CBC_SHA */
    "DHE-RSA-CAMELLIA128-SHA",          /* TLS_DHE_RSA_WITH_CAMELLIA_128_CBC_SHA */
    "ADH-CAMELLIA128-SHA",              /* TLS_DH_anon_WITH_CAMELLIA_128_CBC_SHA */
    "ECDHE-ECDSA-CAMELLIA128-SHA256",   /* TLS_ECDHE_ECDSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "ECDH-ECDSA-CAMELLIA128-SHA256",    /* TLS_ECDH_ECDSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "ECDHE-RSA-CAMELLIA128-SHA256",     /* TLS_ECDHE_RSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "ECDH-RSA-CAMELLIA128-SHA256",      /* TLS_ECDH_RSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "PSK-CAMELLIA128-SHA256",           /* TLS_PSK_WITH_CAMELLIA_128_CBC_SHA256 */
    "DHE-PSK-CAMELLIA128-SHA256",       /* TLS_DHE_PSK_WITH_CAMELLIA_128_CBC_SHA256 */
    "RSA-PSK-CAMELLIA128-SHA256",       /* TLS_RSA_PSK_WITH_CAMELLIA_128_CBC_SHA256 */
    "ECDHE-PSK-CAMELLIA128-SHA256",     /* TLS_ECDHE_PSK_WITH_CAMELLIA_128_CBC_SHA256 */
    "CAMELLIA128-GCM-SHA256",           /* TLS_RSA_WITH_CAMELLIA_128_GCM_SHA256 */
    "DH-RSA-CAMELLIA128-GCM-SHA256",    /* TLS_DH_RSA_WITH_CAMELLIA_128_GCM_SHA256 */
    "DH-DSS-CAMELLIA128-GCM-SHA256",    /* TLS_DH_DSS_WITH_CAMELLIA_128_GCM_SHA256 */
    "ADH-CAMELLIA128-GCM-SHA256",       /* TLS_DH_anon_WITH_CAMELLIA_128_GCM_SHA256 */
    "ECDH-ECDSA-CAMELLIA128-GCM-SHA256",/* TLS_ECDH_ECDSA_WITH_CAMELLIA_128_GCM_SHA256 */
    "ECDH-RSA-CAMELLIA128-GCM-SHA256",  /* TLS_ECDH_RSA_WITH_CAMELLIA_128_GCM_SHA256 */
    "PSK-CAMELLIA128-GCM-SHA256",       /* TLS_PSK_WITH_CAMELLIA_128_GCM_SHA256 */
    "RSA-PSK-CAMELLIA128-GCM-SHA256",   /* TLS_RSA_PSK_WITH_CAMELLIA_128_GCM_SHA256 */
    "CAMELLIA128-SHA256",               /* TLS_RSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "DH-DSS-CAMELLIA128-SHA256",        /* TLS_DH_DSS_WITH_CAMELLIA_128_CBC_SHA256 */
    "DH-RSA-CAMELLIA128-SHA256",        /* TLS_DH_RSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "DHE-DSS-CAMELLIA128-SHA256",       /* TLS_DHE_DSS_WITH_CAMELLIA_128_CBC_SHA256 */
    "DHE-RSA-CAMELLIA128-SHA256",       /* TLS_DHE_RSA_WITH_CAMELLIA_128_CBC_SHA256 */
    "ADH-CAMELLIA128-SHA256",           /* TLS_DH_anon_WITH_CAMELLIA_128_CBC_SHA256 */
    
    /* blacklisted CAMELLIA256 encrpytion ciphers */
    "CAMELLIA256-SHA",                  /* TLS_RSA_WITH_CAMELLIA_256_CBC_SHA */
    "DH-RSA-CAMELLIA256-SHA",           /* TLS_DH_RSA_WITH_CAMELLIA_256_CBC_SHA */
    "DH-DSS-CAMELLIA256-SHA",           /* TLS_DH_DSS_WITH_CAMELLIA_256_CBC_SHA */
    "DHE-DSS-CAMELLIA256-SHA",          /* TLS_DHE_DSS_WITH_CAMELLIA_256_CBC_SHA */
    "DHE-RSA-CAMELLIA256-SHA",          /* TLS_DHE_RSA_WITH_CAMELLIA_256_CBC_SHA */
    "ADH-CAMELLIA256-SHA",              /* TLS_DH_anon_WITH_CAMELLIA_256_CBC_SHA */
    "ECDHE-ECDSA-CAMELLIA256-SHA384",   /* TLS_ECDHE_ECDSA_WITH_CAMELLIA_256_CBC_SHA384 */
    "ECDH-ECDSA-CAMELLIA256-SHA384",    /* TLS_ECDH_ECDSA_WITH_CAMELLIA_256_CBC_SHA384 */
    "ECDHE-RSA-CAMELLIA256-SHA384",     /* TLS_ECDHE_RSA_WITH_CAMELLIA_256_CBC_SHA384 */
    "ECDH-RSA-CAMELLIA256-SHA384",      /* TLS_ECDH_RSA_WITH_CAMELLIA_256_CBC_SHA384 */
    "PSK-CAMELLIA256-SHA384",           /* TLS_PSK_WITH_CAMELLIA_256_CBC_SHA384 */
    "DHE-PSK-CAMELLIA256-SHA384",       /* TLS_DHE_PSK_WITH_CAMELLIA_256_CBC_SHA384 */
    "RSA-PSK-CAMELLIA256-SHA384",       /* TLS_RSA_PSK_WITH_CAMELLIA_256_CBC_SHA384 */
    "ECDHE-PSK-CAMELLIA256-SHA384",     /* TLS_ECDHE_PSK_WITH_CAMELLIA_256_CBC_SHA384 */
    "CAMELLIA256-SHA256",               /* TLS_RSA_WITH_CAMELLIA_256_CBC_SHA256 */
    "DH-DSS-CAMELLIA256-SHA256",        /* TLS_DH_DSS_WITH_CAMELLIA_256_CBC_SHA256 */
    "DH-RSA-CAMELLIA256-SHA256",        /* TLS_DH_RSA_WITH_CAMELLIA_256_CBC_SHA256 */
    "DHE-DSS-CAMELLIA256-SHA256",       /* TLS_DHE_DSS_WITH_CAMELLIA_256_CBC_SHA256 */
    "DHE-RSA-CAMELLIA256-SHA256",       /* TLS_DHE_RSA_WITH_CAMELLIA_256_CBC_SHA256 */
    "ADH-CAMELLIA256-SHA256",           /* TLS_DH_anon_WITH_CAMELLIA_256_CBC_SHA256 */
    "CAMELLIA256-GCM-SHA384",           /* TLS_RSA_WITH_CAMELLIA_256_GCM_SHA384 */
    "DH-RSA-CAMELLIA256-GCM-SHA384",    /* TLS_DH_RSA_WITH_CAMELLIA_256_GCM_SHA384 */
    "DH-DSS-CAMELLIA256-GCM-SHA384",    /* TLS_DH_DSS_WITH_CAMELLIA_256_GCM_SHA384 */
    "ADH-CAMELLIA256-GCM-SHA384",       /* TLS_DH_anon_WITH_CAMELLIA_256_GCM_SHA384 */
    "ECDH-ECDSA-CAMELLIA256-GCM-SHA384",/* TLS_ECDH_ECDSA_WITH_CAMELLIA_256_GCM_SHA384 */
    "ECDH-RSA-CAMELLIA256-GCM-SHA384",  /* TLS_ECDH_RSA_WITH_CAMELLIA_256_GCM_SHA384 */
    "PSK-CAMELLIA256-GCM-SHA384",       /* TLS_PSK_WITH_CAMELLIA_256_GCM_SHA384 */
    "RSA-PSK-CAMELLIA256-GCM-SHA384",   /* TLS_RSA_PSK_WITH_CAMELLIA_256_GCM_SHA384 */
    
    /* The blacklisted ARIA encrpytion ciphers */
    "ARIA128-SHA256",                   /* TLS_RSA_WITH_ARIA_128_CBC_SHA256 */
    "ARIA256-SHA384",                   /* TLS_RSA_WITH_ARIA_256_CBC_SHA384 */
    "DH-DSS-ARIA128-SHA256",            /* TLS_DH_DSS_WITH_ARIA_128_CBC_SHA256 */
    "DH-DSS-ARIA256-SHA384",            /* TLS_DH_DSS_WITH_ARIA_256_CBC_SHA384 */
    "DH-RSA-ARIA128-SHA256",            /* TLS_DH_RSA_WITH_ARIA_128_CBC_SHA256 */
    "DH-RSA-ARIA256-SHA384",            /* TLS_DH_RSA_WITH_ARIA_256_CBC_SHA384 */
    "DHE-DSS-ARIA128-SHA256",           /* TLS_DHE_DSS_WITH_ARIA_128_CBC_SHA256 */
    "DHE-DSS-ARIA256-SHA384",           /* TLS_DHE_DSS_WITH_ARIA_256_CBC_SHA384 */
    "DHE-RSA-ARIA128-SHA256",           /* TLS_DHE_RSA_WITH_ARIA_128_CBC_SHA256 */
    "DHE-RSA-ARIA256-SHA384",           /* TLS_DHE_RSA_WITH_ARIA_256_CBC_SHA384 */
    "ADH-ARIA128-SHA256",               /* TLS_DH_anon_WITH_ARIA_128_CBC_SHA256 */
    "ADH-ARIA256-SHA384",               /* TLS_DH_anon_WITH_ARIA_256_CBC_SHA384 */
    "ECDHE-ECDSA-ARIA128-SHA256",       /* TLS_ECDHE_ECDSA_WITH_ARIA_128_CBC_SHA256 */
    "ECDHE-ECDSA-ARIA256-SHA384",       /* TLS_ECDHE_ECDSA_WITH_ARIA_256_CBC_SHA384 */
    "ECDH-ECDSA-ARIA128-SHA256",        /* TLS_ECDH_ECDSA_WITH_ARIA_128_CBC_SHA256 */
    "ECDH-ECDSA-ARIA256-SHA384",        /* TLS_ECDH_ECDSA_WITH_ARIA_256_CBC_SHA384 */
    "ECDHE-RSA-ARIA128-SHA256",         /* TLS_ECDHE_RSA_WITH_ARIA_128_CBC_SHA256 */
    "ECDHE-RSA-ARIA256-SHA384",         /* TLS_ECDHE_RSA_WITH_ARIA_256_CBC_SHA384 */
    "ECDH-RSA-ARIA128-SHA256",          /* TLS_ECDH_RSA_WITH_ARIA_128_CBC_SHA256 */
    "ECDH-RSA-ARIA256-SHA384",          /* TLS_ECDH_RSA_WITH_ARIA_256_CBC_SHA384 */
    "ARIA128-GCM-SHA256",               /* TLS_RSA_WITH_ARIA_128_GCM_SHA256 */
    "ARIA256-GCM-SHA384",               /* TLS_RSA_WITH_ARIA_256_GCM_SHA384 */
    "DH-DSS-ARIA128-GCM-SHA256",        /* TLS_DH_DSS_WITH_ARIA_128_GCM_SHA256 */
    "DH-DSS-ARIA256-GCM-SHA384",        /* TLS_DH_DSS_WITH_ARIA_256_GCM_SHA384 */
    "DH-RSA-ARIA128-GCM-SHA256",        /* TLS_DH_RSA_WITH_ARIA_128_GCM_SHA256 */
    "DH-RSA-ARIA256-GCM-SHA384",        /* TLS_DH_RSA_WITH_ARIA_256_GCM_SHA384 */
    "ADH-ARIA128-GCM-SHA256",           /* TLS_DH_anon_WITH_ARIA_128_GCM_SHA256 */
    "ADH-ARIA256-GCM-SHA384",           /* TLS_DH_anon_WITH_ARIA_256_GCM_SHA384 */
    "ECDH-ECDSA-ARIA128-GCM-SHA256",    /* TLS_ECDH_ECDSA_WITH_ARIA_128_GCM_SHA256 */
    "ECDH-ECDSA-ARIA256-GCM-SHA384",    /* TLS_ECDH_ECDSA_WITH_ARIA_256_GCM_SHA384 */
    "ECDH-RSA-ARIA128-GCM-SHA256",      /* TLS_ECDH_RSA_WITH_ARIA_128_GCM_SHA256 */
    "ECDH-RSA-ARIA256-GCM-SHA384",      /* TLS_ECDH_RSA_WITH_ARIA_256_GCM_SHA384 */
    "PSK-ARIA128-SHA256",               /* TLS_PSK_WITH_ARIA_128_CBC_SHA256 */
    "PSK-ARIA256-SHA384",               /* TLS_PSK_WITH_ARIA_256_CBC_SHA384 */
    "DHE-PSK-ARIA128-SHA256",           /* TLS_DHE_PSK_WITH_ARIA_128_CBC_SHA256 */
    "DHE-PSK-ARIA256-SHA384",           /* TLS_DHE_PSK_WITH_ARIA_256_CBC_SHA384 */
    "RSA-PSK-ARIA128-SHA256",           /* TLS_RSA_PSK_WITH_ARIA_128_CBC_SHA256 */
    "RSA-PSK-ARIA256-SHA384",           /* TLS_RSA_PSK_WITH_ARIA_256_CBC_SHA384 */
    "ARIA128-GCM-SHA256",               /* TLS_PSK_WITH_ARIA_128_GCM_SHA256 */
    "ARIA256-GCM-SHA384",               /* TLS_PSK_WITH_ARIA_256_GCM_SHA384 */
    "RSA-PSK-ARIA128-GCM-SHA256",       /* TLS_RSA_PSK_WITH_ARIA_128_GCM_SHA256 */
    "RSA-PSK-ARIA256-GCM-SHA384",       /* TLS_RSA_PSK_WITH_ARIA_256_GCM_SHA384 */
    "ECDHE-PSK-ARIA128-SHA256",         /* TLS_ECDHE_PSK_WITH_ARIA_128_CBC_SHA256 */
    "ECDHE-PSK-ARIA256-SHA384",         /* TLS_ECDHE_PSK_WITH_ARIA_256_CBC_SHA384 */

    /* blacklisted SEED encryptions */
    "SEED-SHA",                         /*TLS_RSA_WITH_SEED_CBC_SHA */
    "DH-DSS-SEED-SHA",                  /* TLS_DH_DSS_WITH_SEED_CBC_SHA */
    "DH-RSA-SEED-SHA",                  /* TLS_DH_RSA_WITH_SEED_CBC_SHA */
    "DHE-DSS-SEED-SHA",                 /* TLS_DHE_DSS_WITH_SEED_CBC_SHA */
    "DHE-RSA-SEED-SHA",                 /* TLS_DHE_RSA_WITH_SEED_CBC_SHA */               
    "ADH-SEED-SHA",                     /* TLS_DH_anon_WITH_SEED_CBC_SHA */

    /* blacklisted KRB5 ciphers */
    "KRB5-DES-CBC-SHA",                 /* TLS_KRB5_WITH_DES_CBC_SHA */
    "KRB5-DES-CBC3-SHA",                /* TLS_KRB5_WITH_3DES_EDE_CBC_SHA */
    "KRB5-IDEA-CBC-SHA",                /* TLS_KRB5_WITH_IDEA_CBC_SHA */
    "KRB5-DES-CBC-MD5",                 /* TLS_KRB5_WITH_DES_CBC_MD5 */
    "KRB5-DES-CBC3-MD5",                /* TLS_KRB5_WITH_3DES_EDE_CBC_MD5 */
    "KRB5-IDEA-CBC-MD5",                /* TLS_KRB5_WITH_IDEA_CBC_MD5 */
    "EXP-KRB5-DES-CBC-SHA",             /* TLS_KRB5_EXPORT_WITH_DES_CBC_40_SHA */
    "EXP-KRB5-DES-CBC-MD5",             /* TLS_KRB5_EXPORT_WITH_DES_CBC_40_MD5 */
    "EXP-KRB5-RC2-CBC-SHA",             /* TLS_KRB5_EXPORT_WITH_RC2_CBC_40_SHA */
    "EXP-KRB5-RC2-CBC-MD5",             /* TLS_KRB5_EXPORT_WITH_RC2_CBC_40_MD5 */
  
    /* blacklisted exoticas */
    "DHE-DSS-CBC-SHA",                  /* TLS_DHE_DSS_WITH_DES_CBC_SHA */
    "IDEA-CBC-SHA",                     /* TLS_RSA_WITH_IDEA_CBC_SHA */
    
    /* not really sure if the following names are correct */
    "SSL3_CK_SCSV",                     /* TLS_EMPTY_RENEGOTIATION_INFO_SCSV */
    "SSL3_CK_FALLBACK_SCSV"
};
static size_t RFC7540_names_LEN = sizeof(RFC7540_names)/sizeof(RFC7540_names[0]);


static apr_hash_t *BLCNames;

static void cipher_init(apr_pool_t *pool)
{
    apr_hash_t *hash = apr_hash_make(pool);
    const char *source;
    int i;
    
    source = "rfc7540";
    for (i = 0; i < RFC7540_names_LEN; ++i) {
        apr_hash_set(hash, RFC7540_names[i], APR_HASH_KEY_STRING, source);
    }
    
    BLCNames = hash;
}

static int cipher_is_blacklisted(const char *cipher, const char **psource)
{   
    *psource = apr_hash_get(BLCNames, cipher, APR_HASH_KEY_STRING);
    return !!*psource;
}

/*******************************************************************************
 * Hooks for processing incoming connections:
 * - pre_conn_before_tls switches SSL off for stream connections
 * - process_conn take over connection in case of h2
 */
static int h2_h2_process_conn(conn_rec* c);
static int h2_h2_remove_timeout(conn_rec* c);
static int h2_h2_post_read_req(request_rec *r);


/*******************************************************************************
 * Once per lifetime init, retrieve optional functions
 */
apr_status_t h2_h2_init(apr_pool_t *pool, server_rec *s)
{
    (void)pool;
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "h2_h2, child_init");
    opt_ssl_engine_disable = APR_RETRIEVE_OPTIONAL_FN(ssl_engine_disable);
    opt_ssl_is_https = APR_RETRIEVE_OPTIONAL_FN(ssl_is_https);
    opt_ssl_var_lookup = APR_RETRIEVE_OPTIONAL_FN(ssl_var_lookup);
    
    if (!opt_ssl_is_https || !opt_ssl_var_lookup) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                     APLOGNO(02951) "mod_ssl does not seem to be enabled");
    }
    
    cipher_init(pool);
    
    return APR_SUCCESS;
}

int h2_h2_is_tls(conn_rec *c)
{
    return opt_ssl_is_https && opt_ssl_is_https(c);
}

int h2_tls_disable(conn_rec *c)
{
    if (opt_ssl_engine_disable) {
        return opt_ssl_engine_disable(c);
    }
    return 0;
}

int h2_is_security_compliant(conn_rec *c, int require_all) 
{
    int is_tls = h2_h2_is_tls(c);
    h2_config *cfg = h2_config_get(c);

    if (is_tls && h2_config_geti(cfg, H2_CONF_COMPLIANCE) > 0) {
        /* Check TLS connection for RFC 7540 compliance
         */
        apr_pool_t *pool = c->pool;
        server_rec *s = c->base_server;
        char *val;
        
        if (!opt_ssl_var_lookup) {
            /* unable to check */
            return 0;
        }
        
        /* Need Tlsv1.2 or higher, rfc 7540, ch. 9.2
         */
        val = opt_ssl_var_lookup(pool, s, c, NULL, "SSL_PROTOCOL");
        if (val && *val) {
            if (strncmp("TLS", val, 3) 
                || !strcmp("TLSv1", val) 
                || !strcmp("TLSv1.1", val)) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                          "h2_h2(%ld): tls protocol not suitable: %s", 
                          (long)c->id, val);
                return 0;
            }
        }
        else if (require_all) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                          "h2_h2(%ld): tls protocol is indetermined", (long)c->id);
            return 0;
        }

        /* Check TLS cipher blacklist
         */
        val = opt_ssl_var_lookup(pool, s, c, NULL, "SSL_CIPHER");
        if (val && *val) {
            const char *source;
            if (cipher_is_blacklisted(val, &source)) {
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                              "h2_h2(%ld): tls cipher %s blacklisted by %s", 
                              (long)c->id, val, source);
                return 0;
            }
        }
        else if (require_all) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                          "h2_h2(%ld): tls cipher is indetermined", (long)c->id);
            return 0;
        }
    }
    return 1;
}

/*******************************************************************************
 * Register various hooks
 */
static const char *const mod_reqtimeout[] = { "reqtimeout.c", NULL};

void h2_h2_register_hooks(void)
{
    /* When the connection processing actually starts, we might to
     * take over, if h2* was selected as protocol.
     */
    ap_hook_process_connection(h2_h2_process_conn, 
                               NULL, NULL, APR_HOOK_FIRST);
    /* Perform connection cleanup before the actual processing happens.
     */
    ap_hook_process_connection(h2_h2_remove_timeout, 
                               mod_reqtimeout, NULL, APR_HOOK_LAST);
    
    /* With "H2SerializeHeaders On", we install the filter in this hook
     * that parses the response. This needs to happen before any other post
     * read function terminates the request with an error. Otherwise we will
     * never see the response.
     */
    ap_hook_post_read_request(h2_h2_post_read_req, NULL, NULL, APR_HOOK_REALLY_FIRST);
}

int h2_h2_remove_timeout(conn_rec* c)
{
    h2_ctx *ctx = h2_ctx_get(c);
    
    if (h2_ctx_is_active(ctx) && !h2_ctx_is_task(ctx)) {
        /* cleanup on master h2 connections */
        ap_remove_input_filter_byhandle(c->input_filters, "reqtimeout");
    }
    
    return DECLINED;
}

int h2_h2_process_conn(conn_rec* c)
{
    h2_ctx *ctx = h2_ctx_get(c);
    h2_config *cfg = h2_config_get(c);
    apr_bucket_brigade* temp = NULL;
    int is_tls = h2_h2_is_tls(c);
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "h2_h2, process_conn");
    if (h2_ctx_is_task(ctx)) {
        /* our stream pseudo connection */
        return DECLINED;
    }

    /* If we have not already switched to a h2* protocol and the connection 
     * is on "http/1.1"
     * -> sniff for the magic PRIamble. On TLS, this might trigger the ALPN.
     */
    if (!h2_ctx_protocol_get(c) 
        && !strcmp(AP_PROTOCOL_HTTP1, ap_get_protocol(c))) {
        apr_status_t status = APR_SUCCESS;
        
        if (is_tls) {
            /* trigger the TLS handshake */
            temp = apr_brigade_create(c->pool, c->bucket_alloc);
            status = ap_get_brigade(c->input_filters, temp,
                                    AP_MODE_INIT, APR_BLOCK_READ, 0);
        }

        if (status == APR_SUCCESS) {
            if (h2_ctx_protocol_get(c) 
                || strcmp(AP_PROTOCOL_HTTP1, ap_get_protocol(c))) {
                /* h2 or another protocol has been selected. */
            }
            else {
                /* ALPN might have been triggered, but we're still on
                 * http/1.1. Check the actual bytes read for the H2 Magic
                 * Token, *if* H2Direct mode is enabled here. 
                 */
                if (h2_config_geti(cfg, H2_CONF_DIRECT) > 0) {
                    char *s = NULL;
                    apr_size_t slen;
                    
                    if (!temp) {
                        temp = apr_brigade_create(c->pool, c->bucket_alloc);
                    }
                    status = ap_get_brigade(c->input_filters, temp,
                                            AP_MODE_SPECULATIVE, APR_BLOCK_READ, 24);
                    if (status == APR_SUCCESS) {
                        apr_brigade_pflatten(temp, &s, &slen, c->pool);
                        if ((slen >= 24) && !memcmp(H2_MAGIC_TOKEN, s, 24)) {
                            ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                                          "h2_h2, direct mode detected");
                            h2_ctx_protocol_set(ctx, is_tls? "h2" : "h2c");
                        }
                        else {
                            ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                                          "h2_h2, not detected in %d bytes: %s", 
                                          (int)slen, s);
                        }
                    }
                    else {
                        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, c,
                                      "h2_h2, error reading 24 bytes speculative");
                    }
                }
            }
        }
        else {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, c,
                          "h2_h2, failed to init connection");
        }
        
        if (temp) {
            apr_brigade_destroy(temp);
        }
    }

    /* If "h2" was selected as protocol (by whatever mechanism), take over
     * the connection.
     */
    if (h2_ctx_is_active(ctx)) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                      "h2_h2, connection, h2 active");
        
        return h2_conn_main(c);
    }
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "h2_h2, declined");
    return DECLINED;
}

static int h2_h2_post_read_req(request_rec *r)
{
    h2_ctx *ctx = h2_ctx_rget(r);
    struct h2_task_env *env = h2_ctx_get_task(ctx);
    if (env) {
        /* h2_task connection for a stream, not for h2c */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "adding h1_to_h2_resp output filter");
        if (env->serialize_headers) {
            ap_remove_output_filter_byhandle(r->output_filters, "H1_TO_H2_RESP");
            ap_add_output_filter("H1_TO_H2_RESP", env, r, r->connection);
        }
        else {
            /* replace the core http filter that formats response headers
             * in HTTP/1 with our own that collects status and headers */
            ap_remove_output_filter_byhandle(r->output_filters, "HTTP_HEADER");
            ap_remove_output_filter_byhandle(r->output_filters, "H2_RESPONSE");
            ap_add_output_filter("H2_RESPONSE", env, r, r->connection);
        }
    }
    return DECLINED;
}



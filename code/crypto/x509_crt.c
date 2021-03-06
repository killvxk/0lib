#include "..\zmodule.h"
#include "config.h"

#if defined(POLARSSL_X509_CRT_PARSE_C)

#include "x509_crt.h"
#include "oid.h"
#if defined(POLARSSL_PEM_PARSE_C)
#include "pem.h"
#endif

/*
 *  Version  ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
 */
static int x509_get_version( uint8_t **p,
                             const uint8_t *end,
                             int *ver )
{
    int ret;
    size_t len;

    if( ( ret = asn1_get_tag( p, end, &len,
            ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 0 ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
        {
            *ver = 0;
            return( 0 );
        }

        return( ret );
    }

    end = *p + len;

    if( ( ret = asn1_get_int( p, end, ver ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_VERSION + ret );

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_VERSION +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 *  Validity ::= SEQUENCE {
 *       notBefore      Time,
 *       notAfter       Time }
 */
static int x509_get_dates( uint8_t **p,
                           const uint8_t *end,
                           x509_time *from,
                           x509_time *to )
{
    int ret;
    size_t len;

    if( ( ret = asn1_get_tag( p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_DATE + ret );

    end = *p + len;

    if( ( ret = x509_get_time( p, end, from ) ) != 0 )
        return( ret );

    if( ( ret = x509_get_time( p, end, to ) ) != 0 )
        return( ret );

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_DATE +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 v2/v3 unique identifier (not parsed)
 */
static int x509_get_uid( uint8_t **p,
                         const uint8_t *end,
                         x509_buf *uid, int n )
{
    int ret;

    if( *p == end )
        return( 0 );

    uid->tag = **p;

    if( ( ret = asn1_get_tag( p, end, &uid->len,
            ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | n ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    uid->p = *p;
    *p += uid->len;

    return( 0 );
}

static int x509_get_basic_constraints( uint8_t **p,
                                       const uint8_t *end,
                                       int *ca_istrue,
                                       int *max_pathlen )
{
    int ret;
    size_t len;

    /*
     * BasicConstraints ::= SEQUENCE {
     *      cA                      BOOLEAN DEFAULT FALSE,
     *      pathLenConstraint       INTEGER (0..MAX) OPTIONAL }
     */
    *ca_istrue = 0; /* DEFAULT FALSE */
    *max_pathlen = 0; /* endless */

    if( ( ret = asn1_get_tag( p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    if( *p == end )
        return 0;

    if( ( ret = asn1_get_bool( p, end, ca_istrue ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
            ret = asn1_get_int( p, end, ca_istrue );

        if( ret != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        if( *ca_istrue != 0 )
            *ca_istrue = 1;
    }

    if( *p == end )
        return 0;

    if( ( ret = asn1_get_int( p, end, max_pathlen ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    (*max_pathlen)++;

    return 0;
}

static int x509_get_ns_cert_type( uint8_t **p,
                                       const uint8_t *end,
                                       uint8_t *ns_cert_type)
{
    int ret;
    x509_bitstring bs = { 0, 0, NULL };

    if( ( ret = asn1_get_bitstring( p, end, &bs ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    if( bs.len != 1 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_INVALID_LENGTH );

    /* Get actual bitstring */
    *ns_cert_type = *bs.p;
    return 0;
}

static int x509_get_key_usage( uint8_t **p,
                               const uint8_t *end,
                               uint8_t *key_usage)
{
    int ret;
    x509_bitstring bs = { 0, 0, NULL };

    if( ( ret = asn1_get_bitstring( p, end, &bs ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    if( bs.len < 1 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_INVALID_LENGTH );

    /* Get actual bitstring */
    *key_usage = *bs.p;
    return 0;
}

/*
 * ExtKeyUsageSyntax ::= SEQUENCE SIZE (1..MAX) OF KeyPurposeId
 *
 * KeyPurposeId ::= OBJECT IDENTIFIER
 */
static int x509_get_ext_key_usage( uint8_t **p,
                               const uint8_t *end,
                               x509_sequence *ext_key_usage)
{
    int ret;

    if( ( ret = asn1_get_sequence_of( p, end, ext_key_usage, ASN1_OID ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    /* Sequence length must be >= 1 */
    if( ext_key_usage->buf.p == NULL )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_INVALID_LENGTH );

    return 0;
}

/*
 * SubjectAltName ::= GeneralNames
 *
 * GeneralNames ::= SEQUENCE SIZE (1..MAX) OF GeneralName
 *
 * GeneralName ::= CHOICE {
 *      otherName                       [0]     OtherName,
 *      rfc822Name                      [1]     IA5String,
 *      dNSName                         [2]     IA5String,
 *      x400Address                     [3]     ORAddress,
 *      directoryName                   [4]     Name,
 *      ediPartyName                    [5]     EDIPartyName,
 *      uniformResourceIdentifier       [6]     IA5String,
 *      iPAddress                       [7]     OCTET STRING,
 *      registeredID                    [8]     OBJECT IDENTIFIER }
 *
 * OtherName ::= SEQUENCE {
 *      type-id    OBJECT IDENTIFIER,
 *      value      [0] EXPLICIT ANY DEFINED BY type-id }
 *
 * EDIPartyName ::= SEQUENCE {
 *      nameAssigner            [0]     DirectoryString OPTIONAL,
 *      partyName               [1]     DirectoryString }
 *
 * NOTE: PolarSSL only parses and uses dNSName at this point.
 */
static int x509_get_subject_alt_name( uint8_t **p,
                                      const uint8_t *end,
                                      x509_sequence *subject_alt_name )
{
    int ret;
    size_t len, tag_len;
    asn1_buf *buf;
    uint8_t tag;
    asn1_sequence *cur = subject_alt_name;

    /* Get main sequence tag */
    if( ( ret = asn1_get_tag( p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

    if( *p + len != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    while( *p < end )
    {
        if( ( end - *p ) < 1 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                    POLARSSL_ERR_ASN1_OUT_OF_DATA );

        tag = **p;
        (*p)++;
        if( ( ret = asn1_get_len( p, end, &tag_len ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        if( ( tag & ASN1_CONTEXT_SPECIFIC ) != ASN1_CONTEXT_SPECIFIC )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                    POLARSSL_ERR_ASN1_UNEXPECTED_TAG );

        /* Skip everything but DNS name */
        if( tag != ( ASN1_CONTEXT_SPECIFIC | 2 ) )
        {
            *p += tag_len;
            continue;
        }

        /* Allocate and assign next pointer */
        if( cur->buf.p != NULL )
        {
            cur->next = (asn1_sequence *) memory_alloc(
                 sizeof( asn1_sequence ) );

            if( cur->next == NULL )
                return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                        POLARSSL_ERR_ASN1_MALLOC_FAILED );

            __stosb( cur->next, 0, sizeof( asn1_sequence ) );
            cur = cur->next;
        }

        buf = &(cur->buf);
        buf->tag = tag;
        buf->p = *p;
        buf->len = tag_len;
        *p += buf->len;
    }

    /* Set final sequence entry's next pointer to NULL */
    cur->next = NULL;

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 v3 extensions
 *
 * TODO: Perform all of the basic constraints tests required by the RFC
 * TODO: Set values for undetected extensions to a sane default?
 *
 */
static int x509_get_crt_ext( uint8_t **p,
                             const uint8_t *end,
                             x509_crt *crt )
{
    int ret;
    size_t len;
    uint8_t *end_ext_data, *end_ext_octet;

    if( ( ret = x509_get_ext( p, end, &crt->v3_ext, 3 ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    while( *p < end )
    {
        /*
         * Extension  ::=  SEQUENCE  {
         *      extnID      OBJECT IDENTIFIER,
         *      critical    BOOLEAN DEFAULT FALSE,
         *      extnValue   OCTET STRING  }
         */
        x509_buf extn_oid = {0, 0, NULL};
        int is_critical = 0; /* DEFAULT FALSE */
        int ext_type = 0;

        if( ( ret = asn1_get_tag( p, end, &len,
                ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        end_ext_data = *p + len;

        /* Get extension ID */
        extn_oid.tag = **p;

        if( ( ret = asn1_get_tag( p, end, &extn_oid.len, ASN1_OID ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        extn_oid.p = *p;
        *p += extn_oid.len;

        if( ( end - *p ) < 1 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                    POLARSSL_ERR_ASN1_OUT_OF_DATA );

        /* Get optional critical */
        if( ( ret = asn1_get_bool( p, end_ext_data, &is_critical ) ) != 0 &&
            ( ret != POLARSSL_ERR_ASN1_UNEXPECTED_TAG ) )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        /* Data should be octet string type */
        if( ( ret = asn1_get_tag( p, end_ext_data, &len,
                ASN1_OCTET_STRING ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        end_ext_octet = *p + len;

        if( end_ext_octet != end_ext_data )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                    POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

        /*
         * Detect supported extensions
         */
        ret = oid_get_x509_ext_type( &extn_oid, &ext_type );

        if( ret != 0 )
        {
            /* No parser found, skip extension */
            *p = end_ext_octet;

            if( is_critical )
            {
                /* Data is marked as critical: fail */
                return ( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                        POLARSSL_ERR_ASN1_UNEXPECTED_TAG );
            }
            continue;
        }

        crt->ext_types |= ext_type;

        switch( ext_type )
        {
        case EXT_BASIC_CONSTRAINTS:
            /* Parse basic constraints */
            if( ( ret = x509_get_basic_constraints( p, end_ext_octet,
                    &crt->ca_istrue, &crt->max_pathlen ) ) != 0 )
                return ( ret );
            break;

        case EXT_KEY_USAGE:
            /* Parse key usage */
            if( ( ret = x509_get_key_usage( p, end_ext_octet,
                    &crt->key_usage ) ) != 0 )
                return ( ret );
            break;

        case EXT_EXTENDED_KEY_USAGE:
            /* Parse extended key usage */
            if( ( ret = x509_get_ext_key_usage( p, end_ext_octet,
                    &crt->ext_key_usage ) ) != 0 )
                return ( ret );
            break;

        case EXT_SUBJECT_ALT_NAME:
            /* Parse subject alt name */
            if( ( ret = x509_get_subject_alt_name( p, end_ext_octet,
                    &crt->subject_alt_names ) ) != 0 )
                return ( ret );
            break;

        case EXT_NS_CERT_TYPE:
            /* Parse netscape certificate type */
            if( ( ret = x509_get_ns_cert_type( p, end_ext_octet,
                    &crt->ns_cert_type ) ) != 0 )
                return ( ret );
            break;

        default:
            return( POLARSSL_ERR_X509_FEATURE_UNAVAILABLE );
        }
    }

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * Parse and fill a single X.509 certificate in DER format
 */
static int x509_crt_parse_der_core( x509_crt *crt, const uint8_t *buf,
                                    size_t buflen )
{
    int ret;
    size_t len;
    uint8_t *p, *end, *crt_end;

    /*
     * Check for valid input
     */
    if( crt == NULL || buf == NULL )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    p = (uint8_t *) memory_alloc( len = buflen );

    if( p == NULL )
        return( POLARSSL_ERR_X509_MALLOC_FAILED );

    __movsb( p, buf, buflen );

    crt->raw.p = p;
    crt->raw.len = len;
    end = p + len;

    /*
     * Certificate  ::=  SEQUENCE  {
     *      tbsCertificate       TBSCertificate,
     *      signatureAlgorithm   AlgorithmIdentifier,
     *      signatureValue       BIT STRING  }
     */
    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT );
    }

    if( len > (size_t) ( end - p ) )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }
    crt_end = p + len;

    /*
     * TBSCertificate  ::=  SEQUENCE  {
     */
    crt->tbs.p = p;

    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT + ret );
    }

    end = p + len;
    crt->tbs.len = end - crt->tbs.p;

    /*
     * Version  ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
     *
     * CertificateSerialNumber  ::=  INTEGER
     *
     * signature            AlgorithmIdentifier
     */
    if( ( ret = x509_get_version(  &p, end, &crt->version  ) ) != 0 ||
        ( ret = x509_get_serial(   &p, end, &crt->serial   ) ) != 0 ||
        ( ret = x509_get_alg_null( &p, end, &crt->sig_oid1 ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    crt->version++;

    if( crt->version > 3 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_UNKNOWN_VERSION );
    }

    if( ( ret = x509_get_sig_alg( &crt->sig_oid1, &crt->sig_md,
                                  &crt->sig_pk ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    /*
     * issuer               Name
     */
    crt->issuer_raw.p = p;

    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT + ret );
    }

    if( ( ret = x509_get_name( &p, p + len, &crt->issuer ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    crt->issuer_raw.len = p - crt->issuer_raw.p;

    /*
     * Validity ::= SEQUENCE {
     *      notBefore      Time,
     *      notAfter       Time }
     *
     */
    if( ( ret = x509_get_dates( &p, end, &crt->valid_from,
                                         &crt->valid_to ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    /*
     * subject              Name
     */
    crt->subject_raw.p = p;

    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT + ret );
    }

    if( len && ( ret = x509_get_name( &p, p + len, &crt->subject ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    crt->subject_raw.len = p - crt->subject_raw.p;

    /*
     * SubjectPublicKeyInfo
     */
    if( ( ret = pk_parse_subpubkey( &p, end, &crt->pk ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    /*
     *  issuerUniqueID  [1]  IMPLICIT UniqueIdentifier OPTIONAL,
     *                       -- If present, version shall be v2 or v3
     *  subjectUniqueID [2]  IMPLICIT UniqueIdentifier OPTIONAL,
     *                       -- If present, version shall be v2 or v3
     *  extensions      [3]  EXPLICIT Extensions OPTIONAL
     *                       -- If present, version shall be v3
     */
    if( crt->version == 2 || crt->version == 3 )
    {
        ret = x509_get_uid( &p, end, &crt->issuer_id,  1 );
        if( ret != 0 )
        {
            x509_crt_free( crt );
            return( ret );
        }
    }

    if( crt->version == 2 || crt->version == 3 )
    {
        ret = x509_get_uid( &p, end, &crt->subject_id,  2 );
        if( ret != 0 )
        {
            x509_crt_free( crt );
            return( ret );
        }
    }

    if( crt->version == 3 )
    {
        ret = x509_get_crt_ext( &p, end, crt);
        if( ret != 0 )
        {
            x509_crt_free( crt );
            return( ret );
        }
    }

    if( p != end )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }

    end = crt_end;

    /*
     *  }
     *  -- end of TBSCertificate
     *
     *  signatureAlgorithm   AlgorithmIdentifier,
     *  signatureValue       BIT STRING
     */
    if( ( ret = x509_get_alg_null( &p, end, &crt->sig_oid2 ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    if( crt->sig_oid1.len != crt->sig_oid2.len ||
        memcmp( crt->sig_oid1.p, crt->sig_oid2.p, crt->sig_oid1.len ) != 0 )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_SIG_MISMATCH );
    }

    if( ( ret = x509_get_sig( &p, end, &crt->sig ) ) != 0 )
    {
        x509_crt_free( crt );
        return( ret );
    }

    if( p != end )
    {
        x509_crt_free( crt );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }

    return( 0 );
}

/*
 * Parse one X.509 certificate in DER format from a buffer and add them to a
 * chained list
 */
int x509_crt_parse_der( x509_crt *chain, const uint8_t *buf,
                        size_t buflen )
{
    int ret;
    x509_crt *crt = chain, *prev = NULL;

    /*
     * Check for valid input
     */
    if( crt == NULL || buf == NULL )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    while( crt->version != 0 && crt->next != NULL )
    {
        prev = crt;
        crt = crt->next;
    }

    /*
     * Add new certificate on the end of the chain if needed.
     */
    if ( crt->version != 0 && crt->next == NULL)
    {
        crt->next = (x509_crt *) memory_alloc( sizeof( x509_crt ) );

        if( crt->next == NULL )
            return( POLARSSL_ERR_X509_MALLOC_FAILED );

        prev = crt;
        crt = crt->next;
        x509_crt_init( crt );
    }

    if( ( ret = x509_crt_parse_der_core( crt, buf, buflen ) ) != 0 )
    {
        if( prev )
            prev->next = NULL;

        if( crt != chain )
            memory_free( crt );

        return( ret );
    }

    return( 0 );
}

/*
 * Parse one or more PEM certificates from a buffer and add them to the chained
 * list
 */
int x509_crt_parse( x509_crt *chain, const uint8_t *buf, size_t buflen )
{
    int success = 0, first_error = 0, total_failed = 0;
    int buf_format = X509_FORMAT_DER;

    /*
     * Check for valid input
     */
    if( chain == NULL || buf == NULL )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    /*
     * Determine buffer content. Buffer contains either one DER certificate or
     * one or more PEM certificates.
     */
#if defined(POLARSSL_PEM_PARSE_C)
    if( strstr( (const char *) buf, "-----BEGIN CERTIFICATE-----" ) != NULL )
        buf_format = X509_FORMAT_PEM;
#endif

    if( buf_format == X509_FORMAT_DER )
        return x509_crt_parse_der( chain, buf, buflen );

#if defined(POLARSSL_PEM_PARSE_C)
    if( buf_format == X509_FORMAT_PEM )
    {
        int ret;
        pem_context pem;

        while( buflen > 0 )
        {
            size_t use_len;
            pem_init( &pem );

            ret = pem_read_buffer( &pem,
                           "-----BEGIN CERTIFICATE-----",
                           "-----END CERTIFICATE-----",
                           buf, NULL, 0, &use_len );

            if( ret == 0 )
            {
                /*
                 * Was PEM encoded
                 */
                buflen -= use_len;
                buf += use_len;
            }
            else if( ret == POLARSSL_ERR_PEM_BAD_INPUT_DATA )
            {
                return( ret );
            }
            else if( ret != POLARSSL_ERR_PEM_NO_HEADER_FOOTER_PRESENT )
            {
                pem_free( &pem );

                /*
                 * PEM header and footer were found
                 */
                buflen -= use_len;
                buf += use_len;

                if( first_error == 0 )
                    first_error = ret;

                continue;
            }
            else
                break;

            ret = x509_crt_parse_der( chain, pem.buf, pem.buflen );

            pem_free( &pem );

            if( ret != 0 )
            {
                /*
                 * Quit parsing on a memory error
                 */
                if( ret == POLARSSL_ERR_X509_MALLOC_FAILED )
                    return( ret );

                if( first_error == 0 )
                    first_error = ret;

                total_failed++;
                continue;
            }

            success = 1;
        }
    }
#endif /* POLARSSL_PEM_PARSE_C */

    if( success )
        return( total_failed );
    else if( first_error )
        return( first_error );
    else
        return( POLARSSL_ERR_X509_CERT_UNKNOWN_FORMAT );
}

/*
 * Load one or more certificates and add them to the chained list
 */
int x509_crt_parse_file( x509_crt *chain, const char *path )
{
    int ret;
    size_t n;
    uint8_t *buf;

    if ( ( ret = x509_load_file( path, &buf, &n ) ) != 0 )
        return( ret );

    ret = x509_crt_parse( chain, buf, n );

    __stosb( buf, 0, n + 1 );
    memory_free( buf );

    return( ret );
}

int x509_crt_parse_path( x509_crt *chain, const char *path )
{
    int ret = 0;
    int w_ret;
    WCHAR szDir[MAX_PATH];
    char filename[MAX_PATH];
    char *p;
    int len = (int) strlen( path );

    WIN32_FIND_DATAW file_data;
    HANDLE hFind;

    if( len > MAX_PATH - 3 )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    __stosb( szDir, 0, sizeof(szDir) );
    __stosb( filename, 0, MAX_PATH );
    __movsb( filename, path, len );
    filename[len++] = '\\';
    p = filename + len;
    filename[len++] = '*';

    w_ret = MultiByteToWideChar( CP_ACP, 0, filename, len, szDir,
                                 MAX_PATH - 3 );

    hFind = FindFirstFileW( szDir, &file_data );
    if (hFind == INVALID_HANDLE_VALUE)
        return( POLARSSL_ERR_X509_FILE_IO_ERROR );

    len = MAX_PATH - len;
    do
    {
        __stosb( p, 0, len );

        if( file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            continue;

        w_ret = WideCharToMultiByte( CP_ACP, 0, file_data.cFileName,
                                     lstrlenW(file_data.cFileName),
                                     p, len - 1,
                                     NULL, NULL );

        w_ret = x509_crt_parse_file( chain, filename );
        if( w_ret < 0 )
            ret++;
        else
            ret += w_ret;
    }
    while( FindNextFileW( hFind, &file_data ) != 0 );

    if (GetLastError() != ERROR_NO_MORE_FILES)
        ret = POLARSSL_ERR_X509_FILE_IO_ERROR;

    FindClose( hFind );

    return( ret );
}

#define POLARSSL_ERR_DEBUG_BUF_TOO_SMALL    -2

#define SAFE_SNPRINTF()                         \
{                                               \
    if( ret == -1 )                             \
        return( -1 );                           \
                                                \
    if ( (uint32_t) ret > n ) {             \
        p[n - 1] = '\0';                        \
        return POLARSSL_ERR_DEBUG_BUF_TOO_SMALL;\
    }                                           \
                                                \
    n -= (uint32_t) ret;                    \
    p += (uint32_t) ret;                    \
}

static int x509_info_subject_alt_name( char **buf, size_t *size,
                                       const x509_sequence *subject_alt_name )
{
    size_t i;
    size_t n = *size;
    char *p = *buf;
    const x509_sequence *cur = subject_alt_name;
    const char *sep = "";
    size_t sep_len = 0;

    while( cur != NULL )
    {
        if( cur->buf.len + sep_len >= n )
        {
            *p = '\0';
            return( POLARSSL_ERR_DEBUG_BUF_TOO_SMALL );
        }

        n -= cur->buf.len + sep_len;
        for( i = 0; i < sep_len; i++ )
            *p++ = sep[i];
        for( i = 0; i < cur->buf.len; i++ )
            *p++ = cur->buf.p[i];

        sep = ", ";
        sep_len = 2;

        cur = cur->next;
    }

    *p = '\0';

    *size = n;
    *buf = p;

    return( 0 );
}

#define PRINT_ITEM(i)                           \
    {                                           \
        ret = fn__snprintf( p, n, "%s" i, sep );    \
        SAFE_SNPRINTF();                        \
        sep = ", ";                             \
    }

#define CERT_TYPE(type,name)                    \
    if( ns_cert_type & type )                   \
        PRINT_ITEM( name );

static int x509_info_cert_type( char **buf, size_t *size,
                                uint8_t ns_cert_type )
{
    int ret;
    size_t n = *size;
    char *p = *buf;
    const char *sep = "";

    CERT_TYPE( NS_CERT_TYPE_SSL_CLIENT,         "SSL Client" );
    CERT_TYPE( NS_CERT_TYPE_SSL_SERVER,         "SSL Server" );
    CERT_TYPE( NS_CERT_TYPE_EMAIL,              "Email" );
    CERT_TYPE( NS_CERT_TYPE_OBJECT_SIGNING,     "Object Signing" );
    CERT_TYPE( NS_CERT_TYPE_RESERVED,           "Reserved" );
    CERT_TYPE( NS_CERT_TYPE_SSL_CA,             "SSL CA" );
    CERT_TYPE( NS_CERT_TYPE_EMAIL_CA,           "Email CA" );
    CERT_TYPE( NS_CERT_TYPE_OBJECT_SIGNING_CA,  "Object Signing CA" );

    *size = n;
    *buf = p;

    return( 0 );
}

#define KEY_USAGE(code,name)    \
    if( key_usage & code )      \
        PRINT_ITEM( name );

static int x509_info_key_usage( char **buf, size_t *size,
                                uint8_t key_usage )
{
    int ret;
    size_t n = *size;
    char *p = *buf;
    const char *sep = "";

    KEY_USAGE( KU_DIGITAL_SIGNATURE,    "Digital Signature" );
    KEY_USAGE( KU_NON_REPUDIATION,      "Non Repudiation" );
    KEY_USAGE( KU_KEY_ENCIPHERMENT,     "Key Encipherment" );
    KEY_USAGE( KU_DATA_ENCIPHERMENT,    "Data Encipherment" );
    KEY_USAGE( KU_KEY_AGREEMENT,        "Key Agreement" );
    KEY_USAGE( KU_KEY_CERT_SIGN,        "Key Cert Sign" );
    KEY_USAGE( KU_CRL_SIGN,             "CRL Sign" );

    *size = n;
    *buf = p;

    return( 0 );
}

static int x509_info_ext_key_usage( char **buf, size_t *size,
                                    const x509_sequence *extended_key_usage )
{
    int ret;
    const char *desc;
    size_t n = *size;
    char *p = *buf;
    const x509_sequence *cur = extended_key_usage;
    const char *sep = "";

    while( cur != NULL )
    {
        if( oid_get_extended_key_usage( &cur->buf, &desc ) != 0 )
            desc = "???";

        ret = fn__snprintf(p, n, "%s%s", sep, desc);
        SAFE_SNPRINTF();

        sep = ", ";

        cur = cur->next;
    }

    *size = n;
    *buf = p;

    return( 0 );
}

/*
 * Return an informational string about the certificate.
 */
#define BEFORE_COLON    18
#define BC              "18"
int x509_crt_info( char *buf, size_t size, const char *prefix,
                   const x509_crt *crt )
{
    int ret;
    size_t n;
    char *p;
    const char *desc = NULL;
    char key_size_str[BEFORE_COLON];

    p = buf;
    n = size;

    ret = fn__snprintf(p, n, "%scert. version     : %d\n", prefix, crt->version );
    SAFE_SNPRINTF();
    ret = fn__snprintf(p, n, "%sserial number     : ", prefix );
    SAFE_SNPRINTF();

    ret = x509_serial_gets( p, n, &crt->serial);
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%sissuer name       : ", prefix);
    SAFE_SNPRINTF();
    ret = x509_dn_gets( p, n, &crt->issuer  );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%ssubject name      : ", prefix);
    SAFE_SNPRINTF();
    ret = x509_dn_gets( p, n, &crt->subject );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%sissued  on        : %04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crt->valid_from.year, crt->valid_from.mon,
                   crt->valid_from.day,  crt->valid_from.hour,
                   crt->valid_from.min,  crt->valid_from.sec );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%sexpires on        : %04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crt->valid_to.year, crt->valid_to.mon,
                   crt->valid_to.day,  crt->valid_to.hour,
                   crt->valid_to.min,  crt->valid_to.sec );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%ssigned using      : ", prefix);
    SAFE_SNPRINTF();

    ret = oid_get_sig_alg_desc( &crt->sig_oid1, &desc );
    if (ret != 0) {
        ret = fn__snprintf(p, n, "???");
    }
    else {
        ret = fn__snprintf(p, n, "%s", desc);
    }
    SAFE_SNPRINTF();

    /* Key size */
    if( ( ret = x509_key_size_helper( key_size_str, BEFORE_COLON,
                                      pk_get_name( &crt->pk ) ) ) != 0 )
    {
        return( ret );
    }

    ret = fn__snprintf(p, n, "\n%s%-" BC "s: %d bits", prefix, key_size_str, (int) pk_get_size( &crt->pk ) );
    SAFE_SNPRINTF();

    /*
     * Optional extensions
     */

    if (crt->ext_types & EXT_BASIC_CONSTRAINTS) {
        ret = fn__snprintf(p, n, "\n%sbasic constraints : CA=%s", prefix, crt->ca_istrue ? "true" : "false" );
        SAFE_SNPRINTF();

        if (crt->max_pathlen > 0) {
            ret = fn__snprintf(p, n, ", max_pathlen=%d", crt->max_pathlen - 1);
            SAFE_SNPRINTF();
        }
    }

    if( crt->ext_types & EXT_SUBJECT_ALT_NAME )
    {
        ret = fn__snprintf(p, n, "\n%ssubject alt name  : ", prefix);
        SAFE_SNPRINTF();

        if( ( ret = x509_info_subject_alt_name( &p, &n,
                                            &crt->subject_alt_names ) ) != 0 )
            return( ret );
    }

    if (crt->ext_types & EXT_NS_CERT_TYPE) {
        ret = fn__snprintf(p, n, "\n%scert. type        : ", prefix);
        SAFE_SNPRINTF();

        if( ( ret = x509_info_cert_type( &p, &n, crt->ns_cert_type ) ) != 0 )
            return( ret );
    }

    if (crt->ext_types & EXT_KEY_USAGE) {
        ret = fn__snprintf(p, n, "\n%skey usage         : ", prefix);
        SAFE_SNPRINTF();

        if( ( ret = x509_info_key_usage( &p, &n, crt->key_usage ) ) != 0 )
            return( ret );
    }

    if( crt->ext_types & EXT_EXTENDED_KEY_USAGE )
    {
        ret = fn__snprintf(p, n, "\n%sext key usage     : ", prefix);
        SAFE_SNPRINTF();

        if( ( ret = x509_info_ext_key_usage( &p, &n,
                                             &crt->ext_key_usage ) ) != 0 )
            return( ret );
    }

    ret = fn__snprintf(p, n, "\n");
    SAFE_SNPRINTF();

    return (int) ( size - n );
}

#if defined(POLARSSL_X509_CHECK_KEY_USAGE)
int x509_crt_check_key_usage( const x509_crt *crt, int usage )
{
    if( ( crt->ext_types & EXT_KEY_USAGE ) != 0 &&
        ( crt->key_usage & usage ) != usage )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    return( 0 );
}
#endif

#if defined(POLARSSL_X509_CHECK_EXTENDED_KEY_USAGE)
int x509_crt_check_extended_key_usage( const x509_crt *crt,
                                       const char *usage_oid,
                                       size_t usage_len )
{
    const x509_sequence *cur;

    /* Extension is not mandatory, absent means no restriction */
    if( ( crt->ext_types & EXT_EXTENDED_KEY_USAGE ) == 0 )
        return( 0 );

    /*
     * Look for the requested usage (or wildcard ANY) in our list
     */
    for( cur = &crt->ext_key_usage; cur != NULL; cur = cur->next )
    {
        const x509_buf *cur_oid = &cur->buf;

        if( cur_oid->len == usage_len &&
            memcmp( cur_oid->p, usage_oid, usage_len ) == 0 )
        {
            return( 0 );
        }

        if( OID_CMP( OID_ANY_EXTENDED_KEY_USAGE, cur_oid ) )
            return( 0 );
    }

    return( POLARSSL_ERR_X509_BAD_INPUT_DATA );
}
#endif /* POLARSSL_X509_CHECK_EXTENDED_KEY_USAGE */

#if defined(POLARSSL_X509_CRL_PARSE_C)
/*
 * Return 1 if the certificate is revoked, or 0 otherwise.
 */
int x509_crt_revoked( const x509_crt *crt, const x509_crl *crl )
{
    const x509_crl_entry *cur = &crl->entry;

    while( cur != NULL && cur->serial.len != 0 )
    {
        if( crt->serial.len == cur->serial.len &&
            memcmp( crt->serial.p, cur->serial.p, crt->serial.len ) == 0 )
        {
            if( x509_time_expired( &cur->revocation_date ) )
                return( 1 );
        }

        cur = cur->next;
    }

    return( 0 );
}

/*
 * Check that the given certificate is valid according to the CRL.
 */
static int x509_crt_verifycrl( x509_crt *crt, x509_crt *ca,
                               x509_crl *crl_list)
{
    int flags = 0;
    uint8_t hash[POLARSSL_MD_MAX_SIZE];
    const md_info_t *md_info;

    if( ca == NULL )
        return( flags );

    /*
     * TODO: What happens if no CRL is present?
     * Suggestion: Revocation state should be unknown if no CRL is present.
     * For backwards compatibility this is not yet implemented.
     */

    while( crl_list != NULL )
    {
        if( crl_list->version == 0 ||
            crl_list->issuer_raw.len != ca->subject_raw.len ||
            memcmp( crl_list->issuer_raw.p, ca->subject_raw.p,
                    crl_list->issuer_raw.len ) != 0 )
        {
            crl_list = crl_list->next;
            continue;
        }

        /*
         * Check if the CA is configured to sign CRLs
         */
#if defined(POLARSSL_X509_CHECK_KEY_USAGE)
        if( x509_crt_check_key_usage( ca, KU_CRL_SIGN ) != 0 )
        {
            flags |= BADCRL_NOT_TRUSTED;
            break;
        }
#endif

        /*
         * Check if CRL is correctly signed by the trusted CA
         */
        md_info = md_info_from_type( crl_list->sig_md );
        if( md_info == NULL )
        {
            /*
             * Cannot check 'unknown' hash
             */
            flags |= BADCRL_NOT_TRUSTED;
            break;
        }

        md( md_info, crl_list->tbs.p, crl_list->tbs.len, hash );

        if( pk_can_do( &ca->pk, crl_list->sig_pk ) == 0 ||
            pk_verify( &ca->pk, crl_list->sig_md, hash, md_info->size,
                       crl_list->sig.p, crl_list->sig.len ) != 0 )
        {
            flags |= BADCRL_NOT_TRUSTED;
            break;
        }

        /*
         * Check for validity of CRL (Do not drop out)
         */
        if( x509_time_expired( &crl_list->next_update ) )
            flags |= BADCRL_EXPIRED;

        if( x509_time_future( &crl_list->this_update ) )
            flags |= BADCRL_FUTURE;

        /*
         * Check if certificate is revoked
         */
        if( x509_crt_revoked(crt, crl_list) )
        {
            flags |= BADCERT_REVOKED;
            break;
        }

        crl_list = crl_list->next;
    }
    return flags;
}
#endif /* POLARSSL_X509_CRL_PARSE_C */

// Equal == 0, inequal == 1
static int x509_name_cmp( const void *s1, const void *s2, size_t len )
{
    size_t i;
    uint8_t diff;
    const uint8_t *n1 = s1, *n2 = s2;

    for( i = 0; i < len; i++ )
    {
        diff = n1[i] ^ n2[i];

        if( diff == 0 )
            continue;

        if( diff == 32 &&
            ( ( n1[i] >= 'a' && n1[i] <= 'z' ) ||
              ( n1[i] >= 'A' && n1[i] <= 'Z' ) ) )
        {
            continue;
        }

        return( 1 );
    }

    return( 0 );
}

static int x509_wildcard_verify( const char *cn, x509_buf *name )
{
    size_t i;
    size_t cn_idx = 0;

    if( name->len < 3 || name->p[0] != '*' || name->p[1] != '.' )
        return( 0 );

    for( i = 0; i < strlen( cn ); ++i )
    {
        if( cn[i] == '.' )
        {
            cn_idx = i;
            break;
        }
    }

    if( cn_idx == 0 )
        return( 0 );

    if( strlen( cn ) - cn_idx == name->len - 1 &&
        x509_name_cmp( name->p + 1, cn + cn_idx, name->len - 1 ) == 0 )
    {
        return( 1 );
    }

    return( 0 );
}

/*
* Check if 'parent' is a suitable parent (signing CA) for 'child'.
* Return 0 if yes, -1 if not.
*
* top means parent is a locally-trusted certificate
* bottom means child is the end entity cert
*/
int x509_crt_check_parent(const x509_crt *child, const x509_crt *parent, int top, int bottom)
{
    int need_ca_bit;

    /* Parent must be the issuer */
    if (child->issuer_raw.len != parent->subject_raw.len ||
        memcmp(child->issuer_raw.p, parent->subject_raw.p,
        child->issuer_raw.len) != 0)
    {
        return(-1);
    }

    /* Parent must have the basicConstraints CA bit set as a general rule */
    need_ca_bit = 1;

    /* Exception: v1/v2 certificates that are locally trusted. */
    if (top && parent->version < 3)
        need_ca_bit = 0;

    /* Exception: self-signed end-entity certs that are locally trusted. */
    if (top && bottom &&
        child->raw.len == parent->raw.len &&
        memcmp(child->raw.p, parent->raw.p, child->raw.len) == 0)
    {
        need_ca_bit = 0;
    }

    if (need_ca_bit && !parent->ca_istrue)
        return(-1);

#if defined(POLARSSL_X509_CHECK_KEY_USAGE)
    if (need_ca_bit &&
        x509_crt_check_key_usage(parent, KU_KEY_CERT_SIGN) != 0)
    {
        return(-1);
    }
#endif

    return(0);
}
static int x509_crt_verify_top(x509_crt *child, x509_crt *trust_ca, x509_crl *ca_crl, int path_cnt, int *flags, int (*f_vrfy)(void *, x509_crt *, int, int *), void *p_vrfy )
{
    int ret;
    int ca_flags = 0, check_path_cnt = path_cnt + 1;
    uint8_t hash[POLARSSL_MD_MAX_SIZE];
    const md_info_t *md_info;

    if( x509_time_expired( &child->valid_to ) )
        *flags |= BADCERT_EXPIRED;

    if( x509_time_future( &child->valid_from ) )
        *flags |= BADCERT_FUTURE;

    /*
     * Child is the top of the chain. Check against the trust_ca list.
     */
    *flags |= BADCERT_NOT_TRUSTED;

    md_info = md_info_from_type( child->sig_md );
    if( md_info == NULL )
    {
        /*
         * Cannot check 'unknown', no need to try any CA
         */
        trust_ca = NULL;
    }
    else
        md( md_info, child->tbs.p, child->tbs.len, hash );

    for( /* trust_ca */ ; trust_ca != NULL; trust_ca = trust_ca->next )
    {
        if( x509_crt_check_parent( child, trust_ca, 1, path_cnt == 0 ) != 0 )
            continue;

        /*
         * Reduce path_len to check against if top of the chain is
         * the same as the trusted CA
         */
        if( child->subject_raw.len == trust_ca->subject_raw.len &&
            memcmp( child->subject_raw.p, trust_ca->subject_raw.p,
                            child->issuer_raw.len ) == 0 )
        {
            check_path_cnt--;
        }

        if( trust_ca->max_pathlen > 0 &&
            trust_ca->max_pathlen < check_path_cnt )
        {
            continue;
        }

        if( pk_can_do( &trust_ca->pk, child->sig_pk ) == 0 ||
            pk_verify( &trust_ca->pk, child->sig_md, hash, md_info->size,
                       child->sig.p, child->sig.len ) != 0 )
        {
            continue;
        }

        /*
         * Top of chain is signed by a trusted CA
         */
        *flags &= ~BADCERT_NOT_TRUSTED;
        break;
    }

    /*
     * If top of chain is not the same as the trusted CA send a verify request
     * to the callback for any issues with validity and CRL presence for the
     * trusted CA certificate.
     */
    if( trust_ca != NULL &&
        ( child->subject_raw.len != trust_ca->subject_raw.len ||
          memcmp( child->subject_raw.p, trust_ca->subject_raw.p,
                            child->issuer_raw.len ) != 0 ) )
    {
#if defined(POLARSSL_X509_CRL_PARSE_C)
        /* Check trusted CA's CRL for the chain's top crt */
        *flags |= x509_crt_verifycrl( child, trust_ca, ca_crl );
#else
        ((void) ca_crl);
#endif

        if( x509_time_expired( &trust_ca->valid_to ) )
            ca_flags |= BADCERT_EXPIRED;

        if( x509_time_future( &trust_ca->valid_from ) )
            ca_flags |= BADCERT_FUTURE;

        if( NULL != f_vrfy )
        {
            if( ( ret = f_vrfy( p_vrfy, trust_ca, path_cnt + 1,
                                &ca_flags ) ) != 0 )
            {
                return( ret );
            }
        }
    }

    /* Call callback on top cert */
    if( NULL != f_vrfy )
    {
        if( ( ret = f_vrfy(p_vrfy, child, path_cnt, flags ) ) != 0 )
            return( ret );
    }

    *flags |= ca_flags;

    return( 0 );
}

static int x509_crt_verify_child(
                x509_crt *child, x509_crt *parent, x509_crt *trust_ca,
                x509_crl *ca_crl, int path_cnt, int *flags,
                int (*f_vrfy)(void *, x509_crt *, int, int *),
                void *p_vrfy )
{
    int ret;
    int parent_flags = 0;
    uint8_t hash[POLARSSL_MD_MAX_SIZE];
    x509_crt *grandparent;
    const md_info_t *md_info;

    if( x509_time_expired( &child->valid_to ) )
        *flags |= BADCERT_EXPIRED;

    if( x509_time_future( &child->valid_from ) )
        *flags |= BADCERT_FUTURE;

    md_info = md_info_from_type( child->sig_md );
    if( md_info == NULL )
    {
        /*
         * Cannot check 'unknown' hash
         */
        *flags |= BADCERT_NOT_TRUSTED;
    }
    else
    {
        md( md_info, child->tbs.p, child->tbs.len, hash );

        if( pk_can_do( &parent->pk, child->sig_pk ) == 0 ||
            pk_verify( &parent->pk, child->sig_md, hash, md_info->size,
                       child->sig.p, child->sig.len ) != 0 )
        {
            *flags |= BADCERT_NOT_TRUSTED;
        }
    }

#if defined(POLARSSL_X509_CRL_PARSE_C)
    /* Check trusted CA's CRL for the given crt */
    *flags |= x509_crt_verifycrl(child, parent, ca_crl);
#endif

    /* Look for a grandparent upwards the chain */
    for( grandparent = parent->next;
         grandparent != NULL;
         grandparent = grandparent->next )
    {
        if( x509_crt_check_parent( parent, grandparent, 0, path_cnt == 0 ) == 0 )
            break;
    }

    /* Is our parent part of the chain or at the top? */
    if( grandparent != NULL )
    {
        ret = x509_crt_verify_child( parent, grandparent, trust_ca, ca_crl,
                                path_cnt + 1, &parent_flags, f_vrfy, p_vrfy );
        if( ret != 0 )
            return( ret );
    }
    else
    {
        ret = x509_crt_verify_top( parent, trust_ca, ca_crl,
                                path_cnt + 1, &parent_flags, f_vrfy, p_vrfy );
        if( ret != 0 )
            return( ret );
    }

    /* child is verified to be a child of the parent, call verify callback */
    if( NULL != f_vrfy )
        if( ( ret = f_vrfy( p_vrfy, child, path_cnt, flags ) ) != 0 )
            return( ret );

    *flags |= parent_flags;

    return( 0 );
}

/*
 * Verify the certificate validity
 */
int x509_crt_verify( x509_crt *crt,
                     x509_crt *trust_ca,
                     x509_crl *ca_crl,
                     const char *cn, int *flags,
                     int (*f_vrfy)(void *, x509_crt *, int, int *),
                     void *p_vrfy )
{
    size_t cn_len;
    int ret;
    int pathlen = 0;
    x509_crt *parent;
    x509_name *name;
    x509_sequence *cur = NULL;

    *flags = 0;

    if( cn != NULL )
    {
        name = &crt->subject;
        cn_len = strlen( cn );

        if( crt->ext_types & EXT_SUBJECT_ALT_NAME )
        {
            cur = &crt->subject_alt_names;

            while( cur != NULL )
            {
                if( cur->buf.len == cn_len &&
                    x509_name_cmp( cn, cur->buf.p, cn_len ) == 0 )
                    break;

                if( cur->buf.len > 2 &&
                    memcmp( cur->buf.p, "*.", 2 ) == 0 &&
                            x509_wildcard_verify( cn, &cur->buf ) )
                    break;

                cur = cur->next;
            }

            if( cur == NULL )
                *flags |= BADCERT_CN_MISMATCH;
        }
        else
        {
            while( name != NULL )
            {
                if( OID_CMP( OID_AT_CN, &name->oid ) )
                {
                    if( name->val.len == cn_len &&
                        x509_name_cmp( name->val.p, cn, cn_len ) == 0 )
                        break;

                    if( name->val.len > 2 &&
                        memcmp( name->val.p, "*.", 2 ) == 0 &&
                                x509_wildcard_verify( cn, &name->val ) )
                        break;
                }

                name = name->next;
            }

            if( name == NULL )
                *flags |= BADCERT_CN_MISMATCH;
        }
    }

    /* Look for a parent upwards the chain */
    for( parent = crt->next; parent != NULL; parent = parent->next )
    {
        if( x509_crt_check_parent( crt, parent, 0, pathlen == 0 ) == 0 )
            break;
    }

    /* Are we part of the chain or at the top? */
    if( parent != NULL )
    {
        ret = x509_crt_verify_child( crt, parent, trust_ca, ca_crl,
                                     pathlen, flags, f_vrfy, p_vrfy );
        if( ret != 0 )
            return( ret );
    }
    else
    {
        ret = x509_crt_verify_top( crt, trust_ca, ca_crl,
                                   pathlen, flags, f_vrfy, p_vrfy );
        if( ret != 0 )
            return( ret );
    }

    if( *flags != 0 )
        return( POLARSSL_ERR_X509_CERT_VERIFY_FAILED );

    return( 0 );
}

/*
 * Initialize a certificate chain
 */
void x509_crt_init( x509_crt *crt )
{
    __stosb( crt, 0, sizeof(x509_crt) );
}

/*
 * Unallocate all certificate data
 */
void x509_crt_free( x509_crt *crt )
{
    x509_crt *cert_cur = crt;
    x509_crt *cert_prv;
    x509_name *name_cur;
    x509_name *name_prv;
    x509_sequence *seq_cur;
    x509_sequence *seq_prv;

    if( crt == NULL )
        return;

    do
    {
        pk_free( &cert_cur->pk );

        name_cur = cert_cur->issuer.next;
        while( name_cur != NULL )
        {
            name_prv = name_cur;
            name_cur = name_cur->next;
            __stosb( name_prv, 0, sizeof( x509_name ) );
            memory_free( name_prv );
        }

        name_cur = cert_cur->subject.next;
        while( name_cur != NULL )
        {
            name_prv = name_cur;
            name_cur = name_cur->next;
            __stosb( name_prv, 0, sizeof( x509_name ) );
            memory_free( name_prv );
        }

        seq_cur = cert_cur->ext_key_usage.next;
        while( seq_cur != NULL )
        {
            seq_prv = seq_cur;
            seq_cur = seq_cur->next;
            __stosb( seq_prv, 0, sizeof( x509_sequence ) );
            memory_free( seq_prv );
        }

        seq_cur = cert_cur->subject_alt_names.next;
        while( seq_cur != NULL )
        {
            seq_prv = seq_cur;
            seq_cur = seq_cur->next;
            __stosb( seq_prv, 0, sizeof( x509_sequence ) );
            memory_free( seq_prv );
        }

        if( cert_cur->raw.p != NULL )
        {
            __stosb( cert_cur->raw.p, 0, cert_cur->raw.len );
            memory_free( cert_cur->raw.p );
        }

        cert_cur = cert_cur->next;
    }
    while( cert_cur != NULL );

    cert_cur = crt;
    do
    {
        cert_prv = cert_cur;
        cert_cur = cert_cur->next;

        __stosb( cert_prv, 0, sizeof( x509_crt ) );
        if( cert_prv != crt )
            memory_free( cert_prv );
    }
    while( cert_cur != NULL );
}

#endif /* POLARSSL_X509_CRT_PARSE_C */

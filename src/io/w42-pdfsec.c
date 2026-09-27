/* w42-pdfsec.c - see w42-pdfsec.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pdfsec.h"

#include <glib/gi18n.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_GNUTLS
#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>
#include <gnutls/crypto.h>
#include <gnutls/pkcs12.h>
#include <gnutls/pkcs7.h>
#include <gnutls/x509.h>
#endif

gboolean
w42_pdf_security_available (void)
{
#ifdef HAVE_GNUTLS
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef HAVE_GNUTLS

/* ====================================================================== */
/* The password                                                            */
/* ====================================================================== */

/* The permissions, as the P entry's bits count them from 1: printing (3),
 * changing the document (4), copying (5), adding notes and filling in
 * forms (6), filling in forms (9), copying for accessibility (10),
 * putting the pages together anew (11) and printing well (12).  Bits
 * 1 and 2 are always clear, and all the ones PDF has not given a
 * meaning are set. */
#define PERMIT_ALL       ((gint32) 0xFFFFFFFC)
#define PERMIT_NO_CHANGE ((gint32) (0xFFFFFFFCu & ~((1u << 3) | (1u << 5) | (1u << 8) | (1u << 10))))

struct _W42PdfCrypt {
  guint8             key[32];
  guint8             o[48], u[48], oe[32], ue[32], perms[16];
  gint32             p;
  gnutls_cipher_hd_t cipher;
};

static gboolean
aes_cbc (gnutls_cipher_algorithm_t alg, const guint8 *key, gsize key_len,
         const guint8 *iv, const guint8 *in, gsize len, guint8 *out)
{
  gnutls_cipher_hd_t h;
  gnutls_datum_t k = { (unsigned char *) key, (unsigned int) key_len };
  gnutls_datum_t v = { (unsigned char *) iv, 16 };
  int r;

  if (gnutls_cipher_init (&h, alg, &k, &v) < 0)
    return FALSE;
  r = gnutls_cipher_encrypt2 (h, in, len, out, len);
  gnutls_cipher_deinit (h);
  return r >= 0;
}

/* The password as revision 6 wants it: normalised, as SASLprep does,
 * and in UTF-8 of no more than 127 bytes. */
static char *
prepare_password (const char *password, gsize *len)
{
  char *norm;

  if (password == NULL)
    password = "";
  norm = g_utf8_normalize (password, -1, G_NORMALIZE_NFKC);
  if (norm == NULL)
    norm = g_strdup (password);
  *len = MIN (strlen (norm), (gsize) 127);
  return norm;
}

/* Algorithm 2.B of ISO 32000-2: the hash revision 6 makes of a password,
 * a salt and, for the owner password, the user entry.  Rounds of AES
 * over the password repeated, each hashed with SHA-256, -384 or -512 as
 * the round before decides, at least sixty-four of them: slow on
 * purpose, so that guessing a password costs as much as it can. */
static gboolean
hash_r6 (const guint8 *pw, gsize pw_len, const guint8 salt[8],
         const guint8 *udata, gsize udata_len, guint8 out[32])
{
  guint8 k[64];
  gsize k_len = 32;
  GByteArray *input = g_byte_array_new ();
  gboolean ok = TRUE;

  g_byte_array_append (input, pw, (guint) pw_len);
  g_byte_array_append (input, salt, 8);
  g_byte_array_append (input, udata, (guint) udata_len);
  gnutls_hash_fast (GNUTLS_DIG_SHA256, input->data, input->len, k);
  g_byte_array_free (input, TRUE);

  for (int round = 1; ok; round++)
    {
      gsize block = pw_len + k_len + udata_len;
      guint8 *k1 = g_malloc (block * 64);
      guint8 *e = g_malloc (block * 64);
      guint sum = 0;
      guint8 last;

      for (int i = 0; i < 64; i++)
        {
          guint8 *p = k1 + i * block;

          if (pw_len > 0)
            memcpy (p, pw, pw_len);
          memcpy (p + pw_len, k, k_len);
          if (udata_len > 0)
            memcpy (p + pw_len + k_len, udata, udata_len);
        }
      ok = aes_cbc (GNUTLS_CIPHER_AES_128_CBC, k, 16, k + 16, k1, block * 64, e);
      for (int i = 0; i < 16; i++)
        sum += e[i];
      switch (sum % 3)
        {
        case 0:
          gnutls_hash_fast (GNUTLS_DIG_SHA256, e, block * 64, k);
          k_len = 32;
          break;
        case 1:
          gnutls_hash_fast (GNUTLS_DIG_SHA384, e, block * 64, k);
          k_len = 48;
          break;
        default:
          gnutls_hash_fast (GNUTLS_DIG_SHA512, e, block * 64, k);
          k_len = 64;
          break;
        }
      last = e[block * 64 - 1];
      g_free (k1);
      g_free (e);
      if (round >= 64 && last <= round - 32)
        break;
    }
  memcpy (out, k, 32);
  memset (k, 0, sizeof k);
  return ok;
}

W42PdfCrypt *
w42_pdf_crypt_new (const char *open_password, const char *modify_password,
                   GError **error)
{
  W42PdfCrypt *c = g_new0 (W42PdfCrypt, 1);
  gboolean has_open = open_password != NULL && *open_password != '\0';
  gboolean has_modify = modify_password != NULL && *modify_password != '\0';
  char *user, *owner;
  gsize user_len, owner_len;
  guint8 salts[32], ik[32], zero[16] = { 0 }, perms[16];
  gnutls_datum_t key = { NULL, 32 };
  gboolean ok;

  g_return_val_if_fail (has_open || has_modify, NULL);

  /* Without a password to modify, the owner's is the user's: whoever
   * can open the file can do anything with it, as with no password to
   * modify in Word. */
  c->p = has_modify ? PERMIT_NO_CHANGE : PERMIT_ALL;
  user = prepare_password (open_password, &user_len);
  owner = prepare_password (has_modify ? modify_password : open_password, &owner_len);

  ok = gnutls_rnd (GNUTLS_RND_KEY, c->key, sizeof c->key) == 0 &&
       gnutls_rnd (GNUTLS_RND_NONCE, salts, sizeof salts) == 0;

  /* U: the hash of the user password with its validation salt, then the
   * two salts; UE: the key, encrypted with the hash made with the other. */
  ok = ok && hash_r6 ((const guint8 *) user, user_len, salts, NULL, 0, c->u);
  memcpy (c->u + 32, salts, 16);
  ok = ok && hash_r6 ((const guint8 *) user, user_len, salts + 8, NULL, 0, ik);
  ok = ok && aes_cbc (GNUTLS_CIPHER_AES_256_CBC, ik, 32, zero, c->key, 32, c->ue);

  /* O and OE the same for the owner password, with U mixed in. */
  ok = ok && hash_r6 ((const guint8 *) owner, owner_len, salts + 16, c->u, 48, c->o);
  memcpy (c->o + 32, salts + 16, 16);
  ok = ok && hash_r6 ((const guint8 *) owner, owner_len, salts + 24, c->u, 48, ik);
  ok = ok && aes_cbc (GNUTLS_CIPHER_AES_256_CBC, ik, 32, zero, c->key, 32, c->oe);

  /* Perms: the permissions again, encrypted with the key itself, so that
   * a reader can tell whether P has been tampered with.  One block with
   * a zero IV is the ECB the algorithm asks for. */
  for (int i = 0; i < 4; i++)
    perms[i] = (guint8) ((guint32) c->p >> (8 * i));
  memset (perms + 4, 0xFF, 4);
  perms[8] = 'T';
  memcpy (perms + 9, "adb", 3);
  ok = ok && gnutls_rnd (GNUTLS_RND_NONCE, perms + 12, 4) == 0;
  ok = ok && aes_cbc (GNUTLS_CIPHER_AES_256_CBC, c->key, 32, zero, perms, 16, c->perms);

  key.data = c->key;
  ok = ok && gnutls_cipher_init (&c->cipher, GNUTLS_CIPHER_AES_256_CBC, &key,
                                 &(gnutls_datum_t) { zero, 16 }) == 0;

  memset (ik, 0, sizeof ik);
  memset (user, 0, strlen (user));
  memset (owner, 0, strlen (owner));
  g_free (user);
  g_free (owner);

  if (!ok)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Word42 could not encrypt the PDF."));
      if (c->cipher != NULL)
        gnutls_cipher_deinit (c->cipher);
      memset (c, 0, sizeof *c);
      g_free (c);
      return NULL;
    }
  return c;
}

W42PdfObj *
w42_pdf_crypt_dict (W42PdfCrypt *c)
{
  W42PdfObj *dict = w42_pdf_dict ();
  W42PdfObj *cf = w42_pdf_dict (), *std = w42_pdf_dict ();

  w42_pdf_dict_set (std, "Type", w42_pdf_name ("CryptFilter"));
  w42_pdf_dict_set (std, "CFM", w42_pdf_name ("AESV3"));
  w42_pdf_dict_set (std, "AuthEvent", w42_pdf_name ("DocOpen"));
  w42_pdf_dict_set (std, "Length", w42_pdf_int (32));
  w42_pdf_dict_set (cf, "StdCF", std);

  w42_pdf_dict_set (dict, "Filter", w42_pdf_name ("Standard"));
  w42_pdf_dict_set (dict, "V", w42_pdf_int (5));
  w42_pdf_dict_set (dict, "R", w42_pdf_int (6));
  w42_pdf_dict_set (dict, "Length", w42_pdf_int (256));
  w42_pdf_dict_set (dict, "CF", cf);
  w42_pdf_dict_set (dict, "StmF", w42_pdf_name ("StdCF"));
  w42_pdf_dict_set (dict, "StrF", w42_pdf_name ("StdCF"));
  w42_pdf_dict_set (dict, "O", w42_pdf_string (c->o, sizeof c->o));
  w42_pdf_dict_set (dict, "U", w42_pdf_string (c->u, sizeof c->u));
  w42_pdf_dict_set (dict, "OE", w42_pdf_string (c->oe, sizeof c->oe));
  w42_pdf_dict_set (dict, "UE", w42_pdf_string (c->ue, sizeof c->ue));
  w42_pdf_dict_set (dict, "Perms", w42_pdf_string (c->perms, sizeof c->perms));
  w42_pdf_dict_set (dict, "P", w42_pdf_int (c->p));
  w42_pdf_dict_set (dict, "EncryptMetadata", w42_pdf_bool (TRUE));
  return dict;
}

/* AESV3: a random IV, then the bytes padded to the block as PKCS #7
 * pads them, encrypted in CBC with the file's key as it is. */
GBytes *
w42_pdf_crypt_encrypt (gpointer crypt, const guint8 *data, gsize len)
{
  W42PdfCrypt *c = crypt;
  gsize pad = 16 - len % 16;
  guint8 *plain = g_malloc (len + pad);
  guint8 *out = g_malloc (16 + len + pad);

  if (len > 0)
    memcpy (plain, data, len);
  memset (plain + len, (int) pad, pad);
  if (gnutls_rnd (GNUTLS_RND_NONCE, out, 16) != 0)
    g_error ("GnuTLS has no random numbers");
  gnutls_cipher_set_iv (c->cipher, out, 16);
  if (gnutls_cipher_encrypt2 (c->cipher, plain, len + pad, out + 16, len + pad) < 0)
    g_error ("GnuTLS could not encrypt");
  g_free (plain);
  return g_bytes_new_take (out, 16 + len + pad);
}

void
w42_pdf_crypt_free (W42PdfCrypt *c)
{
  if (c == NULL)
    return;
  gnutls_cipher_deinit (c->cipher);
  memset (c, 0, sizeof *c);
  g_free (c);
}

/* ====================================================================== */
/* The signature                                                           */
/* ====================================================================== */

struct _W42PdfSigner {
  gnutls_x509_crt_t *chain;
  unsigned int       chain_len;
  gnutls_privkey_t   key;
  char              *name;
  gsize              reserve;
};

static void
signer_error (GError **error, const char *path, const char *what)
{
  char *base = g_path_get_basename (path);

  /* Translators: the first %s is a certificate file's name, the second
   * what is wrong with it. */
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("%s: %s"), base, what);
  g_free (base);
}

W42PdfSigner *
w42_pdf_signer_new (const char *path, const char *password, GError **error)
{
  W42PdfSigner *s;
  gnutls_pkcs12_t p12 = NULL;
  gnutls_x509_privkey_t key = NULL;
  gnutls_datum_t datum;
  char *contents = NULL;
  gsize length = 0;
  int r;
  time_t now = time (NULL);

  g_return_val_if_fail (path != NULL, NULL);

  if (!g_file_get_contents (path, &contents, &length, error))
    return NULL;
  datum.data = (unsigned char *) contents;
  datum.size = (unsigned int) length;

  gnutls_pkcs12_init (&p12);
  r = gnutls_pkcs12_import (p12, &datum, GNUTLS_X509_FMT_DER, 0);
  if (r < 0)
    r = gnutls_pkcs12_import (p12, &datum, GNUTLS_X509_FMT_PEM, 0);
  g_free (contents);
  if (r < 0)
    {
      signer_error (error, path, _("this is not a certificate file Word42 can read. "
                                   "It wants a PKCS #12 file (.p12 or .pfx) "
                                   "with the certificate and its private key."));
      gnutls_pkcs12_deinit (p12);
      return NULL;
    }
  if (password == NULL)
    password = "";
  if (gnutls_pkcs12_verify_mac (p12, password) == GNUTLS_E_MAC_VERIFY_FAILED)
    {
      signer_error (error, path, _("the certificate's password is not correct."));
      gnutls_pkcs12_deinit (p12);
      return NULL;
    }

  s = g_new0 (W42PdfSigner, 1);
  r = gnutls_pkcs12_simple_parse (p12, password, &key, &s->chain, &s->chain_len,
                                  NULL, NULL, NULL, 0);
  gnutls_pkcs12_deinit (p12);
  if (r < 0 || key == NULL || s->chain_len == 0)
    {
      signer_error (error, path,
                    r == GNUTLS_E_DECRYPTION_FAILED || r == GNUTLS_E_MAC_VERIFY_FAILED
                    ? _("the certificate's password is not correct.")
                    : _("the file has no certificate with a private key to sign with."));
      if (key != NULL)
        gnutls_x509_privkey_deinit (key);
      w42_pdf_signer_free (s);
      return NULL;
    }
  gnutls_privkey_init (&s->key);
  if (gnutls_privkey_import_x509 (s->key, key, GNUTLS_PRIVKEY_IMPORT_AUTO_RELEASE) < 0)
    {
      gnutls_x509_privkey_deinit (key);
      signer_error (error, path, _("the private key cannot be used to sign."));
      w42_pdf_signer_free (s);
      return NULL;
    }

  /* A signature made with a certificate that has lapsed, or has not
   * begun, is one no reader will accept. */
  if (gnutls_x509_crt_get_expiration_time (s->chain[0]) < now)
    {
      signer_error (error, path, _("the certificate has expired."));
      w42_pdf_signer_free (s);
      return NULL;
    }
  if (gnutls_x509_crt_get_activation_time (s->chain[0]) > now + 300)
    {
      signer_error (error, path, _("the certificate is not valid yet."));
      w42_pdf_signer_free (s);
      return NULL;
    }

  {
    char buf[512];
    size_t size = sizeof buf;
    gnutls_datum_t dn = { NULL, 0 };

    if (gnutls_x509_crt_get_dn_by_oid (s->chain[0], GNUTLS_OID_X520_COMMON_NAME, 0, 0,
                                       buf, &size) == 0 && g_utf8_validate (buf, -1, NULL))
      s->name = g_strdup (buf);
    else if (gnutls_x509_crt_get_dn3 (s->chain[0], &dn, 0) == 0)
      {
        s->name = g_strndup ((const char *) dn.data, dn.size);
        gnutls_free (dn.data);
      }
    if (s->name == NULL || !g_utf8_validate (s->name, -1, NULL))
      {
        g_free (s->name);
        s->name = g_strdup ("");
      }
  }

  /* The certificates go in the signature whole, and the signature is as
   * long as the key; what is over is the structure round them. */
  {
    unsigned int bits = 0;

    gnutls_privkey_get_pk_algorithm (s->key, &bits);
    s->reserve = 4096 + 2 * (bits / 8);
    for (unsigned int i = 0; i < s->chain_len; i++)
      {
        gnutls_datum_t der;

        if (gnutls_x509_crt_export2 (s->chain[i], GNUTLS_X509_FMT_DER, &der) == 0)
          {
            s->reserve += der.size + 64;
            gnutls_free (der.data);
          }
      }
  }
  return s;
}

const char *
w42_pdf_signer_name (W42PdfSigner *s)
{
  return s->name;
}

gsize
w42_pdf_signer_reserve (W42PdfSigner *s)
{
  return s->reserve;
}

GBytes *
w42_pdf_signer_sign (W42PdfSigner *s, GBytes *file, gsize hole_start,
                     gsize hole_end, GError **error)
{
  gsize len;
  const guint8 *d = g_bytes_get_data (file, &len);
  GByteArray *signed_bytes;
  gnutls_pkcs7_t p7 = NULL;
  gnutls_pkcs7_attrs_t attrs = NULL;
  gnutls_datum_t data, cert, out = { NULL, 0 };
  guint8 value[40];
  int r;
  GBytes *result = NULL;

  g_return_val_if_fail (hole_start <= hole_end && hole_end <= len, NULL);

  signed_bytes = g_byte_array_sized_new ((guint) (len - (hole_end - hole_start)));
  g_byte_array_append (signed_bytes, d, (guint) hole_start);
  g_byte_array_append (signed_bytes, d + hole_end, (guint) (len - hole_end));
  data.data = signed_bytes->data;
  data.size = signed_bytes->len;

  /* ESS signing-certificate-v2 (RFC 5035), which PAdES requires: the
   * SHA-256 of the certificate, so that the signature cannot be passed
   * off as another certificate's with the same key.
   *   SEQUENCE { SEQUENCE { SEQUENCE { OCTET STRING hash } } } */
  if (gnutls_x509_crt_export2 (s->chain[0], GNUTLS_X509_FMT_DER, &cert) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Word42 could not sign the PDF."));
      g_byte_array_free (signed_bytes, TRUE);
      return NULL;
    }
  value[0] = 0x30; value[1] = 0x26;
  value[2] = 0x30; value[3] = 0x24;
  value[4] = 0x30; value[5] = 0x22;
  value[6] = 0x04; value[7] = 0x20;
  gnutls_hash_fast (GNUTLS_DIG_SHA256, cert.data, cert.size, value + 8);
  gnutls_free (cert.data);

  gnutls_pkcs7_init (&p7);
  r = gnutls_pkcs7_add_attr (&attrs, "1.2.840.113549.1.9.16.2.47",
                             &(gnutls_datum_t) { value, sizeof value }, 0);
  if (r >= 0)
    r = gnutls_pkcs7_sign (p7, s->chain[0], s->key, &data, attrs, NULL,
                           GNUTLS_DIG_SHA256, GNUTLS_PKCS7_INCLUDE_CERT);
  for (unsigned int i = 1; r >= 0 && i < s->chain_len; i++)
    r = gnutls_pkcs7_set_crt (p7, s->chain[i]);
  if (r >= 0)
    r = gnutls_pkcs7_export2 (p7, GNUTLS_X509_FMT_DER, &out);
  if (r >= 0)
    {
      result = g_bytes_new (out.data, out.size);
      gnutls_free (out.data);
    }
  else
    /* Translators: %s is the reason, as the GnuTLS library gives it, in
     * English. */
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Word42 could not sign the PDF: %s"), gnutls_strerror (r));

  gnutls_pkcs7_attrs_deinit (attrs);
  gnutls_pkcs7_deinit (p7);
  g_byte_array_free (signed_bytes, TRUE);
  return result;
}

void
w42_pdf_signer_free (W42PdfSigner *s)
{
  if (s == NULL)
    return;
  for (unsigned int i = 0; i < s->chain_len; i++)
    gnutls_x509_crt_deinit (s->chain[i]);
  gnutls_free (s->chain);
  if (s->key != NULL)
    gnutls_privkey_deinit (s->key);
  g_free (s->name);
  g_free (s);
}

#else  /* !HAVE_GNUTLS */

static void
no_gnutls (GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               /* Translators: GnuTLS is the name of a program library;
                * keep it as it is. */
               _("This build of Word42 cannot put a password on a PDF or sign one. "
                 "It was built without GnuTLS."));
}

W42PdfCrypt *
w42_pdf_crypt_new (const char *open_password, const char *modify_password,
                   GError **error)
{
  (void) open_password; (void) modify_password;
  no_gnutls (error);
  return NULL;
}

W42PdfObj *
w42_pdf_crypt_dict (W42PdfCrypt *crypt)
{
  (void) crypt;
  return w42_pdf_dict ();
}

GBytes *
w42_pdf_crypt_encrypt (gpointer crypt, const guint8 *data, gsize len)
{
  (void) crypt;
  return g_bytes_new (data, len);
}

void
w42_pdf_crypt_free (W42PdfCrypt *crypt)
{
  (void) crypt;
}

W42PdfSigner *
w42_pdf_signer_new (const char *path, const char *password, GError **error)
{
  (void) path; (void) password;
  no_gnutls (error);
  return NULL;
}

const char *
w42_pdf_signer_name (W42PdfSigner *signer)
{
  (void) signer;
  return "";
}

gsize
w42_pdf_signer_reserve (W42PdfSigner *signer)
{
  (void) signer;
  return 0;
}

GBytes *
w42_pdf_signer_sign (W42PdfSigner *signer, GBytes *file, gsize hole_start,
                     gsize hole_end, GError **error)
{
  (void) signer; (void) file; (void) hole_start; (void) hole_end;
  no_gnutls (error);
  return NULL;
}

void
w42_pdf_signer_free (W42PdfSigner *signer)
{
  (void) signer;
}

#endif

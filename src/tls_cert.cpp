// A self-signed certificate for the phone-calibration page (browsers only allow camera access over HTTPS).
// The key is generated on first boot, on the device, and never leaves it. Each relay has its own.
#include <Preferences.h>

#include "app.h"
#include "tls_cert.h"

#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_X509_CRT_WRITE_C)   // certificate writing is compiled into this mbedTLS build
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>

static bool generate(String& certPem, String& keyPem) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_mpi serial;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_mpi_init(&serial);
  static char certBuf[2048], keyBuf[1024];
  bool ok = false;
  const char* pers = "fwb-cert";
  do {
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers)) != 0) break;
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg) != 0) break;

    const char* name = "CN=fancy2wled.local,O=Fancy2WLED";
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, name) != 0) break;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, name) != 0) break;
    if (mbedtls_mpi_lset(&serial, (mbedtls_mpi_sint)(esp_random() & 0x7FFFFFFF) + 1) != 0) break;
    if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) break;
    // The device has no clock yet, so use a fixed, generous validity window.
    if (mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959") != 0) break;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) break;
    if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) break;
    // subjectAltName: DNS:fancy2wled.local
    static const char kDns[] = "fancy2wled.local";
    unsigned char san[2 + 2 + sizeof(kDns)];
    size_t nl = sizeof(kDns) - 1;
    san[0] = 0x30; san[1] = (unsigned char)(2 + nl);
    san[2] = 0x82; san[3] = (unsigned char)nl;
    memcpy(san + 4, kDns, nl);
    if (mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, 4 + nl) != 0) break;

    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)certBuf, sizeof certBuf, mbedtls_ctr_drbg_random, &drbg) != 0) break;
    if (mbedtls_pk_write_key_pem(&key, (unsigned char*)keyBuf, sizeof keyBuf) != 0) break;
    certPem = certBuf;
    keyPem = keyBuf;
    ok = true;
  } while (false);
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}
#else
static bool generate(String&, String&) { return false; }
#endif

bool tlsGetCert(String& certPem, String& keyPem) {
  Preferences p;
  if (!p.begin("fwb_tls", false)) return false;
  certPem = p.getString("crt", "");
  keyPem = p.getString("key", "");
  if (p.getInt("ver", 0) == 2 && certPem.length() > 200 && keyPem.length() > 100) { p.end(); return true; }
  Serial.println("Generating this relay's HTTPS certificate (first boot only)...");
  uint32_t t0 = millis();
  bool ok = generate(certPem, keyPem);
  if (ok) {
    p.putString("crt", certPem);
    p.putString("key", keyPem);
    p.putInt("ver", 2);
    Serial.printf("Certificate ready (%u ms)\n", (unsigned)(millis() - t0));
  } else {
    Serial.println("Certificate generation failed; the phone calibration page will not be available over HTTPS.");
  }
  p.end();
  return ok;
}


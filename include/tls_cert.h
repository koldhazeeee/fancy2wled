#pragma once
#include <Arduino.h>

// Returns the device's self-signed certificate + private key in PEM, creating them on first use.
bool tlsGetCert(String& certPem, String& keyPem);


/**
  Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
  Use of this source code is governed by a BSD-style license that can be
  found in the LICENSE file.

  Implementation of RSA signature verification which uses a pre-processed key
  for computation.
**/
#include "Rsa2048Sha256.h"

#define SHA256_DIGEST_SIZE 32

UINT64
Mula32 (
  UINT32 A,
  UINT32 B,
  UINT32 C
  )
{
  UINT64 Ret = 0;

  Ret  = A;
  Ret *= B;
  Ret += C;
  return Ret;
}

UINT64
Mulaa32 (
  UINT32 A,
  UINT32 B,
  UINT32 C,
  UINT32 D
  )
{
  UINT64 Ret = 0;

  Ret  = A;
  Ret *= B;
  Ret += C;
  Ret += D;
  return Ret;
}

/**
  A[] -= Mod
**/
static
void
SubMod (
  RsaPublicKey  *Key,
  UINT32        *A
  )
{
  INT64  B     = 0;
  UINT32 Index = 0;
  for (Index = 0; Index < RSANUMWORDS; ++Index) {
    B += (UINT64) A[Index] - Key->N[Index];
    A[Index] = (UINT32) B;
    B >>= 32;
  }
}

//
// Return A[] >= Mod
//
static
int
GeMod (
  RsaPublicKey  *Key,
  const UINT32  *A
  )
{
  UINT32 Index = 0;

  for (Index = RSANUMWORDS; Index;) {
    --Index;
    if (A[Index] < Key->N[Index])
      return 0;
    if (A[Index] > Key->N[Index])
      return 1;
  }
  return 1;
}

//
// Montgomery c[] += a * b[] / R % mod
//
static
void
MontMulAdd (
  RsaPublicKey  *Key,
  UINT32        *C,
  UINT32        Aa,
  UINT32        *Bb
  )
{
  UINT64 A = 0;
  UINT32 D0 = 0;
  UINT64 B = 0;
  UINT32 Index = 0;

  A = Mula32 (Aa, Bb[0], C[0]);
  D0 = (UINT32) A * Key->N0Inv;
  B = Mula32 (D0, Key->N[0], (UINT32) A);

  for (Index = 1; Index < RSANUMWORDS; ++Index) {
    A = Mulaa32 (Aa, Bb[Index], C[Index], (UINT32) (A >> 32));
    B = Mulaa32 (D0, Key->N[Index], (UINT32) A, (UINT32) (B >> 32));
    C[Index - 1] = (UINT32) B;
  }

  A = (A >> 32) + (B >> 32);
  C[Index - 1] = (UINT32) A;

  if (A >> 32) {
    SubMod (Key, C);
  }
}

//
// Montgomery c[] = a[] * b[] / R % mod
//
static
void
MontMul (
  RsaPublicKey  *Key,
  UINT32        *C,
  UINT32        *A,
  UINT32        *B
  )
{
  UINT32 Index = 0;

  ZeroMem (C, RSANUMBYTES);
  for (Index = 0; Index < RSANUMWORDS; ++Index)
    MontMulAdd (Key, C, A[Index], B);
}

/**
  In-place public exponentiation.
  Exponent depends on the configuration (65537 (default), or 3).

  @param Key        Key to use in signing
  @param InOut      Input and output big-endian byte array
  @param Workbuf32  Work buffer; caller must verify this is
                    3 x RSANUMWORDS elements long.
 **/
static
void
ModPow (
  RsaPublicKey  *Key,
  UINT8         *InOut,
  UINT32        *Workbuf32
  )
{
  UINT32 *A     = NULL;
  UINT32 *Ar    = NULL;
  UINT32 *Aar   = NULL;
  UINT32 *Aaa   = NULL;
  INT32  Index  = 0;
  UINT32 Tmp    = 0;

  A = Workbuf32;
  Ar = A + RSANUMWORDS;
  Aar = Ar + RSANUMWORDS;

  //
  // Re-use location
  //
  Aaa = Aar;

  //
  //Convert from big endian byte array to little endian word array
  //
  for (Index = 0; Index < (INT32) RSANUMWORDS; ++Index) {
    Tmp =
      (InOut[((RSANUMWORDS - 1 - Index) * 4) + 0] << 24) |
      (InOut[((RSANUMWORDS - 1 - Index) * 4) + 1] << 16) |
      (InOut[((RSANUMWORDS - 1 - Index) * 4) + 2] << 8) |
      (InOut[((RSANUMWORDS - 1 - Index) * 4) + 3] << 0);
    A[Index] = Tmp;
  }

  MontMul (Key, Ar, A, Key->Rr);
  //
  // Exponent 65537
  //
  for (Index = 0; Index < 16; Index += 2) {
    MontMul (Key, Aar, Ar, Ar);
    MontMul (Key, Ar, Aar, Aar);
  }
  MontMul (Key, Aaa, Ar, A);

  if (GeMod (Key, Aaa)){
    SubMod (Key, Aaa);
  }

  //
  // Convert to bigendian byte array
  //
  for (Index = (INT32) RSANUMWORDS - 1; Index >= 0; --Index) {
    Tmp = Aaa[Index];

    *InOut++ = (UINT8) (Tmp >> 24);
    *InOut++ = (UINT8) (Tmp >> 16);
    *InOut++ = (UINT8) (Tmp >>  8);
    *InOut++ = (UINT8) (Tmp >>  0);
  }
}

/**
  PKCS#1 padding (from the RSA PKCS#1 v2.1 standard)

  The DER-encoded padding is defined as follows :
  0x00 || 0x01 || PS || 0x00 || T

  T: DER Encoded DigestInfo value which depends on the hash function used,
  for SHA-256:
  (0x)30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 || H.

  Length(T) = 51 octets for SHA-256

  PS: octet string consisting of {Length(RSA Key) - Length(T) - 3} 0xFF
 **/
static  UINT8 Sha256Tail[] = {
  0x00, 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
  0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
  0x05, 0x00, 0x04, 0x20
};

#define PKCS_PAD_SIZE (RSANUMBYTES - SHA256_DIGEST_SIZE)

/**
 * Check PKCS#1 padding bytes
 *
 * @param sig  Signature to verify
 * @return 0 if the padding is correct.
 */
static
int
CheckPadding (
  UINT8  *Sig
  )
{
  UINT8   *Ptr   = NULL;
  int     Result = 0;
  UINT32  Index  = 0;

  Ptr = Sig;
  //
  // First 2 bytes are always 0x00 0x01
  //
  Result |= *Ptr++ ^ 0x00;
  Result |= *Ptr++ ^ 0x01;
  //
  // Then 0xff bytes until the tail
  //
  for (Index = 0; Index < PKCS_PAD_SIZE - sizeof (Sha256Tail) - 2; Index++)
    Result |= *Ptr++ ^ 0xff;
  //
  // Check the tail
  //
  Result |= CompareMem (Ptr, Sha256Tail, sizeof (Sha256Tail));
  return Result != 0;
}

/**
  Verify a SHA256WithRSA PKCS#1 v1.5 signature against an expected
  SHA256 hash.

  @param Key  RSA public key
  @param Signature   RSA signature
  @param Sha  SHA-256 digest of the content to verify
  @param Workbuf32   Work buffer; caller must verify this is
        3 x RSANUMWORDS elements long.
  @return 0 on failure, 1 on success.
 **/
int
RsaVerify (
  RsaPublicKey  *Key,
  UINT8         *Signature,
  UINT8         *Sha,
  UINT32        *Workbuf32
  )
{
  UINT8 Buf[RSANUMBYTES];

  //
  // Copy input to local workspace
  //
  CopyMem (Buf, Signature, RSANUMBYTES);

  //
  // In-place exponentiation
  //
  ModPow (Key, Buf, Workbuf32);

  //
  // Check the PKCS#1 padding
  //
  if (CheckPadding (Buf) != 0) {
    return 0;
  }

  //
  // Check the digest
  //
  if (CompareMem (Buf + PKCS_PAD_SIZE, Sha, SHA256_DIGEST_SIZE) != 0) {
    return 0;
  }

  //
  // All checked out OK
  //
  return 1;
}

/*
 *  This file is part of the Off-the-Record Next Generation Messaging
 *  library (libotr-ng).
 *
 *  Copyright (C) 2016-2018, the libotr-ng contributors.
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef S_SPLINT_S
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#include <libotr/b64.h>
#pragma clang diagnostic pop
#endif

#include <string.h>

#define OTRNG_DESERIALIZE_PRIVATE

#include "alloc.h"
#include "deserialize.h"
#include "mpi.h"

INTERNAL otrng_result otrng_deserialize_uint64(uint64_t *n,
                                               const uint8_t *buffer,
                                               size_t buff_len, size_t *nread) {
  if (buff_len < sizeof(uint64_t)) {
    return OTRNG_ERROR;
  }

  *n = ((uint64_t)buffer[7]) | ((uint64_t)buffer[6]) << 8 |
       ((uint64_t)buffer[5]) << 16 | ((uint64_t)buffer[4]) << 24 |
       ((uint64_t)buffer[3]) << 32 | ((uint64_t)buffer[2]) << 40 |
       ((uint64_t)buffer[1]) << 48 | ((uint64_t)buffer[0]) << 56;

  if (nread) {
    *nread = sizeof(uint64_t);
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_uint32(uint32_t *n,
                                               const uint8_t *buffer,
                                               size_t buff_len, size_t *nread) {
  if (buff_len < sizeof(uint32_t)) {
    return OTRNG_ERROR;
  }

  *n = (uint32_t)buffer[3] | ((uint32_t)buffer[2]) << 8 |
       ((uint32_t)buffer[1]) << 16 | ((uint32_t)buffer[0]) << 24;

  if (nread) {
    *nread = sizeof(uint32_t);
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_uint16(uint16_t *n,
                                               const uint8_t *buffer,
                                               size_t buff_len, size_t *nread) {
  if (buff_len < sizeof(uint16_t)) {
    return OTRNG_ERROR;
  }

  *n = buffer[1] | buffer[0] << 8;

  if (nread != NULL) {
    *nread = sizeof(uint16_t);
  }
  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_uint8(uint8_t *n, const uint8_t *buffer,
                                              size_t buff_len, size_t *nread) {
  if (buff_len < sizeof(uint8_t)) {
    return OTRNG_ERROR;
  }

  *n = buffer[0];

  if (nread != NULL) {
    *nread = sizeof(uint8_t);
  }
  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_data(uint8_t **dst, size_t *dst_len,
                                             const uint8_t *buffer,
                                             size_t buff_len, size_t *read) {
  size_t r = 0;
  uint32_t s = 0;
  uint8_t *t;

  /* 4 bytes len */
  if (!otrng_deserialize_uint32(&s, buffer, buff_len, &r)) {
    if (read != NULL) {
      *read = r;
    }

    return OTRNG_ERROR;
  }

  if (read) {
    *read = r;
  }

  if (!s) {
    return OTRNG_SUCCESS;
  }

  buff_len -= r;
  if (buff_len < s) {
    return OTRNG_ERROR;
  }

  t = otrng_xmalloc_z(s);

  memcpy(t, buffer + r, s);

  *dst = t;
  if (read) {
    *read += s;
  }

  if (dst_len) {
    *dst_len = s;
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_bytes_array(uint8_t *dst,
                                                    size_t dst_len,
                                                    const uint8_t *buffer,
                                                    size_t buff_len) {
  if (buff_len < dst_len) {
    return OTRNG_ERROR;
  }

  memcpy(dst, buffer, dst_len);
  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_dh_mpi_otr(dh_mpi *dst,
                                                   const uint8_t *buffer,
                                                   size_t buff_len,
                                                   size_t *read) {
  otrng_mpi_s mpi; // no need to free, because nothing is copied now
  size_t w = 0;
  otrng_result ret;

  if (!otrng_mpi_deserialize_no_copy(&mpi, buffer, buff_len, NULL)) {
    return OTRNG_ERROR;
  }

  ret = otrng_dh_mpi_deserialize(dst, mpi.data, mpi.len, &w);

  if (read) {
    *read = w + 4;
  }

  return ret;
}

INTERNAL otrng_result otrng_deserialize_ec_point(ec_point point,
                                                 const uint8_t *ser,
                                                 size_t ser_len) {
  if (ser_len < ED448_POINT_BYTES) {
    return OTRNG_ERROR;
  }

  return otrng_ec_point_decode(point, ser);
}

INTERNAL otrng_result otrng_deserialize_public_key(otrng_public_key pub,
                                                   const uint8_t *ser,
                                                   size_t ser_len,
                                                   size_t *read) {
  const uint8_t *cursor = ser;
  size_t r = 0;
  uint16_t pubkey_type = 0;

  if (ser_len < ED448_PUBKEY_BYTES) {
    return OTRNG_ERROR;
  }

  if (otrng_deserialize_uint16(&pubkey_type, cursor, ser_len, &r) == 0) {
    return OTRNG_ERROR;
  }

  cursor += r;
  ser_len -= r;

  if (ED448_PUBKEY_TYPE != pubkey_type) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_ec_point(pub, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  if (read) {
    *read = ED448_PUBKEY_BYTES;
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_forging_key(otrng_public_key pub,
                                                    const uint8_t *ser,
                                                    size_t ser_len,
                                                    size_t *read) {
  const uint8_t *cursor = ser;
  size_t r = 0;
  uint16_t pubkey_type = 0;

  if (ser_len < ED448_PUBKEY_BYTES) {
    return OTRNG_ERROR;
  }

  if (otrng_deserialize_uint16(&pubkey_type, cursor, ser_len, &r) == 0) {
    return OTRNG_ERROR;
  }

  cursor += r;
  ser_len -= r;

  if (ED448_FORGINGKEY_TYPE != pubkey_type) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_ec_point(pub, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  if (read) {
    *read = ED448_PUBKEY_BYTES;
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_shared_prekey(
    otrng_shared_prekey_pub shared_prekey, const uint8_t *ser, size_t ser_len,
    size_t *read) {
  const uint8_t *cursor = ser;
  size_t r = 0;
  uint16_t shared_prekey_type = 0;

  if (ser_len < ED448_PUBKEY_BYTES) {
    return OTRNG_ERROR;
  }

  if (otrng_deserialize_uint16(&shared_prekey_type, cursor, ser_len, &r) == 0) {
    return OTRNG_ERROR;
  }

  cursor += r;
  ser_len -= r;

  if (ED448_SHARED_PREKEY_TYPE != shared_prekey_type) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_ec_point(shared_prekey, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  if (read) {
    *read = ED448_SHARED_PREKEY_BYTES;
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_ec_scalar(ec_scalar scalar,
                                                  const uint8_t *ser,
                                                  size_t ser_len) {
  if (ser_len < ED448_SCALAR_BYTES) {
    return OTRNG_ERROR;
  }

  otrng_ec_scalar_decode(scalar, ser);

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_deserialize_ring_sig(ring_sig_s *proof,
                                                 const uint8_t *ser,
                                                 size_t ser_len, size_t *read) {
  const uint8_t *cursor = ser;

  if (ser_len < RING_SIG_BYTES) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_ec_scalar(proof->c1, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  cursor += ED448_SCALAR_BYTES;
  ser_len -= ED448_SCALAR_BYTES;

  if (!otrng_deserialize_ec_scalar(proof->r1, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  cursor += ED448_SCALAR_BYTES;
  ser_len -= ED448_SCALAR_BYTES;

  if (!otrng_deserialize_ec_scalar(proof->c2, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  cursor += ED448_SCALAR_BYTES;
  ser_len -= ED448_SCALAR_BYTES;

  if (!otrng_deserialize_ec_scalar(proof->r2, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  cursor += ED448_SCALAR_BYTES;
  ser_len -= ED448_SCALAR_BYTES;

  if (!otrng_deserialize_ec_scalar(proof->c3, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  cursor += ED448_SCALAR_BYTES;
  ser_len -= ED448_SCALAR_BYTES;

  if (!otrng_deserialize_ec_scalar(proof->r3, cursor, ser_len)) {
    return OTRNG_ERROR;
  }

  if (read) {
    *read = RING_SIG_BYTES;
  }

  return OTRNG_SUCCESS;
}

INTERNAL otrng_result otrng_symmetric_key_deserialize(otrng_keypair_s *pair,
                                                      const char *buffer,
                                                      size_t buff_len) {
  /* (((base64len+3) / 4) * 3) */
  uint8_t *dec = otrng_secure_alloc(((buff_len + 3) / 4) * 3);
  size_t written;

  written = otrl_base64_decode(dec, buffer, buff_len);

  if (written == ED448_PRIVATE_BYTES) {
    if (!otrng_keypair_generate(pair, dec)) {
      otrng_secure_free(dec);
      return OTRNG_ERROR;
    }

    otrng_secure_free(dec);
    return OTRNG_SUCCESS;
  }

  otrng_secure_free(dec);
  return OTRNG_ERROR;
}

INTERNAL otrng_result otrng_symmetric_shared_prekey_deserialize(
    otrng_shared_prekey_pair_s *pair, const char *buffer, size_t buff_len) {
  /* (((base64len+3) / 4) * 3) */
  uint8_t *dec = otrng_secure_alloc(((buff_len + 3) / 4) * 3);
  size_t written;

  written = otrl_base64_decode(dec, buffer, buff_len);

  if (written == ED448_PRIVATE_BYTES) {
    if (!otrng_shared_prekey_pair_generate(pair, dec)) {
      otrng_secure_free(dec);
      return OTRNG_ERROR;
    }

    otrng_secure_free(dec);
    return OTRNG_SUCCESS;
  }

  otrng_secure_free(dec);

  return OTRNG_ERROR;
}

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

#include <assert.h>

#include "prekey_client.h"

#include "alloc.h"
#include "base64.h"
#include "client.h"
#include "dake.h"
#include "deserialize.h"
#include "prekey_proofs.h"
#include "serialize.h"
#include "shake.h"

#ifndef S_SPLINT_S
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#include <libotr/mem.h>
#pragma clang diagnostic pop
#endif

#define OTRNG_PREKEY_CLIENT_MALFORMED_MSG 1
#define OTRNG_PREKEY_CLIENT_INVALID_DAKE2 2
#define OTRNG_PREKEY_CLIENT_INVALID_STORAGE_STATUS 3
#define OTRNG_PREKEY_CLIENT_INVALID_SUCCESS 4
#define OTRNG_PREKEY_CLIENT_INVALID_FAILURE 5

// TODO: this whole file needs refactoring

static void notify_error_callback(otrng_client_s *client, int error) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->notify_error(client, error,
                                         prekey_client->callbacks->ctx);
}

static void prekey_storage_status_received_callback(
    otrng_client_s *client, const otrng_prekey_storage_status_message_s *msg) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->storage_status_received(
      client, msg, prekey_client->callbacks->ctx);
}

static void success_received_callback(otrng_client_s *client) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->success_received(client,
                                             prekey_client->callbacks->ctx);
}

static void failure_received_callback(otrng_client_s *client) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->failure_received(client,
                                             prekey_client->callbacks->ctx);
}

static void no_prekey_in_storage_received_callback(otrng_client_s *client) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->no_prekey_in_storage_received(
      client, prekey_client->callbacks->ctx);
}

static void low_prekey_messages_in_storage_callback(otrng_client_s *client) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->low_prekey_messages_in_storage(
      client, prekey_client->server_identity, prekey_client->callbacks->ctx);
}

static void
prekey_ensembles_received_callback(otrng_client_s *client,
                                   prekey_ensemble_s *const *const ensembles,
                                   uint8_t num_ensembles) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  prekey_client->callbacks->prekey_ensembles_received(
      client, ensembles, num_ensembles, prekey_client->callbacks->ctx);
}

static int build_prekey_publication_message_callback(
    otrng_prekey_publication_message_s *pub_msg, otrng_client_s *client) {
  const otrng_prekey_client_s *prekey_client = client->prekey_client;
  return prekey_client->callbacks->build_prekey_publication_message(
      client, pub_msg, prekey_client->publication_policy,
      prekey_client->callbacks->ctx);
}

API otrng_prekey_client_s *otrng_prekey_client_new() {
  otrng_prekey_client_s *client =
      otrng_secure_alloc(sizeof(otrng_prekey_client_s));

  client->publication_policy =
      otrng_xmalloc_z(sizeof(otrng_prekey_publication_policy_s));

  client->ephemeral_ecdh = otrng_secure_alloc(sizeof(ecdh_keypair_s));

  return client;
}

API void otrng_prekey_client_init(otrng_prekey_client_s *client,
                                  const char *server, const char *our_identity,
                                  uint32_t instance_tag,
                                  const otrng_keypair_s *keypair,
                                  const otrng_client_profile_s *client_profile,
                                  const otrng_prekey_profile_s *prekey_profile,
                                  unsigned int max_published_prekey_message,
                                  unsigned int minimum_stored_prekey_message) {
  if (!client) {
    return;
  }

  if (!server) {
    return;
  }

  if (!our_identity) {
    return;
  }

  if (!instance_tag) {
    return;
  }

  if (!client_profile) {
    return;
  }

  client->instance_tag = instance_tag;
  client->client_profile = client_profile;

  // TODO: Can be null if you dont want to publish it
  client->server_identity = otrng_xstrdup(server);
  client->our_identity = otrng_xstrdup(our_identity);
  client->prekey_profile = prekey_profile;
  client->keypair = keypair;

  otrng_ecdh_keypair_destroy(client->ephemeral_ecdh);
  otrng_secure_free(client->ephemeral_ecdh);
  client->ephemeral_ecdh = otrng_secure_alloc(sizeof(ecdh_keypair_s));
  client->publication_policy->max_published_prekey_message =
      max_published_prekey_message;
  client->publication_policy->minimum_stored_prekey_message =
      minimum_stored_prekey_message;
}

API void otrng_prekey_client_free(otrng_prekey_client_s *client) {
  if (!client) {
    return;
  }

  otrng_ecdh_keypair_destroy(client->ephemeral_ecdh);
  otrng_secure_free(client->ephemeral_ecdh);
  otrng_free(client->server_identity);
  otrng_free(client->our_identity);
  otrng_free(client->publication_policy);

  otrng_secure_free(client);
}

static otrng_result prekey_decode(const char *msg, uint8_t **buffer,
                                  size_t *buff_len) {
  size_t len = strlen(msg);

  if (!len || '.' != msg[len - 1]) {
    return OTRNG_ERROR;
  }

  /* (((base64len+3) / 4) * 3) */
  *buffer = otrng_xmalloc_z(((len - 1 + 3) / 4) * 3);

  *buff_len = otrl_base64_decode(*buffer, msg, len - 1);

  return OTRNG_SUCCESS;
}

static char *prekey_encode(const uint8_t *buffer, size_t buff_len) {
  char *ret = otrng_xmalloc_z(OTRNG_BASE64_ENCODE_LEN(buff_len) + 2);
  size_t l;

  l = otrl_base64_encode(ret, buffer, buff_len);
  ret[l] = '.';
  ret[l + 1] = 0;

  return ret;
}

static otrng_result parse_header(uint8_t *msg_type, const uint8_t *buf,
                                 size_t buflen, size_t *read) {
  size_t r = 0; /* read */
  size_t w = 0; /* walked */

  uint16_t protocol_version = 0;

  if (!otrng_deserialize_uint16(&protocol_version, buf, buflen, &r)) {
    return OTRNG_ERROR;
  }

  w += r;

  if (protocol_version != OTRNG_PROTOCOL_VERSION_4) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint8(msg_type, buf + w, buflen - w, &r)) {
    return OTRNG_ERROR;
  }

  w += r;

  if (read) {
    *read = w;
  }

  return OTRNG_SUCCESS;
}

tstatic otrng_result otrng_prekey_dake1_message_serialize(
    uint8_t **ser, size_t *ser_len, const otrng_prekey_dake1_message_s *msg) {

  uint8_t *client_profile_buffer = NULL;
  size_t client_profile_buff_len = 0;
  size_t ret_len;
  uint8_t *ret;
  size_t w = 0;

  if (!otrng_client_profile_serialize(&client_profile_buffer,
                                      &client_profile_buff_len,
                                      msg->client_profile)) {
    return OTRNG_ERROR;
  }

  ret_len = 2 + 1 + 4 + client_profile_buff_len + ED448_POINT_BYTES;
  ret = otrng_xmalloc_z(ret_len);

  w += otrng_serialize_uint16(ret + w, OTRNG_PROTOCOL_VERSION_4);
  w += otrng_serialize_uint8(ret + w, OTRNG_PREKEY_DAKE1_MSG);
  w += otrng_serialize_uint32(ret + w, msg->client_instance_tag);
  w += otrng_serialize_bytes_array(ret + w, client_profile_buffer,
                                   client_profile_buff_len);
  w += otrng_serialize_ec_point(ret + w, msg->I);
  otrng_free(client_profile_buffer);

  *ser = ret;
  if (ser_len) {
    *ser_len = w;
  }

  return OTRNG_SUCCESS;
}

tstatic void
otrng_prekey_dake1_message_destroy(otrng_prekey_dake1_message_s *msg) {
  if (!msg) {
    return;
  }

  otrng_client_profile_destroy(msg->client_profile);
  otrng_free(msg->client_profile);
  msg->client_profile = NULL;
  otrng_ec_point_destroy(msg->I);
}

tstatic otrng_result otrng_prekey_dake2_message_deserialize(
    otrng_prekey_dake2_message_s *dst, const uint8_t *ser, size_t ser_len) {

  size_t w = 0;
  size_t read = 0;
  uint8_t msg_type = 0;
  const uint8_t *composite_identity_start;

  if (!parse_header(&msg_type, ser, ser_len, &w)) {
    return OTRNG_ERROR;
  }

  if (msg_type != OTRNG_PREKEY_DAKE2_MSG) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint32(&dst->client_instance_tag, ser + w, ser_len - w,
                                &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  composite_identity_start = ser + w;
  if (!otrng_deserialize_data(&dst->server_identity, &dst->server_identity_len,
                              ser + w, ser_len - w, &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  if (!otrng_deserialize_public_key(dst->server_pub_key, ser + w, ser_len - w,
                                    &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  /* Store the composite identity, so we can use it to generate `t` */
  dst->composite_identity_len = ser + w - composite_identity_start;
  dst->composite_identity = otrng_xmalloc(dst->composite_identity_len);
  memcpy(dst->composite_identity, composite_identity_start,
         dst->composite_identity_len);

  if (!otrng_deserialize_ec_point(dst->S, ser + w, ser_len - w)) {
    return OTRNG_ERROR;
  }

  w += ED448_POINT_BYTES;

  if (!otrng_deserialize_ring_sig(dst->sigma, ser + w, ser_len - w, NULL)) {
    return OTRNG_ERROR;
  }

  return OTRNG_SUCCESS;
}

static char *start_dake_and_then_send(otrng_prekey_client_s *client,
                                      otrng_prekey_next_message next) {
  uint8_t *sym = otrng_secure_alloc(ED448_PRIVATE_BYTES);
  uint8_t *ser = NULL;
  size_t ser_len = 0;
  otrng_result success;
  char *ret;
  otrng_prekey_dake1_message_s msg;

  msg.client_instance_tag = client->instance_tag;

  msg.client_profile = otrng_xmalloc_z(sizeof(otrng_client_profile_s));
  if (!otrng_client_profile_copy(msg.client_profile, client->client_profile)) {
    otrng_secure_free(sym);
    return NULL;
  }

  random_bytes(sym, ED448_PRIVATE_BYTES);

  if (!otrng_ecdh_keypair_generate(client->ephemeral_ecdh, sym)) {
    otrng_secure_free(sym);
    otrng_prekey_dake1_message_destroy(&msg);
    return NULL;
  }

  otrng_secure_free(sym);

  otrng_ec_point_copy(msg.I, client->ephemeral_ecdh->pub);

  success = otrng_prekey_dake1_message_serialize(&ser, &ser_len, &msg);
  otrng_prekey_dake1_message_destroy(&msg);

  if (!success) {
    return NULL;
  }

  ret = prekey_encode(ser, ser_len);
  otrng_free(ser);

  client->after_dake = next;

  return ret;
}

API char *
otrng_prekey_client_request_storage_information(otrng_prekey_client_s *client) {
  return start_dake_and_then_send(client,
                                  OTRNG_PREKEY_STORAGE_INFORMATION_REQUEST);
}

API char *otrng_prekey_client_publish(otrng_prekey_client_s *client) {
  return start_dake_and_then_send(client, OTRNG_PREKEY_PREKEY_PUBLICATION);
}

static otrng_result otrng_prekey_ensemble_query_retrieval_message_serialize(
    uint8_t **dst, size_t *len,
    const otrng_prekey_ensemble_query_retrieval_message_s *msg) {
  size_t w = 0;

  if (!len || !dst) {
    return OTRNG_ERROR;
  }

  *len = 2 + 1 + 4 + (4 + strlen(msg->identity)) +
         (4 + otrng_strlen_ns(msg->versions));
  *dst = otrng_xmalloc(*len);

  w += otrng_serialize_uint16(*dst, OTRNG_PROTOCOL_VERSION_4);
  w += otrng_serialize_uint8(*dst + w,
                             OTRNG_PREKEY_ENSEMBLE_QUERY_RETRIEVAL_MSG);
  w += otrng_serialize_uint32(*dst + w, msg->instance_tag);
  w += otrng_serialize_data(*dst + w, (uint8_t *)msg->identity,
                            strlen(msg->identity));
  if (otrng_serialize_data(*dst + w, (uint8_t *)msg->versions,
                           otrng_strlen_ns(msg->versions)) == 0) {
    otrng_free(*dst);
    return OTRNG_ERROR;
  }

  return OTRNG_SUCCESS;
}

static void otrng_prekey_ensemble_query_retrieval_message_destroy(
    otrng_prekey_ensemble_query_retrieval_message_s *msg) {
  if (!msg) {
    return;
  }

  otrng_free(msg->identity);
  msg->identity = NULL;

  otrng_free(msg->versions);
  msg->versions = NULL;
}

API char *otrng_prekey_client_retrieve_prekeys(const char *identity,
                                               const char *versions,
                                               otrng_prekey_client_s *client) {
  uint8_t *ser = NULL;
  size_t ser_len = 0;
  otrng_result success;
  char *ret;

  otrng_prekey_ensemble_query_retrieval_message_s msg[1];

  msg->identity = otrng_xstrdup(identity);
  msg->versions = versions ? otrng_xstrdup(versions) : NULL;
  msg->instance_tag = client->instance_tag;

  success = otrng_prekey_ensemble_query_retrieval_message_serialize(
      &ser, &ser_len, msg);

  otrng_prekey_ensemble_query_retrieval_message_destroy(msg);

  if (!success) {
    return NULL;
  }

  ret = prekey_encode(ser, ser_len);
  otrng_free(ser);
  return ret;
}

API void otrng_prekey_client_set_client_profile_publication(
    otrng_prekey_client_s *client) {
  client->publication_policy->publish_client_profile = otrng_true;
}

API void otrng_prekey_client_set_prekey_profile_publication(
    otrng_prekey_client_s *client) {
  client->publication_policy->publish_prekey_profile = otrng_true;
}

static uint8_t *otrng_prekey_client_get_expected_composite_phi(
    size_t *len, const otrng_prekey_client_s *client) {
  uint8_t *dst = NULL;
  size_t size, w = 0;

  if (!client->server_identity || !client->our_identity) {
    return NULL;
  }

  size = 4 + strlen(client->server_identity) + 4 + strlen(client->our_identity);
  dst = otrng_xmalloc(size);

  w += otrng_serialize_data(dst + w, (const uint8_t *)client->our_identity,
                            strlen(client->our_identity));
  if (otrng_serialize_data(dst + w, (const uint8_t *)client->server_identity,
                           strlen(client->server_identity)) == 0) {
    otrng_free(dst);
    return NULL;
  }

  if (len) {
    *len = size;
  }

  return dst;
}

static uint8_t usage_auth = 0x11;
static const char *prekey_hash_domain = "OTR-Prekey-Server";

static otrng_result kdf_init_with_usage(goldilocks_shake256_ctx_p hash,
                                        uint8_t usage) {
  if (!hash_init_with_usage_and_domain_separation(hash, usage,
                                                  prekey_hash_domain)) {
    return OTRNG_ERROR;
  }

  return OTRNG_SUCCESS;
}

static otrng_bool
otrng_prekey_dake2_message_valid(const otrng_prekey_dake2_message_s *msg,
                                 const otrng_prekey_client_s *client) {
  // The spec says:
  // "Ensure the identity element of the Prekey Server Composite Identity is
  // correct." We make this check implicitly by verifying the ring signature
  // (which contains this value as part of its "composite identity".

  // TODO: Check if the fingerprint from the key received in this message is
  // what we expect. Through a callback maybe, since the user may need to take
  // action.

  size_t composite_phi_len = 0;
  uint8_t *composite_phi = otrng_prekey_client_get_expected_composite_phi(
      &composite_phi_len, client);
  uint8_t *our_profile = NULL;
  size_t our_profile_len = 0;
  size_t tlen, w;
  uint8_t *t;
  uint8_t usage_initator_client_profile = 0x02;
  uint8_t usage_initiator_prekey_composite_identity = 0x03;
  uint8_t usage_initiator_prekey_composite_phi = 0x04;
  otrng_bool ret;

  if (!composite_phi) {
    return otrng_false;
  }

  if (!otrng_client_profile_serialize(&our_profile, &our_profile_len,
                                      client->client_profile)) {
    otrng_free(composite_phi);
    return otrng_false;
  }

  tlen = 1 + 3 * HASH_BYTES + 2 * ED448_POINT_BYTES;
  t = otrng_xmalloc_z(tlen);

  *t = 0x0;
  w = 1;

  if (!shake_256_prekey_server_kdf(t + w, HASH_BYTES,
                                   usage_initator_client_profile, our_profile,
                                   our_profile_len)) {
    otrng_free(composite_phi);
    otrng_free(t);
    otrng_free(our_profile);
    return otrng_false;
  }

  otrng_free(our_profile);

  w += HASH_BYTES;

  /* Both composite identity AND composite phi have the server's bare JID */
  if (!shake_256_prekey_server_kdf(
          t + w, HASH_BYTES, usage_initiator_prekey_composite_identity,
          msg->composite_identity, msg->composite_identity_len)) {
    otrng_free(composite_phi);
    otrng_free(t);
    return otrng_false;
  }

  w += HASH_BYTES;

  w += otrng_serialize_ec_point(t + w, client->ephemeral_ecdh->pub);
  w += otrng_serialize_ec_point(t + w, msg->S);

  if (!shake_256_prekey_server_kdf(t + w, HASH_BYTES,
                                   usage_initiator_prekey_composite_phi,
                                   composite_phi, composite_phi_len)) {
    otrng_free(composite_phi);
    otrng_free(t);
    return otrng_false;
  }

  otrng_free(composite_phi);

  ret = otrng_rsig_verify_with_usage_and_domain(
      usage_auth, prekey_hash_domain, msg->sigma, client->keypair->pub,
      msg->server_pub_key, client->ephemeral_ecdh->pub, t, tlen);
  otrng_free(t);

  return ret;
}

tstatic otrng_result
otrng_prekey_dake3_message_append_storage_information_request(
    otrng_prekey_dake3_message_s *dake_3, uint8_t mac_key[MAC_KEY_BYTES]) {
  uint8_t msg_type = OTRNG_PREKEY_STORAGE_INFO_REQ_MSG;
  size_t w = 0;
  uint8_t usage_receiver_client_profile = 0x0A;
  goldilocks_shake256_ctx_p hd;

  dake_3->msg = otrng_xmalloc_z(2 + 1 + MAC_KEY_BYTES);
  dake_3->msg_len = OTRNG_DAKE3_MSG_LEN;

  w += otrng_serialize_uint16(dake_3->msg, OTRNG_PROTOCOL_VERSION_4);
  w += otrng_serialize_uint8(dake_3->msg + w, msg_type);

  /* MAC: KDF(usage_storage_info_MAC, prekey_mac_k || msg type, 64) */
  if (!kdf_init_with_usage(hd, usage_receiver_client_profile)) {
    return OTRNG_ERROR;
  }

  if (hash_update(hd, mac_key, MAC_KEY_BYTES) == GOLDILOCKS_FAILURE) {
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  if (hash_update(hd, &msg_type, 1) == GOLDILOCKS_FAILURE) {
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  hash_final(hd, dake_3->msg + w, HASH_BYTES);
  hash_destroy(hd);

  return OTRNG_SUCCESS;
}

// TODO: make sure that the message buffer is large enough
static otrng_result
otrng_prekey_dake3_message_append_prekey_publication_message(
    otrng_prekey_publication_message_s *pub_msg,
    otrng_prekey_dake3_message_s *dake_3, uint8_t mac_key[MAC_KEY_BYTES],
    uint8_t mac[HASH_BYTES]) {
  uint8_t *client_profile = NULL;
  size_t client_profile_len = 0;
  uint8_t *prekey_profile = NULL;
  uint8_t *proofs = NULL;
  size_t proof_buf_len = 0;
  size_t prekey_profile_len = 0;
  size_t size;
  uint8_t msg_type = OTRNG_PREKEY_PUBLICATION_MSG;
  size_t w = 0;
  const uint8_t *prekey_messages_beginning;
  uint8_t usage_prekey_message = 0x0E;
  uint8_t prekey_messages_kdf[HASH_BYTES];
  uint8_t prekey_proofs_kdf[HASH_BYTES];

  uint8_t usage_pre_MAC = 0x09;
  uint8_t usage_proof_message_ecdh = 0x13;
  uint8_t usage_proof_message_dh = 0x14;
  uint8_t usage_proof_shared_ecdh = 0x15;
  uint8_t usage_mac_proofs = 0x16;
  uint8_t one = 1, zero = 0;

  ec_scalar *values_priv_ecdh;
  ec_point *values_pub_ecdh;
  dh_mpi *values_priv_dh;
  dh_mpi *values_pub_dh;
  size_t proof_index = 0;

  ecdh_proof_s prekey_message_proof_ecdh;
  dh_proof_s prekey_message_proof_dh;
  ecdh_proof_s prekey_profile_proof;

  goldilocks_shake256_ctx_p hd;

  int i;

  memset(prekey_messages_kdf, 0, HASH_BYTES);
  memset(prekey_proofs_kdf, 0, HASH_BYTES);

  if (pub_msg->client_profile) {
    if (!otrng_client_profile_serialize(&client_profile, &client_profile_len,
                                        pub_msg->client_profile)) {
      return OTRNG_ERROR;
    }
  }

  if (pub_msg->prekey_profile) {
    if (!otrng_prekey_profile_serialize(&prekey_profile, &prekey_profile_len,
                                        pub_msg->prekey_profile)) {
      otrng_free(client_profile);
      return OTRNG_ERROR;
    }
  }

  if (pub_msg->num_prekey_messages > 0) {
    proof_buf_len += PROOF_C_SIZE + ED448_SCALAR_BYTES;
    proof_buf_len += PROOF_C_SIZE + DH_MPI_MAX_BYTES;
  }
  if (pub_msg->prekey_profile != NULL) {
    proof_buf_len += PROOF_C_SIZE + ED448_SCALAR_BYTES;
  }

  size = 2 + 1 + 1 + (4 + pub_msg->num_prekey_messages * PRE_KEY_MAX_BYTES) +
         1 + client_profile_len + 1 + prekey_profile_len + proof_buf_len +
         MAC_KEY_BYTES;
  dake_3->msg = otrng_xmalloc_z(size);

  w += otrng_serialize_uint16(dake_3->msg, OTRNG_PROTOCOL_VERSION_4);
  w += otrng_serialize_uint8(dake_3->msg + w, msg_type);

  w += otrng_serialize_uint8(dake_3->msg + w, pub_msg->num_prekey_messages);

  prekey_messages_beginning = dake_3->msg + w;
  for (i = 0; i < pub_msg->num_prekey_messages; i++) {
    size_t w2 = 0;
    if (!otrng_prekey_message_serialize(dake_3->msg + w, size - w, &w2,
                                        pub_msg->prekey_messages[i])) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      return OTRNG_ERROR;
    }
    w += w2;
  }

  if (pub_msg->num_prekey_messages > 0) {
    values_priv_ecdh = otrng_secure_alloc_array(pub_msg->num_prekey_messages,
                                                sizeof(ec_scalar));
    values_pub_ecdh =
        otrng_xmalloc_z(pub_msg->num_prekey_messages * sizeof(ec_point));

    values_priv_dh =
        otrng_secure_alloc_array(pub_msg->num_prekey_messages, sizeof(dh_mpi));
    values_pub_dh =
        otrng_xmalloc_z(pub_msg->num_prekey_messages * sizeof(dh_mpi));

    for (i = 0; i < pub_msg->num_prekey_messages; i++) {
      *values_pub_ecdh[i] = *pub_msg->prekey_messages[i]->y->pub;
      *values_priv_ecdh[i] = *pub_msg->prekey_messages[i]->y->priv;
      values_pub_dh[i] = pub_msg->prekey_messages[i]->b->pub;
      values_priv_dh[i] = pub_msg->prekey_messages[i]->b->priv;
    }

    if (!otrng_ecdh_proof_generate(
            &prekey_message_proof_ecdh, (const ec_scalar *)values_priv_ecdh,
            (const ec_point *)values_pub_ecdh, pub_msg->num_prekey_messages,
            mac, usage_proof_message_ecdh)) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      otrng_secure_free(values_priv_ecdh);
      otrng_free(values_pub_ecdh);
      otrng_secure_free(values_priv_dh);
      otrng_free(values_pub_dh);
      return OTRNG_ERROR;
    }

    if (!otrng_dh_proof_generate(&prekey_message_proof_dh, values_priv_dh,
                                 values_pub_dh, pub_msg->num_prekey_messages,
                                 mac, usage_proof_message_dh, NULL)) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      otrng_secure_free(values_priv_ecdh);
      otrng_free(values_pub_ecdh);
      otrng_secure_free(values_priv_dh);
      otrng_free(values_pub_dh);
      return OTRNG_ERROR;
    }

    otrng_secure_free(values_priv_ecdh);
    otrng_free(values_pub_ecdh);
    otrng_secure_free(values_priv_dh);
    otrng_free(values_pub_dh);
  }

  if (pub_msg->prekey_profile != NULL) {
    proof_buf_len += PROOF_C_SIZE + ED448_SCALAR_BYTES;
    values_priv_ecdh = otrng_secure_alloc_array(1, sizeof(ec_scalar));
    values_pub_ecdh = otrng_xmalloc_z(1 * sizeof(ec_point));

    *values_pub_ecdh[0] = *pub_msg->prekey_profile->keys->pub;
    *values_priv_ecdh[0] = *pub_msg->prekey_profile->keys->priv;

    if (!otrng_ecdh_proof_generate(&prekey_profile_proof,
                                   (const ec_scalar *)values_priv_ecdh,
                                   (const ec_point *)values_pub_ecdh, 1, mac,
                                   usage_proof_shared_ecdh)) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      otrng_secure_free(values_priv_ecdh);
      otrng_free(values_pub_ecdh);
      return OTRNG_ERROR;
    }

    otrng_secure_free(values_priv_ecdh);
    otrng_free(values_pub_ecdh);
  }

  proofs = otrng_xmalloc_z(proof_buf_len * sizeof(uint8_t));

  if (pub_msg->num_prekey_messages > 0) {
    proof_index += otrng_ecdh_proof_serialize(proofs + proof_index,
                                              &prekey_message_proof_ecdh);
    proof_index += otrng_dh_proof_serialize(proofs + proof_index,
                                            &prekey_message_proof_dh);
  }

  if (pub_msg->prekey_profile != NULL) {
    proof_index +=
        otrng_ecdh_proof_serialize(proofs + proof_index, &prekey_profile_proof);
  }

  if (!shake_256_prekey_server_kdf(prekey_proofs_kdf, HASH_BYTES,
                                   usage_mac_proofs, proofs, proof_index)) {
    otrng_free(proofs);
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    return OTRNG_ERROR;
  }

  if (!shake_256_prekey_server_kdf(
          prekey_messages_kdf, HASH_BYTES, usage_prekey_message,
          prekey_messages_beginning,
          dake_3->msg + w - prekey_messages_beginning)) {
    otrng_free(proofs);
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    return OTRNG_ERROR;
  }

  w += otrng_serialize_uint8(dake_3->msg + w, pub_msg->client_profile ? 1 : 0);
  w += otrng_serialize_bytes_array(dake_3->msg + w, client_profile,
                                   client_profile_len);

  w += otrng_serialize_uint8(dake_3->msg + w, pub_msg->prekey_profile ? 1 : 0);
  w += otrng_serialize_bytes_array(dake_3->msg + w, prekey_profile,
                                   prekey_profile_len);

  w += otrng_serialize_bytes_array(dake_3->msg + w, proofs, proof_index);

  otrng_free(proofs);

  /* MAC: KDF(usage_preMAC, prekey_mac_k || message type
            || N || KDF(usage_prekey_message, Prekey Messages, 64)
            || K || KDF(usage_client_profile, Client Profile, 64)
            || J || KDF(usage_prekey_profile, Prekey Profile, 64)
            || KDF(usage_mac_proofs, Proofs, 64),
        64) */
  if (!kdf_init_with_usage(hd, usage_pre_MAC)) {
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    return OTRNG_ERROR;
  }

  if (hash_update(hd, mac_key, MAC_KEY_BYTES) == GOLDILOCKS_FAILURE) {
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  if (hash_update(hd, &msg_type, 1) == GOLDILOCKS_FAILURE) {
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  if (hash_update(hd, &pub_msg->num_prekey_messages, 1) == GOLDILOCKS_FAILURE) {
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  if (hash_update(hd, prekey_messages_kdf, HASH_BYTES) == GOLDILOCKS_FAILURE) {
    otrng_free(client_profile);
    otrng_free(prekey_profile);
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  if (pub_msg->client_profile) {
    uint8_t usage_client_profile = 0x0F;
    uint8_t client_profile_kdf[HASH_BYTES];

    memset(client_profile_kdf, 0, HASH_BYTES);

    if (!shake_256_prekey_server_kdf(client_profile_kdf, HASH_BYTES,
                                     usage_client_profile, client_profile,
                                     client_profile_len)) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

    if (hash_update(hd, &one, 1) == GOLDILOCKS_FAILURE) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

    if (hash_update(hd, client_profile_kdf, HASH_BYTES) == GOLDILOCKS_FAILURE) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

  } else {
    if (hash_update(hd, &zero, 1) == GOLDILOCKS_FAILURE) {
      otrng_free(client_profile);
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }
  }
  otrng_free(client_profile);

  if (pub_msg->prekey_profile) {
    uint8_t prekey_profile_kdf[HASH_BYTES];
    uint8_t usage_prekey_profile = 0x10;

    memset(prekey_profile_kdf, 0, HASH_BYTES);

    if (!shake_256_prekey_server_kdf(prekey_profile_kdf, HASH_BYTES,
                                     usage_prekey_profile, prekey_profile,
                                     prekey_profile_len)) {
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

    if (hash_update(hd, &one, 1) == GOLDILOCKS_FAILURE) {
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

    if (hash_update(hd, prekey_profile_kdf, HASH_BYTES) == GOLDILOCKS_FAILURE) {
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }

  } else {
    if (hash_update(hd, &zero, 1) == GOLDILOCKS_FAILURE) {
      otrng_free(prekey_profile);
      hash_destroy(hd);
      return OTRNG_ERROR;
    }
  }
  otrng_free(prekey_profile);

  if (hash_update(hd, prekey_proofs_kdf, HASH_BYTES) == GOLDILOCKS_FAILURE) {
    hash_destroy(hd);
    return OTRNG_ERROR;
  }

  hash_final(hd, dake_3->msg + w, HASH_BYTES);
  hash_destroy(hd);

  dake_3->msg_len = w + HASH_BYTES;

  return OTRNG_SUCCESS;
}

tstatic otrng_result otrng_prekey_dake3_message_serialize(
    uint8_t **ser, size_t *ser_len, const otrng_prekey_dake3_message_s *msg) {
  size_t ret_len =
      2 + 1 + 4 + RING_SIG_BYTES + (4 + msg->msg_len) + ED448_POINT_BYTES;
  uint8_t *ret = otrng_xmalloc_z(ret_len);
  size_t w = 0;

  w += otrng_serialize_uint16(ret + w, OTRNG_PROTOCOL_VERSION_4);
  w += otrng_serialize_uint8(ret + w, OTRNG_PREKEY_DAKE3_MSG);
  w += otrng_serialize_uint32(ret + w, msg->client_instance_tag);
  w += otrng_serialize_ring_sig(ret + w, msg->sigma);
  w += otrng_serialize_data(ret + w, msg->msg, msg->msg_len);

  *ser = ret;
  if (ser_len) {
    *ser_len = w;
  }

  return OTRNG_SUCCESS;
}

tstatic void
otrng_prekey_dake3_message_init(otrng_prekey_dake3_message_s *dake_3) {
  memset(dake_3, 0, sizeof(otrng_prekey_dake3_message_s));
  dake_3->sigma = otrng_xmalloc_z(sizeof(ring_sig_s));
}

tstatic void
otrng_prekey_dake3_message_destroy(otrng_prekey_dake3_message_s *dake_3) {
  if (!dake_3) {
    return;
  }

  otrng_free(dake_3->msg);
  dake_3->msg = NULL;

  otrng_ring_sig_destroy(dake_3->sigma);
  otrng_free(dake_3->sigma);
  dake_3->sigma = NULL;
}

static otrng_prekey_publication_message_s *
otrng_prekey_publication_message_new() {
  otrng_prekey_publication_message_s *msg =
      malloc(sizeof(otrng_prekey_publication_message_s));
  if (!msg) {
    return NULL;
  }

  msg->client_profile = NULL;
  msg->prekey_profile = NULL;
  msg->prekey_messages = NULL;

  return msg;
}

static void otrng_prekey_publication_message_destroy(
    otrng_prekey_publication_message_s *msg) {
  int i;

  if (!msg) {
    return;
  }

  if (msg->prekey_messages) {
    for (i = 0; i < msg->num_prekey_messages; i++) {
      otrng_prekey_message_free(msg->prekey_messages[i]);
    }

    otrng_free(msg->prekey_messages);
    msg->prekey_messages = NULL;
  }

  otrng_client_profile_free(msg->client_profile);
  msg->client_profile = NULL;

  otrng_prekey_profile_free(msg->prekey_profile);
  msg->prekey_profile = NULL;
}

tstatic char *send_dake3(const otrng_prekey_dake2_message_s *dake_2,
                         otrng_client_s *client) {
  otrng_prekey_dake3_message_s dake_3;
  size_t composite_phi_len = 0;
  uint8_t *composite_phi;
  uint8_t *our_profile = NULL;
  size_t our_profile_len = 0;
  size_t tlen;
  uint8_t *t;
  size_t w = 1;
  uint8_t usage_receiver_client_profile = 0x05;
  uint8_t usage_receiver_prekey_composite_identity = 0x06;
  uint8_t usage_receiver_prekey_composite_phi = 0x07;
  uint8_t *shared_secret = otrng_secure_alloc(HASH_BYTES);
  uint8_t *ecdh_shared = otrng_secure_alloc(ED448_POINT_BYTES);
  uint8_t usage_SK = 0x01;
  uint8_t usage_preMAC_key = 0x08;
  uint8_t usage_proof_context = 0x12;
  otrng_result success;
  uint8_t *ser = NULL;
  size_t ser_len = 0;
  char *ret;
  uint8_t mac[HASH_BYTES];
  otrng_prekey_client_s *prekey_client = client->prekey_client;

  otrng_prekey_dake3_message_init(&dake_3);
  dake_3.client_instance_tag = prekey_client->instance_tag;

  composite_phi = otrng_prekey_client_get_expected_composite_phi(
      &composite_phi_len, prekey_client);
  if (!composite_phi) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  if (!otrng_client_profile_serialize(&our_profile, &our_profile_len,
                                      prekey_client->client_profile)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    otrng_free(composite_phi);
    return NULL;
  }

  tlen = 1 + 3 * HASH_BYTES + 2 * ED448_POINT_BYTES;
  t = otrng_xmalloc_z(tlen);

  *t = 0x1;

  if (!shake_256_prekey_server_kdf(t + w, HASH_BYTES,
                                   usage_receiver_client_profile, our_profile,
                                   our_profile_len)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    otrng_free(our_profile);
    otrng_free(composite_phi);
    otrng_free(t);
    return NULL;
  }

  otrng_free(our_profile);

  w += HASH_BYTES;

  /* Both composite identity AND composite phi have the server's bare JID */
  if (!shake_256_prekey_server_kdf(
          t + w, HASH_BYTES, usage_receiver_prekey_composite_identity,
          dake_2->composite_identity, dake_2->composite_identity_len)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    otrng_free(composite_phi);
    otrng_free(t);
    return NULL;
  }

  w += HASH_BYTES;

  w += otrng_serialize_ec_point(t + w, prekey_client->ephemeral_ecdh->pub);
  w += otrng_serialize_ec_point(t + w, dake_2->S);

  if (!shake_256_prekey_server_kdf(t + w, HASH_BYTES,
                                   usage_receiver_prekey_composite_phi,
                                   composite_phi, composite_phi_len)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    otrng_free(composite_phi);
    otrng_free(t);
    return NULL;
  }

  otrng_free(composite_phi);

  /* H_a, sk_ha, {H_a, H_s, S}, t */
  if (!otrng_rsig_authenticate_with_usage_and_domain(
          usage_auth, prekey_hash_domain, dake_3.sigma,
          prekey_client->keypair->priv, prekey_client->keypair->pub,
          prekey_client->keypair->pub, dake_2->server_pub_key, dake_2->S, t,
          tlen)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    otrng_free(t);
    return NULL;
  }

  otrng_free(t);

  /* ECDH(i, S) */
  // TODO: check is the ephemeral is erased
  if (otrng_failed(otrng_ecdh_shared_secret(ecdh_shared, ED448_POINT_BYTES,
                                            prekey_client->ephemeral_ecdh->priv,
                                            dake_2->S))) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  /* SK = KDF(0x01, ECDH(i, S), 64) */
  if (!shake_256_prekey_server_kdf(shared_secret, HASH_BYTES, usage_SK,
                                   ecdh_shared, ED448_POINT_BYTES)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  /* prekey_mac_k = KDF(0x08, SK, 64) */
  if (!shake_256_prekey_server_kdf(prekey_client->mac_key, MAC_KEY_BYTES,
                                   usage_preMAC_key, shared_secret,
                                   HASH_BYTES)) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  /* Attach MESSAGE in the message */
  if (prekey_client->after_dake == OTRNG_PREKEY_STORAGE_INFORMATION_REQUEST) {
    if (!otrng_prekey_dake3_message_append_storage_information_request(
            &dake_3, prekey_client->mac_key)) {
      otrng_secure_free(shared_secret);
      otrng_secure_free(ecdh_shared);
      return NULL;
    }
  } else if (prekey_client->after_dake == OTRNG_PREKEY_PREKEY_PUBLICATION) {
    otrng_prekey_publication_message_s *pub_msg =
        otrng_prekey_publication_message_new();
    if (!build_prekey_publication_message_callback(pub_msg, client)) {
      otrng_secure_free(shared_secret);
      otrng_secure_free(ecdh_shared);
      return NULL;
    }

    /* mac for proofs = KDF(0x12, SK, 64) */
    if (!shake_256_prekey_server_kdf(mac, HASH_BYTES, usage_proof_context,
                                     shared_secret, HASH_BYTES)) {
      otrng_secure_free(shared_secret);
      otrng_secure_free(ecdh_shared);
      return NULL;
    }

    success = otrng_prekey_dake3_message_append_prekey_publication_message(
        pub_msg, &dake_3, prekey_client->mac_key, mac);
    otrng_prekey_publication_message_destroy(pub_msg);

    if (!success) {
      otrng_secure_free(shared_secret);
      otrng_secure_free(ecdh_shared);
      return NULL;
    }
  } else {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  prekey_client->after_dake = 0;

  success = otrng_prekey_dake3_message_serialize(&ser, &ser_len, &dake_3);
  otrng_prekey_dake3_message_destroy(&dake_3);

  if (!success) {
    otrng_secure_free(shared_secret);
    otrng_secure_free(ecdh_shared);
    return NULL;
  }

  ret = prekey_encode(ser, ser_len);
  otrng_free(ser);
  otrng_secure_free(shared_secret);
  otrng_secure_free(ecdh_shared);

  return ret;
}

static char *process_received_dake2(const otrng_prekey_dake2_message_s *msg,
                                    otrng_client_s *client) {

  if (msg->client_instance_tag != client->prekey_client->instance_tag) {
    return NULL;
  }

  if (!otrng_prekey_dake2_message_valid(msg, client->prekey_client)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_INVALID_DAKE2);
    return NULL;
  }

  return send_dake3(msg, client);
}

tstatic void
otrng_prekey_dake2_message_init(otrng_prekey_dake2_message_s *dake_2) {
  memset(dake_2, 0, sizeof(otrng_prekey_dake2_message_s));
  dake_2->sigma = otrng_xmalloc_z(sizeof(ring_sig_s));
}

tstatic void
otrng_prekey_dake2_message_destroy(otrng_prekey_dake2_message_s *dake_2) {
  if (!dake_2) {
    return;
  }

  if (dake_2->composite_identity) {
    otrng_free(dake_2->composite_identity);
    dake_2->composite_identity = NULL;
  }

  if (dake_2->server_identity) {
    otrng_free(dake_2->server_identity);
    dake_2->server_identity = NULL;
  }

  otrng_ec_point_destroy(dake_2->S);
  otrng_ring_sig_destroy(dake_2->sigma);
  otrng_free(dake_2->sigma);
  dake_2->sigma = NULL;
}

static char *receive_dake2(const uint8_t *decoded, size_t decoded_len,
                           otrng_client_s *client) {
  otrng_prekey_dake2_message_s msg;
  char *ret = NULL;

  otrng_prekey_dake2_message_init(&msg);
  if (!otrng_prekey_dake2_message_deserialize(&msg, decoded, decoded_len)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  ret = process_received_dake2(&msg, client);
  otrng_prekey_dake2_message_destroy(&msg);

  return ret;
}

static otrng_bool otrng_prekey_storage_status_message_valid(
    const otrng_prekey_storage_status_message_s *msg,
    const uint8_t mac_key[MAC_KEY_BYTES]) {

  size_t bufl = 1 + 4 + 4;
  uint8_t *buf = otrng_xmalloc_z(bufl);
  uint8_t mac_tag[HASH_BYTES];
  uint8_t usage_status_MAC = 0x0B;
  goldilocks_shake256_ctx_p hmac;

  *buf = OTRNG_PREKEY_STORAGE_STATUS_MSG; /* message type */
  if (otrng_serialize_uint32(buf + 1, msg->client_instance_tag) == 0) {
    otrng_free(buf);
    return otrng_false;
  }

  if (otrng_serialize_uint32(buf + 5, msg->stored_prekeys) == 0) {
    otrng_free(buf);
    return otrng_false;
  }

  /* KDF(usage_status_MAC, prekey_mac_k || message type || receiver instance
   tag
   || Stored Prekey Messages Number, 64) */
  if (!kdf_init_with_usage(hmac, usage_status_MAC)) {
    otrng_free(buf);
    return otrng_false;
  }

  if (hash_update(hmac, mac_key, MAC_KEY_BYTES) == GOLDILOCKS_FAILURE) {
    hash_destroy(hmac);
    otrng_free(buf);
    return otrng_false;
  }

  if (hash_update(hmac, buf, bufl) == GOLDILOCKS_FAILURE) {
    hash_destroy(hmac);
    otrng_free(buf);
    return otrng_false;
  }

  hash_final(hmac, mac_tag, HASH_BYTES);
  hash_destroy(hmac);

  otrng_free(buf);

  if (otrl_mem_differ(mac_tag, msg->mac, HASH_BYTES) != 0) {
    otrng_secure_wipe(mac_tag, HASH_BYTES);
    return otrng_false;
  }

  return otrng_true;
}

static char *process_received_storage_status(
    const otrng_prekey_storage_status_message_s *msg, otrng_client_s *client) {
  if (msg->client_instance_tag != client->prekey_client->instance_tag) {
    return NULL;
  }

  if (!otrng_prekey_storage_status_message_valid(
          msg, client->prekey_client->mac_key)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_INVALID_STORAGE_STATUS);
    return NULL;
  }

  if (msg->stored_prekeys < client->prekey_client->publication_policy
                                ->minimum_stored_prekey_message) {
    client->prekey_msgs_num_to_publish =
        client->prekey_client->publication_policy
            ->max_published_prekey_message -
        msg->stored_prekeys;
    low_prekey_messages_in_storage_callback(client);
  }

  prekey_storage_status_received_callback(client, msg);
  return NULL;
}

tstatic otrng_result otrng_prekey_storage_status_message_deserialize(
    otrng_prekey_storage_status_message_s *dst, const uint8_t *ser,
    size_t ser_len) {
  size_t w = 0;
  size_t read = 0;

  uint8_t msg_type = 0;

  if (!parse_header(&msg_type, ser, ser_len, &w)) {
    return OTRNG_ERROR;
  }

  if (msg_type != OTRNG_PREKEY_STORAGE_STATUS_MSG) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint32(&dst->client_instance_tag, ser + w, ser_len - w,
                                &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  if (!otrng_deserialize_uint32(&dst->stored_prekeys, ser + w, ser_len - w,
                                &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  if (!otrng_deserialize_bytes_array(dst->mac, DATA_MSG_MAC_BYTES, ser + w,
                                     ser_len - w)) {
    return OTRNG_ERROR;
  }

  w += DATA_MSG_MAC_BYTES;

  return OTRNG_SUCCESS;
}

tstatic void otrng_prekey_storage_status_message_destroy(
    otrng_prekey_storage_status_message_s *msg) {
  if (!msg) {
    return;
  }

  msg->client_instance_tag = 0;
  msg->stored_prekeys = 0;
  otrng_secure_wipe(msg->mac, DATA_MSG_MAC_BYTES);
}

static char *receive_storage_status(const uint8_t *decoded, size_t decoded_len,
                                    otrng_client_s *client) {
  otrng_prekey_storage_status_message_s msg[1];
  char *ret;

  if (!otrng_prekey_storage_status_message_deserialize(msg, decoded,
                                                       decoded_len)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  ret = process_received_storage_status(msg, client);

  otrng_prekey_storage_status_message_destroy(msg);
  return ret;
}

static char *receive_success(const uint8_t *decoded, size_t decoded_len,
                             otrng_client_s *client) {
  uint32_t instance_tag = 0;
  size_t read = 0;
  uint8_t mac_tag[HASH_BYTES];
  uint8_t usage_success_MAC = 0x0C;
  goldilocks_shake256_ctx_p hash;

  memset(mac_tag, 0, HASH_BYTES);

  if (decoded_len < OTRNG_PREKEY_SUCCESS_MSG_LEN) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (!otrng_deserialize_uint32(&instance_tag, decoded + 3, decoded_len - 3,
                                &read)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (instance_tag != client->prekey_client->instance_tag) {
    return NULL;
  }

  if (!kdf_init_with_usage(hash, usage_success_MAC)) {
    return NULL;
  }

  if (hash_update(hash, client->prekey_client->mac_key, MAC_KEY_BYTES) ==
      GOLDILOCKS_FAILURE) {
    hash_destroy(hash);
    return NULL;
  }

  if (hash_update(hash, decoded + 2, 5) == GOLDILOCKS_FAILURE) {
    hash_destroy(hash);
    return NULL;
  }

  hash_final(hash, mac_tag, HASH_BYTES);
  hash_destroy(hash);

  if (otrl_mem_differ(mac_tag, decoded + 7, HASH_BYTES) != 0) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_INVALID_SUCCESS);
  } else {
    success_received_callback(client);
  }

  otrng_secure_wipe(mac_tag, HASH_BYTES);
  return NULL;
}

static char *receive_failure(const uint8_t *decoded, size_t decoded_len,
                             otrng_client_s *client) {
  uint32_t instance_tag = 0;
  size_t read = 0;
  uint8_t mac_tag[HASH_BYTES];
  uint8_t usage_failure_MAC = 0x0D;

  goldilocks_shake256_ctx_p hash;

  memset(mac_tag, 0, HASH_BYTES);

  if (decoded_len < OTRNG_PREKEY_FAILURE_MSG_LEN) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (!otrng_deserialize_uint32(&instance_tag, decoded + 3, decoded_len - 3,
                                &read)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (instance_tag != client->prekey_client->instance_tag) {
    return NULL;
  }

  if (!kdf_init_with_usage(hash, usage_failure_MAC)) {
    return NULL;
  }

  if (hash_update(hash, client->prekey_client->mac_key, MAC_KEY_BYTES) ==
      GOLDILOCKS_FAILURE) {
    hash_destroy(hash);
    return NULL;
  }

  if (hash_update(hash, decoded + 2, 5) == GOLDILOCKS_FAILURE) {
    hash_destroy(hash);
    return NULL;
  }

  hash_final(hash, mac_tag, HASH_BYTES);
  hash_destroy(hash);

  if (otrl_mem_differ(mac_tag, decoded + 7, HASH_BYTES) != 0) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_INVALID_SUCCESS);
  } else {
    failure_received_callback(client);
  }

  otrng_secure_wipe(mac_tag, HASH_BYTES);
  return NULL;
}

static char *receive_no_prekey_in_storage(const uint8_t *decoded,
                                          size_t decoded_len,
                                          otrng_client_s *client) {
  uint32_t instance_tag = 0;
  size_t read = 0;

  if (!otrng_deserialize_uint32(&instance_tag, decoded + 3, decoded_len - 3,
                                &read)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (instance_tag != client->prekey_client->instance_tag) {
    return NULL;
  }

  no_prekey_in_storage_received_callback(client);
  return NULL;
}

static void process_received_prekey_ensemble_retrieval(
    otrng_prekey_ensemble_retrieval_message_s *msg, otrng_client_s *client) {
  int i;

  if (msg->instance_tag != client->prekey_client->instance_tag) {
    return;
  }

  for (i = 0; i < msg->num_ensembles; i++) {
    if (!otrng_prekey_ensemble_validate(msg->ensembles[i])) {
      otrng_prekey_ensemble_destroy(msg->ensembles[i]);
      msg->ensembles[i] = NULL;
    }
  }

  prekey_ensembles_received_callback(client, msg->ensembles,
                                     msg->num_ensembles);
}

tstatic otrng_result otrng_prekey_ensemble_retrieval_message_deserialize(
    otrng_prekey_ensemble_retrieval_message_s *dst, const uint8_t *ser,
    size_t ser_len) {
  size_t w = 0;
  size_t read = 0;
  uint8_t l = 0;

  uint8_t msg_type = 0;

  int i;

  if (!parse_header(&msg_type, ser, ser_len, &w)) {
    return OTRNG_ERROR;
  }

  if (msg_type != OTRNG_PREKEY_ENSEMBLE_RETRIEVAL_MSG) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint32(&dst->instance_tag, ser + w, ser_len - w,
                                &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  if (!otrng_deserialize_uint8(&l, ser + w, ser_len - w, &read)) {
    return OTRNG_ERROR;
  }

  w += read;

  dst->ensembles = otrng_xmalloc_z(sizeof(prekey_ensemble_s *) * l);

  dst->num_ensembles = l;

  for (i = 0; i < l; i++) {
    dst->ensembles[i] = otrng_prekey_ensemble_new();

    if (!otrng_prekey_ensemble_deserialize(dst->ensembles[i], ser + w,
                                           ser_len - w, &read)) {
      return OTRNG_ERROR;
    }

    w += read;
  }

  return OTRNG_SUCCESS;
}

tstatic void otrng_prekey_ensemble_retrieval_message_destroy(
    otrng_prekey_ensemble_retrieval_message_s *msg) {
  int i;

  if (!msg) {
    return;
  }

  if (msg->ensembles) {
    for (i = 0; i < msg->num_ensembles; i++) {
      otrng_prekey_ensemble_free(msg->ensembles[i]);
    }
    otrng_free(msg->ensembles);
  }

  msg->ensembles = NULL;
}

static char *receive_prekey_ensemble_retrieval(const uint8_t *decoded,
                                               size_t decoded_len,
                                               otrng_client_s *client) {
  otrng_prekey_ensemble_retrieval_message_s msg[1];

  if (!otrng_prekey_ensemble_retrieval_message_deserialize(msg, decoded,
                                                           decoded_len)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    otrng_prekey_ensemble_retrieval_message_destroy(msg);
    return NULL;
  }

  process_received_prekey_ensemble_retrieval(msg, client);
  otrng_prekey_ensemble_retrieval_message_destroy(msg);
  return NULL;
}

static char *receive_decoded(const uint8_t *decoded, size_t decoded_len,
                             otrng_client_s *client) {
  uint8_t msg_type = 0;
  char *ret = NULL;

  if (!parse_header(&msg_type, decoded, decoded_len, NULL)) {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
    return NULL;
  }

  if (msg_type == OTRNG_PREKEY_DAKE2_MSG) {
    ret = receive_dake2(decoded, decoded_len, client);
  } else if (msg_type == OTRNG_PREKEY_SUCCESS_MSG) {
    ret = receive_success(decoded, decoded_len, client);
  } else if (msg_type == OTRNG_PREKEY_FAILURE_MSG) {
    ret = receive_failure(decoded, decoded_len, client);
  } else if (msg_type == OTRNG_PREKEY_NO_PREKEY_IN_STORAGE_MSG) {
    ret = receive_no_prekey_in_storage(decoded, decoded_len, client);
  } else if (msg_type == OTRNG_PREKEY_ENSEMBLE_RETRIEVAL_MSG) {
    ret = receive_prekey_ensemble_retrieval(decoded, decoded_len, client);
  } else if (msg_type == OTRNG_PREKEY_STORAGE_STATUS_MSG) {
    ret = receive_storage_status(decoded, decoded_len, client);
  } else {
    notify_error_callback(client, OTRNG_PREKEY_CLIENT_MALFORMED_MSG);
  }

  return ret;
}

/* TODO: this function should probably return otrng_bool instead */
API otrng_result otrng_prekey_client_receive(char **to_send, const char *server,
                                             const char *msg,
                                             otrng_client_s *client) {
  uint8_t *ser = NULL;
  size_t ser_len = 0;

  assert(client != NULL);
  assert(client->prekey_client != NULL);

  /* It should only process prekey server messages from the expected server.
     This avoids processing any plaintext message from a party as a
     malformed prekey server message. */
  if (strcmp(client->prekey_client->server_identity, server) != 0) {
    return OTRNG_ERROR;
  }

  // TODO: process fragmented messages

  /* If it fails to decode it was not a prekey server message. */
  if (!prekey_decode(msg, &ser, &ser_len)) {
    return OTRNG_ERROR;
  }

  /* In any other case, it returns SUCCESS because we processed the message.
     Even if there was an error processing it. We should consider informing the
     error while processing using callbacks.
  */
  *to_send = receive_decoded(ser, ser_len, client);
  otrng_free(ser);

  return OTRNG_SUCCESS;
}

INTERNAL otrng_prekey_dake2_message_s *otrng_prekey_dake2_message_new() {
  otrng_prekey_dake2_message_s *dake_2 =
      otrng_xmalloc_z(sizeof(otrng_prekey_dake2_message_s));
  otrng_prekey_dake2_message_init(dake_2);
  return dake_2;
}

INTERNAL otrng_prekey_dake3_message_s *otrng_prekey_dake3_message_new() {
  otrng_prekey_dake3_message_s *dake_3 =
      otrng_xmalloc_z(sizeof(otrng_prekey_dake3_message_s));
  otrng_prekey_dake3_message_init(dake_3);
  return dake_3;
}

INTERNAL otrng_result otrng_prekey_success_message_deserialize(
    otrng_prekey_success_message_s *destination, const uint8_t *source,
    size_t source_len) {
  const uint8_t *cursor = source;
  int64_t len = source_len;
  size_t read = 0;
  uint8_t message_type = 0;

  uint16_t protocol_version = 0;
  if (!otrng_deserialize_uint16(&protocol_version, cursor, len, &read)) {
    return OTRNG_ERROR;
  }

  cursor += read;
  len -= read;

  if (protocol_version != OTRNG_PROTOCOL_VERSION_4) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint8(&message_type, cursor, len, &read)) {
    return OTRNG_ERROR;
  }

  cursor += read;
  len -= read;

  if (message_type != OTRNG_PREKEY_SUCCESS_MSG) {
    return OTRNG_ERROR;
  }

  if (!otrng_deserialize_uint32(&destination->client_instance_tag, cursor, len,
                                &read)) {
    return OTRNG_ERROR;
  }

  cursor += read;
  len -= read;

  return otrng_deserialize_bytes_array(destination->success_mac, HASH_BYTES,
                                       cursor, len);
}

API void otrng_prekey_client_add_prekey_messages_for_publication(
    otrng_client_s *client, otrng_prekey_publication_message_s *msg) {
  size_t max = otrng_list_len(client->our_prekeys);
  size_t real = 0;
  prekey_message_s **msg_list = otrng_xmalloc(max * sizeof(prekey_message_s *));
  list_element_s *current = client->our_prekeys;

  for (; current; current = current->next) {
    prekey_message_s *pm = current->data;
    if (pm->should_publish && !pm->is_publishing) {
      msg_list[real] = otrng_prekey_message_create_copy(pm);
      pm->is_publishing = otrng_true;
      real++;
    }
  }

  // Since we are shrinking the array, there is no way this can fail, so no need
  // to check the result
  msg->prekey_messages = realloc(msg_list, real * sizeof(prekey_message_s *));
  msg->num_prekey_messages = real;
}

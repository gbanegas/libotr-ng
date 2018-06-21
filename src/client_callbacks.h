/*
 *  This file is part of the Off-the-Record Next Generation Messaging
 *  library (libotr-ng).
 *
 *  Copyright (C) 2016-2018, the libotr-ng contributors.
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
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

#ifndef OTRNG_CLIENT_CALLBACKS_H
#define OTRNG_CLIENT_CALLBACKS_H

#include "fingerprint.h"
#include "shared.h"

typedef enum {
  OTRNG_SMPEVENT_NONE = 0,
  OTRNG_SMPEVENT_ASK_FOR_SECRET = 1,
  OTRNG_SMPEVENT_ASK_FOR_ANSWER = 2,
  OTRNG_SMPEVENT_IN_PROGRESS = 3,
  OTRNG_SMPEVENT_SUCCESS = 4,
  OTRNG_SMPEVENT_CHEATED = 5,
  OTRNG_SMPEVENT_FAILURE = 6,
  OTRNG_SMPEVENT_ABORT = 7,
  OTRNG_SMPEVENT_ERROR = 8,
} otrng_smp_event_t;

typedef struct otrng_conversation_state_s otrng_client_conversation_s;

typedef struct otrng_client_callbacks_s {
  /* Create a private key for the given accountname/protocol if
   * desired. */
  void (*create_privkey)(const void *client_opdata); // TODO: @client This
                                                     // should receive a
                                                     // otrng_client_state_s

  /* Create a shared prekey for the given accountname/protocol if not available.
   */
  void (*create_shared_prekey)(const otrng_client_conversation_s *);

  /* A connection has entered a secure state. */
  void (*gone_secure)(const otrng_client_conversation_s *);

  /* A connection has left a secure state. */
  void (*gone_insecure)(const otrng_client_conversation_s *);

  /* A fingerprint was seen in this connection. */
  void (*fingerprint_seen)(const otrng_fingerprint_p,
                           const otrng_client_conversation_s *);

  /* A v3 fingerprint was seen in this connection. */
  void (*fingerprint_seen_v3)(const v3_fingerprint_p,
                              const otrng_client_conversation_s *);

  /* Update the authentication UI and prompt the user to enter a shared secret.
   *      The sender application should call otrl_message_initiate_smp,
   *      passing NULL as the question.
   *      When the receiver application resumes the SM protocol by calling
   *      otrl_message_respond_smp with the secret answer. */
  void (*smp_ask_for_secret)(const otrng_client_conversation_s *);

  /* Same as smp_ask_for_secret but sender calls otrl_message_initiate_smp_q
   * instead) */
  void (*smp_ask_for_answer)(const uint8_t *question, const size_t q_len,
                             const otrng_client_conversation_s *);

  /* Update the authentication UI with respect to SMP events
   * These are the possible events:
   * - OTRL_SMPEVENT_CHEATED
   *      abort the current auth and update the auth progress dialog
   *      with progress_percent. otrl_message_abort_smp should be called to
   *      stop the SM protocol.
   * - OTRL_SMPEVENT_INPROGRESS       and
   *   OTRL_SMPEVENT_SUCCESS          and
   *   OTRL_SMPEVENT_FAILURE          and
   *   OTRL_SMPEVENT_ABORT
   *      update the auth progress dialog with progress_percent
   * - OTRL_SMPEVENT_ERROR
   *      (same as OTRL_SMPEVENT_CHEATED)
   * */
  void (*smp_update)(const otrng_smp_event_t event,
                     const uint8_t progress_percent,
                     const otrng_client_conversation_s *);

  /* We received a request from the buddy to use the current "extra"
   * symmetric key.  The key will be passed in symkey, of length
   * EXTRA_SYMMETRIC_KEY_BYTES.  The requested use, as well as use-specific
   * data will be passed so that the applications can communicate other
   * information (some id for the data transfer, for example). */
  void (*received_extra_symm_key)(const otrng_client_conversation_s *,
                                  unsigned int use,
                                  const unsigned char *use_data,
                                  size_t use_data_len,
                                  const unsigned char *extra_sym_key);
} otrng_client_callbacks_s, otrng_client_callbacks_p[1];

INTERNAL void
otrng_client_callbacks_create_privkey(const otrng_client_callbacks_s *cb,
                                      const void *client_opdata);

INTERNAL void
otrng_client_callbacks_gone_secure(const otrng_client_callbacks_s *cb,
                                   const otrng_client_conversation_s *conv);

INTERNAL void
otrng_client_callbacks_gone_insecure(const otrng_client_callbacks_s *cb,
                                     const otrng_client_conversation_s *conv);

INTERNAL void otrng_client_callbacks_fingerprint_seen(
    const otrng_client_callbacks_s *cb, const otrng_fingerprint_p fp,
    const otrng_client_conversation_s *conv);

INTERNAL void otrng_client_callbacks_fingerprint_seen_v3(
    const otrng_client_callbacks_s *cb, const v3_fingerprint_p fp,
    const otrng_client_conversation_s *conv);

INTERNAL void otrng_client_callbacks_smp_ask_for_answer(
    const otrng_client_callbacks_s *cb, const char *question,
    const otrng_client_conversation_s *conv);

INTERNAL void otrng_client_callbacks_smp_ask_for_secret(
    const otrng_client_callbacks_s *cb,
    const otrng_client_conversation_s *conv);

INTERNAL void otrng_client_callbacks_smp_update(
    const otrng_client_callbacks_s *cb, const otrng_smp_event_t event,
    const uint8_t progress_percent, const otrng_client_conversation_s *conv);

#ifdef OTRNG_CLIENT_CALLBACKS_PRIVATE
#endif

#endif

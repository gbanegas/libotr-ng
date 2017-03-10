#include <glib.h>
#include <string.h>

#include "../otrv4.h"

void
test_otrv4_builds_query_message(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  char *message = "And some random invitation text.";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  char *query_message = NULL;
  otrv4_build_query_message(&query_message, otrv4_fixture->otr, message, strlen(message));

  char *expected_qm = "?OTRv4? And some random invitation text.";
  g_assert_cmpstr(query_message, ==, expected_qm);

  free(query_message);
}

void
test_otrv4_builds_query_message_v34(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  char *message = "And some random invitation text.";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V3 | OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  char *query_message = NULL;
  otrv4_build_query_message(&query_message, otrv4_fixture->otr, message, strlen(message));

  char *expected_qm = "?OTRv43? And some random invitation text.";
  g_assert_cmpstr(query_message, ==, expected_qm);

  free(query_message);
}

void
test_otrv4_builds_whitespace_tag(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  char *expected_tag = " \t  \t\t\t\t \t \t \t    \t\t \t  And some random invitation text.";
  char *message = "And some random invitation text.";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  char *whitespace_tag = NULL;
  otrv4_build_whitespace_tag(&whitespace_tag, otrv4_fixture->otr, message, strlen(message));
  g_assert_cmpstr(whitespace_tag, ==, expected_tag);
  free(whitespace_tag);
}

void
test_otrv4_builds_whitespace_tag_v34(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  char *expected_tag = " \t  \t\t\t\t \t \t \t    \t\t \t    \t\t  \t\tAnd some random invitation text";
  char *message = "And some random invitation text";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V3 | OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  char *whitespace_tag = NULL;
  otrv4_build_whitespace_tag(&whitespace_tag, otrv4_fixture->otr, message, strlen(message));
  g_assert_cmpstr(whitespace_tag, ==, expected_tag);
  free(whitespace_tag);
}

void
test_otrv4_receives_plaintext_without_ws_tag_on_start(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);
  otrv4_response_t *response = otrv4_response_new();
  otrv4_assert(otrv4_receive_message(response, "Some random text.", 17, otrv4_fixture->otr));

  g_assert_cmpstr(response->to_display, ==, "Some random text.");

  otrv4_response_free(response);
}

void
test_otrv4_receives_plaintext_without_ws_tag_not_on_start(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);
  otrv4_fixture->otr->state = OTRV4_STATE_AKE_IN_PROGRESS;

  otrv4_response_t *response = otrv4_response_new();
  otrv4_assert(otrv4_receive_message(response, "Some random text.", 17, otrv4_fixture->otr));

  g_assert_cmpstr(response->to_display, ==, "Some random text.");
  g_assert_cmpint(response->warning, ==, OTRV4_WARN_RECEIVED_UNENCRYPTED);

  otrv4_response_free(response);
}

void
test_otrv4_receives_plaintext_with_ws_tag(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_response_t *response = otrv4_response_new();
  string_t message = " \t  \t\t\t\t \t \t \t    \t\t \t  And some random invitation text.";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  otrv4_assert(otrv4_receive_message(response, message, strlen(message), otrv4_fixture->otr));
  g_assert_cmpstr(response->to_display, ==, "And some random invitation text.");
  otrv4_assert(response->to_send);
  g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_AKE_IN_PROGRESS);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_4);

  otrv4_response_free(response);
}

void
test_otrv4_receives_plaintext_with_ws_tag_v3(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_response_t *response = otrv4_response_new();
  string_t message = " \t  \t\t\t\t \t \t \t    \t\t  \t\tAnd some random invitation text.";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V3 };
  otrv4_start(otrv4_fixture->otr, policy);
  otrv4_assert(otrv4_receive_message(response, message, strlen(message), otrv4_fixture->otr));

  //g_assert_cmpstr(response->to_display, ==, "And some random invitation text.");
  //g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_AKE_IN_PROGRESS);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_3);

  otrv4_response_free(response);
}

void
test_otrv4_receives_query_message(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_response_t *response = otrv4_response_new();
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);
  otrv4_assert(otrv4_receive_message(response, "?OTRv4? And some random invitation text.", 40, otrv4_fixture->otr));

  otrv4_assert(response->to_send);
  g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_AKE_IN_PROGRESS);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_4);

  otrv4_response_free(response);
}

void
test_otrv4_receives_query_message_v3(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V3 };
  otrv4_start(otrv4_fixture->otr, policy);

  otrv4_response_t *response = otrv4_response_new();
  otrv4_assert(otrv4_receive_message(response, "?OTRv3? And some random invitation text.", 40, otrv4_fixture->otr));

  //TODO: How to assert the pointer is not null without g_assert_nonnull?
  //g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_AKE_IN_PROGRESS);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_3);

  otrv4_response_free(response);
}

void
test_otrv4_receives_pre_key_on_start(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  dake_identity_message_t *identity_message = dake_identity_message_new(NULL); //TODO: add profile
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);

  uint8_t *serialized = NULL;
  otrv4_assert(dake_identity_message_aprint(&serialized, NULL, identity_message));

  char message[1000];
  strcpy(message, "?OTR:");
  memcpy(message + 5, serialized, strlen((const char*)serialized) + 1);

  otrv4_response_t *response = otrv4_response_new();
  otrv4_assert(otrv4_receive_message(response, message, strlen(message), otrv4_fixture->otr));

  g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_ENCRYPTED_MESSAGES);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_4);
  g_assert_cmpstr(response->to_display, ==, NULL);
  otrv4_assert(response->to_send);

  dake_dre_auth_t *dre_auth = malloc(sizeof(dake_dre_auth_t));
  //TODO: should base64 decode the message to respond after ?OTR and then
  //deserialize
  dake_dre_auth_deserialize(dre_auth, (uint8_t*) response->to_send, 0);

  free(serialized);
  otrv4_response_free(response);
}

void
test_otrv4_receives_identity_message_invalid_on_start(otrv4_fixture_t *otrv4_fixture, gconstpointer data) {
  char *identity_message = "?OTR:";
  otrv4_policy_t policy = { .allows = OTRV4_ALLOW_V4 };
  otrv4_start(otrv4_fixture->otr, policy);
  otrv4_response_t *response = otrv4_response_new();
  otrv4_assert(otrv4_receive_message(response, identity_message, 5, otrv4_fixture->otr));

  g_assert_cmpint(otrv4_fixture->otr->state, ==, OTRV4_STATE_START);
  g_assert_cmpint(otrv4_fixture->otr->running_version, ==, OTRV4_VERSION_4);
  g_assert_cmpstr(response->to_display, ==, NULL);
  g_assert_cmpstr(response->to_send, ==, NULL);

  otrv4_response_free(response);
}

void
test_otrv4_destroy() {
  cs_keypair_t keypair;
  cs_keypair_generate(keypair);
  otrv4_t *otr = otrv4_new(keypair);

  otrv4_assert(otr->profile != NULL);
  otrv4_destroy(otr);

  otrv4_assert(otr->keypair == NULL);
  otrv4_assert(otr->keys->current == NULL);
  otrv4_assert(otr->profile == NULL);

  free(otr);
}


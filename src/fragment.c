#include "fragment.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAGMENT_FORMAT "?OTR|%08x|%08x,%05x,%05x,%s,"

void fragment_message_free(fragment_message_t *message) {
  for (int i = 0; i < message->total; free(message->pieces[i++])) {}
  free(message->pieces);
  message->pieces = NULL;

  free(message);
}

fragment_context_t *fragment_context_new(void) {
  fragment_context_t *context = malloc(sizeof(fragment_context_t));
  context->N = 0;
  context->K = 0;
  context->fragment = NULL;
  context->fragment_len = 0;
  context->status = OTR4_FRAGMENT_UNFRAGMENTED;

  return context;
}

void fragment_context_free(fragment_context_t *context) {
  context->N = 0;
  context->K = 0;
  context->status = OTR4_FRAGMENT_UNFRAGMENTED;
  free(context->fragment);
  context->fragment = NULL;
  free(context);
}

otr4_err_t otr4_fragment_message(int mms, fragment_message_t *fragments,
                                 int our_instance, int their_instance,
                                 const string_t message) {
  size_t msglen = strlen(message);
  size_t limit_piece = mms - FRAGMENT_HEADER_LEN;
  string_t *pieces;
  int piece_len = 0;

  fragments->total = ((msglen - 1) / (mms - FRAGMENT_HEADER_LEN)) + 1;
  if (fragments->total > 65535)
    return OTR4_ERROR;

  pieces = malloc(fragments->total * sizeof(string_t));
  if (!pieces)
    return OTR4_ERROR;

  int curfrag;
  for (curfrag = 1; curfrag <= fragments->total; curfrag++) {
    int index_len = 0;
    string_t piece = NULL;
    string_t piece_data = NULL;

    if (msglen - index_len < limit_piece)
      piece_len = msglen - index_len;
    else
      piece_len = limit_piece;

    piece_data = malloc(piece_len + 1);
    if (!piece_data) {
      int i;
      for (i = 0; i < fragments->total; free(pieces[i++])) {}
      return OTR4_ERROR;
    }

    strncpy(piece_data, message, piece_len);
    piece_data[piece_len] = 0;

    piece = malloc(piece_len + FRAGMENT_HEADER_LEN + 1);
    if (!piece) {
      for (int i = 0; i < fragments->total; free(pieces[i++])) {}
      free(piece_data);
      return OTR4_ERROR;
    }

    snprintf(piece, piece_len + FRAGMENT_HEADER_LEN, FRAGMENT_FORMAT,
             our_instance, their_instance, curfrag, fragments->total,
             piece_data);
    piece[piece_len + FRAGMENT_HEADER_LEN] = 0;

    pieces[curfrag - 1] = piece;

    free(piece_data);
    index_len += piece_len;
    message += piece_len;
  }

  fragments->pieces = pieces;
  return OTR4_SUCCESS;
}

static bool is_fragment(const string_t message) {
  // TODO: should test if ends with , ?
  return strstr(message, "?OTR|") != NULL;
}

otr4_err_t otr4_defragment_message(fragment_context_t *context,
                                   const string_t message) {
  if (!is_fragment(message)) {
    context->fragment_len = strlen(message);
    context->fragment = malloc(context->fragment_len + 1);
    if (!context->fragment)
      return OTR4_ERROR;

    strcpy(context->fragment, message);
    context->N = 0;
    context->K = 0;
    context->status = OTR4_FRAGMENT_UNFRAGMENTED;
    return OTR4_SUCCESS;
  }

  int sender_tag = 0, receiver_tag = 0, start = 0, end = 0;
  int k = 0, n = 0;
  context->status = OTR4_FRAGMENT_INCOMPLETE;

  const string_t format = "?OTR|%08x|%08x,%05x,%05x,%n%*[^,],%n";
  sscanf(message, format, &sender_tag, &receiver_tag, &k, &n,
         &start, &end);

  char *buff = NULL;
  context->N = n;

  if (k == 1) {
    if (end <= start)
      return OTR4_ERROR;

    context->fragment_len = 0;
    int buff_len = end - start - 1;
    buff = malloc(buff_len + 1);
    if (!buff)
      return OTR4_ERROR;

    memmove(buff, message + start, buff_len);
    context->fragment_len += buff_len;
    buff[context->fragment_len] = '\0';
    context->fragment = buff;
    context->K = k;
  } else {
      if (n == context->N && k == context->K + 1) {
        int buff_len = end - start - 1;
        size_t new_buff_len = context->fragment_len + buff_len + 1;
        buff = realloc(context->fragment, new_buff_len);
        if (!buff)
          return OTR4_ERROR;

        memmove(buff+context->fragment_len, message + start, buff_len);
        context->fragment_len += buff_len;
        buff[context->fragment_len] = '\0';
        context->fragment = buff;
        context->K = k;
      } else {
        free(context->fragment);
        context->fragment = "";
        context->K = 0;
        context->N = 0;
      }
  }

  if (context->N > 0 && context->N == context->K) {
    context->status = OTR4_FRAGMENT_COMPLETE;
  }

  return OTR4_SUCCESS;
}

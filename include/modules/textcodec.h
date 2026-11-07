#ifndef TEXTCODEC_H
#define TEXTCODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "types.h"

typedef enum {
  TD_ENC_UTF8 = 0,
  TD_ENC_UTF16LE,
  TD_ENC_UTF16BE,
  TD_ENC_WINDOWS_1252,
  TD_ENC_ISO_8859_2,
  /* Any encoding an embedder supplies through td_provider_t. The concrete
   * encoding lives in td_state_t::provider_label, not in this enum. */
  TD_ENC_PROVIDER,
} td_encoding_t;

/* Host-supplied codec table. ant ships no legacy CJK tables of its own: an
 * embedder that already has them (a browser's TextCodec, an OS converter)
 * registers them here and ant routes TextDecoder through it.
 *
 * Both callbacks are optional; returning 0 from resolve() means "I do not know
 * this label" and ant keeps its own table as the final answer.
 *
 *   resolve: normalise a label. On success write a NUL-terminated canonical
 *            name into out and return 1.
 *   decode:  decode in[0..in_len) into out as UTF-8. Write at most out_cap
 *            bytes and return the number written, or -1 on invalid input.
 *            decode_mask (set by resolve) carries the fatal flag through, so
 *            an embedder can decide whether invalid bytes become U+FFFD.
 *
 * The provider is process-wide and must outlive every engine. */
typedef struct td_provider {
  int (*resolve)(
    void *user, const char *label, size_t len,
    char *out, size_t cap
  );
  long (*decode)(
    void *user, const char *label,
    const uint8_t *in, size_t in_len,
    char *out, size_t out_cap, bool fatal
  );
  void *user;
} td_provider_t;

typedef struct {
  td_encoding_t encoding;
  uint8_t pending[4];
  int pending_len;
  bool fatal;
  bool ignore_bom;
  bool bom_seen;
  /* TD_ENC_PROVIDER only: the canonical label resolve() produced. */
  char provider_label[32];
} td_state_t;

void init_textcodec_module(ant_t *js);
td_state_t *td_state_new(td_encoding_t enc, bool fatal, bool ignore_bom);

void td_set_provider(const td_provider_t *provider);
const td_provider_t *td_get_provider(void);

ant_value_t td_decode(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream);
ant_value_t te_encode(ant_t *js, const char *str, size_t str_len);


#endif

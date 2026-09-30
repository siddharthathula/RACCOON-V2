/* RACCOON V2 - declarations for the generated wire-format parity reference.
 *
 * The implementations live in build/parity_generated.c, produced by tools/gen_parity.py by
 * actually running the Python tuner code. They are declared here so the C test and the
 * generated file agree on the signatures, and so the compiler can check them.
 */
#ifndef RC_PARITY_H
#define RC_PARITY_H

#ifdef __cplusplus
extern "C" {
#endif

/** The hex frame the Python implementation produces for a payload given as hex. */
const char *rc_parity_python_frame(const char *hex_payload);

/** 1 if the Python decoder accepts this hex frame, 0 if it rejects it, -1 if unknown. */
int rc_parity_python_decode_check(const char *hex);

/** 1 if the Python live-data decode matches the expected field values, 0 otherwise. */
int rc_parity_live_check(const char *hex);

/** The firmware's own live calibration image as hex, dumped from a compiled program. */
const char *rc_parity_c_default_image(void);

/** The tuner's CRC-32C of "" and "abc", as eight hex digits each. */
const char *tuner_crc32c_probe(void);

/** The tuner's unlock proof for a fixed salt over the default image, as hex. */
const char *tuner_unlock_proof(void);

#ifdef __cplusplus
}
#endif
#endif /* RC_PARITY_H */

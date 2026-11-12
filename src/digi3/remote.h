/* Settings changed over the air: an APRS message to this digipeater carries a command from the serial command line, authenticated with a code.
 * Plain C, no Arduino or radio code, so it is tested on a PC (test/host/test_remote.c) and the code tool (tools/remote_code.py) can be checked
 * against it.
 *
 * The message text is   <counter> <command> <code>        e.g.  1794200000 set moving on K7QD2MXA
 *   counter  a number that must be higher than the last one this digi accepted (the tool uses the time in seconds), so a recorded message
 *            cannot be sent again
 *   command  a serial command: get, set, save, beacon or reboot (callsign and radio settings cannot be changed over the air)
 *   code     8 letters and digits: SipHash-2-4, keyed with the secret, of "MYCALL|counter|command", the low 40 bits in base 32
 * The command is readable by anyone, as amateur radio requires; only the code proves it came from someone who holds the secret, and the
 * secret itself never goes on the air. The secret is this chip's 96-bit unique ID unless one is set over the serial port (set secret ...). */
#ifndef D3_REMOTE_H
#define D3_REMOTE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D3R_CODE_LEN 8

uint64_t d3r_siphash(const uint8_t key[16], const uint8_t *msg, size_t len);

/* The key: from the chip ID (the three words as the chip gives them), or from a secret typed over the serial port. */
void d3r_key_from_uid(uint32_t w0, uint32_t w1, uint32_t w2, uint8_t key[16]);
void d3r_key_from_secret(const char *secret, uint8_t key[16]);

/* The code for a command (out gets D3R_CODE_LEN characters and a NUL). */
void d3r_code(const uint8_t key[16], const char *mycall, uint32_t counter, const char *command, char *out);

/* An APRS message (the information field after the source's path) addressed to mycall?  On 1: the text and the message number
 * (empty if none) are copied out. */
int d3r_message_to(const char *info, size_t ilen, const char *mycall, char *text, size_t tcap, char *msgno, size_t mcap);

/* Split "<counter> <command> <code>". Returns 1 on success. command gets the text between, single spaces as sent. */
int d3r_split(const char *text, uint32_t *counter, char *command, size_t ccap, char *code, size_t kcap);

/* 1 if the code matches (letters in either case). */
int d3r_check(const uint8_t key[16], const char *mycall, uint32_t counter, const char *command, const char *code);

#ifdef __cplusplus
}
#endif
#endif

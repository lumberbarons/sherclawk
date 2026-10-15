/* Public TLS value types used by ChatNetwork/RunHost. No network code links in
 * this suite; the transport suites separately use Certainly's real header. */
#ifndef SHERCLAWK_RUN_CTL_TEST_CERTAINLY_H
#define SHERCLAWK_RUN_CTL_TEST_CERTAINLY_H
typedef struct MacTLS_Context MacTLS_Context;
typedef enum {
    kMacTLS_Idle, kMacTLS_Connecting, kMacTLS_Handshaking,
    kMacTLS_Connected, kMacTLS_Closing, kMacTLS_Closed, kMacTLS_Error
} MacTLS_State;
typedef enum {
    kMacTLS_VersionUnknown, kMacTLS_Version12, kMacTLS_Version13
} MacTLS_Version;
#endif

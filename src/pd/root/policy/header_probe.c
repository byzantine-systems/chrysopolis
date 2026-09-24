/* Diagnostic-only compile probe for Root's private pure policy header.
 * Including it twice also verifies its guard without a BEAM include path. */
#define ROOT_POLICY_HEADER "root_policy.h"
#include ROOT_POLICY_HEADER
#include ROOT_POLICY_HEADER

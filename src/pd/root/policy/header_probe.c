/* Diagnostic-only compile probe for Root's private pure headers. Including
 * each twice also verifies its guard without a BEAM include path. */
#define ROOT_POLICY_HEADER "root_policy.h"
#include ROOT_POLICY_HEADER
#include ROOT_POLICY_HEADER

#define ROOT_SPEC_HEADER "spec.h"
#include ROOT_SPEC_HEADER
#include ROOT_SPEC_HEADER

/* Compile one contract header in isolation and include it twice. */
#ifndef RUNTIME_CONTRACT_HEADER
#error "RUNTIME_CONTRACT_HEADER must name one internal runtime header"
#endif

#include RUNTIME_CONTRACT_HEADER
#include RUNTIME_CONTRACT_HEADER

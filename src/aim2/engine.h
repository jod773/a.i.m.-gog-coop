// A.I.M. 2: Clan Wars engine objects used by the co-op code.
#pragma once
#include "aim2.h"

typedef struct { char s[32]; } name32;
typedef struct { char owned; int cap; int pos; uint8_t *buf; } gstream;   // engine memory stream

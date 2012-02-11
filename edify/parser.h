#ifndef _PARSER_H_
#define _PARSER_H_

#include "yydefs.h"

#ifdef YY_USE_CONST
#define yyconst const
#else
#define yyconst
#endif

#ifndef YY_TYPEDEF_YY_BUFFER_STATE
#define YY_TYPEDEF_YY_BUFFER_STATE
typedef struct yy_buffer_state *YY_BUFFER_STATE;
#endif

extern int yyparse(Expr** root, int* error_count);

extern YY_BUFFER_STATE yy_scan_string (yyconst char *yy_str);
extern YY_BUFFER_STATE yy_scan_bytes (yyconst char *bytes, int len);

#endif

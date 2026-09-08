// Lua parser (source code -> bytecode).
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"
#include "lexer.h"

#ifdef UNIT_TESTS
extern void parser_unit_tests(int &Passed, int &Total);
#endif

extern GCproto *lj_parse(LexState *ls);

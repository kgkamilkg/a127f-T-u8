#!/bin/bash
sed -i 's/^YYLTYPE yylloc;/extern YYLTYPE yylloc;/' scripts/dtc/dtc-lexer.l

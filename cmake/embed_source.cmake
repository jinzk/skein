if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
  message(FATAL_ERROR "embed_source.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "embedded source not found: ${INPUT}")
endif()

file(READ "${INPUT}" CONTENT)
file(WRITE "${OUTPUT}"
  "/* Generated from ${INPUT}. Do not edit. */\n"
  "static const char ${SYMBOL}[] = R\"SKEIN_EMBEDDED(\n"
  "${CONTENT}"
  "\n)SKEIN_EMBEDDED\";\n")

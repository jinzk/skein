" Vim syntax file for Skein.
" Language: Skein
" Maintainer: Skein contributors

if exists('b:current_syntax')
  finish
endif

syntax case match

" Strings and characters ----------------------------------------------------
syntax region skeinString start=+"+ skip=+\\\\\|\\"+ end=+"+
      \ contains=skeinEscape,@Spell
syntax region skeinChar start=+'+ skip=+\\\\\|\\'+ end=+'+
      \ contains=skeinEscape

syntax match skeinEscape '\\[ntr\\"''0]' contained
syntax match skeinEscape '\\u[0-9A-Fa-f]\{4}' contained
syntax match skeinEscape '\\u{[0-9A-Fa-f]\{1,6}}' contained

" Numbers -------------------------------------------------------------------
" Integers first, floats second: a later match wins at the same position.
syntax match skeinNumber '\<0[xX][0-9A-Fa-f_]\+\>'
syntax match skeinNumber '\<0[bB][01_]\+\>'
syntax match skeinNumber '\<0[oO][0-7_]\+\>'
syntax match skeinNumber '\<\d[0-9_]*\>'
syntax match skeinFloat '\<\d[0-9_]*\.\([0-9_]*\)\([eE][+-]\?[0-9_]\+\)\?'
syntax match skeinFloat '\<\d[0-9_]*[eE][+-]\?[0-9_]\+\>'
syntax match skeinFloat '\<\.[0-9][0-9_]*\([eE][+-]\?[0-9_]\+\)\?'

" Generic declarations and calls -------------------------------------------
" These are defined before keywords and builtins so that a later keyword or
" builtin match wins when both start at the same position.
syntax match skeinFunctionDef '\<func\s\+\zs\h\w*'
syntax match skeinTypeDef '\<\(struct\|enum\|trait\|impl\|package\|extern\)\s\+\zs\h\w*'
syntax match skeinStructInit '\<\u\w*\ze\s*{'
syntax match skeinFunction '\<\h\w*\ze\s*('
syntax match skeinMethod '\.\h\w*\ze\s*('

" std.<module>.<function> ----------------------------------------------------
syntax match skeinStdNamespace '\<std\ze\.'
syntax match skeinStdModule '\<std\.\h\w*\>'
syntax match skeinStdMember '\<std\.\h\w*\.\h\w*\>'

" Decorators and enum access -------------------------------------------------
syntax match skeinDecorator '@\h\w*'
syntax match skeinEnumMember '\<\u\w*\ze\.'
syntax match skeinEnumVariant '\.\zs\u\w*'

" Keywords ------------------------------------------------------------------
syntax keyword skeinKeyword let const func yield defer launch
syntax keyword skeinKeyword in is as
syntax keyword skeinConditional if else switch case default match
syntax keyword skeinRepeat while do for
syntax keyword skeinStatement return break continue fail on error recover pass
syntax keyword skeinDeclaration package import include export extern
syntax keyword skeinDeclaration struct impl trait enum
syntax keyword skeinBoolean true false
syntax keyword skeinNull null
syntax keyword skeinSelf self

" Labels are defined after keywords so the whole `label name` form wins.
syntax match skeinLabel '\<label\s\+\h\w*\ze\s*:'

" Types ---------------------------------------------------------------------
syntax keyword skeinType any int float number bool string char byte bytes
syntax keyword skeinType list dict deque heap set function builtin callable
syntax keyword skeinType userdata result error task generator socket listener
syntax keyword skeinType websocket stream pair tuple union

" Builtins ------------------------------------------------------------------
syntax keyword skeinBuiltin sqrt exp log abs sin cos tan asin acos atan atan2
      \ pow floor ceil round trunc log2 log10 hypot min max sign clamp fmod mod
      \ is_nan is_inf is_finite cbrt gcd lcm degrees radians
syntax keyword skeinBuiltin len range print println str kind_of type_name
      \ type_package type_module_name type_source format
syntax keyword skeinBuiltin read_file write_file append_file file_exists
      \ file_open file_read file_write file_seek file_tell file_eof file_close
      \ file_flush input split split_ws trim find substr
syntax keyword skeinBuiltin bytes_from_string bytes_to_string bytes_from_char
      \ char_from_bytes bytes_len bytes_get bytes_slice bytes_concat bytes_hex
      \ bytes_from_hex bytes_to_int int_to_bytes read_bytes write_bytes
      \ append_bytes file_read_bytes file_write_bytes

" Operators ------------------------------------------------------------------
syntax match skeinOperator '==\|!=\|<>\|<=\|>=\|&&\|||\|++\|--\|+=\|-=\|\*=\|/=\|%=\|<<\|>>\|->\|=>\|\.\.\.\?'
syntax match skeinOperator '[+\-*/%&|^~!<>=]'

" Comments -------------------------------------------------------------------
" Defined last so `//` wins over the division operator at the same position.
syntax match skeinComment '//.*$' contains=@Spell

" Highlight links ------------------------------------------------------------
highlight def link skeinComment Comment
highlight def link skeinString String
highlight def link skeinChar Character
highlight def link skeinEscape SpecialChar
highlight def link skeinFloat Float
highlight def link skeinNumber Number
highlight def link skeinKeyword Keyword
highlight def link skeinConditional Conditional
highlight def link skeinRepeat Repeat
highlight def link skeinStatement Statement
highlight def link skeinDeclaration StorageClass
highlight def link skeinBoolean Boolean
highlight def link skeinNull Constant
highlight def link skeinSelf Identifier
highlight def link skeinType Type
highlight def link skeinBuiltin Function
highlight def link skeinStdMember Function
highlight def link skeinStdNamespace Identifier
highlight def link skeinStdModule Identifier
highlight def link skeinTypeDef Structure
highlight def link skeinFunctionDef Function
highlight def link skeinStructInit Type
highlight def link skeinFunction Function
highlight def link skeinMethod Function
highlight def link skeinDecorator PreProc
highlight def link skeinEnumMember Type
highlight def link skeinEnumVariant Constant
highlight def link skeinLabel Label
highlight def link skeinOperator Operator

let b:current_syntax = 'skein'

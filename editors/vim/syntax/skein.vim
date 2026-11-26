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
syntax match skeinTypeDef '\<\(struct\|enum\|proto\|impl\|package\|extern\)\s\+\zs\h\w*'
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
syntax keyword skeinKeyword let const func yield defer launch wait
syntax keyword skeinKeyword in is as
syntax keyword skeinConditional if else switch case default match
syntax keyword skeinRepeat while do for
syntax keyword skeinStatement return break continue goto fail on error recover pass
syntax keyword skeinDeclaration package import include export extern
syntax keyword skeinDeclaration struct impl proto enum
syntax keyword skeinBoolean true false
syntax keyword skeinNull null
syntax keyword skeinSelf self

" Labels are a statement-start `name:` form; this is a line-start heuristic.
syntax match skeinLabel '^\s*\h\w*\ze\s*:'

" Types ---------------------------------------------------------------------
syntax keyword skeinType any int float number bool string char byte bytes
syntax keyword skeinType list dict deque heap set function builtin callable
syntax keyword skeinType userdata result error task generator socket listener
syntax keyword skeinType websocket stream pair tuple union

" Builtins ------------------------------------------------------------------
syntax keyword skeinBuiltin len range print println input str kind_of type_name
      \ type_package type_module_name type_source

" Placeholders ---------------------------------------------------------------
syntax match skeinPlaceholder '\$[1-9]\|\$'

" Operators ------------------------------------------------------------------
syntax match skeinOperator '|>\|==\|!=\|<>\|<=\|>=\|&&\|||\|++\|--\|+=\|-=\|\*=\|/=\|%=\|<<\|>>\|->\|=>\|\.\.\.\?'
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
highlight def link skeinPlaceholder Special
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

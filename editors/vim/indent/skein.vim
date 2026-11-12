" Skein indentation: brace-based with a little help for closing braces.
if exists('b:did_indent')
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetSkeinIndent()
setlocal indentkeys=0{,0},0),0],!^F,o,O
setlocal nolisp
setlocal nosmartindent
setlocal autoindent

function! GetSkeinIndent() abort
  let l:prev = prevnonblank(v:lnum - 1)
  if l:prev == 0
    return 0
  endif

  let l:indent = indent(l:prev)
  let l:prevline = getline(l:prev)
  let l:curline = getline(v:lnum)

  if l:curline =~# '^\s*[]})]'
    let l:indent -= shiftwidth()
  endif

  let l:code = substitute(l:prevline, '//.*$', '', '')
  if l:code =~# '{\s*$'
    let l:indent += shiftwidth()
  endif

  return max([0, l:indent])
endfunction

let b:undo_indent =
      \ 'setlocal indentexpr< indentkeys< lisp< smartindent< autoindent<'
      \ . ' | silent! delfunction GetSkeinIndent'

" Skein helpers: run a file and open a REPL.
"
" Set g:skein_executable to use a binary other than `skein`:
"   let g:skein_executable = './build/skein'

function! skein#executable() abort
  return get(g:, 'skein_executable', 'skein')
endfunction

function! skein#run(args) abort
  let l:target = empty(a:args) ? expand('%:p') : a:args
  if empty(l:target)
    echohl ErrorMsg
    echomsg 'Skein: no file to run'
    echohl None
    return
  endif

  if &modified && !empty(expand('%'))
    write
  endif

  execute '!' . shellescape(skein#executable()) . ' ' . shellescape(l:target)
endfunction

function! skein#repl() abort
  let l:cmd = shellescape(skein#executable()) . ' -i'
  if has('terminal')
    execute 'terminal' l:cmd
  else
    execute '!' . l:cmd
  endif
endfunction

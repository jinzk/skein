" Skein filetype settings.
if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

setlocal commentstring=//\ %s
setlocal comments=://
setlocal suffixesadd=.skein

command! -buffer -bar -nargs=? -complete=file SkeinRun call skein#run(<q-args>)
command! -buffer -bar SkeinRepl call skein#repl()

let b:undo_ftplugin =
      \ 'setlocal commentstring< comments< suffixesadd<'
      \ . ' | silent! delcommand SkeinRun'
      \ . ' | silent! delcommand SkeinRepl'

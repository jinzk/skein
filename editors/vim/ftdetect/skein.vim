" Skein filetype detection.
augroup skein_filetype
  autocmd!
  autocmd BufRead,BufNewFile *.skein setfiletype skein
augroup END

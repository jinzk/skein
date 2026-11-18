-- Minimal Neovim 0.5+ bootstrap for `skein lsp --stdio`.
--
-- Usage in init.lua:
--   require('skein.lsp').setup()
--
-- Optional overrides:
--   require('skein.lsp').setup({
--     cmd = {'/path/to/skein', 'lsp', '--stdio', '--typecheck=strict'},
--     root_dir = vim.fn.getcwd(),
--   })

local M = {}

local function notify(message)
  if vim.notify then
    vim.notify(message, vim.log and vim.log.levels and vim.log.levels.WARN or nil)
  else
    print(message)
  end
end

function M.setup(opts)
  if not vim.lsp or not vim.lsp.start then
    notify('skein.lsp requires Neovim 0.5 or newer')
    return false
  end

  opts = opts or {}
  local group = vim.api.nvim_create_augroup('SkeinLsp', { clear = true })
  vim.api.nvim_create_autocmd('FileType', {
    group = group,
    pattern = 'skein',
    callback = function(args)
      local root_dir = opts.root_dir
      if type(root_dir) == 'function' then
        root_dir = root_dir(args)
      end
      if not root_dir or root_dir == '' then
        root_dir = vim.fn.getcwd()
      end

      local executable = vim.g.skein_executable or 'skein'
      local config = {
        name = opts.name or 'skein',
        cmd = opts.cmd or { executable, 'lsp', '--stdio' },
        root_dir = root_dir,
      }
      if opts.on_attach then
        config.on_attach = opts.on_attach
      end
      if opts.capabilities then
        config.capabilities = opts.capabilities
      end
      if opts.settings then
        config.settings = opts.settings
      end

      vim.lsp.start(config, {
        reuse_client = function(client, started_config)
          return client.name == started_config.name
        end,
      })
    end,
  })
  return true
end

return M

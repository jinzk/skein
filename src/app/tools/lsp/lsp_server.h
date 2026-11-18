#ifndef SKEIN_LSP_SERVER_H
#define SKEIN_LSP_SERVER_H

#include <cstdio>
#include <string>
#include <vector>

#include "typecheck.h"

namespace skein {
namespace lsp {

struct lsp_options {
    std::vector<std::string> package_dirs;
    std::string encoding;
    skein::typecheck::Mode typecheck_mode = skein::typecheck::Mode::Warn;
};

int run_lsp_server(const lsp_options &options, FILE *input, FILE *output);

} // namespace lsp
} // namespace skein

#endif

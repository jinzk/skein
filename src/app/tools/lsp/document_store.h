#ifndef SKEIN_LSP_DOCUMENT_STORE_H
#define SKEIN_LSP_DOCUMENT_STORE_H

#include <map>
#include <string>
#include <vector>

#include "tools/tool_analysis.h"

namespace skein {
namespace lsp {

struct lsp_document {
    std::string uri;
    int version = 0;
    std::string text;
    std::vector<analysis_symbol> symbols;
};

class document_store {
public:
    lsp_document *find(const std::string &uri);
    const lsp_document *find(const std::string &uri) const;
    void put(const lsp_document &document);
    void erase(const std::string &uri);

private:
    std::map<std::string, lsp_document> documents_;
};

} // namespace lsp
} // namespace skein

#endif

#include "document_store.h"

namespace skein {
namespace lsp {

lsp_document *document_store::find(const std::string &uri) {
    auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
}

const lsp_document *document_store::find(const std::string &uri) const {
    auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
}

void document_store::put(const lsp_document &document) {
    documents_[document.uri] = document;
}

void document_store::erase(const std::string &uri) { documents_.erase(uri); }

} // namespace lsp
} // namespace skein

#include "builtins_internal.h"

#include "diagnostics.h"
#include "typeinfo.h"

namespace {

const char *meta_builtin_name(int functype) {
    switch (functype) {
        case B_meta_type_info: return "std.meta.type_info";
        case B_meta_fields: return "std.meta.fields";
        case B_meta_variants: return "std.meta.variants";
        case B_meta_methods: return "std.meta.methods";
        case B_meta_traits: return "std.meta.traits";
        case B_meta_implements: return "std.meta.implements";
        case B_meta_has_type: return "std.meta.has_type";
        default: return "std.meta";
    }
}

} // namespace

struct value_t *call_meta_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    const std::string &importer_source = diag_current_source_name();
    const std::string &importer_module = diag_current_package_name();

    if (functype == B_meta_implements) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    meta_builtin_name(functype), (int)args.size());
            return NULL;
        }
        return typeinfo_implements(importer_source.c_str(),
                                   importer_module.c_str(), args[0],
                                   args[1]);
    }

    if (args.size() != 1) {
        yyerror("%s expects 1 argument, got %d",
                meta_builtin_name(functype), (int)args.size());
        return NULL;
    }

    switch (functype) {
        case B_meta_type_info:
            return typeinfo_type_info(importer_source.c_str(),
                                      importer_module.c_str(), args[0]);
        case B_meta_fields:
            return typeinfo_fields(importer_source.c_str(),
                                   importer_module.c_str(), args[0]);
        case B_meta_variants:
            return typeinfo_variants(importer_source.c_str(),
                                     importer_module.c_str(), args[0]);
        case B_meta_methods:
            return typeinfo_methods(importer_source.c_str(),
                                    importer_module.c_str(), args[0]);
        case B_meta_traits:
            return typeinfo_traits(importer_source.c_str(),
                                   importer_module.c_str(), args[0]);
        case B_meta_has_type:
            return typeinfo_has_type(importer_source.c_str(),
                                     importer_module.c_str(), args[0]);
        default:
            yyerror("unknown std.meta function");
            return NULL;
    }
}

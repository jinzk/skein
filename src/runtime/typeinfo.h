#ifndef SKEIN_TYPEINFO_H
#define SKEIN_TYPEINFO_H

struct value_t;

/*
 * Read-only type introspection used by std.meta.
 *
 * Every function takes a type argument: a struct/enum value, a value whose
 * runtime type is a built-in named type, or a type-name string.  Results are
 * freshly allocated ordinary values; no runtime type pointers are exposed.
 */
struct value_t *typeinfo_type_info(const char *importer_source,
                                   const char *importer_module,
                                   struct value_t *type_arg);
struct value_t *typeinfo_fields(const char *importer_source,
                                const char *importer_module,
                                struct value_t *type_arg);
struct value_t *typeinfo_variants(const char *importer_source,
                                  const char *importer_module,
                                  struct value_t *type_arg);
struct value_t *typeinfo_methods(const char *importer_source,
                                 const char *importer_module,
                                 struct value_t *type_arg);
struct value_t *typeinfo_traits(const char *importer_source,
                                const char *importer_module,
                                struct value_t *type_arg);
struct value_t *typeinfo_implements(const char *importer_source,
                                    const char *importer_module,
                                    struct value_t *type_arg,
                                    struct value_t *trait_arg);
struct value_t *typeinfo_has_type(const char *importer_source,
                                  const char *importer_module,
                                  struct value_t *type_arg);

#endif

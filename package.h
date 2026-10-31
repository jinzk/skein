#ifndef CALC_PACKAGE_H
#define CALC_PACKAGE_H

#include <string>
#include <vector>

struct value_t;
struct scope_t;

/* Configure package search roots before parsing the entry source. */
void package_configure(const std::string &source_path,
                       const std::vector<std::string> &cli_dirs);

/* True while an imported package file is being parsed/evaluated. */
bool package_is_loading();

/* Directory used as the lexical base for . and .. imports in the file
 * currently being parsed. */
const std::string &package_current_import_base();

/* Load a package path and return a new namespace value.  base_dir is the
 * lexical directory captured for relative imports. */
value_t *package_import(const char *dotted_path, const char *base_dir,
                        std::string &error);

/* Build a dictionary from exported symbols in `scope`. */
value_t *package_build_namespace(scope_t *scope, std::string &error);

/* Publish the namespace produced by a package block. */
bool package_publish(const char *name, value_t *namespace_value,
                     std::string &error);

#endif

#ifndef SKEIN_GET_TOOL_H
#define SKEIN_GET_TOOL_H

#include <string>
#include <vector>

int run_get_tool(const std::vector<std::string> &args,
                 const std::string &program);
int run_mod_init_tool(const std::vector<std::string> &args,
                      const std::string &program);
int run_mod_clean_tool(const std::vector<std::string> &args,
                       const std::string &program);
int run_mod_download_tool(const std::vector<std::string> &args,
                          const std::string &program);
int run_mod_verify_tool(const std::vector<std::string> &args,
                        const std::string &program);
int run_mod_tidy_tool(const std::vector<std::string> &args,
                      const std::string &program);

#endif

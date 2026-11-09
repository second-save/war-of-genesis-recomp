#pragma once
#include "module.h"
#include <string>

namespace gen {

extern const LiftedModule kLifted_main;
extern const LiftedModule kLifted_chp;
extern const LiftedModule kLifted_pack;
extern const LiftedModule kLifted_chrsys;

const LiftedModule* find_module_by_name(const std::string& name);

}  // namespace gen

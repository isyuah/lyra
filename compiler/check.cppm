module;
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
void check(const Module& functions, const std::vector<Source>& sources);
}

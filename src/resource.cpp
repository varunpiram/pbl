#include "resource.hpp"
namespace blocker {
Resource::~Resource() = default;
Action::~Action() = default;
bool Block::blocks() const { return true; }
void Block::execute(Resource& resource) const { resource.block(); }
bool Unblock::blocks() const { return false; }
void Unblock::execute(Resource& resource) const { resource.unblock(); }
}

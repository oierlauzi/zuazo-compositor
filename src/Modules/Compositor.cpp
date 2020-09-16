#include <zuazo/Modules/Compositor.h>

#include <cassert>

namespace Zuazo::Modules {

std::unique_ptr<Compositor> Compositor::s_singleton;

Compositor::Compositor() 
	: Instance::Module(std::string(name), version)
{
}

Compositor::~Compositor() = default;


const Compositor& Compositor::get() {
	if(!s_singleton) {
		s_singleton = std::unique_ptr<Compositor>(new Compositor);
	}

	assert(s_singleton);
	return *s_singleton;
}

}
#pragma once

#include <zuazo/Instance.h>

#include <memory>

namespace Zuazo::Modules {

class Compositor final
	: public Instance::Module
{
public:
	~Compositor();

	static constexpr std::string_view name = "Compositor";
	static constexpr Version version = Version(0, 1, 0);

	static const Compositor& 				get();

private:
	Compositor();
	Compositor(const Compositor& other) = delete;

	Compositor& 							operator=(const Compositor& other) = delete;

	static std::unique_ptr<Compositor> 		s_singleton;
};

}
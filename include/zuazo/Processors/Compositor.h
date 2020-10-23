#pragma once

#include "../LayerData.h"

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Signal/SourceLayout.h>

namespace Zuazo::Processors {

struct CompositorImpl;
class Compositor 
	: private Utils::Pimpl<CompositorImpl>
	, public ZuazoBase
	, public VideoBase
	, public Signal::SourceLayout<Video>
{
	friend CompositorImpl;
public:
	Compositor(	Instance& instance, 
				std::string name, 
				VideoMode videoMode = VideoMode::ANY );
	Compositor(const Compositor& other) = delete;
	Compositor(Compositor&& other);
	virtual ~Compositor();

	Compositor& 			operator=(const Compositor& other) = delete;
	Compositor& 			operator=(Compositor&& other);

	void					setLayerCount(size_t count);
	size_t					getLayerCount() const;

};

}

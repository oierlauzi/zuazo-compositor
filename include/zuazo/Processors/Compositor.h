#pragma once

#include "../RendererBase.h"

#include <zuazo/ZuazoBase.h>
#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/DepthStencilFormat.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Signal/SourceLayout.h>
#include <zuazo/Math/Transform.h>

namespace Zuazo::Processors {

struct CompositorImpl;
class Compositor 
	: private Utils::Pimpl<CompositorImpl>
	, public ZuazoBase
	, public VideoBase
	, public RendererBase
	, public Signal::SourceLayout<Video>
{
	friend CompositorImpl;
public:
	Compositor(	Instance& instance, 
				std::string name, 
				VideoMode videoMode = VideoMode::ANY,
				Utils::Limit<DepthStencilFormat> depthStencil = Utils::Any<DepthStencilFormat>() );
	Compositor(const Compositor& other) = delete;
	Compositor(Compositor&& other);
	virtual ~Compositor();

	Compositor& 							operator=(const Compositor& other) = delete;
	Compositor& 							operator=(Compositor&& other);

};

}
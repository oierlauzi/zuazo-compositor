#pragma once

#include <zuazo/RendererBase.h>
#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/Signal/ConsumerLayout.h>
#include <zuazo/Signal/DummyPad.h>
#include <zuazo/Layers/VideoSurface.h>

#include <type_traits>

namespace Zuazo::Consumers {

template<typename R>
class RendererWrapper 
	: public ZuazoBase
	, public VideoBase
	, public VideoScalerBase
	, public Signal::ConsumerLayout<Video>
{
public:
	using renderer_type = R;

	static_assert(std::is_convertible<renderer_type*, RendererBase*>::value, "Renderer must expose RendererBase interface");
	static_assert(std::is_convertible<renderer_type*, ZuazoBase*>::value, "Renderer must expose ZuazoBase interface");
	static_assert(std::is_convertible<renderer_type*, VideoBase*>::value, "Renderer must expose VideoBase interface");

	template<typename... Params>
	RendererWrapper(Instance& instance,
					std::string name,
					Params&&... params );
	RendererWrapper(const RendererWrapper& other) = delete;
	RendererWrapper(RendererWrapper&& other) = delete;
	virtual ~RendererWrapper() = default;

	RendererWrapper&					operator=(const RendererWrapper& other) = delete;
	RendererWrapper&					operator=(RendererWrapper&& other) = delete;

	renderer_type&						getRenderer() noexcept;
	const renderer_type&				getRenderer() const noexcept;

private:
	renderer_type						m_renderer;
	Signal::DummyPad<Video>				m_input;
	Layers::VideoSurface				m_surface;


};

}

#include "RendererWrapper.inl"
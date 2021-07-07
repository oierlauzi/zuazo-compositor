#include "RendererWrapper.h"

namespace Zuazo::Consumers {

template<typename R>
template<typename... Params>
inline RendererWrapper<R>::RendererWrapper(	Instance& instance,
											std::string name,
											Params&&... params )
	: ZuazoBase(
		instance,
		std::move(name),
		{},
		{},
		[] (ZuazoBase& base) { //Open callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_renderer.open();
			wrapper.m_surface.open();
		},
		[] (ZuazoBase& base, std::unique_lock<Instance>& lock) { //Async open callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_renderer.asyncOpen(lock);
			wrapper.m_surface.asyncOpen(lock);
		},
		[] (ZuazoBase& base) { //Close callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_renderer.close();
			wrapper.m_surface.close();
		},
		[] (ZuazoBase& base, std::unique_lock<Instance>& lock) { //Async close callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_renderer.asyncClose(lock);
			wrapper.m_surface.asyncClose(lock);
		},
		{} )
	, VideoBase(
		[] (VideoBase& base, const VideoMode& videoMode) { //VideoMode callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_renderer.setVideoMode(videoMode);
		} )
	, VideoScalerBase(
		[] (VideoScalerBase& base, ScalingMode scalingMode) { //ScalingMode callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_surface.setScalingMode(scalingMode);
		},
		[] (VideoScalerBase& base, ScalingFilter scalingFilter) { //ScalingFilter callback
			auto& wrapper = static_cast<RendererWrapper&>(base);
			wrapper.m_surface.setScalingFilter(scalingFilter);
		} )
	, Signal::ConsumerLayout<Video>(
		m_input.getInput() )
	, m_renderer(
		instance,
		getName() + " - Renderer",
		std::forward<Params>(params)... )
	, m_input(
		*this,
		std::string(Signal::makeInputName<Video>()) )
	, m_surface(
		instance,
		getName() + " - Surface",
		m_renderer.getViewportSize() )
{
	//HACK this is to avoid a segfault, as the previous initialization
	//Leaves it as a dangling ptr
	static_cast<Signal::ConsumerLayout<Video>&>(*this) = 
		Signal::ConsumerLayout<Video>(m_input.getInput());

	//Register the pads
	Signal::Layout::registerPad(m_input.getInput());

	//Configure the renderer
	m_renderer.setDepthStencilFormat(DepthStencilFormat::none);
	m_renderer.setLayers({ m_surface });

	//Route the input
	m_surface << m_input;

	//Tie the callbacks
	m_renderer.setVideoModeNegotiationCallback(
		[this](VideoBase& base, const std::vector<VideoMode>& videoModes) -> VideoMode {
			this->setVideoModeCompatibility(videoModes);
			return this->getVideoMode();
		}
	);
	m_renderer.setViewportSizeCallback(
		std::bind(&Layers::VideoSurface::setSize, std::ref(m_surface), std::placeholders::_2)
	);
}

template<typename R>
inline typename RendererWrapper<R>::renderer_type&
RendererWrapper<R>::getRenderer() noexcept {
	return m_renderer;
}

template<typename R>
inline const typename RendererWrapper<R>::renderer_type&
RendererWrapper<R>::getRenderer() const noexcept {
	return m_renderer;
}

}
#pragma once

#include "LayerBase.h"
#include "../../LayerData.h"

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/Signal/ProcessorLayout.h>
#include <zuazo/Utils/Pimpl.h>

#include <functional>

namespace Zuazo::Processors::Layers {

struct VideoSurfaceImpl;
class VideoSurface
	: private Utils::Pimpl<VideoSurfaceImpl>
	, public ZuazoBase
	, public LayerBase
	, public VideoScalerBase
	, public Signal::ProcessorLayout<Video, LayerDataStream>
{
	friend VideoSurfaceImpl;
public:
	VideoSurface(	Instance& instance,
					std::string name,
					const RendererBase* renderer,
					Math::Vec2f size );
	VideoSurface(const VideoSurface& other) = delete;
	VideoSurface(VideoSurface&& other);
	virtual ~VideoSurface();

	VideoSurface&							operator=(const VideoSurface& other) = delete;
	VideoSurface&							operator=(VideoSurface&& other);

	void									setSize(Math::Vec2f size);
	Math::Vec2f								getSize() const;

};

}
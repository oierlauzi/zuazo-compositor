#pragma once

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/LayerBase.h>
#include <zuazo/Signal/ConsumerLayout.h>
#include <zuazo/Utils/Pimpl.h>

#include <functional>

namespace Zuazo::Layers {

struct VideoSurfaceImpl;
class VideoSurface
	: private Utils::Pimpl<VideoSurfaceImpl>
	, public ZuazoBase
	, public LayerBase
	, public VideoScalerBase
	, public Signal::ConsumerLayout<Video>
{
	friend VideoSurfaceImpl;
public:
	VideoSurface(	Instance& instance,
					std::string name,
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
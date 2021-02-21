#pragma once

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/LayerBase.h>
#include <zuazo/Signal/ConsumerLayout.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Math/Bezier.h>

#include <functional>

namespace Zuazo::Processors::Layers {

struct BezierCropImpl;
class BezierCrop
	: private Utils::Pimpl<BezierCropImpl>
	, public ZuazoBase
	, public LayerBase
	, public VideoScalerBase
	, public Signal::ConsumerLayout<Video>
{
	friend BezierCropImpl;
public:
	using BezierLoop = Math::BezierLoop<Math::Vec2f, 3>;

	BezierCrop(	Instance& instance,
				std::string name,
				const RendererBase* renderer,
				BezierLoop shape );
	BezierCrop(const BezierCrop& other) = delete;
	BezierCrop(BezierCrop&& other);
	virtual ~BezierCrop();

	BezierCrop&								operator=(const BezierCrop& other) = delete;
	BezierCrop&								operator=(BezierCrop&& other);

	void									setShape(BezierLoop shape);
	const BezierLoop&						getShape() const;

};

}
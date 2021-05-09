#pragma once

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/LayerBase.h>
#include <zuazo/Signal/ConsumerLayout.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Math/BezierLoop.h>

#include <functional>

namespace Zuazo::Layers {

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
				Math::Vec2f size,
				Utils::BufferView<const BezierLoop> crop );
	BezierCrop(const BezierCrop& other) = delete;
	BezierCrop(BezierCrop&& other);
	virtual ~BezierCrop();

	BezierCrop&								operator=(const BezierCrop& other) = delete;
	BezierCrop&								operator=(BezierCrop&& other);

	void									setSize(Math::Vec2f size);
	Math::Vec2f								getSize() const;

	void									setCrop(Utils::BufferView<const BezierLoop> crop);
	Utils::BufferView<const BezierLoop>		getCrop() const;

	void									setLineColor(const Math::Vec4f& color);
	const Math::Vec4f&						getLineColor() const;

	void									setLineWidth(float width);
	float									getLineWidth() const;

	void									setLineSmoothness(float smoothness);
	float									getLineSmoothness() const;

};

}
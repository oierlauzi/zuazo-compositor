#pragma once

#include "RendererBase.h"

#include <zuazo/BlendingMode.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Graphics/Vulkan.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Math/Transform.h>

#include <functional>

namespace Zuazo {

class LayerBase {
public:
	using TransformCallback = std::function<void(LayerBase&, const Math::Transformf&)>;
	using OpacityCallback = std::function<void(LayerBase&, float)>;
	using BlendingModeCallback = std::function<void(LayerBase&, BlendingMode)>;
	using DrawCallback = std::function<void(const LayerBase&, Graphics::CommandBuffer&)>;
	using RenderPassCallback = std::function<void(LayerBase&, vk::RenderPass)>;

	LayerBase(	const RendererBase* renderer,
				TransformCallback transformCbk = {},
				OpacityCallback opacityCbk = {},
				BlendingModeCallback blendingModeCbk = {},
				DrawCallback drawCbk = {},
				RenderPassCallback renderPassCbk = {} );
	LayerBase(const LayerBase& other) = delete;
	LayerBase(LayerBase&& other);
	virtual ~LayerBase();

	LayerBase&							operator=(const LayerBase& other) = delete;
	LayerBase&							operator=(LayerBase&& other);

	void								setRenderer(const RendererBase* renderer);
	const RendererBase* 				getRenderer() const;

	void								setTransform(const Math::Transformf& trans);
	const Math::Transformf& 			getTransform() const;
	
	void								setOpacity(float opa);
	float								getOpacity() const;

	void								setBlendingMode(BlendingMode mode);
	BlendingMode						getBlendingMode() const;

	void								draw(Graphics::CommandBuffer& cmd) const;

	vk::RenderPass						getRenderPass() const;

protected:
	void								setTransformCallback(TransformCallback cbk);
	const TransformCallback&			getTransformCallback() const;

	void								setOpacityCallback(OpacityCallback cbk);
	const OpacityCallback&				getOpacityCallback() const;

	void								setBlendingModeCallback(BlendingModeCallback cbk);
	const BlendingModeCallback&			getBlendingModeCallback() const;

	void								setDrawCallback(DrawCallback cbk);
	const DrawCallback&					getDrawCallback() const;

	void								setRenderPassCallback(RenderPassCallback cbk);
	const RenderPassCallback&			getRenderPassCallback() const;

private:
	struct Impl;
	Utils::Pimpl<Impl>					m_impl;

};


}
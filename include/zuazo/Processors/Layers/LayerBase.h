#pragma once

#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Graphics/Vulkan.h>
#include <zuazo/Graphics/Frame.h>

#include <functional>

namespace Zuazo::Processors::Layers {

class LayerBase {
public:
	using RenderPassCallback = std::function<void(LayerBase&, vk::RenderPass)>;

	LayerBase(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& frameDesc, 
				DepthStencilFormat depthStencil,
				RenderPassCallback renderPassCbk = {});
	LayerBase(const LayerBase& other) = delete;
	LayerBase(LayerBase&& other);
	virtual ~LayerBase();

	LayerBase&							operator=(const LayerBase& other) = delete;
	LayerBase&							operator=(LayerBase&& other);

	void								setFramebufferLayout(	const Graphics::Frame::Descriptor& frameDesc,
																DepthStencilFormat depthStencil);

	const Graphics::Frame::Descriptor&	getFramebufferFrameDescriptor() const;
	DepthStencilFormat					getFramebufferDepthStencilFormat() const;

	vk::RenderPass						getRenderPass() const;
	vk::DescriptorSetLayout				getBaseDescriptorSetLayout() const;

protected:
	void								setRenderPassCallback(RenderPassCallback cbk);
	const RenderPassCallback&			getRenderPassCallback() const;

private:
	struct Impl;
	Utils::Pimpl<Impl>				m_impl;

};


}
#include <zuazo/Processors/Layers/LayerBase.h>

#include <zuazo/Processors/Compositor.h>

#include <utility>

namespace Zuazo::Processors::Layers {

struct LayerBase::Impl {
	std::reference_wrapper<const Graphics::Vulkan> 	vulkan; 
	Graphics::Frame::Descriptor 					frameDescriptor;
	DepthStencilFormat 								depthStencilFormat;
	RenderPassCallback								renderPassCallback;

	vk::RenderPass									renderPass;
	vk::DescriptorSetLayout							baseDescriptorSet;

	Impl(	const Graphics::Vulkan& vulkan, 
			const Graphics::Frame::Descriptor& frameDesc, 
			DepthStencilFormat depthStencil,
			RenderPassCallback renderPassCbk )
		: vulkan(vulkan)
		, frameDescriptor(frameDesc)
		, depthStencilFormat(depthStencil)
		, renderPassCallback(std::move(renderPassCbk))
		, renderPass(Compositor::getRenderPass(vulkan, frameDescriptor, depthStencilFormat))
		, baseDescriptorSet(Compositor::getDescriptorSetLayout(vulkan))
	{
	}

	~Impl() = default;


	void setFramebufferLayout(	LayerBase& base,
								const Graphics::Frame::Descriptor& frameDesc,
								DepthStencilFormat depthStencil)
	{
		frameDescriptor = frameDesc;
		depthStencilFormat = depthStencil;

		const auto newRenderPass = Compositor::getRenderPass(vulkan, frameDescriptor, depthStencilFormat);
		if(newRenderPass != renderPass) {
			renderPass = newRenderPass;
			Utils::invokeIf(renderPassCallback, base, renderPass);
		}
	}


	const Graphics::Frame::Descriptor& getFramebufferFrameDescriptor() const {
		return frameDescriptor;
	}

	DepthStencilFormat getFramebufferDepthStencilFormat() const {
		return depthStencilFormat;
	}


	vk::RenderPass getRenderPass() const {
		return renderPass;
	}

	vk::DescriptorSetLayout getBaseDescriptorSetLayout() const {
		return baseDescriptorSet;
	}


	void setRenderPassCallback(RenderPassCallback cbk) {
		renderPassCallback = std::move(cbk);
	}

	const RenderPassCallback& getRenderPassCallback() const {
		return renderPassCallback;
	}
		
};


LayerBase::LayerBase(	const Graphics::Vulkan& vulkan, 
						const Graphics::Frame::Descriptor& frameDesc, 
						DepthStencilFormat depthStencil,
						RenderPassCallback renderPassCbk )
	: m_impl({}, vulkan, frameDesc, depthStencil, std::move(renderPassCbk))
{
}

LayerBase::LayerBase(LayerBase&& other) = default;

LayerBase::~LayerBase() = default;

LayerBase& LayerBase::operator=(LayerBase&& other) = default;



void LayerBase::setFramebufferLayout(	const Graphics::Frame::Descriptor& frameDesc,
										DepthStencilFormat depthStencil)
{
	m_impl->setFramebufferLayout(*this, frameDesc, depthStencil);
}


const Graphics::Frame::Descriptor& LayerBase::getFramebufferFrameDescriptor() const {
	return m_impl->getFramebufferFrameDescriptor();
}

DepthStencilFormat LayerBase::getFramebufferDepthStencilFormat() const {
	return m_impl->getFramebufferDepthStencilFormat();
}


vk::RenderPass LayerBase::getRenderPass() const {
	return m_impl->getRenderPass();
}

vk::DescriptorSetLayout LayerBase::getBaseDescriptorSetLayout() const {
	return m_impl->getBaseDescriptorSetLayout();
}


void LayerBase::setRenderPassCallback(RenderPassCallback cbk) {
	m_impl->setRenderPassCallback(std::move(cbk));
}

const LayerBase::RenderPassCallback& LayerBase::getRenderPassCallback() const {
	return m_impl->getRenderPassCallback();
}



}
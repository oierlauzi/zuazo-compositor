#include <zuazo/RendererBase.h>

#include <zuazo/Utils/StaticId.h>

namespace Zuazo {

struct RendererBase::Impl {
	enum VideoModeCallbacks {
		DSCBK_INTERNAL,
		DSCBK_EXTERNAL,
		DSCBK_COUNT
	};

	Utils::Limit<DepthStencilFormat>					depthStencilFmtLimits;
	Utils::Limit<DepthStencilFormat>					depthStencilFmtCompatibility;
	Utils::Limit<DepthStencilFormat>					depthStencilFmt;

	Camera												camera;	

	DepthStencilFormatCallback							depthStencilFmtCompatibilityCbk;
	std::array<DepthStencilFormatCallback, DSCBK_COUNT>	depthStencilFmtCbk;
	CameraCallback										cameraCbk;
	RenderPassQueryCallback								renderPassQueryCbk;


	Impl(	Utils::Limit<DepthStencilFormat> depthStencil,
			DepthStencilFormatCallback internalDepthStencilFormatCbk, 
			CameraCallback cameraCbk, 
			RenderPassQueryCallback renderPassQueryCbk)
		: depthStencilFmtLimits(std::move(depthStencil))
		, depthStencilFmtCompatibility()
		, depthStencilFmt()
		, camera()
		, depthStencilFmtCompatibilityCbk()
		, depthStencilFmtCbk{ std::move(internalDepthStencilFormatCbk) }
		, cameraCbk(std::move(cameraCbk))
		, renderPassQueryCbk(std::move(renderPassQueryCbk))
	{
	}

	~Impl() = default;


	void setDepthStencilFormatCompatibilityCallback(DepthStencilFormatCallback cbk) {
		depthStencilFmtCompatibilityCbk = std::move(cbk);
	}

	const DepthStencilFormatCallback& getDepthStencilFormatCompatibilityCallback() const {
		return depthStencilFmtCompatibilityCbk;
	}


	void setDepthStencilFormatCallback(DepthStencilFormatCallback cbk) {
		depthStencilFmtCbk[DSCBK_EXTERNAL] = std::move(cbk);
	}

	const DepthStencilFormatCallback& getDepthStencilFormatCallback() const {
		return depthStencilFmtCbk[DSCBK_EXTERNAL];
	}


	void setDepthStencilFormatLimits(RendererBase& base, Utils::Limit<DepthStencilFormat> lim) {
		if(depthStencilFmtLimits != lim) {
			depthStencilFmtLimits = std::move(lim);
			updateDepthStencilFormat(base);
		}
	}

	const Utils::Limit<DepthStencilFormat>& getDepthStencilFormatLimits() const {
		return depthStencilFmtLimits;
	}

	const Utils::Limit<DepthStencilFormat>& getDepthStencilFormatCompatibility() const {
		return depthStencilFmtCompatibility;
	}

	const Utils::Limit<DepthStencilFormat>& getDepthStencilFormat() const {
		return depthStencilFmt;
	}



	void setCamera(RendererBase& base, const Camera& cam) {
		if(camera != cam) {
			camera = cam;
			Utils::invokeIf(cameraCbk, base, camera);
		}
	}

	const Camera& getCamera() const {
		return camera;
	}


	vk::RenderPass getRenderPass(const RendererBase& base) const {
		vk::RenderPass result = {};

		if(renderPassQueryCbk) {
			result = renderPassQueryCbk(base);
		}

		return result;
	}

	static vk::DescriptorSetLayout getDescriptorSetLayout(const Graphics::Vulkan& vulkan) {
		static const Utils::StaticId id;

		auto result = vulkan.createDescriptorSetLayout(id);

		if(!result) {
			//Create the bindings
			const std::array bindings = {
				vk::DescriptorSetLayoutBinding(	//UBO binding
					DESCRIPTOR_BINDING_PROJECTION_MATRIX,			//Binding
					vk::DescriptorType::eUniformBuffer,				//Type
					1,												//Count
					vk::ShaderStageFlagBits::eVertex,				//Shader stage
					nullptr											//Immutable samplers
				), 
				vk::DescriptorSetLayoutBinding(	//UBO binding
					DESCRIPTOR_BINDING_COLOR_TRANSFER,				//Binding
					vk::DescriptorType::eUniformBuffer,				//Type
					1,												//Count
					vk::ShaderStageFlagBits::eFragment,				//Shader stage
					nullptr											//Immutable samplers
				), 
			};

			const vk::DescriptorSetLayoutCreateInfo createInfo(
				{},
				bindings.size(), bindings.data()
			);

			result = vulkan.createDescriptorSetLayout(id, createInfo);
		}

		assert(result);
		return result;
	}


	void setDepthStencilFormatCompatibility(RendererBase& base, Utils::Limit<DepthStencilFormat> comp) {
		if(depthStencilFmtCompatibility != comp) {
			depthStencilFmtCompatibility = std::move(comp);
			Utils::invokeIf(depthStencilFmtCompatibilityCbk, base, depthStencilFmtCompatibility);
			updateDepthStencilFormat(base);
		}
	}


	void setInternalDepthStencilFormatCallback(DepthStencilFormatCallback cbk) {
		depthStencilFmtCbk[DSCBK_INTERNAL] = std::move(cbk);
	}
	
	const DepthStencilFormatCallback& getInternalDepthStencilFormatCallback() const {
		return depthStencilFmtCbk[DSCBK_INTERNAL];
	}
	

	void setCameraCallback(CameraCallback cbk) {
		cameraCbk = std::move(cbk);
	}

	const CameraCallback& getCameraCallback() const {
		return cameraCbk;
	}


	void setRenderPassQueryCallbackCallback(RenderPassQueryCallback cbk) {
		renderPassQueryCbk = std::move(cbk);
	}

	const RenderPassQueryCallback& getRenderPassQueryCallback() const {
		return renderPassQueryCbk;
	}
	
private:
	void updateDepthStencilFormat(RendererBase& base) noexcept {
		auto newDepthStencilFormat = depthStencilFmtCompatibility.intersect(depthStencilFmtLimits);

		if(newDepthStencilFormat != depthStencilFmt) {
			//Depth Stencil format has changed
			depthStencilFmt = std::move(newDepthStencilFormat);

			//Call the callbacks
			for(const auto& cbk : depthStencilFmtCbk) {
				Utils::invokeIf(cbk, base, depthStencilFmt);
			}
		}
	}

};



RendererBase::RendererBase(	Utils::Limit<DepthStencilFormat> depthStencil,
							DepthStencilFormatCallback internalDepthStencilFormatCbk,
							CameraCallback cameraCbk,
							RenderPassQueryCallback renderPassQueryCbk )
	: m_impl({}, std::move(depthStencil), std::move(internalDepthStencilFormatCbk), std::move(cameraCbk), std::move(renderPassQueryCbk))
{
}

RendererBase::RendererBase(RendererBase&& other) = default;

RendererBase::~RendererBase() = default; 

RendererBase& RendererBase::operator=(RendererBase&& other) = default;



void RendererBase::setDepthStencilFormatCompatibilityCallback(DepthStencilFormatCallback cbk) {
	m_impl->setDepthStencilFormatCompatibilityCallback(std::move(cbk));
}

const RendererBase::DepthStencilFormatCallback& RendererBase::getDepthStencilFormatCompatibilityCallback() const {
	return m_impl->getDepthStencilFormatCompatibilityCallback();
}


void RendererBase::setDepthStencilFormatCallback(DepthStencilFormatCallback cbk) {
	m_impl->setDepthStencilFormatCallback(std::move(cbk));
}

const RendererBase::DepthStencilFormatCallback& RendererBase::getDepthStencilFormatCallback() const {
	return m_impl->getDepthStencilFormatCallback();
}


void RendererBase::setDepthStencilFormatLimits(Utils::Limit<DepthStencilFormat> lim) {
	m_impl->setDepthStencilFormatLimits(*this, std::move(lim));
}

const Utils::Limit<DepthStencilFormat>&	RendererBase::getDepthStencilFormatLimits() const {
	return m_impl->getDepthStencilFormatLimits();
}

const Utils::Limit<DepthStencilFormat>& RendererBase::getDepthStencilFormatCompatibility() const {
	return m_impl->getDepthStencilFormatCompatibility();
}

const Utils::Limit<DepthStencilFormat>&	RendererBase::getDepthStencilFormat() const {
	return m_impl->getDepthStencilFormat();
}


void RendererBase::setCamera(const Camera& cam) {
	m_impl->setCamera(*this, cam);
}

const RendererBase::Camera& RendererBase::getCamera() const {
	return m_impl->getCamera();
}


vk::RenderPass RendererBase::getRenderPass() const {
	return m_impl->getRenderPass(*this);
}

vk::DescriptorSetLayout	RendererBase::getDescriptorSetLayout(const Graphics::Vulkan& vulkan) {
	return Impl::getDescriptorSetLayout(vulkan);
}



void RendererBase::setDepthStencilFormatCompatibility(Utils::Limit<DepthStencilFormat> comp) {
	m_impl->setDepthStencilFormatCompatibility(*this, std::move(comp));
}

void RendererBase::setInternalDepthStencilFormatCallback(DepthStencilFormatCallback cbk) {
	m_impl->setInternalDepthStencilFormatCallback(std::move(cbk));
}

const RendererBase::DepthStencilFormatCallback& RendererBase::getInternalDepthStencilFormatCallback() const {
	return m_impl->getInternalDepthStencilFormatCallback();
}


void RendererBase::setCameraCallback(CameraCallback cbk) {
	m_impl->setCameraCallback(std::move(cbk));
}

const RendererBase::CameraCallback&	RendererBase::getCameraCallback() const {
	return m_impl->getCameraCallback();
}


void RendererBase::setRenderPassQueryCallbackCallback(RenderPassQueryCallback cbk) {
	m_impl->setRenderPassQueryCallbackCallback(std::move(cbk));
}

const RendererBase::RenderPassQueryCallback& RendererBase::getRenderPassQueryCallback() const {
	return m_impl->getRenderPassQueryCallback();
}

}